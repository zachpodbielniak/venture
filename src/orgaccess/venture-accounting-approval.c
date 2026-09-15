/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "%s", message);
	return FALSE;
}

gboolean
venture_accounting_approval_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error)
{
	(void)removal;
	if (record == NULL || database == NULL || !VENTURE_IS_ACCOUNTING_APPROVAL(record))
		return TRUE;
	if (g_object_get_data(G_OBJECT(database), "venture-accounting-approval-permit") == record)
	{
		g_object_set_data(G_OBJECT(database), "venture-accounting-approval-permit", NULL);
		return TRUE;
	}
	return refuse(error, "accounting approvals are durable service records");
}

static gboolean
save_approval(VentureDatabase *database, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	g_object_set_data(G_OBJECT(database), "venture-accounting-approval-permit", record);
	ok = venture_database_save(database, record, actor, error);
	g_object_set_data(G_OBJECT(database), "venture-accounting-approval-permit", NULL);
	return ok;
}

static gboolean
rule_enabled(VentureDatabase *database, gint64 org, const gchar *action, gboolean *enabled, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	*enabled = FALSE;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "accounting_approval_rule") == G_TYPE_INVALID)
		return TRUE;
	query = venture_query_new(VENTURE_TYPE_ACCOUNTING_APPROVAL_RULE);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(database, query, error);
	if (rows == NULL)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *name = NULL;
		gboolean required = FALSE;
		g_object_get(g_ptr_array_index(rows, i), "action", &name, "require-second-actor", &required, NULL);
		if (required && g_strcmp0(name, action) == 0)
			*enabled = TRUE;
	}
	return TRUE;
}

static void
append_proposal(GString *material, VentureEntity *entity)
{
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *json = NULL;
	JsonObject *object;

	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(entity), FALSE);
	object = json_node_get_object(node);
	/* A retried unsaved operation has a fresh UUID, but the same business
	 * content. Persisted identities and versions remain part of consent. */
	if (!venture_entity_is_persisted(entity))
	{
		json_object_remove_member(object, "uuid");
		json_object_remove_member(object, "created_at");
		json_object_remove_member(object, "updated_at");
	}
	json = venture_json_to_string(node, FALSE);
	g_string_append_printf(material, ":%s:%s", venture_entity_get_entity_name(entity), json);
}

static gchar *
proposal_digest(const gchar *action, VentureEntity *entity, GPtrArray *details)
{
	g_autoptr(GString) material = g_string_new(action);
	guint i;

	append_proposal(material, entity);
	if (details != NULL)
		for (i = 0; i < details->len; i++)
			append_proposal(material, g_ptr_array_index(details, i));
	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, material->str, -1);
}

gboolean
venture_accounting_approval_allow(VentureDatabase *database, const gchar *action,
	VentureEntity *entity, GPtrArray *details, const VentureActor *actor,
	VentureEntity **approval, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	const gchar *name;
	g_autofree gchar *digest = NULL;
	gint64 org, id;
	gboolean enabled;
	guint i;
	*approval = NULL;
	if (database == NULL || entity == NULL || actor == NULL || actor->name == NULL)
		return TRUE;
	org = venture_entity_get_organization_id(entity);
	if (!rule_enabled(database, org, action, &enabled, error))
		return FALSE;
	if (!enabled)
		return TRUE;
	id = venture_entity_get_id(entity);
	name = venture_entity_get_entity_name(entity);
	digest = proposal_digest(action, entity, details);
	query = venture_query_new(VENTURE_TYPE_ACCOUNTING_APPROVAL);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_string(query, "action", VENTURE_FILTER_OP_EQ, action, NULL);
	venture_query_add_filter_string(query, "record-type", VENTURE_FILTER_OP_EQ, name, NULL);
	venture_query_add_filter_string(query, "proposal-digest", VENTURE_FILTER_OP_EQ, digest, NULL);
	rows = venture_database_find(database, query, error);
	if (rows == NULL)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *state = NULL;
		g_autofree gchar *proposer = NULL;
		g_object_get(row, "state", &state, "proposer", &proposer, NULL);
		if (g_strcmp0(state, "pending") != 0)
			continue;
		if (g_strcmp0(proposer, actor->name) == 0)
			return refuse(error, "post/pay requires a second actor");
		*approval = g_object_ref(row);
		return TRUE;
	}
	{
		g_autoptr(VentureAccountingApproval) pending = venture_accounting_approval_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(pending), org);
		g_object_set(pending, "action", action, "record-type", name, "record-id", id,
			"proposer", actor->name, "state", "pending", "proposal-digest", digest, NULL);
		if (!save_approval(database, VENTURE_ENTITY(pending), actor, error))
			return FALSE;
	}
	return refuse(error, "post/pay requires a second actor");
}

gboolean
venture_accounting_approval_consume(VentureDatabase *database, VentureEntity *approval,
	const VentureActor *actor, GError **error)
{
	if (approval == NULL)
		return TRUE;
	g_object_set(approval, "state", "applied", "approver", actor && actor->name ? actor->name : "", NULL);
	return save_approval(database, approval, actor, error);
}
