/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

#define POST_ORIGIN "orgaccess:journal-post:"

static gchar *
journal_fingerprint(VentureDatabase *database, VentureEntity *journal, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL_LINE);
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *text = NULL;
	guint i;
	venture_query_add_filter_int(query, "journal-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(journal), NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	lines = venture_database_find(database, query, error);
	if (NULL == lines)
		return NULL;
	json_builder_begin_array(builder);
	for (i = 0; i < lines->len; i++)
		json_builder_add_value(builder, venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(lines, i)), TRUE));
	json_builder_end_array(builder);
	node = json_builder_get_root(builder);
	text = venture_json_to_string(node, FALSE);
	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
}

gboolean
venture_orgaccess_apply_post(VentureDatabase *database, VentureEntity *original,
	const gchar *via, const VentureActor *actor, GError **error)
{
	VentureAccessPolicy *policy = venture_database_get_access_policy(database);
	g_autofree gchar *fingerprint = NULL;
	g_autoptr(VentureJournal) posted = NULL;
	if (!VENTURE_IS_JOURNAL(original) || !g_str_has_prefix(via, POST_ORIGIN))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Invalid journal posting proposal");
		return FALSE;
	}
	if (!venture_access_policy_check_write(policy, original, "write", error))
		return FALSE;
	if (!venture_database_begin(database, error))
		return FALSE;
	fingerprint = journal_fingerprint(database, original, error);
	if (NULL == fingerprint)
		goto failed;
	if (0 != g_strcmp0(fingerprint, via + strlen(POST_ORIGIN)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Journal lines changed after the posting was proposed; propose it again");
		goto failed;
	}
	posted = venture_posting_service_post(venture_database_get_posting_service(database),
		VENTURE_JOURNAL(original), NULL, NULL, actor, error);
	if (NULL == posted)
		goto failed;
	return venture_database_commit(database, error);
failed:
	venture_database_rollback(database);
	return FALSE;
}

JsonNode *
venture_orgaccess_post_journal(VentureContext *context, const VentureAuthPrincipal *principal,
	gint64 id, gboolean force_proposal, gboolean *staged_out, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	VentureAccessPolicy *policy = venture_database_get_access_policy(database);
	g_autoptr(VentureAccessScope) request_scope = venture_orgaccess_enter_ai(context, principal);
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) journal = NULL;
	g_autoptr(VentureEntity) proposed = NULL;
	g_autoptr(VentureJournal) posted = NULL;
	g_autofree gchar *fingerprint = NULL;
	g_autofree gchar *via = NULL;
	VentureConfirmation *confirmation;
	VentureActor actor;
	gboolean propose;
	gint state;
	*staged_out = FALSE;
	if (!venture_context_module_enabled(context, "orgaccess") || !venture_context_module_enabled(context, "ledger"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Journal posting is unavailable");
		return NULL;
	}
	if (NULL == principal || !principal->authenticated || principal->role == VENTURE_USER_ROLE_VIEWER)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Journal posting requires the editor role or higher");
		return NULL;
	}
	/* Membership is checked before any fetched content is returned. Editors
	 * may propose a post even though they cannot read the financial tables. */
	internal = venture_access_policy_enter(policy, NULL);
	journal = venture_database_get(database, VENTURE_TYPE_JOURNAL, id, error);
	g_clear_object(&internal);
	if (NULL == journal || venture_entity_is_deleted(journal))
	{
		if (NULL == error || NULL == *error)
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No such record");
		return NULL;
	}
	propose = venture_access_policy_requires_approval(policy, principal, "post", journal, error);
	if (NULL != error && NULL != *error)
		return NULL;
	if (!propose && !venture_access_policy_can(policy, principal, "write", journal, error))
		return NULL;
	propose = propose || force_proposal;
	g_object_get(journal, "state", &state, NULL);
	if (state != VENTURE_JOURNAL_DRAFT)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Only a draft journal may be posted");
		return NULL;
	}
	actor.kind = principal->token_id > 0 ? VENTURE_ACTOR_KIND_IMPORT : VENTURE_ACTOR_KIND_USER;
	actor.name = principal->name;
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	if (!propose)
	{
		posted = venture_posting_service_post(venture_database_get_posting_service(database), VENTURE_JOURNAL(journal), NULL, NULL, &actor, error);
		return NULL != posted ? venture_serializable_to_json(VENTURE_SERIALIZABLE(posted), FALSE) : NULL;
	}
	internal = venture_access_policy_enter(policy, NULL);
	proposed = venture_database_get(database, VENTURE_TYPE_JOURNAL, id, error);
	fingerprint = journal_fingerprint(database, journal, error);
	g_clear_object(&internal);
	if (NULL == proposed || NULL == fingerprint)
		return NULL;
	g_object_set(proposed, "state", VENTURE_JOURNAL_POSTED, NULL);
	via = g_strconcat(POST_ORIGIN, fingerprint, NULL);
	confirmation = venture_confirmation_store_stage(venture_context_get_confirmations(context),
		VENTURE_AUDIT_ACTION_UPDATE, proposed, journal, &actor, via, error);
	if (NULL == confirmation)
		return NULL;
	*staged_out = TRUE;
	return venture_confirmation_to_json(confirmation);
}

HtmxResponse *
venture_orgaccess_web_post(VentureAuth *auth, VentureContext *context, HtmxRequest *request, GHashTable *params)
{
	g_autoptr(VentureAuthPrincipal) principal = venture_auth_authenticate(auth, request);
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *body = NULL;
	HtmxResponse *response;
	gboolean staged = FALSE;
	guint status;
	const gchar *stage = htmx_request_get_query_param(request, "stage");
	if (NULL != stage && 0 != g_strcmp0(stage, "1") && 0 != g_strcmp0(stage, "true") &&
		0 != g_strcmp0(stage, "0") && 0 != g_strcmp0(stage, "false"))
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "stage must be 1, true, 0 or false");
	if (NULL == error && venture_auth_require(auth, principal, VENTURE_USER_ROLE_EDITOR, &error))
		node = venture_orgaccess_post_journal(context, principal,
			g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10),
			0 == g_strcmp0(stage, "1") || 0 == g_strcmp0(stage, "true"), &staged, &error);
	status = NULL != node ? (staged ? 202 : 200) : (NULL != error ? venture_error_to_http_status(error->code) : 500);
	body = NULL != node ? venture_json_to_string(node, FALSE) : venture_json_error_to_string(error, FALSE);
	response = htmx_response_new_with_content(body);
	htmx_response_set_content_type(response, "application/json; charset=utf-8");
	htmx_response_set_status(response, status);
	return response;
}
