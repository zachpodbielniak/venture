/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Surface adapters share the enclosing web server's authentication helpers. */
#include "sequences/venture-sequence-tracking-private.h"
static void
venture_web_sequence_prefill(VentureEntity *record, HtmxRequest *request)
{
	const gchar *contact;
	if (G_OBJECT_TYPE(record) != VENTURE_TYPE_SEQUENCE_ENROLLMENT)
		return;
	contact = htmx_request_get_query_param(request, "contact_id");
	if (contact != NULL)
		venture_entity_set_field_from_string(record, "contact-id", contact, NULL);
}

static HtmxResponse *
venture_web_sequence_enroll(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) sequence = NULL;
	g_autoptr(VentureEntity) enrollment = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	VentureActor actor;
	gboolean stage;
	gint64 id;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "sequences");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	sequence = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_SEQUENCE, id, &error);
	if (sequence == NULL)
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Sequence record does not exist");
		return venture_web_error_response(error);
	}
	body = htmx_request_get_json(request, &error);
	if (body == NULL)
		return venture_web_error_response(error);
	enrollment = g_object_new(VENTURE_TYPE_SEQUENCE_ENROLLMENT, NULL);
	if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(enrollment), body, &error))
		return venture_web_error_response(error);
	g_object_set(enrollment, "sequence-id", id,
		"organization-id", venture_entity_get_organization_id(sequence), NULL);
	if (!venture_confirmation_parse_stage_flag(htmx_request_get_query_param(request, "stage"), &stage, &error))
		return venture_web_error_response(error);
	if (stage)
		return venture_web_api_stage(self, enrollment, NULL, principal, VENTURE_AUDIT_ACTION_CREATE);
	venture_auth_to_actor(principal, &actor);
	if (!venture_sequence_service_enroll(venture_sequence_service_get(venture_context_get_database(self->context)), enrollment, &actor, &error))
		return venture_web_error_response(error);
	result = venture_serializable_to_json(VENTURE_SERIALIZABLE(enrollment), FALSE);
	return venture_web_json_response(result, 201);
}

static gboolean
venture_web_sequence_direct(HtmxRequest *request, GError **error)
{
	gboolean stage;
	if (!venture_confirmation_parse_stage_flag(htmx_request_get_query_param(request, "stage"), &stage, error))
		return FALSE;
	if (stage)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
			"Only sequence enrollment can be staged; this action was not performed");
		return FALSE;
	}
	return TRUE;
}

static HtmxResponse *
venture_web_sequence_action(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) enrollment = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	VentureActor actor;
	gint64 id = g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10);
	gboolean ui = g_str_has_prefix(htmx_request_get_path(request), "/ui/");
	gate = ui ? venture_web_ui_require_session(self, request) : venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "sequences");
	if (gate != NULL)
		return gate;
	if (!venture_web_sequence_direct(request, &error))
		return venture_web_error_response(error);
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return venture_web_error_response(error);
	enrollment = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_SEQUENCE_ENROLLMENT, id, &error);
	if (enrollment == NULL)
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Sequence record does not exist");
		return venture_web_error_response(error);
	}
	venture_auth_to_actor(principal, &actor);
	if (!venture_sequence_service_transition(venture_sequence_service_get(venture_context_get_database(self->context)),
		venture_entity_get_organization_id(enrollment), id, g_hash_table_lookup(params, "action"),
		"Manual operator action", &actor, &error))
		return venture_web_error_response(error);
	if (ui)
	{
		g_autofree gchar *path = g_strdup_printf("/e/sequence_enrollment/%" G_GINT64_FORMAT, id);
		return venture_web_redirect_to(path);
	}
	g_clear_object(&enrollment);
	enrollment = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_SEQUENCE_ENROLLMENT, id, &error);
	if (enrollment == NULL)
	{
		if (error == NULL)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Sequence record does not exist");
		return venture_web_error_response(error);
	}
	result = venture_serializable_to_json(VENTURE_SERIALIZABLE(enrollment), FALSE);
	return venture_web_json_response(result, 200);
}

static HtmxResponse *
venture_web_sequence_run(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(GDateTime) as_of = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;
	VentureActor actor;
	JsonObject *object;
	gint64 org;
	gint count;
	const gchar *time;
	(void)params;
	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "sequences");
	if (gate != NULL)
		return gate;
	if (!venture_web_sequence_direct(request, &error))
		return venture_web_error_response(error);
	body = htmx_request_get_json(request, &error);
	if (body == NULL)
		return venture_web_error_response(error);
	if (!JSON_NODE_HOLDS_OBJECT(body))
	{
		venture_set_error_validation(&error, "body", "Expected a JSON object");
		return venture_web_error_response(error);
	}
	object = json_node_get_object(body);
	if (json_object_has_member(object, "as_of"))
	{
		JsonNode *value = json_object_get_member(object, "as_of");
		if (!JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_STRING)
		{
			venture_set_error_validation(&error, "as_of", "Expected an ISO timestamp string");
			return venture_web_error_response(error);
		}
	}
	time = venture_json_object_get_string(object, "as_of", NULL);
	as_of = time != NULL ? g_date_time_new_from_iso8601(time, NULL) : g_date_time_new_now_utc();
	if (as_of == NULL)
	{
		venture_set_error_validation(&error, "as_of", "Expected an ISO timestamp with timezone");
		return venture_web_error_response(error);
	}
	org = venture_json_object_get_int(object, "organization_id", venture_context_get_default_organization_id(self->context));
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	count = venture_sequence_service_run_due(venture_sequence_service_get(venture_context_get_database(self->context)), org, as_of, &actor, &error);
	if (count < 0)
		return venture_web_error_response(error);
	json_node_take_object(result, json_object_new());
	json_object_set_int_member(json_node_get_object(result), "processed", count);
	return venture_web_json_response(result, 200);
}

static void
venture_web_sequence_fields(VentureWebServer *self, GString *content, VentureEntity *row)
{
	g_autoptr(GPtrArray) specs = venture_entity_get_field_specs(row);
	guint i;
	/* The same reading as a record's Details: what is set, nothing
	 * else -- an enrollment that has not exited has no exit reason. */
	g_string_append(content, "<dl class=\"facts\">");
	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);
		const gchar *name = venture_field_spec_get_name(spec);
		if (g_strcmp0(name, "status") && g_strcmp0(name, "current-step") &&
			g_strcmp0(name, "next-run-at") && g_strcmp0(name, "exit-reason") &&
			g_strcmp0(name, "enrollment-reason") && g_strcmp0(name, "state") &&
			g_strcmp0(name, "subject") && g_strcmp0(name, "error"))
			continue;
		if (venture_web_value_is_empty(row, spec))
			continue;
		g_string_append(content, "<div class=\"fact\"><dt>");
		venture_html_escape_append(content, venture_field_spec_get_label(spec));
		g_string_append(content, "</dt><dd>");
		venture_web_append_detail_value(self, content, row, spec);
		g_string_append(content, "</dd></div>");
	}
	g_string_append(content, "</dl>");
}

static void
venture_web_sequence_panel(VentureWebServer *self, GString *content,
	VentureAuthPrincipal *principal, VentureEntity *record)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) enrollments = NULL;
	guint i;
	GType type = G_OBJECT_TYPE(record);
	gboolean editor = venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, NULL);
	if ((type != VENTURE_TYPE_CONTACT && type != VENTURE_TYPE_SEQUENCE_ENROLLMENT) ||
		!venture_web_module_enabled(self, "sequences"))
		return;
	if (editor && type == VENTURE_TYPE_CONTACT)
		g_string_append_printf(content, "<p><a class=\"btn\" href=\"/e/sequence_enrollment/new?contact_id=%" G_GINT64_FORMAT "\">Enroll in sequence</a></p>", venture_entity_get_id(record));
	query = venture_query_new(VENTURE_TYPE_SEQUENCE_ENROLLMENT);
	venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, venture_entity_get_organization_id(record), NULL);
	venture_query_add_filter_int(query, type == VENTURE_TYPE_CONTACT ? "contact-id" : "id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(record), NULL);
	venture_query_set_limit(query, 0);
	enrollments = venture_database_find(venture_context_get_database(self->context), query, NULL);
	if (enrollments == NULL)
		return;
	g_string_append(content, "<section class=\"card\"><div class=\"card-head\"><h2>Sequence history</h2></div><div class=\"card-body\">");
	for (i = 0; i < enrollments->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(enrollments, i);
		g_autoptr(VentureQuery) history_query = venture_query_new(VENTURE_TYPE_SEQUENCE_DELIVERY);
		g_autoptr(GPtrArray) history = NULL;
		guint j;
		g_string_append_printf(content, "<h3><a href=\"/e/sequence_enrollment/%" G_GINT64_FORMAT "\">Enrollment #%" G_GINT64_FORMAT "</a></h3>", venture_entity_get_id(row), venture_entity_get_id(row));
		venture_web_sequence_fields(self, content, row);
		if (editor)
		{
			const gchar *actions[] = { "pause", "resume", "exit" };
			for (j = 0; j < G_N_ELEMENTS(actions); j++)
				g_string_append_printf(content, "<form method=\"post\" action=\"/ui/sequence_enrollment/%" G_GINT64_FORMAT "/%s\"><button class=\"btn\">%s</button></form>", venture_entity_get_id(row), actions[j], actions[j]);
		}
		venture_query_add_filter_int(history_query, "organization-id", VENTURE_FILTER_OP_EQ, venture_entity_get_organization_id(row), NULL);
		venture_query_add_filter_int(history_query, "enrollment-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(row), NULL);
		venture_query_set_limit(history_query, 0);
		history = venture_database_find(venture_context_get_database(self->context), history_query, NULL);
		for (j = 0; history != NULL && j < history->len; j++)
			venture_web_sequence_fields(self, content, g_ptr_array_index(history, j));
	}
	g_string_append(content, "</div></section>");
}

/* Public tracking endpoints. They never require a session, never reveal
 * whether a token exists beyond the 404, and record nothing for unknown
 * tokens. Both are 404 when the module is off. */
static HtmxResponse *
venture_web_sequence_tracking_missing(void)
{
	HtmxResponse *response = htmx_response_new_with_content("Not found");
	htmx_response_set_status(response, 404);
	return response;
}

/* Read for the machine-open judgement and never stored or logged. */
static const gchar *
venture_web_sequence_user_agent(HtmxRequest *request)
{
	SoupServerMessage *message = htmx_request_get_message(request);
	return message ? soup_message_headers_get_one(soup_server_message_get_request_headers(message), "User-Agent") : NULL;
}

static HtmxResponse *
venture_web_sequence_open(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autofree gchar *token = g_strdup(g_hash_table_lookup(params, "token"));
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	HtmxResponse *response;
	const guint8 *pixel;
	gsize length;
	(void)request;
	if (!venture_web_module_enabled(self, "sequences") || token == NULL || !g_str_has_suffix(token, ".gif"))
		return venture_web_sequence_tracking_missing();
	token[strlen(token) - 4] = '\0';
	if (!venture_sequence_service_record_open_full(venture_sequence_service_get(venture_context_get_database(self->context)),
		token, venture_web_sequence_user_agent(request), now, NULL, &error))
		return venture_web_sequence_tracking_missing();
	pixel = venture_sequence_tracking_pixel(&length);
	bytes = g_bytes_new_static(pixel, length);
	response = htmx_response_new();
	htmx_response_set_bytes(response, bytes);
	htmx_response_set_content_type(response, "image/gif");
	htmx_response_add_header(response, "Cache-Control", "no-store, private");
	return response;
}

static HtmxResponse *
venture_web_sequence_click(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	const gchar *token = g_hash_table_lookup(params, "token");
	const gchar *text = g_hash_table_lookup(params, "n");
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL;
	gchar *end = NULL;
	gint64 position;
	(void)request;
	if (!venture_web_module_enabled(self, "sequences") || token == NULL || text == NULL)
		return venture_web_sequence_tracking_missing();
	position = g_ascii_strtoll(text, &end, 10);
	if (position <= 0 || end == text || *end != '\0')
		return venture_web_sequence_tracking_missing();
	url = venture_sequence_service_record_click_full(venture_sequence_service_get(venture_context_get_database(self->context)),
		token, position, venture_web_sequence_user_agent(request), now, NULL, &error);
	if (url == NULL)
		return venture_web_sequence_tracking_missing();
	return venture_web_redirect_to(url);
}

/* The journey map on a sequence's page: every step in order, what it does,
 * how many enrollments it has seen, and for a branch where yes and no go.
 * Drawn from the records; nothing is stored for the drawing. */
static void
venture_web_sequence_step_tally(VentureDatabase *database, VentureEntity *step, gint64 *done, gint64 *yes, gint64 *no, gint64 *skipped)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SEQUENCE_DELIVERY);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	*done = *yes = *no = *skipped = 0;
	venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, venture_entity_get_organization_id(step), NULL);
	venture_query_add_filter_int(query, "step-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(step), NULL);
	venture_query_set_limit(query, 10000);
	rows = venture_database_find(database, query, NULL);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *taken = NULL;
		gint state = 0;
		g_object_get(row, "state", &state, "branch-result", &taken, NULL);
		if (!g_strcmp0(taken, "yes")) (*yes)++;
		else if (!g_strcmp0(taken, "no")) (*no)++;
		else if (state == 3) (*skipped)++;
		else (*done)++;
	}
}

static void
venture_web_sequence_journey_block(VentureWebServer *self, GString *content, VentureEntity *record)
{
	static const gchar *const channels[] = { "Email", "Call task", "Text task", "Wait", "Branch" };
	static const gchar *const conditions[] = { "", "opened the last email", "clicked a link in the last email", "replied" };
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) steps = NULL;
	guint i;
	if (G_OBJECT_TYPE(record) != VENTURE_TYPE_SEQUENCE || !venture_web_module_enabled(self, "sequences"))
		return;
	query = venture_query_new(VENTURE_TYPE_SEQUENCE_STEP);
	venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, venture_entity_get_organization_id(record), NULL);
	venture_query_add_filter_int(query, "sequence-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(record), NULL);
	venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 200);
	steps = venture_database_find(database, query, NULL);
	g_string_append(content, "<section class=\"card journey\"><div class=\"card-head\"><h2>Journey</h2>"
		"<a class=\"btn\" href=\"/e/sequence_step/new\">Add step</a></div><div class=\"card-body\">");
	if (steps == NULL || steps->len == 0)
	{
		g_string_append(content, "<p class=\"muted\">No steps yet. Add an email, a wait or a branch to start the journey.</p></div></section>");
		return;
	}
	g_string_append(content, "<ol class=\"journey-map\" style=\"list-style:none;margin:0;padding:0;display:grid;gap:0.75rem\">");
	for (i = 0; i < steps->len; i++)
	{
		VentureEntity *step = g_ptr_array_index(steps, i);
		g_autofree gchar *subject = NULL;
		gint64 position = 0, days = 0, hours = 0, yes_at = 0, no_at = 0, then_at = 0, done, yes, no, skipped;
		gint channel = 0, on = 0;
		gboolean active = TRUE, end = FALSE;
		g_object_get(step, "position", &position, "channel", &channel, "subject", &subject, "delay-days", &days,
			"delay-hours", &hours, "branch-on", &on, "yes-position", &yes_at, "no-position", &no_at,
			"then-position", &then_at, "end-after", &end, "active", &active, NULL);
		venture_web_sequence_step_tally(database, step, &done, &yes, &no, &skipped);
		g_string_append_printf(content, "<li style=\"border:1px solid var(--border);padding:0.75rem;%s\">"
			"<div style=\"display:flex;flex-wrap:wrap;gap:0.5rem;align-items:baseline\">"
			"<span class=\"badge\">Step %" G_GINT64_FORMAT "</span><strong>%s</strong>",
			channel == 4 ? "border-left:4px solid var(--accent)" : "", position, channels[CLAMP(channel, 0, 4)]);
		if (days || hours)
		{
			g_string_append(content, "<span class=\"muted\">after ");
			if (days) g_string_append_printf(content, "%" G_GINT64_FORMAT " %s", days, days == 1 ? "day" : "days");
			if (days && hours) g_string_append(content, ", ");
			if (hours) g_string_append_printf(content, "%" G_GINT64_FORMAT " %s", hours, hours == 1 ? "hour" : "hours");
			g_string_append(content, "</span>");
		}
		if (!active) g_string_append(content, "<span class=\"badge warning\">Off</span>");
		g_string_append_printf(content, "<a href=\"/e/sequence_step/%" G_GINT64_FORMAT "\" style=\"margin-left:auto\">Edit</a></div>", venture_entity_get_id(step));
		if (channel == 4)
		{
			g_autofree gchar *yes_text = yes_at > 0 ? g_strdup_printf("step %" G_GINT64_FORMAT, yes_at) : g_strdup("the next step");
			g_autofree gchar *no_text = no_at > 0 ? g_strdup_printf("step %" G_GINT64_FORMAT, no_at) : g_strdup("the next step");
			g_string_append_printf(content, "<p style=\"margin:0.4rem 0\">Waits, then asks: has the contact %s?</p>"
				"<div style=\"display:flex;flex-wrap:wrap;gap:0.5rem\">"
				"<span class=\"badge positive\">Yes &rarr; %s &middot; %" G_GINT64_FORMAT "</span>"
				"<span class=\"badge negative\">No &rarr; %s &middot; %" G_GINT64_FORMAT "</span></div>",
				conditions[CLAMP(on, 0, 3)], yes_text, yes, no_text, no);
		}
		else
		{
			if (subject != NULL && *subject)
			{
				g_string_append(content, "<p style=\"margin:0.4rem 0;overflow-wrap:anywhere\">");
				venture_html_escape_append(content, subject);
				g_string_append(content, "</p>");
			}
			g_string_append_printf(content, "<p class=\"muted\" style=\"margin:0\">%" G_GINT64_FORMAT " reached", done);
			if (skipped) g_string_append_printf(content, " &middot; %" G_GINT64_FORMAT " skipped", skipped);
			if (then_at > 0) g_string_append_printf(content, " &middot; then step %" G_GINT64_FORMAT, then_at);
			if (end) g_string_append(content, " &middot; the journey ends here");
			g_string_append(content, "</p>");
		}
		g_string_append(content, "</li>");
	}
	g_string_append(content, "</ol><p class=\"muted\">A branch waits its delay and then looks at the latest email: opens and clicks count only "
		"when they are a person's, never a mail scanner's. Steps a path jumps over are recorded as skipped.</p></div></section>");
}
