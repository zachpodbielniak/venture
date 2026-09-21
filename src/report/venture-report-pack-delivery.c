/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Scheduled report delivery: a pack with recipients and deliver=email hands
 * its last retained output to the transactional mail outbox. Nothing here
 * talks to a relay; the outbox sweep does, later, with its own retry and
 * uncertainty rules (docs/mail.org). The pack keeps the outcome of the
 * hand-over so a refused delivery is visible without a log.
 */
#include "venture.h"
#include <string.h>

gboolean
venture_report_pack_delivery_address_is_valid(const gchar *address)
{
	const gchar *at;
	const gchar *p;
	if (address == NULL || address[0] == '\0' || strlen(address) > 254)
		return FALSE;
	at = strchr(address, '@');
	if (at == NULL || at == address || at[1] == '\0' || strchr(at + 1, '@') != NULL)
		return FALSE;
	for (p = address; *p != '\0'; p++)
	{
		if ((guchar)*p < 0x21 || strchr(",;<>()\"[]\\", *p) != NULL)
			return FALSE;
	}
	/* The domain is a dotted name: no empty labels at either end or inside. */
	if (at[1] == '.' || at[strlen(at) - 1] == '.' || strstr(at + 1, "..") != NULL)
		return FALSE;
	return TRUE;
}

static gboolean
refuse(GError **error, const gchar *field, const gchar *message)
{
	venture_set_error_validation(error, field, "%s", message);
	return FALSE;
}

gboolean
venture_report_pack_delivery_validate(VentureEntity *pack, GError **error)
{
	g_autofree gchar *deliver = NULL;
	g_autofree gchar *recipients = NULL;
	g_autoptr(GString) normalized = g_string_new(NULL);
	g_auto(GStrv) parts = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_REPORT_PACK(pack), FALSE);
	g_object_get(pack, "deliver", &deliver, "recipients", &recipients, NULL);
	if (deliver == NULL || deliver[0] == '\0')
	{
		g_object_set(pack, "deliver", "none", NULL);
		g_free(deliver);
		deliver = g_strdup("none");
	}
	else if (g_strcmp0(deliver, "none") != 0 && g_strcmp0(deliver, "email") != 0)
		return refuse(error, "deliver", "deliver must be none or email");
	parts = recipients != NULL && recipients[0] != '\0' ? g_strsplit(recipients, ",", -1) : NULL;
	for (i = 0; parts != NULL && parts[i] != NULL; i++)
	{
		const gchar *address = g_strstrip(parts[i]);
		if (address[0] == '\0')
			return refuse(error, "recipients", "recipients has an empty entry; separate addresses with commas and no trailing comma");
		if (!venture_report_pack_delivery_address_is_valid(address))
		{
			venture_set_error_validation(error, "recipients", "%s is not a valid email address", address);
			return FALSE;
		}
		if (normalized->len > 0)
			g_string_append_c(normalized, ',');
		g_string_append(normalized, address);
	}
	if (g_strcmp0(deliver, "email") == 0 && normalized->len == 0)
		return refuse(error, "recipients", "deliver=email needs at least one recipient");
	if (g_strcmp0(recipients, normalized->str) != 0)
		g_object_set(pack, "recipients", normalized->str, NULL);
	return TRUE;
}

/* A file name from a title: ASCII letters and digits, runs of anything else
 * folded to one hyphen, so "Month-end pack" attaches as month-end-pack.json. */
static gchar *
slug(const gchar *title, const gchar *fallback)
{
	g_autoptr(GString) out = g_string_new(NULL);
	const gchar *p;
	gboolean pending = FALSE;
	for (p = title != NULL ? title : ""; *p != '\0' && out->len < 64; p++)
	{
		if (g_ascii_isalnum(*p))
		{
			if (pending && out->len > 0)
				g_string_append_c(out, '-');
			g_string_append_c(out, g_ascii_tolower(*p));
			pending = FALSE;
		}
		else
			pending = TRUE;
	}
	if (out->len == 0)
		g_string_append(out, fallback);
	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* The CSV the web export would produce for one retained result, built from
 * the formatted twins the JSON carries beside each raw cell. */
static gchar *
csv_from_result(JsonObject *result)
{
	g_autoptr(GString) text = g_string_new(NULL);
	JsonArray *columns;
	JsonArray *rows;
	guint i;
	guint j;
	if (!json_object_has_member(result, "columns") || !json_object_has_member(result, "rows"))
		return NULL;
	columns = json_object_get_array_member(result, "columns");
	rows = json_object_get_array_member(result, "rows");
	if (columns == NULL || rows == NULL || json_array_get_length(columns) == 0 || json_array_get_length(rows) == 0)
		return NULL;
	for (i = 0; i < json_array_get_length(columns); i++)
	{
		JsonObject *column = json_array_get_object_element(columns, i);
		g_autofree gchar *escaped = venture_csv_escape(venture_json_object_get_string(column, "label", ""));
		if (i > 0)
			g_string_append_c(text, ',');
		g_string_append(text, escaped);
	}
	g_string_append_c(text, '\n');
	for (j = 0; j < json_array_get_length(rows); j++)
	{
		JsonObject *row = json_array_get_object_element(rows, j);
		for (i = 0; i < json_array_get_length(columns); i++)
		{
			JsonObject *column = json_array_get_object_element(columns, i);
			g_autofree gchar *key = g_strconcat(venture_json_object_get_string(column, "key", ""), "_formatted", NULL);
			g_autofree gchar *escaped = venture_csv_escape(venture_json_object_get_string(row, key, ""));
			if (i > 0)
				g_string_append_c(text, ',');
			g_string_append(text, escaped);
		}
		g_string_append_c(text, '\n');
	}
	return g_string_free(g_steal_pointer(&text), FALSE);
}

static void
add_inline_attachment(JsonBuilder *builder, const gchar *name, const gchar *mime, const gchar *text)
{
	g_autofree gchar *encoded = g_base64_encode((const guchar *)text, strlen(text));
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "type");
	json_builder_add_string_value(builder, "inline");
	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, name);
	json_builder_set_member_name(builder, "mime");
	json_builder_add_string_value(builder, mime);
	json_builder_set_member_name(builder, "data");
	json_builder_add_string_value(builder, encoded);
	json_builder_end_object(builder);
}

/* The organization's report_pack template, or the built-in wording. */
static VentureMailTemplate *
load_template(VentureDatabase *database, gint64 organization_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_TEMPLATE);
	g_autoptr(VentureEntity) stored = NULL;
	VentureMailTemplate *template;
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ, VENTURE_REPORT_PACK_MAIL_TEMPLATE, NULL);
	stored = venture_database_find_one(database, query, error);
	if (error != NULL && *error != NULL)
		return NULL;
	if (stored != NULL)
		return VENTURE_MAIL_TEMPLATE(g_steal_pointer(&stored));
	template = venture_mail_template_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(template), organization_id);
	g_object_set(template, "name", VENTURE_REPORT_PACK_MAIL_TEMPLATE,
		"subject", "Report pack {name}: {period}",
		"text-body", "The report pack {name} for {period} ran at {run_at}.\n\n{reports}\n"
			"The pack's output is attached as JSON, with a CSV for each tabular report.\n",
		"html-body", "", NULL);
	return template;
}

/* Composes the message for the retained output and hands it to the outbox.
 * Every refusal is a validation error naming what to fix. */
static VentureMailMessage *
enqueue_output(VentureContext *context, VentureReportPack *pack, GDateTime *clock,
	const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	VentureMailOutbox *outbox;
	g_autoptr(VentureMailer) mailer = NULL;
	g_autofree gchar *deliver = NULL;
	g_autofree gchar *recipients = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *output = NULL;
	g_autoptr(GDateTime) run_at = NULL;
	g_autoptr(JsonNode) parsed = NULL;
	g_autoptr(JsonBuilder) attachments = json_builder_new();
	g_autoptr(JsonNode) attachment_node = NULL;
	g_autofree gchar *attachment_json = NULL;
	g_autoptr(JsonObject) values = json_object_new();
	g_autoptr(GString) reports = g_string_new(NULL);
	g_autoptr(GString) period = g_string_new(NULL);
	g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(VentureMailTemplate) template = NULL;
	g_autoptr(VentureMailMessage) message = NULL;
	g_autofree gchar *pack_slug = NULL;
	g_autofree gchar *file_name = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *when = NULL;
	g_autofree gchar *stamp = NULL;
	JsonArray *results;
	gint64 organization_id;
	guint i;

	g_object_get(pack, "deliver", &deliver, "recipients", &recipients, "name", &name,
		"last-output", &output, "last-run-at", &run_at, NULL);
	organization_id = venture_entity_get_organization_id(VENTURE_ENTITY(pack));
	if (g_strcmp0(deliver, "email") != 0)
	{
		refuse(error, "deliver", "The pack's deliver is none; set deliver=email and recipients to mail its output");
		return NULL;
	}
	if (recipients == NULL || recipients[0] == '\0')
	{
		refuse(error, "recipients", "The pack has no recipients");
		return NULL;
	}
	if (output == NULL || output[0] == '\0' || run_at == NULL)
	{
		refuse(error, "last_output", "The pack has no retained output to deliver; run it first");
		return NULL;
	}
	if (!venture_context_module_enabled(context, "mail") || (outbox = venture_context_get_mail_outbox(context)) == NULL)
	{
		refuse(error, "mail", "The mail module is off, so the pack's output cannot be mailed");
		return NULL;
	}
	g_object_get(outbox, "mailer", &mailer, NULL);
	if (mailer == NULL)
	{
		refuse(error, "mail", "No mail transport is configured, so the pack's output cannot be mailed");
		return NULL;
	}
	if (VENTURE_IS_SMTP_MAILER(mailer))
	{
		g_autoptr(VentureConfig) config = NULL;
		g_autofree gchar *host = NULL;
		g_object_get(mailer, "config", &config, NULL);
		if (config != NULL)
			g_object_get(config, "mail-host", &host, NULL);
		if (config != NULL && (host == NULL || host[0] == '\0'))
		{
			refuse(error, "mail", "No mail transport is configured (mail.host is empty), so the pack's output cannot be mailed");
			return NULL;
		}
	}
	parsed = json_from_string(output, error);
	if (parsed == NULL)
		return NULL;
	if (!JSON_NODE_HOLDS_ARRAY(parsed))
	{
		refuse(error, "last_output", "The retained output is not a result array");
		return NULL;
	}
	results = json_node_get_array(parsed);

	/* One line per report for the body; the period is the distinct labels. */
	for (i = 0; i < json_array_get_length(results); i++)
	{
		JsonNode *element = json_array_get_element(results, i);
		JsonObject *result;
		const gchar *title;
		const gchar *label = "";
		JsonArray *metrics;
		guint m;
		if (!JSON_NODE_HOLDS_OBJECT(element))
			continue;
		result = json_node_get_object(element);
		title = venture_json_object_get_string(result, "title", "Report");
		if (json_object_has_member(result, "period") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(result, "period")))
			label = venture_json_object_get_string(json_object_get_object_member(result, "period"), "label", "");
		if (label[0] != '\0' && !g_ptr_array_find_with_equal_func(labels, label, g_str_equal, NULL))
		{
			g_ptr_array_add(labels, g_strdup(label));
			if (period->len > 0)
				g_string_append(period, ", ");
			g_string_append(period, label);
		}
		g_string_append_printf(reports, "- %s%s%s\n", title, label[0] != '\0' ? ", " : "", label);
		metrics = json_object_has_member(result, "metrics") ? json_object_get_array_member(result, "metrics") : NULL;
		for (m = 0; metrics != NULL && m < json_array_get_length(metrics); m++)
		{
			JsonObject *metric = json_array_get_object_element(metrics, m);
			g_string_append_printf(reports, "    %s: %s\n", venture_json_object_get_string(metric, "label", ""),
				venture_json_object_get_string(metric, "formatted", ""));
		}
	}
	if (period->len == 0)
		g_string_append(period, "the last run");

	json_builder_begin_array(attachments);
	pack_slug = slug(name, "report-pack");
	file_name = g_strconcat(pack_slug, ".json", NULL);
	add_inline_attachment(attachments, file_name, "application/json", output);
	for (i = 0; i < json_array_get_length(results); i++)
	{
		JsonNode *element = json_array_get_element(results, i);
		g_autofree gchar *csv = NULL;
		g_autofree gchar *title_slug = NULL;
		g_autofree gchar *csv_name = NULL;
		if (!JSON_NODE_HOLDS_OBJECT(element))
			continue;
		csv = csv_from_result(json_node_get_object(element));
		if (csv == NULL)
			continue;
		title_slug = slug(venture_json_object_get_string(json_node_get_object(element), "title", NULL), "report");
		csv_name = json_array_get_length(results) > 1 ?
			g_strdup_printf("%s-%u.csv", title_slug, i + 1) : g_strconcat(title_slug, ".csv", NULL);
		add_inline_attachment(attachments, csv_name, "text/csv", csv);
	}
	json_builder_end_array(attachments);
	attachment_node = json_builder_get_root(attachments);
	attachment_json = json_to_string(attachment_node, FALSE);

	template = load_template(database, organization_id, error);
	if (template == NULL)
		return NULL;
	when = g_date_time_format(run_at, "%Y-%m-%d %H:%M UTC");
	json_object_set_string_member(values, "name", name != NULL ? name : "");
	json_object_set_string_member(values, "period", period->str);
	json_object_set_string_member(values, "run_at", when);
	json_object_set_string_member(values, "reports", reports->str);
	message = venture_mail_template_render_values(template, values, error);
	if (message == NULL)
		return NULL;
	/* The key ties one message to one delivery clock: the sweep's as-of for
	 * a scheduled run, the moment of the request for a deliberate re-send. */
	stamp = g_date_time_format_iso8601(clock);
	key = g_strdup_printf("report_pack:%s:%s", venture_entity_get_uuid(VENTURE_ENTITY(pack)), stamp);
	g_object_set(message, "to", recipients, "attachments", attachment_json, "idempotency-key", key,
		"related-type", "report_pack", "related-id", venture_entity_get_id(VENTURE_ENTITY(pack)), NULL);
	{
		g_autoptr(GError) unavailable = NULL;
		g_autoptr(VentureMailer) prepared = NULL;
		g_autofree gchar *reason = NULL;
		/* A selector existing is not evidence that this organization has
		 * transport. Preflight the retained report's actual message without
		 * submitting it; the outbox selects again at the first attempt. */
		venture_entity_set_organization_id(VENTURE_ENTITY(message), organization_id);
		prepared = venture_mailer_prepare(mailer, message, &unavailable);
		if (prepared == NULL)
		{
			reason = g_strdup_printf("No mail transport is available: %s",
				unavailable != NULL ? unavailable->message : "configuration is unavailable");
			refuse(error, "mail", reason);
			return NULL;
		}
	}
	return venture_mail_outbox_enqueue(outbox, message, actor, error);
}

VentureMailMessage *
venture_report_pack_service_deliver(VentureReportPackService *self, VentureContext *context,
	VentureReportPack *pack, GDateTime *now, const VentureActor *actor, gboolean *recorded, GError **error)
{
	VentureDatabase *database;
	g_autoptr(GDateTime) clock = NULL;
	g_autoptr(GError) local = NULL;
	g_autoptr(VentureMailMessage) queued = NULL;
	g_return_val_if_fail(VENTURE_IS_REPORT_PACK_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_REPORT_PACK(pack), NULL);
	if (recorded != NULL)
		*recorded = FALSE;
	database = venture_context_get_database(context);
	clock = now != NULL ? g_date_time_to_utc(now) : venture_time_now();
	/* The hand-over and its record commit together: a queued message the
	 * pack does not know about would be re-sent by the next deliberate
	 * deliver, and a record of a message that was never queued would not. */
	if (!venture_database_begin(database, error))
		return NULL;
	queued = enqueue_output(context, pack, clock, actor, &local);
	if (queued != NULL)
	{
		g_autofree gchar *message_id = NULL;
		g_object_get(queued, "message-id", &message_id, NULL);
		g_object_set(pack, "last-delivered-at", clock, "last-delivery-message-id", message_id,
			"last-delivery-mail-id", venture_entity_get_id(VENTURE_ENTITY(queued)), "last-delivery-error", "", NULL);
		if (!venture_database_save(database, VENTURE_ENTITY(pack), actor, error))
		{
			venture_database_rollback(database);
			return NULL;
		}
		if (!venture_database_commit(database, error))
			return NULL;
		if (recorded != NULL)
			*recorded = TRUE;
		return g_steal_pointer(&queued);
	}
	venture_database_rollback(database);
	/* A refusal is recorded on the pack in a transaction of its own, so it
	 * is visible on the record and never disturbs the retained run. */
	g_object_set(pack, "last-delivered-at", NULL, "last-delivery-message-id", "",
		"last-delivery-mail-id", (gint64)0, "last-delivery-error", local->message, NULL);
	if (!venture_database_save(database, VENTURE_ENTITY(pack), actor, error))
		return NULL;
	if (recorded != NULL)
		*recorded = TRUE;
	g_propagate_error(error, g_steal_pointer(&local));
	return NULL;
}
