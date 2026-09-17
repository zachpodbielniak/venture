/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

struct _VentureLeadService {
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
	GPtrArray *pending;
	gboolean converting;
};
G_DEFINE_FINAL_TYPE(VentureLeadService, venture_lead_service, G_TYPE_OBJECT)

enum { CONVERTING, CONVERTED, N_SIGNALS };
static guint signals[N_SIGNALS];

static gboolean
first_error(GSignalInvocationHint *hint, GValue *accumulator, const GValue *value, gpointer data)
{
	(void)hint; (void)data;
	if (g_value_get_boxed(value) == NULL) return TRUE;
	g_value_copy(value, accumulator);
	return FALSE;
}

static void
transaction_finished(VentureDatabase *db, gboolean committed, VentureLeadService *self)
{
	g_autoptr(GPtrArray) pending = self->pending;
	guint i;
	(void)db;
	self->pending = g_ptr_array_new_with_free_func(g_object_unref);
	if (committed)
		for (i = 0; i < pending->len; i++)
			g_signal_emit(self, signals[CONVERTED], 0, g_ptr_array_index(pending, i));
}

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureLeadService: %s", message);
	return FALSE;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	VentureLeadService *self = VENTURE_LEAD_SERVICE(object);
	if (id == 1) g_value_set_object(value, self->database);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureLeadService *self = VENTURE_LEAD_SERVICE(object);
	if (id == 1)
	{
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
		if (self->database != NULL)
			g_signal_connect_object(self->database, "transaction-finished", G_CALLBACK(transaction_finished), self, 0);
	}
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VentureLeadService *self = VENTURE_LEAD_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_clear_pointer(&self->pending, g_ptr_array_unref);
	G_OBJECT_CLASS(venture_lead_service_parent_class)->finalize(object);
}

static void
venture_lead_service_class_init(VentureLeadServiceClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->get_property = get_property;
	object->set_property = set_property;
	object->finalize = finalize;
	/**
	 * VentureLeadService::converting:
	 * @self: the lead service
	 * @lead: detached qualified lead snapshot
	 *
	 * Emitted inside the transaction before conversion writes. Return an owned
	 * GError to veto; snapshot edits are ignored. First error stops emission.
	 */
	signals[CONVERTING] = g_signal_new("converting", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		0, first_error, NULL, NULL, G_TYPE_ERROR, 1, VENTURE_TYPE_ENTITY);
	/**
	 * VentureLeadService::converted:
	 * @self: the lead service
	 * @lead: converted lead snapshot
	 *
	 * Emitted only after the outermost transaction commits. Rollback discards it.
	 */
	signals[CONVERTED] = g_signal_new("converted", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		0, NULL, NULL, NULL, G_TYPE_NONE, 1, VENTURE_TYPE_ENTITY);
	g_object_class_install_property(object, 1, g_param_spec_object("database", "Database",
		"Owning repository", VENTURE_TYPE_DATABASE,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_lead_service_init(VentureLeadService *self)
{
	self->pending = g_ptr_array_new_with_free_func(g_object_unref);
}

static gchar *
normalize(const gchar *value, guint kind)
{
	g_autofree gchar *text = g_ascii_strdown(value != NULL ? value : "", -1);
	g_strstrip(text);
	if (kind == 0)
	{
		gchar *at = strchr(text, '@');
		gchar *plus = strchr(text, '+');
		if (at != NULL && plus != NULL && plus < at)
			memmove(plus, at, strlen(at) + 1);
	}
	else if (kind == 1)
	{
		gchar *p;
		gchar *out = text;
		for (p = text; *p != '\0'; p++)
			if (g_ascii_isdigit(*p)) *out++ = *p;
		*out = '\0';
	}
	else if (*text != '\0')
	{
		g_autofree gchar *uri_text = NULL;
		g_autoptr(GUri) uri = NULL;
		const gchar *host;
		uri_text = strstr(text, "://") != NULL ? g_strdup(text) : g_strconcat("https://", text, NULL);
		uri = g_uri_parse(uri_text, G_URI_FLAGS_NONE, NULL);
		host = uri != NULL ? g_uri_get_host(uri) : NULL;
		if (host == NULL) return g_strdup("");
		if (g_str_has_prefix(host, "www.")) host += 4;
		return g_strdup(host);
	}
	return g_steal_pointer(&text);
}

static gchar *
string_field(VentureEntity *entity, const gchar *name)
{
	gchar *value = NULL;
	g_object_get(entity, name, &value, NULL);
	return value;
}

static GPtrArray *
rows(VentureLeadService *self, GType type, gint64 organization, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, organization);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(self->database, query, error);
}

static VentureEntity *
find_duplicate(VentureLeadService *self, VentureEntity *lead, GError **error)
{
	GType types[] = { VENTURE_TYPE_LEAD, VENTURE_TYPE_CONTACT, VENTURE_TYPE_COMPANY };
	const gchar *fields[] = { "email", "phone", "website" };
	guint t;
	for (t = 0; t < G_N_ELEMENTS(types); t++)
	{
		g_autoptr(GPtrArray) found = rows(self, types[t], venture_entity_get_organization_id(lead), error);
		guint i;
		if (found == NULL) return NULL;
		for (i = 0; i < found->len; i++)
		{
			VentureEntity *other = g_ptr_array_index(found, i);
			guint k;
			if (G_OBJECT_TYPE(other) == G_OBJECT_TYPE(lead) &&
				venture_entity_get_id(other) == venture_entity_get_id(lead)) continue;
			for (k = 0; k < G_N_ELEMENTS(fields); k++)
			{
				g_autofree gchar *a = string_field(lead, fields[k]);
				g_autofree gchar *b = string_field(other, fields[k]);
				g_autofree gchar *na = normalize(a, k);
				g_autofree gchar *nb = normalize(b, k);
				if (*na != '\0' && g_str_equal(na, nb)) return g_object_ref(other);
			}
		}
	}
	return NULL;
}

static gboolean
assign(VentureLeadService *self, VentureEntity *lead, GError **error)
{
	g_autoptr(GPtrArray) rules = rows(self, VENTURE_TYPE_LEAD_ASSIGNMENT_RULE,
		venture_entity_get_organization_id(lead), error);
	g_autofree gchar *source = string_field(lead, "source");
	VentureEntity *best = NULL;
	gint specificity = -1;
	gint64 venture = 0;
	guint i;
	if (rules == NULL) return FALSE;
	g_object_get(lead, "venture-id", &venture, NULL);
	for (i = 0; i < rules->len; i++)
	{
		VentureEntity *rule = g_ptr_array_index(rules, i);
		g_autofree gchar *filter = string_field(rule, "source");
		gint64 filter_venture = 0;
		gboolean active = FALSE;
		gint score;
		g_object_get(rule, "active", &active, "venture-id", &filter_venture, NULL);
		if (!active || (filter_venture != 0 && filter_venture != venture) ||
			(!venture_string_is_empty(filter) && g_strcmp0(source, filter) != 0)) continue;
		score = (filter_venture != 0) + !venture_string_is_empty(filter);
		if (score > specificity) { best = rule; specificity = score; }
	}
	if (best != NULL)
	{
		g_autofree gchar *assignees = string_field(best, "assignees");
		g_auto(GStrv) names = g_strsplit(assignees != NULL ? assignees : "", ",", -1);
		g_autoptr(GPtrArray) pool = g_ptr_array_new();
		VentureRoutingStrategy strategy;
		gint64 cursor = 0;
		guint chosen = 0;
		for (i = 0; names[i] != NULL; i++)
			if (*g_strstrip(names[i]) != '\0') g_ptr_array_add(pool, names[i]);
		if (pool->len == 0) return refuse(error, VENTURE_ERROR_VALIDATION, "assignment rule names nobody");
		g_object_get(best, "strategy", &strategy, "cursor", &cursor, NULL);
		if (strategy == VENTURE_ROUTING_STRATEGY_ROUND_ROBIN)
		{
			chosen = (guint)(MAX(cursor, 0) % pool->len);
			g_object_set(best, "cursor", (gint64)((chosen + 1) % pool->len), NULL);
			if (!venture_database_save(self->database, best, NULL, error)) return FALSE;
		}
		else if (strategy == VENTURE_ROUTING_STRATEGY_LEAST_BUSY)
		{
			gint64 fewest = G_MAXINT64;
			for (i = 0; i < pool->len; i++)
			{
				g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_LEAD);
				gint64 count;
				venture_query_set_organization(query, venture_entity_get_organization_id(lead));
				venture_query_add_filter_string(query, "owner", VENTURE_FILTER_OP_EQ, g_ptr_array_index(pool, i), NULL);
				venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE, "converted", NULL);
				venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE, "unqualified", NULL);
				venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_NE, "recycled", NULL);
				count = venture_database_count(self->database, query, error);
				if (count < 0) return FALSE;
				if (count < fewest) { fewest = count; chosen = i; }
			}
		}
		g_object_set(lead, "owner", g_ptr_array_index(pool, chosen), NULL);
	}
	return TRUE;
}

static gboolean
write_record(VentureLeadService *self, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = entity;
	ok = venture_database_save(self->database, entity, actor, error);
	self->writing = NULL;
	return ok;
}

static gboolean
history(VentureLeadService *self, VentureEntity *subject, const gchar *title,
	const gchar *body, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureInteraction) event = venture_interaction_new();
	g_autoptr(GDateTime) now = venture_time_now();
	const gchar *reference;
	reference = VENTURE_IS_LEAD(subject) ? "lead-id" :
		(VENTURE_IS_CONTACT(subject) ? "contact-id" : "company-id");
	g_object_set(event, "organization-id", venture_entity_get_organization_id(subject),
		reference, venture_entity_get_id(subject), "subject", title, "body", body,
		"occurred-at", now, NULL);
	return write_record(self, VENTURE_ENTITY(event), actor, error);
}

static gboolean
validate_state(VentureEntity *entity, VentureEntity *previous, GError **error)
{
	VentureLeadStatus state, old = VENTURE_LEAD_NEW;
	g_autofree gchar *reason = string_field(entity, "unqualified-reason");
	g_autoptr(GDateTime) until = NULL;
	gint64 company = 0, contact = 0, deal = 0;
	g_object_get(entity, "status", &state, "recycle-until", &until,
		"converted-company-id", &company, "converted-contact-id", &contact,
		"converted-deal-id", &deal, NULL);
	if (previous != NULL) g_object_get(previous, "status", &old, NULL);
	if (state == VENTURE_LEAD_CONVERTED || old == VENTURE_LEAD_CONVERTED ||
		company != 0 || contact != 0 || deal != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "conversion fields are written only by the conversion action");
	if ((state == VENTURE_LEAD_UNQUALIFIED || state == VENTURE_LEAD_RECYCLED) && venture_string_is_empty(reason))
		return refuse(error, VENTURE_ERROR_VALIDATION, "unqualified and recycled leads require a reason");
	if (state == VENTURE_LEAD_RECYCLED && until == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "recycled leads require recycle_until");
	return TRUE;
}

static VentureEntity *
save_lead(VentureLeadService *self, VentureEntity *entity, const gchar *policy,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) duplicate = NULL;
	g_autoptr(VentureEntity) previous = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gboolean changed = FALSE;
	guint i;
	const gchar *fields[] = { "email", "phone", "website" };
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "lead") == G_TYPE_INVALID)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "leads module is disabled");
		return NULL;
	}
	if (!venture_database_begin(self->database, error)) return NULL;
	if (venture_entity_is_persisted(entity))
	{
		VentureLeadStatus state, old;
		g_autofree gchar *owner = string_field(entity, "owner");
		g_autofree gchar *old_owner = NULL;
		previous = venture_database_get(self->database, G_OBJECT_TYPE(entity), venture_entity_get_id(entity), error);
		if (previous == NULL) goto fail;
		old_owner = string_field(previous, "owner");
		g_object_get(entity, "status", &state, NULL);
		g_object_get(previous, "status", &old, NULL);
		changed = old != state || g_strcmp0(owner, old_owner) != 0;
	}
	if (!validate_state(entity, previous, error)) goto fail;
	for (i = 0; i < G_N_ELEMENTS(fields); i++)
	{
		g_autofree gchar *raw = string_field(entity, fields[i]);
		g_autofree gchar *normal = normalize(raw, i);
		g_object_set(entity, fields[i], normal, NULL);
	}
	if (previous == NULL)
	{
		g_autofree gchar *owner = string_field(entity, "owner");
		if (g_strcmp0(policy, "create") != 0)
			duplicate = find_duplicate(self, entity, &local_error);
		if (local_error != NULL) { g_propagate_error(error, g_steal_pointer(&local_error)); goto fail; }
		if (duplicate != NULL)
		{
			g_autofree gchar *source = string_field(entity, "source");
			if (g_strcmp0(policy, "merge") != 0)
			{
				refuse(error, VENTURE_ERROR_ALREADY_EXISTS, "duplicate lead, contact or company");
				goto fail;
			}
			if (VENTURE_IS_LEAD(duplicate))
			{
				g_object_set(duplicate, "last-activity-at", now, NULL);
				if (!write_record(self, duplicate, actor, error)) goto fail;
			}
			if (!history(self, duplicate, "Lead capture", source, actor, error)) goto fail;
			if (!venture_database_commit(self->database, error)) return NULL;
			return g_steal_pointer(&duplicate);
		}
		if (venture_string_is_empty(owner) && !assign(self, entity, error)) goto fail;
		g_object_set(entity, "first-seen-at", now, "last-activity-at", now, NULL);
	}
	if (changed) g_object_set(entity, "last-activity-at", now, NULL);
	if (!write_record(self, entity, actor, error)) goto fail;
	if (changed)
	{
		VentureLeadStatus state;
		g_autofree gchar *reason = string_field(entity, "unqualified-reason");
		g_autofree gchar *owner = string_field(entity, "owner");
		g_autofree gchar *body = NULL;
		g_object_get(entity, "status", &state, NULL);
		body = g_strdup_printf("Status: %s; owner: %s; reason: %s",
			venture_enum_to_nick(venture_lead_status_get_type(), state),
			owner != NULL ? owner : "", reason != NULL ? reason : "");
		if (!history(self, entity, "Lead qualification or assignment", body, actor, error)) goto fail;
	}
	if (!venture_database_commit(self->database, error)) return NULL;
	return g_object_ref(entity);
fail:
	venture_database_rollback(self->database);
	return NULL;
}

static gboolean
validate_form(VentureLeadService *self, VentureEntity *entity, GError **error)
{
	g_autofree gchar *token = string_field(entity, "public-token");
	g_autofree gchar *policy = string_field(entity, "on-duplicate");
	g_autofree gchar *redirect = string_field(entity, "redirect-url");
	g_autoptr(JsonNode) fields = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_LEAD_FORM);
	g_autoptr(GPtrArray) forms = NULL;
	guint i;
	g_object_get(entity, "fields", &fields, NULL);
	if (fields != NULL && !JSON_NODE_HOLDS_NULL(fields))
	{
		JsonArray *array;
		if (!JSON_NODE_HOLDS_ARRAY(fields)) return refuse(error, VENTURE_ERROR_VALIDATION, "form fields must be a JSON array of field names");
		array = json_node_get_array(fields);
		for (i = 0; i < json_array_get_length(array); i++)
		{
			JsonNode *field = json_array_get_element(array, i);
			if (!JSON_NODE_HOLDS_VALUE(field) || json_node_get_value_type(field) != G_TYPE_STRING)
				return refuse(error, VENTURE_ERROR_VALIDATION, "form field names must be strings");
		}
	}
	if (!venture_string_is_empty(redirect) && !g_str_has_prefix(redirect, "https://") &&
		!g_str_has_prefix(redirect, "http://") && !(redirect[0] == '/' && redirect[1] != '/'))
		return refuse(error, VENTURE_ERROR_VALIDATION, "redirect_url must be HTTP(S) or a local path");
	if (venture_string_is_empty(policy)) g_object_set(entity, "on-duplicate", "merge", NULL);
	else if (!g_str_equal(policy, "merge") && !g_str_equal(policy, "reject") && !g_str_equal(policy, "create"))
		return refuse(error, VENTURE_ERROR_VALIDATION, "on_duplicate must be reject, merge or create");
	if (venture_string_is_empty(token))
	{
		g_free(token);
		token = g_uuid_string_random();
		g_object_set(entity, "public-token", token, NULL);
	}
	/* Public tokens are capabilities: ambiguous tokens must never select another tenant. */
	venture_query_set_limit(query, 0);
	venture_query_add_filter_string(query, "public-token", VENTURE_FILTER_OP_EQ, token, NULL);
	forms = venture_database_find(self->database, query, error);
	if (forms == NULL) return FALSE;
	for (i = 0; i < forms->len; i++)
		if (venture_entity_get_id(g_ptr_array_index(forms, i)) != venture_entity_get_id(entity))
			return refuse(error, VENTURE_ERROR_ALREADY_EXISTS, "public token is already in use");
	return TRUE;
}

static gboolean
save_interaction(VentureLeadService *self, VentureEntity *event, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) lead = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(GDateTime) previous = NULL;
	gint64 id = 0;
	g_object_get(event, "lead-id", &id, "occurred-at", &date, NULL);
	if (!venture_database_begin(self->database, error)) return FALSE;
	lead = venture_database_get(self->database, VENTURE_TYPE_LEAD, id, error);
	if (lead == NULL || venture_entity_get_organization_id(lead) != venture_entity_get_organization_id(event))
	{
		if (error == NULL || *error == NULL) refuse(error, VENTURE_ERROR_VALIDATION, "interaction must name a lead in its organization");
		goto fail;
	}
	if (date == NULL) { date = venture_time_now(); g_object_set(event, "occurred-at", date, NULL); }
	if (!write_record(self, event, actor, error)) goto fail;
	g_object_get(lead, "last-activity-at", &previous, NULL);
	if (previous == NULL || g_date_time_compare(date, previous) > 0)
	{
		g_object_set(lead, "last-activity-at", date, NULL);
		if (!write_record(self, lead, NULL, error)) goto fail;
	}
	return venture_database_commit(self->database, error);
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

gboolean
venture_lead_service_save_hook(VentureLeadService *self, VentureEntity *entity,
	const VentureActor *actor, gboolean *handled, GError **error)
{
	g_autoptr(VentureEntity) saved = NULL;
	*handled = FALSE;
	if (self->writing == entity) { self->writing = NULL; return TRUE; }
	if (VENTURE_IS_LEAD_FORM(entity)) return validate_form(self, entity, error);
	if (VENTURE_IS_INTERACTION(entity))
	{
		gint64 lead_id = 0, company_id = 0, contact_id = 0, deal_id = 0;
		g_object_get(entity, "lead-id", &lead_id, "company-id", &company_id, "contact-id", &contact_id, "deal-id", &deal_id, NULL);
		if (lead_id == 0 && company_id == 0 && contact_id == 0 && deal_id == 0)
			return refuse(error, VENTURE_ERROR_VALIDATION, "interaction requires a lead, company, contact or deal");
		if (lead_id != 0) { *handled = TRUE; return save_interaction(self, entity, actor, error); }
	}
	if (!VENTURE_IS_LEAD(entity)) return TRUE;
	*handled = TRUE;
	saved = save_lead(self, entity, "reject", actor, error);
	return saved != NULL;
}

gboolean
venture_lead_service_capture(VentureLeadService *self, const gchar *token, JsonObject *fields, gchar **redirect_url, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) forms = NULL;
	g_autoptr(VentureEntity) lead = NULL;
	g_autoptr(VentureEntity) saved = NULL;
	g_autofree gchar *honeypot = NULL;
	g_autofree gchar *policy = NULL;
	g_autofree gchar *name = NULL;
	g_autoptr(JsonNode) allowed = NULL;
	VentureEntity *form;
	gboolean active = FALSE;
	gint64 venture = 0;
	guint i;
	const gchar *inputs[] = { "name", "company_name", "email", "phone", "website", "source", "notes" };
	if (redirect_url != NULL) *redirect_url = NULL;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "lead_form") == G_TYPE_INVALID)
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "capture form unavailable");
	if (!venture_database_begin(self->database, error)) return FALSE;
	query = venture_query_new(VENTURE_TYPE_LEAD_FORM);
	venture_query_set_limit(query, 2);
	venture_query_add_filter_string(query, "public-token", VENTURE_FILTER_OP_EQ, token, NULL);
	forms = venture_database_find(self->database, query, error);
	if (forms == NULL) goto fail;
	if (forms->len != 1) { refuse(error, VENTURE_ERROR_NOT_FOUND, "capture form unavailable"); goto fail; }
	form = g_ptr_array_index(forms, 0);
	g_object_get(form, "active", &active, "venture-id", &venture, "honeypot", &honeypot,
		"on-duplicate", &policy, "name", &name, "fields", &allowed, NULL);
	if (!active) { refuse(error, VENTURE_ERROR_NOT_FOUND, "capture form unavailable"); goto fail; }
	if (!venture_string_is_empty(honeypot) && json_object_has_member(fields, honeypot) &&
		!venture_string_is_empty(venture_json_object_get_string(fields, honeypot, "")))
		return venture_database_commit(self->database, error);
	lead = VENTURE_ENTITY(venture_lead_new());
	g_object_set(lead, "organization-id", venture_entity_get_organization_id(form), "venture-id", venture, "source", name, NULL);
	for (i = 0; i < G_N_ELEMENTS(inputs); i++)
	{
		g_autofree gchar *property = g_strdup(inputs[i]);
		JsonNode *node;
		gboolean permitted = allowed == NULL || JSON_NODE_HOLDS_NULL(allowed);
		guint j;
		if (allowed != NULL && JSON_NODE_HOLDS_ARRAY(allowed))
		{
			JsonArray *array = json_node_get_array(allowed);
			for (j = 0; j < json_array_get_length(array); j++)
				if (g_strcmp0(json_array_get_string_element(array, j), inputs[i]) == 0) permitted = TRUE;
		}
		if (!permitted || !json_object_has_member(fields, inputs[i])) continue;
		node = json_object_get_member(fields, inputs[i]);
		if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "capture fields must be strings"); goto fail;
		}
		g_strdelimit(property, "_", '-');
		g_object_set(lead, property, json_node_get_string(node), NULL);
	}
	saved = save_lead(self, lead, policy != NULL ? policy : "merge", NULL, error);
	if (saved == NULL) goto fail;
	if (!venture_database_commit(self->database, error)) return FALSE;
	if (redirect_url != NULL) *redirect_url = string_field(form, "redirect-url");
	return TRUE;
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

static VentureEntity *
conversion_record(VentureLeadService *self, VentureEntity *lead, GType type, gint64 id,
	const gchar *name, gint64 company_id, const VentureActor *actor, GError **error)
{
	VentureEntity *result;
	gint64 venture = 0, campaign = 0;
	g_autofree gchar *source = string_field(lead, "source");
	if (id != 0)
	{
		result = venture_database_get(self->database, type, id, error);
		if (result == NULL && (error == NULL || *error == NULL)) refuse(error, VENTURE_ERROR_NOT_FOUND, "conversion link does not exist");
		if (result != NULL && venture_entity_get_organization_id(result) != venture_entity_get_organization_id(lead))
		{
			g_object_unref(result);
			refuse(error, VENTURE_ERROR_VALIDATION, "conversion links must belong to the lead organization");
			return NULL;
		}
		return result;
	}
	g_object_get(lead, "venture-id", &venture, "campaign-id", &campaign, NULL);
	result = g_object_new(type, "name", name, "organization-id", venture_entity_get_organization_id(lead),
		"venture-id", venture, "campaign-id", campaign, "source", source, NULL);
	if (type == VENTURE_TYPE_CONTACT || type == VENTURE_TYPE_DEAL)
		g_object_set(result, "company-id", company_id, NULL);
	if (type == VENTURE_TYPE_CONTACT)
	{
		g_autofree gchar *email = string_field(lead, "email");
		g_autofree gchar *phone = string_field(lead, "phone");
		g_autofree gchar *website = string_field(lead, "website");
		g_object_set(result, "email", email, "phone", phone, "website", website, NULL);
	}
	if (!venture_database_save(self->database, result, actor, error)) g_clear_object(&result);
	return result;
}

static gboolean
validate_options(JsonObject *options, GError **error)
{
	const gchar *keys[] = { "company_id", "contact_id", "deal" };
	guint i;
	if (options == NULL) return TRUE;
	for (i = 0; i < G_N_ELEMENTS(keys); i++)
	{
		JsonNode *node;
		if (!json_object_has_member(options, keys[i])) continue;
		node = json_object_get_member(options, keys[i]);
		if (!JSON_NODE_HOLDS_VALUE(node) ||
			json_node_get_value_type(node) != (i == 2 ? G_TYPE_BOOLEAN : G_TYPE_INT64))
			return refuse(error, VENTURE_ERROR_VALIDATION, "conversion expects integer link ids and a boolean deal flag");
		if (i != 2 && json_node_get_int(node) < 0)
			return refuse(error, VENTURE_ERROR_VALIDATION, "conversion link ids must be nonnegative");
	}
	return TRUE;
}

VentureEntity *
venture_lead_service_convert(VentureLeadService *self, VentureEntity *lead,
	JsonObject *options, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(VentureEntity) deal = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *company_name = NULL;
	VentureLeadStatus state;
	gint64 company_id, contact_id;
	gboolean make_deal;
	/* A converting callback must not start a second conversion before the
	 * first has written its status, including conversion of another lead. */
	if (self->database == NULL || self->converting)
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "conversion unavailable or reentered");
		return NULL;
	}
	if (!validate_options(options, error)) return NULL;
	if (!VENTURE_IS_LEAD(lead) || !venture_entity_is_persisted(lead))
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "convert a saved lead"); return NULL;
	}
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "lead") == G_TYPE_INVALID)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "leads module is disabled"); return NULL;
	}
	if (!venture_database_begin(self->database, error)) return NULL;
	self->converting = TRUE;
	current = venture_database_get(self->database, VENTURE_TYPE_LEAD, venture_entity_get_id(lead), error);
	if (current == NULL)
	{
		if (error == NULL || *error == NULL) refuse(error, VENTURE_ERROR_NOT_FOUND, "lead no longer exists");
		goto fail;
	}
	if (venture_entity_is_deleted(current))
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "lead no longer exists"); goto fail;
	}
	g_object_get(current, "status", &state, "name", &name, "company-name", &company_name, NULL);
	if (venture_entity_get_version(current) != venture_entity_get_version(lead) || state == VENTURE_LEAD_CONVERTED)
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "lead changed or was already converted"); goto fail;
	}
	if (state != VENTURE_LEAD_QUALIFIED)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "qualify the lead before conversion"); goto fail;
	}
	{
		g_autoptr(VentureEntity) snapshot = VENTURE_ENTITY(venture_lead_new());
		g_autoptr(GError) veto = NULL;
		venture_entity_copy_properties_from(snapshot, current, FALSE);
		g_signal_emit(self, signals[CONVERTING], 0, snapshot, &veto);
		if (veto != NULL) { g_propagate_error(error, g_steal_pointer(&veto)); goto fail; }
	}
	company_id = options != NULL ? venture_json_object_get_int(options, "company_id", 0) : 0;
	contact_id = options != NULL ? venture_json_object_get_int(options, "contact_id", 0) : 0;
	make_deal = options == NULL || venture_json_object_get_bool(options, "deal", TRUE);
	company = conversion_record(self, current, VENTURE_TYPE_COMPANY, company_id,
		venture_string_is_empty(company_name) ? name : company_name, 0, actor, error);
	if (company == NULL) goto fail;
	company_id = venture_entity_get_id(company);
	contact = conversion_record(self, current, VENTURE_TYPE_CONTACT, contact_id, name, company_id, actor, error);
	if (contact == NULL) goto fail;
	g_object_get(contact, "company-id", &contact_id, NULL);
	if (contact_id != 0 && contact_id != company_id)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "existing contact belongs to another company"); goto fail;
	}
	if (make_deal)
	{
		gint64 venture = 0, campaign = 0;
		g_autofree gchar *source = string_field(current, "source");
		g_object_get(current, "venture-id", &venture, "campaign-id", &campaign, NULL);
		deal = VENTURE_ENTITY(venture_deal_new());
		g_object_set(deal, "organization-id", venture_entity_get_organization_id(current), "name", name,
			"company-id", company_id, "contact-id", venture_entity_get_id(contact), "venture-id", venture,
			"source", source, "campaign-id", campaign, NULL);
		if (!venture_database_save(self->database, deal, actor, error)) goto fail;
	}
	{
		g_autoptr(GDateTime) now = venture_time_now();
		g_object_set(current, "last-activity-at", now, NULL);
	}
	g_object_set(current, "status", VENTURE_LEAD_CONVERTED, "converted-company-id", company_id,
		"converted-contact-id", venture_entity_get_id(contact),
		"converted-deal-id", deal != NULL ? venture_entity_get_id(deal) : (gint64)0, NULL);
	if (!write_record(self, current, actor, error) ||
		!history(self, current, "Lead converted", "Company and contact linked; attribution retained", actor, error)) goto fail;
	g_ptr_array_add(self->pending, g_object_ref(current));
	if (!venture_database_commit(self->database, error))
	{
		self->converting = FALSE;
		return NULL;
	}
	self->converting = FALSE;
	return g_steal_pointer(&current);
fail:
	venture_database_rollback(self->database);
	self->converting = FALSE;
	return NULL;
}

gboolean
venture_lead_service_reassign(VentureLeadService *self, VentureEntity *lead,
	const gchar *owner, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) saved = NULL;
	if (!venture_database_begin(self->database, error)) return FALSE;
	if (owner != NULL) g_object_set(lead, "owner", owner, NULL);
	else if (!assign(self, lead, error)) goto fail;
	saved = save_lead(self, lead, "reject", actor, error);
	if (saved == NULL) goto fail;
	return venture_database_commit(self->database, error);
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

VentureConfirmation *
venture_lead_service_stage_convert(VentureLeadService *self, VentureConfirmationStore *store,
	VentureEntity *lead, JsonObject *options, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) staged = VENTURE_ENTITY(venture_lead_new());
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *text = NULL;
	VentureLeadStatus status;
	if (!validate_options(options, error)) return NULL;
	if (self->database == NULL) return NULL;
	g_object_get(lead, "status", &status, NULL);
	if (status != VENTURE_LEAD_QUALIFIED)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "qualify the lead before conversion"); return NULL;
	}
	venture_entity_copy_properties_from(staged, lead, FALSE);
	if (options != NULL) json_node_set_object(node, options);
	else json_node_take_object(node, json_object_new());
	text = venture_json_to_string(node, FALSE);
	venture_entity_set_attribute(staged, "lead_conversion_options", text);
	g_object_set(staged, "status", VENTURE_LEAD_CONVERTED, NULL);
	return venture_confirmation_store_stage(store, VENTURE_AUDIT_ACTION_UPDATE, staged,
		lead, actor, "lead-convert", error);
}

gboolean
venture_lead_service_apply_staged(VentureLeadService *self, VentureEntity *staged,
	const VentureActor *actor, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(VentureEntity) converted = NULL;
	const gchar *text = venture_entity_get_attribute(staged, "lead_conversion_options");
	JsonNode *node;
	if (text == NULL || !json_parser_load_from_data(parser, text, -1, error)) return FALSE;
	node = json_parser_get_root(parser);
	if (!JSON_NODE_HOLDS_OBJECT(node)) return refuse(error, VENTURE_ERROR_VALIDATION, "invalid staged options");
	converted = venture_lead_service_convert(self, staged, json_node_get_object(node), actor, error);
	return converted != NULL;
}

gchar *
venture_lead_normalize_email(const gchar *value)
{
	return normalize(value, 0);
}
