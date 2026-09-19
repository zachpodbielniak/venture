/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
/* Routing rules, the round-robin rota and the scoring formula. The lead
 * service owns the transaction and the timeline; this file only evaluates. */
#include "venture.h"
#include <string.h>

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureLeadService: %s", message);
	return FALSE;
}

typedef struct { gchar *field; gchar *value; gboolean pattern; } Condition;

static void
condition_free(gpointer data)
{
	Condition *condition = data;
	g_free(condition->field);
	g_free(condition->value);
	g_free(condition);
}

/* One condition per line or semicolon; the first '=' or '~' splits it. */
static GPtrArray *
parse(const gchar *conditions, GError **error)
{
	g_autoptr(GPtrArray) parsed = g_ptr_array_new_with_free_func(condition_free);
	g_auto(GStrv) lines = g_strsplit_set(conditions != NULL ? conditions : "", "\n;", -1);
	guint i;
	for (i = 0; lines[i] != NULL; i++)
	{
		gchar *line = g_strstrip(lines[i]);
		gchar *split = line + strcspn(line, "=~");
		Condition *condition;
		gchar *field;
		guint k;
		if (*line == '\0') continue;
		if (*split == '\0')
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "each condition is field=value or field~pattern");
			return NULL;
		}
		condition = g_new0(Condition, 1);
		g_ptr_array_add(parsed, condition);
		condition->pattern = *split == '~';
		condition->field = g_strstrip(g_strndup(line, split - line));
		condition->value = g_strstrip(g_strdup(split + 1));
		field = condition->field;
		for (k = 0; field[k] != '\0'; k++)
			if (!g_ascii_isalnum(field[k]) && field[k] != '_' && field[k] != '-') break;
		if (*field == '\0' || field[k] != '\0')
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "condition fields are lead field or attribute names");
			return NULL;
		}
		if (condition->pattern)
		{
			g_autoptr(GError) local_error = NULL;
			g_autoptr(GRegex) regex = g_regex_new(condition->value, G_REGEX_CASELESS, 0, &local_error);
			if (regex == NULL)
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
					"VentureLeadService: invalid pattern for %s: %s", field, local_error->message);
				return NULL;
			}
		}
	}
	return g_steal_pointer(&parsed);
}

gboolean
venture_lead_conditions_validate(const gchar *conditions, GError **error)
{
	g_autoptr(GPtrArray) parsed = parse(conditions, error);
	return parsed != NULL;
}

/* The lead's value for a field as text, plus the referenced record's name
 * when the field is a reference, so "campaign=Spring" reads naturally. */
static void
lead_value(VentureDatabase *database, VentureEntity *lead, const gchar *field, gchar **text, gchar **name)
{
	GObjectClass *klass = G_OBJECT_GET_CLASS(lead);
	g_autofree gchar *property = venture_entity_column_to_property(field);
	GParamSpec *spec = g_object_class_find_property(klass, property);
	const gchar *target;
	*text = NULL;
	*name = NULL;
	if (spec == NULL)
	{
		g_autofree gchar *reference = g_strconcat(property, "-id", NULL);
		spec = g_object_class_find_property(klass, reference);
		if (spec != NULL) { g_free(property); property = g_steal_pointer(&reference); }
	}
	if (spec == NULL)
	{
		g_autofree gchar *underscored = g_strdelimit(g_strdup(field), "-", '_');
		const gchar *attribute = venture_entity_get_attribute(lead, field);
		if (attribute == NULL) attribute = venture_entity_get_attribute(lead, underscored);
		*text = g_strdup(attribute != NULL ? attribute : "");
		return;
	}
	{
		g_auto(GValue) value = G_VALUE_INIT;
		g_autoptr(JsonNode) node = NULL;
		g_value_init(&value, spec->value_type);
		g_object_get_property(G_OBJECT(lead), property, &value);
		node = venture_json_node_from_value(&value);
		if (JSON_NODE_HOLDS_VALUE(node))
		{
			GType type = json_node_get_value_type(node);
			if (type == G_TYPE_STRING) *text = g_strdup(json_node_get_string(node));
			else if (type == G_TYPE_BOOLEAN) *text = g_strdup(json_node_get_boolean(node) ? "true" : "false");
			else if (type == G_TYPE_INT64) *text = g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));
			else *text = g_strdup_printf("%g", json_node_get_double(node));
		}
		if (*text == NULL) *text = g_strdup("");
	}
	target = venture_entity_class_get_reference(VENTURE_ENTITY_GET_CLASS(lead), property);
	if (target != NULL && spec->value_type == G_TYPE_INT64)
	{
		GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), target);
		gint64 id = 0;
		g_object_get(lead, property, &id, NULL);
		if (type != G_TYPE_INVALID && id != 0)
		{
			g_autoptr(VentureEntity) referenced = venture_database_get(database, type, id, NULL);
			if (referenced != NULL && g_object_class_find_property(G_OBJECT_GET_CLASS(referenced), "name") != NULL)
				g_object_get(referenced, "name", name, NULL);
		}
	}
}

gboolean
venture_lead_conditions_match(VentureDatabase *database, VentureEntity *lead,
	const gchar *conditions, gboolean *matched, GError **error)
{
	g_autoptr(GPtrArray) parsed = parse(conditions, error);
	guint i;
	*matched = FALSE;
	if (parsed == NULL) return FALSE;
	for (i = 0; i < parsed->len; i++)
	{
		Condition *condition = g_ptr_array_index(parsed, i);
		g_autofree gchar *text = NULL;
		g_autofree gchar *name = NULL;
		gboolean holds;
		lead_value(database, lead, condition->field, &text, &name);
		if (condition->pattern)
		{
			g_autoptr(GRegex) regex = g_regex_new(condition->value, G_REGEX_CASELESS, 0, NULL);
			holds = g_regex_match(regex, text, 0, NULL) || (name != NULL && g_regex_match(regex, name, 0, NULL));
		}
		else holds = g_ascii_strcasecmp(text, condition->value) == 0 ||
			(name != NULL && g_ascii_strcasecmp(name, condition->value) == 0);
		if (!holds) return TRUE;
	}
	*matched = TRUE;
	return TRUE;
}

static gboolean
same_organization(VentureDatabase *database, GType type, gint64 id, gint64 organization, const gchar *what, GError **error)
{
	g_autoptr(VentureEntity) target = NULL;
	g_autofree gchar *message = NULL;
	if (id == 0)
	{
		message = g_strdup_printf("this action requires a %s", what);
		return refuse(error, VENTURE_ERROR_VALIDATION, message);
	}
	target = venture_database_get(database, type, id, NULL);
	if (target == NULL || venture_entity_is_deleted(target) || venture_entity_get_organization_id(target) != organization)
	{
		message = g_strdup_printf("the %s must exist in the rule's organization", what);
		return refuse(error, VENTURE_ERROR_VALIDATION, message);
	}
	return TRUE;
}

static gboolean
orgaccess_enabled(GError **error)
{
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "team_membership") == G_TYPE_INVALID)
		return refuse(error, VENTURE_ERROR_VALIDATION, "round-robin routing needs the orgaccess module");
	return TRUE;
}

gboolean
venture_lead_routing_validate_rule(VentureDatabase *database, VentureEntity *rule, GError **error)
{
	g_autofree gchar *conditions = NULL;
	g_object_get(rule, "conditions", &conditions, NULL);
	if (!venture_lead_conditions_validate(conditions, error)) return FALSE;
	if (VENTURE_IS_LEAD_ROUTING_RULE(rule))
	{
		g_autofree gchar *assign_to = NULL;
		VentureLeadRoutingAction action;
		gint64 team = 0, venture = 0, organization = venture_entity_get_organization_id(rule);
		g_object_get(rule, "action", &action, "assign-to", &assign_to, "team-id", &team, "venture-id", &venture, NULL);
		switch (action)
		{
		case VENTURE_LEAD_ROUTING_ASSIGN_USER:
			if (venture_string_is_empty(assign_to))
				return refuse(error, VENTURE_ERROR_VALIDATION, "assign_user needs assign_to");
			break;
		case VENTURE_LEAD_ROUTING_ROUND_ROBIN:
			if (!orgaccess_enabled(error)) return FALSE;
			if (!same_organization(database, VENTURE_TYPE_TEAM, team, organization, "team", error)) return FALSE;
			break;
		case VENTURE_LEAD_ROUTING_ASSIGN_VENTURE:
			if (!same_organization(database, VENTURE_TYPE_VENTURE, venture, organization, "venture", error)) return FALSE;
			break;
		default:
			return refuse(error, VENTURE_ERROR_VALIDATION, "unknown routing action");
		}
	}
	return TRUE;
}

static GPtrArray *
rows(VentureDatabase *database, GType type, gint64 organization, const gchar *order, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, organization);
	venture_query_set_limit(query, 0);
	if (order != NULL) venture_query_add_order(query, order, VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(database, query, error);
}

static gboolean
active(VentureEntity *entity)
{
	gboolean value = FALSE;
	g_object_get(entity, "active", &value, NULL);
	return value;
}

/* Usernames of the team's members who still hold an active team membership,
 * an active organization membership and an active account, by user id. */
static GPtrArray *
rota(VentureDatabase *database, gint64 team, gint64 organization, GError **error)
{
	g_autoptr(GPtrArray) members = NULL;
	g_autoptr(GPtrArray) memberships = rows(database, VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, organization, "user-id", error);
	g_autoptr(GPtrArray) pool = g_ptr_array_new_with_free_func(g_free);
	guint i, j;
	if (memberships == NULL) return NULL;
	members = rows(database, VENTURE_TYPE_TEAM_MEMBERSHIP, organization, "user-id", error);
	if (members == NULL) return NULL;
	for (i = 0; i < members->len; i++)
	{
		VentureEntity *member = g_ptr_array_index(members, i);
		g_autoptr(VentureEntity) account = NULL;
		g_autofree gchar *username = NULL;
		gint64 member_team = 0, user = 0;
		gboolean admitted = FALSE;
		g_object_get(member, "team-id", &member_team, "user-id", &user, NULL);
		if (member_team != team || !active(member)) continue;
		for (j = 0; j < memberships->len && !admitted; j++)
		{
			VentureEntity *membership = g_ptr_array_index(memberships, j);
			gint64 member_user = 0;
			g_object_get(membership, "user-id", &member_user, NULL);
			admitted = member_user == user && active(membership);
		}
		if (!admitted) continue;
		account = venture_database_get(database, VENTURE_TYPE_USER, user, NULL);
		if (account == NULL || venture_entity_is_deleted(account) || !active(account)) continue;
		g_object_get(account, "username", &username, NULL);
		if (venture_string_is_empty(username)) continue;
		g_ptr_array_add(pool, g_steal_pointer(&username));
	}
	return g_steal_pointer(&pool);
}

static gboolean
act(VentureDatabase *database, VentureEntity *lead, VentureEntity *rule, GError **error)
{
	VentureLeadRoutingAction action;
	g_autofree gchar *assign_to = NULL;
	gint64 team = 0, venture = 0, cursor = 0;
	g_object_get(rule, "action", &action, "assign-to", &assign_to, "team-id", &team,
		"venture-id", &venture, "cursor", &cursor, NULL);
	if (action == VENTURE_LEAD_ROUTING_ASSIGN_USER)
		g_object_set(lead, "owner", assign_to, NULL);
	else if (action == VENTURE_LEAD_ROUTING_ASSIGN_VENTURE)
		g_object_set(lead, "venture-id", venture, NULL);
	else
	{
		g_autoptr(GPtrArray) pool = NULL;
		guint chosen;
		if (!orgaccess_enabled(error)) return FALSE;
		pool = rota(database, team, venture_entity_get_organization_id(lead), error);
		if (pool == NULL) return FALSE;
		if (pool->len == 0) return refuse(error, VENTURE_ERROR_VALIDATION, "round-robin team has no active members");
		chosen = (guint)(MAX(cursor, 0) % pool->len);
		g_object_set(lead, "owner", g_ptr_array_index(pool, chosen), NULL);
		g_object_set(rule, "cursor", (gint64)((chosen + 1) % pool->len), NULL);
		if (!venture_database_save(database, rule, NULL, error)) return FALSE;
	}
	g_object_set(lead, "routing-rule-id", venture_entity_get_id(rule), NULL);
	return TRUE;
}

gboolean
venture_lead_routing_apply(VentureDatabase *database, VentureEntity *lead,
	gchar **rule_name, gboolean *configured, GError **error)
{
	g_autoptr(GPtrArray) rules = NULL;
	guint i;
	*rule_name = NULL;
	*configured = FALSE;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "lead_routing_rule") == G_TYPE_INVALID)
		return TRUE;
	rules = rows(database, VENTURE_TYPE_LEAD_ROUTING_RULE, venture_entity_get_organization_id(lead), "position", error);
	if (rules == NULL) return FALSE;
	for (i = 0; i < rules->len; i++)
	{
		VentureEntity *rule = g_ptr_array_index(rules, i);
		g_autofree gchar *conditions = NULL;
		gboolean matched = FALSE;
		if (!active(rule)) continue;
		*configured = TRUE;
		g_object_get(rule, "conditions", &conditions, NULL);
		if (!venture_lead_conditions_match(database, lead, conditions, &matched, error)) return FALSE;
		if (!matched) continue;
		if (!act(database, lead, rule, error)) return FALSE;
		g_object_get(rule, "name", rule_name, NULL);
		return TRUE;
	}
	g_object_set(lead, "routing-rule-id", (gint64)0, NULL);
	return TRUE;
}

gboolean
venture_lead_scoring_compute(VentureDatabase *database, VentureEntity *lead,
	gint64 *score, gchar **rule_ids, GError **error)
{
	g_autoptr(GPtrArray) rules = NULL;
	g_autoptr(GString) fired = g_string_new("");
	guint i;
	*score = 0;
	*rule_ids = NULL;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "lead_scoring_rule") != G_TYPE_INVALID)
	{
		rules = rows(database, VENTURE_TYPE_LEAD_SCORING_RULE, venture_entity_get_organization_id(lead), NULL, error);
		if (rules == NULL) return FALSE;
	}
	for (i = 0; rules != NULL && i < rules->len; i++)
	{
		VentureEntity *rule = g_ptr_array_index(rules, i);
		g_autofree gchar *conditions = NULL;
		gint64 points = 0;
		gboolean matched = FALSE;
		if (!active(rule)) continue;
		g_object_get(rule, "conditions", &conditions, "points", &points, NULL);
		if (!venture_lead_conditions_match(database, lead, conditions, &matched, error)) return FALSE;
		if (!matched) continue;
		*score += points;
		g_string_append_printf(fired, "%s%" G_GINT64_FORMAT, fired->len > 0 ? "," : "", venture_entity_get_id(rule));
	}
	*rule_ids = g_strdup(fired->str);
	return TRUE;
}
