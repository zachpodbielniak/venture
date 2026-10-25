/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

/* A complete export must not require the operator to discover that a
 * supported vendor has no mapping for one of its ordinary CRM modules. */
static void
test_complete_vendor_maps(void)
{
	static const gchar *const sources[] = { "hubspot", "zoho_crm", "salesforce" };
	static const gchar *const modules[] = {
		"companies", "contacts", "deals", "notes", "emails", "calls",
		"meetings", "tasks", "leads", "products", "price_books",
		"price_book_entries", "quotes", "quote_lines", "sales_orders",
		"sales_order_lines", "invoices", "invoice_lines", "campaigns",
		"cases", "vendors", "users", "attachments", "stage_history", "payments"
	};
	guint s, m;
	for (s = 0; s < G_N_ELEMENTS(sources); s++)
	{
		for (m = 0; m < G_N_ELEMENTS(modules); m++)
		{
			g_autoptr(JsonObject) columns = NULL;
			g_test_message("%s / %s", sources[s], modules[m]);
			columns = venture_crm_import_default_columns(sources[s], modules[m]);
			g_assert_nonnull(columns);
			g_assert_true(json_object_has_member(columns, "id"));
		}
	}
}

static guint
complete_count(VentureDatabase *db, gint64 org, const gchar *name)
{
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	query = venture_query_new(type);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(rows);
	return rows->len;
}

static VentureEntity *
complete_record(VentureDatabase *db, gint64 org, VentureEntity *batch, const gchar *object, const gchar *source_id)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CRM_IMPORT_ROW);
	g_autoptr(VentureEntity) row = NULL;
	g_autofree gchar *type = NULL;
	g_autoptr(GError) error = NULL;
	gint64 id = 0;
	VentureEntity *record;
	venture_query_set_organization(query, org);
	g_assert_true(venture_query_add_filter_int(query, "import-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(batch), &error));
	g_assert_true(venture_query_add_filter_string(query, "source-object", VENTURE_FILTER_OP_EQ, object, &error));
	g_assert_true(venture_query_add_filter_string(query, "source-id", VENTURE_FILTER_OP_EQ, source_id, &error));
	row = venture_database_find_one(db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(row);
	g_object_get(row, "record-type", &type, "record-id", &id, NULL);
	g_assert_cmpint(id, >, 0);
	record = venture_database_get(db, venture_entity_registry_lookup(venture_entity_registry_get_default(), type), id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(record);
	return record;
}

static void
complete_assert_values(VentureDatabase *db, gint64 org, VentureEntity *batch)
{
	g_autoptr(VentureEntity) company = complete_record(db, org, batch, "company", "company-1");
	g_autoptr(VentureEntity) contact = complete_record(db, org, batch, "contact", "contact-1");
	g_autoptr(VentureEntity) attachment = complete_record(db, org, batch, "attachment", "attachment-1");
	g_autoptr(VentureEntity) history = complete_record(db, org, batch, "stage_history", "stage-event-1");
	g_autoptr(VentureEntity) line = complete_record(db, org, batch, "invoice_line", "invoice-line-1");
	g_autoptr(VentureMoney) price = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *owner = NULL, *encoded = NULL, *stamp = NULL;
	g_autofree guchar *bytes = NULL;
	gint64 company_id = 0;
	gsize length;
	g_object_get(contact, "company-id", &company_id, NULL);
	g_assert_cmpint(company_id, ==, venture_entity_get_id(company));
	g_assert_cmpstr(venture_entity_get_attribute(company, "legacy_label"), ==, "synthetic-companies");
	g_assert_cmpstr(venture_entity_get_attribute(contact, "legacy_label"), ==, "synthetic-contacts");
	g_object_get(history, "entered-at", &when, "by", &owner, NULL);
	stamp = g_date_time_format(when, "%Y-%m-%dT%H:%M:%SZ");
	g_assert_cmpstr(stamp, ==, "2025-04-03T14:15:16Z");
	g_assert_cmpstr(owner, ==, "synthetic-operator");
	g_object_get(attachment, "content-base64", &encoded, NULL);
	bytes = g_base64_decode(encoded, &length);
	g_assert_cmpmem(bytes, length, "Synthetic attachment for issue 210.\n", 36);
	g_assert_cmpuint(complete_count(db, org, "record_link"), ==, 3);
	g_object_get(line, "unit-price", &price, NULL);
	g_assert_cmpint(venture_money_get_amount(price), ==, 12550);
	g_assert_cmpuint(complete_count(db, org, "mail_message"), ==, 0);
	g_assert_cmpuint(complete_count(db, org, "text_message"), ==, 0);
	{
		g_autoptr(VentureEntity) order = complete_record(db, org, batch, "sales_order", "order-1");
		g_autoptr(GError) error = NULL;
		VentureSalesOrderService *service = venture_sales_order_service_get(db);
		g_assert_false(venture_sales_order_service_set_draft_deleted(service, order, TRUE, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
		g_assert_false(venture_sales_order_service_set_draft_deleted(service, company, TRUE, NULL, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_cmpuint(complete_count(db, org, "sales_order"), ==, 1);
	}

}

static void
complete_audit(VentureDatabase *database, VentureAuditEntry *entry, gpointer data)
{
	guint *events = data;
	(void)database;
	(void)entry;
	(*events)++;
}

static void
test_complete_export(gconstpointer data)
{
	static const gchar *const names[] = {
		"company", "contact", "deal", "lead", "product", "price_list",
		"price_list_item", "quote", "quote_line", "sales_order",
		"sales_order_line", "invoice", "invoice_line", "campaign",
		"ticket", "document", "interaction", "activity"
	};
	static const guint expected[] = { 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 3, 4, 1 };
	const gchar *scenario = data;
	gboolean bad_attachment = g_str_equal(scenario, "bad-attachment");
	gboolean missing_owner = g_str_equal(scenario, "unmapped-owner");
	gboolean custom_login = g_str_equal(scenario, "custom-login");
	gboolean duplicate_id = g_str_equal(scenario, "duplicate-id");
	gboolean historical = g_str_has_prefix(scenario, "historical-");
	gboolean short_payment = g_str_equal(scenario, "short-payment");
	gboolean full_history = g_str_has_prefix(scenario, "history:") || short_payment;
	gboolean invalid = bad_attachment || missing_owner || custom_login || duplicate_id || historical;
	const gchar *source = short_payment ? "hubspot" : (full_history ? scenario + 8 : (invalid ? "hubspot" : scenario));
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureVenture) venture = venture_venture_new();
	g_autoptr(VentureEntity) batch = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonObject) csv = json_object_new();
	g_autoptr(JsonObject) stages = json_object_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
	g_autoptr(VentureEntity) stage = NULL;
	g_autoptr(GList) files = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("venture-crm-complete-XXXXXX", &error);
	g_autofree gchar *folder = g_build_filename("tests", "fixtures", "crm-import", "complete", source, NULL);
	g_autofree gchar *path = g_build_filename(folder, full_history ? "history-manifest.json" : "manifest.json", NULL);
	JsonObject *manifest, *file_map;
	VentureCrmImportService *service;
	GList *l;
	gint64 org;
	guint i, audit_events = 0;
	gboolean ok;
	g_assert_no_error(error);
	g_object_set(config, "state-dir", dir, NULL);
	db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	context = venture_context_new(config, db);
	org = venture_context_get_default_organization_id(context);
	g_assert_cmpint(venture_deal_service_ensure_default(venture_database_get_deal_service(db), org, &error), >, 0);
	g_assert_no_error(error);
	g_object_set(venture, "name", "Synthetic import target", "slug", "synthetic-import-target", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(venture), org);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(venture), NULL, &error));
	g_assert_no_error(error);
	venture_query_set_organization(query, org);
	g_assert_true(venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ, "proposal", &error));
	stage = venture_database_find_one(db, query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(stage);
	g_assert_true(json_parser_load_from_file(parser, path, &error));
	g_assert_no_error(error);
	manifest = json_node_get_object(json_parser_get_root(parser));
	json_object_set_int_member(manifest, "venture_id", venture_entity_get_id(VENTURE_ENTITY(venture)));
	json_object_set_int_member(stages, "proposal", venture_entity_get_id(stage));
	if (full_history) {
		const gchar *closed_stages[] = { "won", "lost" };
		g_autoptr(VentureLossReason) reason = venture_loss_reason_new();
		guint n;
		g_object_set(reason, "name", "Source opportunity lost", "active", TRUE, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(reason), org);
		g_assert_true(venture_database_save(db, VENTURE_ENTITY(reason), NULL, &error)); g_assert_no_error(error);
		json_object_set_int_member(manifest, "loss_reason_id", venture_entity_get_id(VENTURE_ENTITY(reason)));
		for (n = 0; n < G_N_ELEMENTS(closed_stages); n++) {
			g_autoptr(VentureQuery) sq = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
			g_autoptr(VentureEntity) target = NULL;
			venture_query_set_organization(sq, org);
			g_assert_true(venture_query_add_filter_string(sq, "name", VENTURE_FILTER_OP_EQ, closed_stages[n], &error));
			target = venture_database_find_one(db, sq, &error); g_assert_no_error(error); g_assert_nonnull(target);
			json_object_set_int_member(stages, closed_stages[n], venture_entity_get_id(target));
		}
	}
	json_object_set_object_member(manifest, "stage_map", g_steal_pointer(&stages));
	file_map = json_object_get_object_member(manifest, "files");
	files = json_object_get_members(file_map);
	for (l = files; l != NULL; l = l->next)
	{
		g_autofree gchar *file = g_build_filename(folder, json_object_get_string_member(file_map, l->data), NULL);
		g_autofree gchar *text = NULL;
		g_assert_true(g_file_get_contents(file, &text, NULL, &error));
		g_assert_no_error(error);
		json_object_set_string_member(csv, l->data, text);
	}
	json_object_remove_member(manifest, "files");
	json_object_set_object_member(manifest, "csv", g_steal_pointer(&csv));
	if (bad_attachment)
	{
		JsonObject *tables = json_object_get_object_member(manifest, "csv");
		const gchar *old = json_object_get_string_member(tables, "attachments");
		g_auto(GStrv) parts = g_strsplit(old, "U3ludGhldGljIGF0dGFjaG1lbnQgZm9yIGlzc3VlIDIxMC4K", 2);
		g_autofree gchar *bad = g_strjoinv("!!!!", parts);
		json_object_set_string_member(tables, "attachments", bad);
	}
	if (historical)
	{
		JsonObject *tables = json_object_get_object_member(manifest, "csv");
		const gchar *module = scenario + strlen("historical-");
		const gchar *old = json_object_get_string_member(tables, module);
		g_auto(GStrv) parts = g_strsplit(old, ",draft,", 2);
		g_autofree gchar *changed = g_strjoinv(g_str_equal(module, "invoices") ? ",sent," : (g_str_equal(module, "quotes") ? ",accepted," : ",fulfilled,"), parts);
		json_object_set_string_member(tables, module, changed);
	}
	if (short_payment) {
		JsonObject *tables = json_object_get_object_member(manifest, "csv");
		g_auto(GStrv) parts = g_strsplit(json_object_get_string_member(tables, "payments"), "125.50", 2);
		g_autofree gchar *changed = g_strjoinv("100.00", parts);
		json_object_set_string_member(tables, "payments", changed);
	}
	if (missing_owner)
		json_object_remove_member(json_object_get_object_member(manifest, "owner_map"), "owner-1");
	if (custom_login)
		json_object_set_string_member(json_object_get_object_member(json_object_get_object_member(manifest, "custom_objects"), "installations"), "target", "users");
	if (duplicate_id)
	{
		JsonObject *tables = json_object_get_object_member(manifest, "csv");
		const gchar *old = json_object_get_string_member(tables, "contacts");
		const gchar *row = strchr(old, '\n');
		g_autofree gchar *duplicate = g_strconcat(old, row + 1, NULL);
		json_object_set_string_member(tables, "contacts", duplicate);
	}
	g_signal_connect(db, "audit", G_CALLBACK(complete_audit), &audit_events);
	service = venture_crm_import_service_get(db);
	batch = venture_crm_import_service_preview(service, org, manifest, NULL, &error);
	if (custom_login || duplicate_id)
	{
		g_assert_null(batch);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_cmpuint(complete_count(db, org, "crm_import"), ==, 0);
		goto cleanup;
	}
	g_assert_no_error(error);
	g_assert_nonnull(batch);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(complete_count(db, org, names[i]), ==, 0);
	ok = venture_crm_import_service_import(service, VENTURE_CRM_IMPORT(batch), NULL, &error);
	if (bad_attachment || missing_owner || short_payment)
	{
		g_autofree gchar *state = NULL;
		g_assert_false(ok);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		for (i = 0; i < G_N_ELEMENTS(names); i++)
			g_assert_cmpuint(complete_count(db, org, names[i]), ==, 0);
		g_assert_cmpuint(complete_count(db, org, "accounting_custom_field"), ==, 0);
		g_assert_cmpuint(complete_count(db, org, "record_link"), ==, 0);
		if (short_payment) {
			g_assert_cmpuint(complete_count(db, org, "payment"), ==, 0);
			g_assert_cmpuint(complete_count(db, org, "invoice_event"), ==, 0);
			g_assert_cmpuint(complete_count(db, org, "sales_credit"), ==, 0);
		}

		g_object_get(batch, "state", &state, NULL);
		g_assert_cmpstr(state, ==, "preview");
		goto cleanup;
	}
	g_assert_no_error(error);
	g_assert_true(ok);
	if (historical || full_history) {
		g_autoptr(VentureEntity) invoice = complete_record(db, org, batch, "invoice", "invoice-1");
		g_autoptr(VentureMoney) balance = NULL;
		g_autoptr(GDateTime) cutoff = g_date_time_new_from_iso8601("2025-04-04T00:00:00Z", NULL);
		gint status;
		ok = venture_crm_import_service_import(service, VENTURE_CRM_IMPORT(batch), NULL, &error);
		g_assert_no_error(error); g_assert_true(ok);
		g_object_get(invoice, "status", &status, NULL);
		if (full_history || g_str_equal(scenario, "historical-invoices")) {
			balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(db), venture_entity_get_id(invoice), cutoff, &error);
			g_assert_no_error(error); g_assert_nonnull(balance);
			g_assert_cmpint(venture_money_get_amount(balance), ==, 12550);
		}
		if (full_history) {
			g_autoptr(VentureEntity) won = complete_record(db, org, batch, "deal", "deal-won");
			g_autoptr(GDateTime) closed = NULL;
			g_autofree gchar *closed_date = NULL;
			g_autoptr(VentureEntity) quote = complete_record(db, org, batch, "quote", "quote-1");
			g_autoptr(VentureEntity) order = complete_record(db, org, batch, "sales_order", "order-1");
			g_autoptr(VentureEntity) matched = NULL;
			g_autofree gchar *order_status = NULL;
			gint quote_status;
			g_object_get(won, "closed-at", &closed, NULL); g_assert_nonnull(closed);
			closed_date = g_date_time_format(closed, "%Y-%m-%d"); g_assert_cmpstr(closed_date, ==, "2025-04-05");
			g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_PAID);
			g_clear_pointer(&balance, venture_money_free);
			balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(db), venture_entity_get_id(invoice), NULL, &error);
			g_assert_no_error(error); g_assert_cmpint(venture_money_get_amount(balance), ==, 0);
			g_object_get(quote, "status", &quote_status, NULL); g_assert_cmpint(quote_status, ==, VENTURE_QUOTE_ACCEPTED);
			g_object_get(order, "status", &order_status, NULL); g_assert_cmpstr(order_status, ==, "fulfilled");
			g_assert_cmpuint(complete_count(db, org, "fulfillment"), ==, 1);
			g_assert_cmpuint(complete_count(db, org, "payment"), ==, 1);
			g_assert_cmpuint(complete_count(db, org, "mail_message"), ==, 0);
			g_assert_cmpuint(complete_count(db, org, "text_message"), ==, 0);
			/* Different vendor ids force document-number matching, as when
			 * the same documents were already loaded through accounting. */
			{
				JsonObject *tables = json_object_get_object_member(manifest, "csv");
				g_autoptr(GList) members = json_object_get_members(tables);
				GList *member;
				for (member = members; member != NULL; member = member->next) {
					const gchar *name = member->data;
					if (g_strcmp0(name, "companies") && g_strcmp0(name, "contacts") && g_strcmp0(name, "invoices") &&
						g_strcmp0(name, "invoice_lines") && g_strcmp0(name, "payments")) json_object_remove_member(tables, name);
				}
				json_object_remove_member(manifest, "custom_objects");
				json_object_remove_member(manifest, "custom_fields");
				json_object_set_string_member(json_object_get_object_member(json_object_get_object_member(manifest, "columns"), "invoice_lines"), "product_id", "No catalog column in accounting export");
			}
			json_object_set_string_member(manifest, "source", !g_strcmp0(source, "hubspot") ? "zoho_crm" : "hubspot");
			matched = venture_crm_import_service_preview(service, org, manifest, NULL, &error);
			g_assert_no_error(error); g_assert_nonnull(matched);
			ok = venture_crm_import_service_import(service, VENTURE_CRM_IMPORT(matched), NULL, &error);
			g_assert_no_error(error); g_assert_true(ok);
			g_assert_no_error(error);
			g_assert_cmpuint(complete_count(db, org, "invoice"), ==, 1);
			g_assert_cmpuint(complete_count(db, org, "payment"), ==, 1);
			ok = venture_crm_import_service_rollback(service, VENTURE_CRM_IMPORT(matched), NULL, &error);
			g_assert_no_error(error); g_assert_true(ok);
			g_clear_pointer(&balance, venture_money_free);
			balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(db), venture_entity_get_id(invoice), NULL, &error);
			g_assert_no_error(error); g_assert_cmpint(venture_money_get_amount(balance), ==, 0);
			ok = venture_crm_import_service_rollback(service, VENTURE_CRM_IMPORT(batch), NULL, &error);
			g_assert_no_error(error); g_assert_true(ok);
			g_clear_object(&invoice);
			invoice = complete_record(db, org, batch, "invoice", "invoice-1");
			g_object_get(invoice, "status", &status, NULL); g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_VOID);
			g_clear_pointer(&balance, venture_money_free);
			balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(db), venture_entity_get_id(invoice), cutoff, &error);
			g_assert_no_error(error); g_assert_cmpint(venture_money_get_amount(balance), ==, 12550);
			g_clear_pointer(&balance, venture_money_free);
			balance = venture_settlement_service_invoice_balance(venture_settlement_service_get(db), venture_entity_get_id(invoice), NULL, &error);
			g_assert_no_error(error); g_assert_cmpint(venture_money_get_amount(balance), ==, 0);
			g_assert_cmpuint(complete_count(db, org, "refund"), ==, 1);
			g_assert_cmpuint(complete_count(db, org, "document"), ==, 0);
			g_assert_cmpuint(complete_count(db, org, "record_link"), ==, 0);
		}
		goto cleanup;
	}
	complete_assert_values(db, org, batch);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		g_test_message("%s: %s", source, names[i]);
		g_assert_cmpuint(complete_count(db, org, names[i]), ==, expected[i]);
	}
	ok = venture_crm_import_service_import(service, VENTURE_CRM_IMPORT(batch), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
	complete_assert_values(db, org, batch);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(complete_count(db, org, names[i]), ==, expected[i]);
	{
		g_autoptr(VentureEntity) matched = venture_crm_import_service_preview(service, org, manifest, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(matched);
		ok = venture_crm_import_service_import(service, VENTURE_CRM_IMPORT(matched), NULL, &error);
			g_assert_no_error(error); g_assert_true(ok);
		g_assert_no_error(error);
		for (i = 0; i < G_N_ELEMENTS(names); i++)
			g_assert_cmpuint(complete_count(db, org, names[i]), ==, expected[i]);
		g_assert_true(venture_crm_import_service_rollback(service, VENTURE_CRM_IMPORT(matched), NULL, &error));
		g_assert_no_error(error);
		for (i = 0; i < G_N_ELEMENTS(names); i++)
			g_assert_cmpuint(complete_count(db, org, names[i]), ==, expected[i]);
	}
	ok = venture_crm_import_service_rollback(service, VENTURE_CRM_IMPORT(batch), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(complete_count(db, org, names[i]), ==, 0);
	{
		g_autoptr(VentureEntity) restored = venture_crm_import_service_preview(service, org, manifest, NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(restored);
		g_assert_true(venture_crm_import_service_import(service, VENTURE_CRM_IMPORT(restored), NULL, &error));
		g_assert_no_error(error);
		for (i = 0; i < G_N_ELEMENTS(names); i++)
			g_assert_cmpuint(complete_count(db, org, names[i]), ==, expected[i]);
		g_assert_true(venture_crm_import_service_rollback(service, VENTURE_CRM_IMPORT(restored), NULL, &error));
		g_assert_no_error(error);
		for (i = 0; i < G_N_ELEMENTS(names); i++)
			g_assert_cmpuint(complete_count(db, org, names[i]), ==, 0);
	}
	g_assert_cmpuint(complete_count(db, org, "record_link"), ==, 0);
	g_assert_cmpuint(complete_count(db, org, "accounting_custom_field"), ==, 0);
	/* Only the two retained original stage-history entries keep index rows. */
	g_assert_cmpuint(complete_count(db, org, "custom_field_value"), ==, 2);
cleanup:
	g_assert_cmpuint(audit_events, ==, 0);
	g_clear_error(&error);
	{
		g_autoptr(VentureCompany) ordinary = venture_company_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(ordinary), org);
		g_object_set(ordinary, "name", "Ordinary post-import write", NULL);
		g_assert_true(venture_database_save(db, VENTURE_ENTITY(ordinary), NULL, &error));
		g_assert_no_error(error);
		g_assert_cmpuint(audit_events, >, 0);
	}
	g_signal_handlers_disconnect_by_data(db, &audit_events);
	g_clear_object(&context);
	g_clear_object(&db);
	venture_test_remove_tree(dir);
}

static void
test_accounting_match(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("venture-crm-accounting-XXXXXX", &error);
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(JsonParser) books_parser = json_parser_new(), crm_parser = json_parser_new();
	g_autoptr(VentureEntity) cutover = NULL, batch = NULL, invoice = NULL;
	g_autofree gchar *number = NULL, *report = NULL;
	gint64 org;
	gboolean ok;
	gint status;
	const gchar *books = "{\"source\":\"zoho_books\",\"cutoff\":\"2025-05-01\","
		"\"customers\":[{\"source_id\":\"account-210\",\"name\":\"Synthetic Accounting Customer\"}],"
		"\"open_ar\":[{\"source_id\":\"books-invoice-210\",\"customer_source_id\":\"account-210\","
		"\"number\":\"BOOKS-210\",\"amount\":\"125.50 USD\",\"paid\":\"50 USD\",\"date\":\"2025-04-03\"}]}";
	const gchar *crm = "{\"source\": \"zoho_crm\", \"csv\": {\"companies\": \"id,name\\ncrm-company-210,Synthetic Accounting Customer\\n\", \"invoices\": \"id,number,accounting_source_id,company_id,status\\ncrm-invoice-210,CRM-210,books-invoice-210,crm-company-210,partially_paid\\n\", \"payments\": \"id,invoice_id,date,amount,currency,method\\nreceipt-210,crm-invoice-210,2025-04-05T00:00:00Z,50.00,USD,bank_transfer\\n\"}, \"columns\": {\"companies\": {\"id\": \"id\", \"name\": \"name\"}, \"invoices\": {\"id\": \"id\", \"number\": \"number\", \"accounting_source_id\": \"accounting_source_id\", \"company_id\": \"company_id\", \"status\": \"status\"}, \"payments\": {\"id\": \"id\", \"invoice_id\": \"invoice_id\", \"date\": \"date\", \"amount\": \"amount\", \"currency\": \"currency\", \"method\": \"method\"}}}";
	g_assert_no_error(error);
	g_object_set(config, "state-dir", dir, NULL);
	db = venture_database_new("sqlite://:memory:", &error); g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	context = venture_context_new(config, db); org = venture_context_get_default_organization_id(context);
	g_assert_true(json_parser_load_from_data(books_parser, books, -1, &error)); g_assert_no_error(error);
	cutover = venture_cutover_service_preview(venture_cutover_service_get(db), org,
		json_node_get_object(json_parser_get_root(books_parser)), NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(cutover);
	ok = venture_cutover_service_import(venture_cutover_service_get(db), VENTURE_ACCOUNTING_CUTOVER(cutover), NULL, &error);
	g_assert_no_error(error); g_assert_true(ok);
	g_assert_cmpuint(complete_count(db, org, "invoice"), ==, 1);
	g_assert_true(json_parser_load_from_data(crm_parser, crm, -1, &error)); g_assert_no_error(error);
	batch = venture_crm_import_service_preview(venture_crm_import_service_get(db), org,
		json_node_get_object(json_parser_get_root(crm_parser)), NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(batch);
	ok = venture_crm_import_service_import(venture_crm_import_service_get(db), VENTURE_CRM_IMPORT(batch), NULL, &error);
	g_assert_no_error(error); g_assert_true(ok);
	g_assert_cmpuint(complete_count(db, org, "invoice"), ==, 1);
	invoice = complete_record(db, org, batch, "invoice", "crm-invoice-210");
	g_object_get(invoice, "number", &number, "status", &status, NULL);
	g_assert_cmpstr(number, ==, "BOOKS-210"); g_assert_cmpint(status, ==, VENTURE_INVOICE_STATUS_PARTIALLY_PAID);
	g_assert_cmpuint(complete_count(db, org, "payment"), ==, 1);
	g_object_get(batch, "report", &report, NULL);
	g_assert_nonnull(strstr(report, "invoice crm-invoice-210: matched"));
	g_assert_true(venture_crm_import_service_rollback(venture_crm_import_service_get(db), VENTURE_CRM_IMPORT(batch), NULL, &error));
	g_assert_no_error(error); g_assert_cmpuint(complete_count(db, org, "invoice"), ==, 1);
	g_clear_object(&context); g_clear_object(&db); venture_test_remove_tree(dir);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/crm-import-complete/vendor-maps", test_complete_vendor_maps);
	g_test_add_data_func("/crm-import-complete/hubspot", "hubspot", test_complete_export);
	g_test_add_data_func("/crm-import-complete/zoho", "zoho_crm", test_complete_export);
	g_test_add_data_func("/crm-import-complete/salesforce", "salesforce", test_complete_export);
	g_test_add_data_func("/crm-import-complete/bad-attachment-atomic", "bad-attachment", test_complete_export);
	g_test_add_data_func("/crm-import-complete/unmapped-owner-atomic", "unmapped-owner", test_complete_export);
	g_test_add_data_func("/crm-import-complete/custom-login-refused", "custom-login", test_complete_export);
	g_test_add_data_func("/crm-import-complete/duplicate-id-refused", "duplicate-id", test_complete_export);
	g_test_add_data_func("/crm-import-complete/historical-invoices", "historical-invoices", test_complete_export);
	g_test_add_data_func("/crm-import-complete/historical-quotes", "historical-quotes", test_complete_export);
	g_test_add_data_func("/crm-import-complete/historical-orders", "historical-sales_orders", test_complete_export);
	g_test_add_data_func("/crm-import-complete/history-hubspot", "history:hubspot", test_complete_export);
	g_test_add_data_func("/crm-import-complete/history-zoho", "history:zoho_crm", test_complete_export);
	g_test_add_data_func("/crm-import-complete/history-salesforce", "history:salesforce", test_complete_export);
	g_test_add_func("/crm-import-complete/zoho-books-match", test_accounting_match);
	g_test_add_data_func("/crm-import-complete/short-payment-atomic", "short-payment", test_complete_export);
	return g_test_run();
}
