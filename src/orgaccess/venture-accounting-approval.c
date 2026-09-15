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
	g_autofree gchar *identity = NULL;
	const VentureAuthPrincipal *principal;
	gint64 org, id;
	gboolean enabled;
	guint i;
	*approval = NULL;
	if (database == NULL || entity == NULL)
		return TRUE;
	if (g_object_get_data(G_OBJECT(database), "venture-accounting-operation") != NULL)
	{
		g_autoptr(VentureAccountingOperation) nested = venture_accounting_operation_begin(database,
			action, entity, details, NULL, venture_entity_get_organization_id(entity), actor, error);
		return nested != NULL;
	}
	principal = venture_access_policy_get_actor(venture_database_get_access_policy(database));
	/* Token labels and browser names are audit descriptions, not people.
	 * Bind consent to the authenticated account even if it uses two tokens. */
	if (principal != NULL && principal->authenticated && principal->user_id > 0)
		identity = g_strdup_printf("user:%" G_GINT64_FORMAT, principal->user_id);
	else if (actor != NULL && actor->name != NULL)
		identity = g_strdup(actor->name);
	else
		return TRUE;
	org = venture_entity_get_organization_id(entity);
	if (!rule_enabled(database, org, action, &enabled, error))
		return FALSE;
	if (!enabled)
		return TRUE;
	if (venture_database_has_transaction(database))
		return refuse(error, "Whole-operation approval must precede the enclosing transaction");
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
		if (g_strcmp0(proposer, identity) == 0)
			return refuse(error, "post/pay requires a second actor");
		*approval = g_object_ref(row);
		return TRUE;
	}
	{
		g_autoptr(VentureAccountingApproval) pending = venture_accounting_approval_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(pending), org);
		g_object_set(pending, "action", action, "record-type", name, "record-id", id,
			"proposer", identity, "state", "pending", "proposal-digest", digest, NULL);
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

/* A scope is deliberately lexical rather than a durable "approved" flag on
 * a document: it grants one exact command, to one account, on one thread. */
struct _VentureAccountingOperation
{
	VentureDatabase *database;
	VentureEntity *approval;
	GThread *thread;
	gchar *identity;
	gchar *rules_revision;
	gint64 organization;
	gboolean owner;
	gboolean enforced;
	gboolean finished;
};

static gchar *
operation_identity(VentureDatabase *database, const VentureActor *actor)
{
	const VentureAuthPrincipal *principal;
	principal = venture_access_policy_get_actor(venture_database_get_access_policy(database));
	if (principal != NULL && principal->authenticated && principal->user_id > 0)
		return g_strdup_printf("user:%" G_GINT64_FORMAT, principal->user_id);
	return actor != NULL && actor->name != NULL ? g_strdup(actor->name) : NULL;
}

/* Hash canonical JSON, including nested maps supplied through generic actions.
 * Object member insertion order cannot distinguish two identical commands. */
static void
append_canonical(GString *output, JsonNode *node)
{
	if (JSON_NODE_HOLDS_OBJECT(node))
	{
		JsonObject *object = json_node_get_object(node);
		g_autoptr(GList) members = json_object_get_members(object);
		GList *item;
		members = g_list_sort(members, (GCompareFunc)g_strcmp0);
		g_string_append_c(output, '{');
		for (item = members; item != NULL; item = item->next)
		{
			g_autoptr(JsonNode) key = json_node_new(JSON_NODE_VALUE);
			g_autofree gchar *quoted = NULL;
			json_node_set_string(key, item->data);
			quoted = venture_json_to_string(key, FALSE);
			g_string_append(output, quoted);
			g_string_append_c(output, ':');
			append_canonical(output, json_object_get_member(object, item->data));
			g_string_append_c(output, ',');
		}
		g_string_append_c(output, '}');
	}
	else if (JSON_NODE_HOLDS_ARRAY(node))
	{
		JsonArray *array = json_node_get_array(node);
		guint i;
		g_string_append_c(output, '[');
		for (i = 0; i < json_array_get_length(array); i++)
		{
			append_canonical(output, json_array_get_element(array, i));
			g_string_append_c(output, ',');
		}
		g_string_append_c(output, ']');
	}
	else
	{
		g_autofree gchar *text = venture_json_to_string(node, FALSE);
		g_string_append(output, text);
	}
}

static void
append_operation_record(GString *material, VentureEntity *record)
{
	g_autoptr(JsonNode) node = venture_serializable_to_json(VENTURE_SERIALIZABLE(record), TRUE);
	JsonObject *object = json_node_get_object(node);
	if (!venture_entity_is_persisted(record))
	{
		json_object_remove_member(object, "uuid");
		json_object_remove_member(object, "created_at");
		json_object_remove_member(object, "updated_at");
	}
	g_string_append_printf(material, ":%s:", venture_entity_get_entity_name(record));
	append_canonical(material, node);
}

static gboolean
snapshot_excluded(const gchar *name)
{
	/* Evidence and delivery queues are effects of requesting consent, not
	 * inputs to the financial calculation. Everything else is conservatively
	 * included, so a new module cannot accidentally omit a dependency. */
	static const gchar *const excluded[] = {
		"accounting_approval", "audit_entry", "audit_log", "notification", "watch",
		"webhook_delivery", "chat_thread", "chat_message", "mail_message", "mail_delivery",
		"mail_attempt", "session", "user_session", "user", "api_token", NULL
	};
	return g_strv_contains(excluded, name);
}

static gboolean
append_operation_snapshot(VentureDatabase *database, gint64 org, GString *material, GError **error)
{
	g_auto(GStrv) names = venture_entity_registry_list_names(venture_entity_registry_get_default());
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	guint i, j;
	for (i = 0; names[i] != NULL; i++)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		GType type;
		if (snapshot_excluded(names[i]))
			continue;
		type = venture_entity_registry_lookup(venture_entity_registry_get_default(), names[i]);
		query = venture_query_new(type);
		venture_query_set_limit(query, 0);
		venture_query_set_include_deleted(query, TRUE);
		/* Organization-zero rows can contain shared configuration. Include
		 * them too; organization records themselves are selected by ID. */
		venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
		rows = venture_database_find(database, query, error);
		if (rows == NULL)
			return FALSE;
		g_string_append_printf(material, ":table:%s:", names[i]);
		for (j = 0; j < rows->len; j++)
		{
			VentureEntity *row = g_ptr_array_index(rows, j);
			gint64 row_org = venture_entity_get_organization_id(row);
			if (org == 0 || row_org == 0 || row_org == org)
				append_operation_record(material, row);
		}
	}
	return TRUE;
}

/* Generic financial saves can run several service hooks before returning.
 * Determine their ownership from module metadata, never a parallel type list. */
gboolean
venture_accounting_operation_is_financial(VentureEntity *record)
{
	static const gchar *const modules[] = {
		"sales", "finance", "ledger", "invoicing", "receivables", "payables",
		"banking", "assets", "billing", "claims", "payroll", "cutover", "setup",
		"equity", "goods", "projects", "progress", "quotes", "autojournal", NULL
	};
	const VentureModuleInfo *infos;
	gsize count, i;
	guint j;
	infos = venture_module_registry_get_builtin_infos(&count);
	for (i = 0; i < count; i++)
	{
		if (infos[i].entity_types == NULL || !g_strv_contains(modules, infos[i].name))
			continue;
		for (j = 0; infos[i].entity_types[j] != NULL; j++)
			if (G_OBJECT_TYPE(record) == infos[i].entity_types[j]())
				return TRUE;
	}
	return FALSE;
}

void
venture_accounting_operation_suspend(VentureDatabase *database)
{
	guint count = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(database), "venture-accounting-suspended"));
	g_object_set_data(G_OBJECT(database), "venture-accounting-suspended", GUINT_TO_POINTER(count + 1));
}

void
venture_accounting_operation_resume(VentureDatabase *database)
{
	guint count = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(database), "venture-accounting-suspended"));
	g_return_if_fail(count > 0);
	g_object_set_data(G_OBJECT(database), "venture-accounting-suspended", GUINT_TO_POINTER(count - 1));
}

VentureAccountingOperation *
venture_accounting_operation_begin(VentureDatabase *database, const gchar *operation,
	VentureEntity *subject, GPtrArray *details, GVariant *arguments, gint64 organization,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) scope = g_new0(VentureAccountingOperation, 1);
	g_autoptr(GRecMutexLocker) lock = database != NULL ? venture_database_lock_scope(database) : NULL;
	g_autoptr(GVariant) owned_arguments = arguments != NULL ? g_variant_ref_sink(arguments) : NULL;
	g_autofree gchar *identity = NULL;
	g_autoptr(GString) material = NULL;
	g_autofree gchar *digest = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	VentureAccountingOperation *parent;
	gboolean post, pay;
	const gchar *action;
	guint i;

	if (database == NULL)
		return g_steal_pointer(&scope);
	scope->database = g_object_ref(database);
	scope->organization = organization;
	scope->thread = g_thread_self();
	identity = operation_identity(database, actor);
	parent = g_object_get_data(G_OBJECT(database), "venture-accounting-operation");
	if (parent != NULL)
	{
		if (!orm_connection_in_transaction(venture_database_get_connection(database)))
		{
			refuse(error, "The accounting transaction was already rolled back");
			return NULL;
		}
		/* An unprotected outer command is not consent for a protected child
		 * in another organization (or after a policy change within the command).
		 * Its transaction cannot publish independent pending consent safely. */
		if (!parent->enforced)
		{
			if (!rule_enabled(database, organization, "post", &post, error) ||
				!rule_enabled(database, organization, "pay", &pay, error))
				return NULL;
			if (post || pay)
			{
				refuse(error, "A protected child requires whole-operation approval before the enclosing transaction");
				return NULL;
			}
		}
		if (parent->enforced && (parent->approval == NULL ||
			g_object_get_data(G_OBJECT(database), "venture-accounting-suspended") != NULL ||
			parent->thread != scope->thread ||
			(parent->organization != 0 && parent->organization != organization) ||
			(identity != NULL && g_strcmp0(identity, parent->identity) != 0)))
		{
			refuse(error, "An accounting approval cannot authorize another actor, organization or event subscriber");
			return NULL;
		}
		return g_steal_pointer(&scope);
	}
	/* Trusted unattended services retain their existing policy. An active
	 * authenticated principal never becomes trusted by dropping its actor. */
	if (identity == NULL)
		return g_steal_pointer(&scope);
	/* Read policy inside the same serializable transaction as execution.
	 * Even the no-rule case is a policy decision: a concurrent rule update
	 * must not produce a mixed snapshot of authorization and business data. */
	if (!venture_database_has_transaction(database))
	{
		if (!venture_database_begin_serializable(database, error))
			return NULL;
		scope->owner = TRUE;
	}
	if (!rule_enabled(database, organization, "post", &post, error) ||
		!rule_enabled(database, organization, "pay", &pay, error))
		return NULL;
	if (!post && !pay)
	{
		if (scope->owner)
			g_object_set_data(G_OBJECT(database), "venture-accounting-operation", scope);
		return g_steal_pointer(&scope);
	}
	if (!scope->owner)
	{
		refuse(error, "Accounting approval must begin before the enclosing business transaction");
		return NULL;
	}
	scope->enforced = TRUE;
	scope->identity = g_steal_pointer(&identity);
	action = post && pay ? "post+pay" : post ? "post" : "pay";
	/* Install the lazy built-in adapters before capturing their registry.
	 * Replacing a runtime callback or restarting the process invalidates
	 * outstanding consent, even when a plugin reuses the old rule name. */
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "posting_profile") != 0 &&
		venture_entity_registry_lookup(venture_entity_registry_get_default(), "journal") != 0)
		venture_database_get_autojournal_service(database);
	scope->rules_revision = g_strdup(venture_posting_rule_registry_get_revision(
		venture_posting_service_get_rules(venture_database_get_posting_service(database))));
	material = g_string_new("venture-whole-operation-v1:" VENTURE_VERSION_S ":");
	g_string_append_printf(material, "rules:%s:", scope->rules_revision);
	g_string_append_printf(material, "%s:%s:%" G_GINT64_FORMAT, operation, action, organization);
	if (subject != NULL)
		append_operation_record(material, subject);
	if (details != NULL)
		for (i = 0; i < details->len; i++)
			append_operation_record(material, g_ptr_array_index(details, i));
	if (owned_arguments != NULL)
	{
		g_autofree gchar *text = g_variant_print(owned_arguments, TRUE);
		g_string_append_printf(material, ":args:%s", text);
	}
	if (!append_operation_snapshot(database, organization, material, error))
		return NULL;
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, material->str, -1);
	query = venture_query_new(VENTURE_TYPE_ACCOUNTING_APPROVAL);
	venture_query_set_organization(query, organization);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_string(query, "action", VENTURE_FILTER_OP_EQ, action, NULL);
	venture_query_add_filter_string(query, "proposal-digest", VENTURE_FILTER_OP_EQ, digest, NULL);
	rows = venture_database_find(database, query, error);
	if (rows == NULL)
		return NULL;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *state = NULL;
		g_autofree gchar *proposer = NULL;
		g_object_get(row, "state", &state, "proposer", &proposer, NULL);
		if (g_strcmp0(state, "pending") != 0)
			continue;
		{
			g_autoptr(GDateTime) created = NULL;
			g_autoptr(GDateTime) now = venture_time_now();
			g_object_get(row, "created-at", &created, NULL);
			if (created == NULL || g_date_time_difference(now, created) > G_TIME_SPAN_DAY ||
				g_date_time_compare(created, now) > 0)
				continue;
		}
		if (g_strcmp0(proposer, scope->identity) == 0)
		{
			refuse(error, "The whole business operation requires a second account");
			return NULL;
		}
		scope->approval = g_object_ref(row);
		g_object_set_data(G_OBJECT(database), "venture-accounting-operation", scope);
		return g_steal_pointer(&scope);
	}
	{
		g_autoptr(VentureAccountingApproval) pending = venture_accounting_approval_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(pending), organization);
		g_object_set(pending, "action", action,
			"record-type", subject != NULL ? venture_entity_get_entity_name(subject) : operation,
			"record-id", subject != NULL ? venture_entity_get_id(subject) : (gint64)0,
			"proposer", scope->identity, "state", "pending", "proposal-digest", digest, NULL);
		g_object_set_data(G_OBJECT(database), "venture-accounting-operation", scope);
		if (!save_approval(database, VENTURE_ENTITY(pending), actor, error))
			return NULL;
		/* No command has run: this transaction contains only the proposal.
		 * Publishing it here cannot accidentally commit any business writes. */
		g_object_set_data(G_OBJECT(database), "venture-accounting-operation", NULL);
		scope->finished = TRUE;
		if (!venture_database_commit(database, error))
			return NULL;
	}
	refuse(error, "The whole business operation requires a second account; repeat the exact command to approve");
	return NULL;
}

gboolean
venture_accounting_operation_finish(VentureAccountingOperation *operation, GError **error)
{
	VentureActor audit_actor;
	if (operation == NULL || operation->finished)
		return refuse(error, "Accounting operation is missing or already finished");
	if (operation->database != NULL && operation->thread != g_thread_self())
		return refuse(error, "An accounting operation must finish on its owning thread");
	if (!operation->owner)
	{
		operation->finished = TRUE;
		return TRUE;
	}
	/* Save using the stable account identity, not the approving token label.
	 * The same transaction contains both consumption and the financial writes. */
	if (!orm_connection_in_transaction(venture_database_get_connection(operation->database)))
		return refuse(error, "The accounting transaction was already rolled back");
	if (operation->approval != NULL)
	{
		const gchar *revision = venture_posting_rule_registry_get_revision(
			venture_posting_service_get_rules(venture_database_get_posting_service(operation->database)));
		if (g_strcmp0(revision, operation->rules_revision) != 0)
			return refuse(error, "Posting policy changed during the approved operation; propose it again");
		g_object_set(operation->approval, "state", "applied", "approver", operation->identity, NULL);
		audit_actor.kind = VENTURE_ACTOR_KIND_USER;
		audit_actor.name = operation->identity;
		audit_actor.prompt = NULL;
		audit_actor.request_id = NULL;
		audit_actor.approved_by = operation->identity;
		if (!save_approval(operation->database, operation->approval, &audit_actor, error))
			return FALSE;
	}
	g_object_set_data(G_OBJECT(operation->database), "venture-accounting-operation", NULL);
	operation->finished = TRUE;
	return venture_database_commit(operation->database, error);
}

void
venture_accounting_operation_free(VentureAccountingOperation *operation)
{
	if (operation == NULL)
		return;
	if (operation->owner && !operation->finished)
	{
		g_object_set_data(G_OBJECT(operation->database), "venture-accounting-operation", NULL);
		venture_database_rollback(operation->database);
	}
	g_clear_object(&operation->approval);
	g_clear_object(&operation->database);
	g_free(operation->identity);
	g_free(operation->rules_revision);
	g_free(operation);
}

gboolean
venture_accounting_operation_is_approved(VentureDatabase *database)
{
	g_autoptr(GRecMutexLocker) lock = database != NULL ? venture_database_lock_scope(database) : NULL;
	VentureAccountingOperation *operation;
	if (database == NULL)
		return FALSE;
	operation = g_object_get_data(G_OBJECT(database), "venture-accounting-operation");
	return operation != NULL && operation->enforced && operation->approval != NULL;
}

gboolean
venture_accounting_operation_guard_write(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GRecMutexLocker) lock = venture_database_lock_scope(database);
	g_autoptr(VentureAccountingOperation) nested = NULL;
	VentureAccountingOperation *parent = g_object_get_data(G_OBJECT(database), "venture-accounting-operation");
	if (venture_database_has_transaction(database) &&
		!orm_connection_in_transaction(venture_database_get_connection(database)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE, "The transaction was already rolled back");
		return FALSE;
	}
	if (parent == NULL || !parent->enforced || snapshot_excluded(venture_entity_get_entity_name(record)))
		return TRUE;
	nested = venture_accounting_operation_begin(database, "record.derived", record, NULL, NULL,
		venture_entity_get_organization_id(record), actor, error);
	return nested != NULL;
}
