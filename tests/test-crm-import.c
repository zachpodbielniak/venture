/*
 * test-crm-import.c - HubSpot / Zoho CRM / Salesforce migration importer.
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
	gint64 loss_reason;
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureLossReason) reason = venture_loss_reason_new();
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	g_assert_cmpint(venture_deal_service_ensure_default(venture_database_get_deal_service(f->db), f->org, &error), >, 0);
	g_assert_no_error(error);
	g_object_set(reason, "name", "No budget", "active", TRUE, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(reason), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(reason), NULL, &error));
	g_assert_no_error(error);
	f->loss_reason = venture_entity_get_id(VENTURE_ENTITY(reason));
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
actor_init(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_USER;
	actor->name = "migrator";
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

static gchar *
fixture_text(const gchar *name)
{
	g_autofree gchar *path = g_build_filename("tests", "fixtures", "crm-import", name, NULL);
	gchar *text = NULL;
	g_autoptr(GError) error = NULL;
	g_assert_true(g_file_get_contents(path, &text, NULL, &error));
	g_assert_no_error(error);
	return text;
}

static JsonObject *
parse_json(const gchar *json)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	g_assert_true(json_parser_load_from_data(parser, json, -1, &error));
	g_assert_no_error(error);
	return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

static gint64
stage_id(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
	g_autoptr(VentureEntity) stage = NULL;
	venture_query_set_organization(query, f->org);
	g_assert_true(venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ, name, NULL));
	stage = venture_database_find_one(f->db, query, NULL);
	g_assert_nonnull(stage);
	return venture_entity_get_id(stage);
}

/* A manifest inlines the fixture CSVs under "csv", the way venturectl
 * does after reading the files the manifest names. */
static JsonObject *
manifest(Fixture *f, const gchar *source, const gchar *const *files, const gchar *stage_map, const gchar *extra)
{
	g_autoptr(GString) json = g_string_new(NULL);
	g_autoptr(JsonObject) csv = json_object_new();
	g_autoptr(JsonNode) csv_node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *csv_text = NULL;
	g_autofree gchar *map = NULL;
	guint i;
	for (i = 0; files[i] != NULL; i += 2)
	{
		g_autofree gchar *text = fixture_text(files[i + 1]);
		json_object_set_string_member(csv, files[i], text);
	}
	json_node_set_object(csv_node, csv);
	csv_text = venture_json_to_string(csv_node, FALSE);
	if (stage_map == NULL)
		map = g_strdup_printf("{\"presentationscheduled\":%" G_GINT64_FORMAT ",\"closedwon\":%" G_GINT64_FORMAT
			",\"closedlost\":%" G_GINT64_FORMAT "}", stage_id(f, "proposal"), stage_id(f, "won"), stage_id(f, "lost"));
	else
		map = g_strdup(stage_map);
	g_string_append_printf(json, "{\"source\":\"%s\",\"csv\":%s,\"stage_map\":%s,\"loss_reason_id\":%" G_GINT64_FORMAT
		",\"owner_map\":{\"Jane Doe\":\"jane\",\"Sam Lee\":\"sam\"}%s%s}",
		source, csv_text, map, f->loss_reason, extra != NULL ? "," : "", extra != NULL ? extra : "");
	return parse_json(json->str);
}

static const gchar *const hubspot_files[] = {
	"companies", "hubspot-companies.csv", "contacts", "hubspot-contacts.csv", "deals", "hubspot-deals.csv",
	"notes", "hubspot-notes.csv", "tasks", "hubspot-tasks.csv", NULL
};

static GPtrArray *
live(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	GPtrArray *rows;
	venture_query_set_organization(query, f->org);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, NULL);
	g_assert_nonnull(rows);
	return rows;
}

static guint
count(Fixture *f, GType type)
{
	g_autoptr(GPtrArray) rows = live(f, type);
	return rows->len;
}

static VentureEntity *
row_for(Fixture *f, VentureEntity *batch, const gchar *object, const gchar *source_id)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CRM_IMPORT_ROW);
	VentureEntity *row;
	venture_query_set_organization(query, f->org);
	g_assert_true(venture_query_add_filter_int(query, "import-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(batch), NULL));
	g_assert_true(venture_query_add_filter_string(query, "source-object", VENTURE_FILTER_OP_EQ, object, NULL));
	g_assert_true(venture_query_add_filter_string(query, "source-id", VENTURE_FILTER_OP_EQ, source_id, NULL));
	row = venture_database_find_one(f->db, query, NULL);
	g_assert_nonnull(row);
	return row;
}

static VentureEntity *
record_of(Fixture *f, VentureEntity *batch, const gchar *object, const gchar *source_id, GType type)
{
	g_autoptr(VentureEntity) row = row_for(f, batch, object, source_id);
	gint64 id = 0;
	VentureEntity *record;
	g_object_get(row, "record-id", &id, NULL);
	g_assert_cmpint(id, >, 0);
	record = venture_database_get(f->db, type, id, NULL);
	g_assert_nonnull(record);
	return record;
}

static gchar *
row_status(Fixture *f, VentureEntity *batch, const gchar *object, const gchar *source_id, gchar **exception)
{
	g_autoptr(VentureEntity) row = row_for(f, batch, object, source_id);
	gchar *status = NULL;
	gchar *message = NULL;
	g_object_get(row, "status", &status, "exception", &message, NULL);
	if (exception != NULL)
		*exception = message;
	else
		g_free(message);
	return status;
}

static VentureEntity *
preview(Fixture *f, JsonObject *payload)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *batch;
	VentureActor actor;
	actor_init(&actor);
	batch = venture_crm_import_service_preview(venture_crm_import_service_get(f->db), f->org, payload, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(batch);
	return batch;
}

static void
import(Fixture *f, VentureEntity *batch)
{
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	actor_init(&actor);
	g_assert_true(venture_crm_import_service_import(venture_crm_import_service_get(f->db),
		VENTURE_CRM_IMPORT(batch), &actor, &error));
	g_assert_no_error(error);
}

static gchar *
string_of(VentureEntity *entity, const gchar *name)
{
	gchar *value = NULL;
	g_object_get(entity, name, &value, NULL);
	return value;
}

static gint64
int_of(VentureEntity *entity, const gchar *name)
{
	gint64 value = 0;
	g_object_get(entity, name, &value, NULL);
	return value;
}

/* DONE WHEN 1: preview -> import -> activate mirrors cutover, rows carry
 * source object, source id, record type, record id and status, and a rerun
 * of the same batch creates nothing twice. What breaks if this regresses:
 * a migration run twice doubles every customer. */
static void
test_lifecycle_idempotent(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = manifest(f, "hubspot", hubspot_files, NULL, NULL);
	g_autoptr(VentureEntity) batch = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *report = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *type = NULL;
	VentureActor actor;
	guint companies, contacts, deals, interactions, activities;
	(void)data;
	actor_init(&actor);
	batch = preview(f, payload);
	g_object_get(batch, "state", &state, "report", &report, NULL);
	g_assert_cmpstr(state, ==, "preview");
	g_assert_nonnull(strstr(report, "attachments"));
	rows = live(f, VENTURE_TYPE_CRM_IMPORT_ROW);
	g_assert_cmpuint(rows->len, ==, 3 + 4 + 3 + 4 + 3);
	g_clear_pointer(&status, g_free);
	status = row_status(f, batch, "deal", "301", NULL);
	g_assert_cmpstr(status, ==, "preview");
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, 0);

	import(f, batch);
	g_clear_pointer(&state, g_free);
	g_object_get(batch, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "imported");
	companies = count(f, VENTURE_TYPE_COMPANY);
	contacts = count(f, VENTURE_TYPE_CONTACT);
	deals = count(f, VENTURE_TYPE_DEAL);
	interactions = count(f, VENTURE_TYPE_INTERACTION);
	activities = count(f, VENTURE_TYPE_ACTIVITY);
	g_assert_cmpuint(companies, ==, 3);
	g_assert_cmpuint(contacts, ==, 4);
	g_assert_cmpuint(deals, ==, 3);
	/* Four notes plus the completed task become history. */
	g_assert_cmpuint(interactions, ==, 5);
	/* Two open tasks become next actions. */
	g_assert_cmpuint(activities, ==, 2);
	g_clear_pointer(&status, g_free);
	status = row_status(f, batch, "company", "101", NULL);
	g_assert_cmpstr(status, ==, "imported");
	{
		g_autoptr(VentureEntity) row = row_for(f, batch, "company", "101");
		type = string_of(row, "record-type");
		g_assert_cmpstr(type, ==, "company");
		g_assert_cmpint(int_of(row, "record-id"), >, 0);
	}

	/* Rerun on the same batch and a second batch of the same export. */
	import(f, batch);
	{
		g_autoptr(JsonObject) again = manifest(f, "hubspot", hubspot_files, NULL, NULL);
		g_autoptr(VentureEntity) second = preview(f, again);
		import(f, second);
		g_clear_pointer(&status, g_free);
		status = row_status(f, second, "contact", "202", NULL);
		g_assert_cmpstr(status, ==, "matched");
	}
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, companies);
	g_assert_cmpuint(count(f, VENTURE_TYPE_CONTACT), ==, contacts);
	g_assert_cmpuint(count(f, VENTURE_TYPE_DEAL), ==, deals);
	g_assert_cmpuint(count(f, VENTURE_TYPE_INTERACTION), ==, interactions);
	g_assert_cmpuint(count(f, VENTURE_TYPE_ACTIVITY), ==, activities);

	g_assert_true(venture_crm_import_service_activate(venture_crm_import_service_get(f->db),
		VENTURE_CRM_IMPORT(batch), &actor, &error));
	g_assert_no_error(error);
	g_clear_pointer(&state, g_free);
	g_object_get(batch, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "active");
	g_assert_false(venture_crm_import_service_rollback(venture_crm_import_service_get(f->db),
		VENTURE_CRM_IMPORT(batch), &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, companies);
}

/* DONE WHEN 2: dedupe by normalised email and domain against records that
 * already exist, resolve contact -> company by source id, and record a
 * conflicting match as an exception without overwriting. What breaks if
 * this regresses: the migration silently rewrites a customer's phone
 * number from a stale export. */
static void
test_dedupe_and_links(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureCompany) acme = venture_company_new();
	g_autoptr(VentureContact) alice = venture_contact_new();
	g_autoptr(VentureContact) bob = venture_contact_new();
	g_autoptr(JsonObject) payload = NULL;
	g_autoptr(VentureEntity) batch = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *exception = NULL;
	(void)data;
	g_object_set(acme, "name", "Acme Corp", "website", "http://WWW.acme.com/", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(acme), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(acme), NULL, &error));
	g_object_set(alice, "name", "Alice Adams", "email", "alice.adams@ACME.com", "phone", "+1 555 0101",
		"company-id", venture_entity_get_id(VENTURE_ENTITY(acme)), NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(alice), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(alice), NULL, &error));
	g_object_set(bob, "name", "Robert Brown", "email", "bob@globex.example", "phone", "+1 555 9999", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(bob), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(bob), NULL, &error));
	g_assert_no_error(error);

	payload = manifest(f, "hubspot", hubspot_files, NULL, NULL);
	batch = preview(f, payload);
	import(f, batch);

	/* Acme matched by domain, Alice by email with the +tag and case
	 * removed: neither is duplicated. */
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, 3);
	g_assert_cmpuint(count(f, VENTURE_TYPE_CONTACT), ==, 4);
	status = row_status(f, batch, "company", "101", &exception);
	g_assert_cmpstr(status, ==, "matched");
	{
		g_autoptr(VentureEntity) matched = record_of(f, batch, "company", "101", VENTURE_TYPE_COMPANY);
		g_autofree gchar *name = string_of(matched, "name");
		g_assert_cmpint(venture_entity_get_id(matched), ==, venture_entity_get_id(VENTURE_ENTITY(acme)));
		g_assert_cmpstr(name, ==, "Acme Corp");
	}
	g_clear_pointer(&status, g_free);
	g_clear_pointer(&exception, g_free);
	status = row_status(f, batch, "contact", "201", &exception);
	g_assert_cmpstr(status, ==, "matched");

	/* Bob's phone differs: exception on the row, existing untouched, the
	 * source still resolves to him so his deal and note attach. */
	g_clear_pointer(&status, g_free);
	g_clear_pointer(&exception, g_free);
	status = row_status(f, batch, "contact", "202", &exception);
	g_assert_cmpstr(status, ==, "exception");
	g_assert_nonnull(exception);
	g_assert_nonnull(strstr(exception, "phone"));
	{
		g_autoptr(VentureEntity) kept = venture_database_get(f->db, VENTURE_TYPE_CONTACT,
			venture_entity_get_id(VENTURE_ENTITY(bob)), NULL);
		g_autofree gchar *phone = string_of(kept, "phone");
		g_autofree gchar *name = string_of(kept, "name");
		g_assert_cmpstr(phone, ==, "+1 555 9999");
		g_assert_cmpstr(name, ==, "Robert Brown");
	}
	{
		g_autoptr(VentureEntity) deal = record_of(f, batch, "deal", "302", VENTURE_TYPE_DEAL);
		g_assert_cmpint(int_of(deal, "contact-id"), ==, venture_entity_get_id(VENTURE_ENTITY(bob)));
	}

	/* Carol is new and links to Initech through the source company id. */
	{
		g_autoptr(VentureEntity) carol = record_of(f, batch, "contact", "203", VENTURE_TYPE_CONTACT);
		g_autoptr(VentureEntity) initech = record_of(f, batch, "company", "103", VENTURE_TYPE_COMPANY);
		g_autofree gchar *source = string_of(carol, "source");
		g_assert_cmpint(int_of(carol, "company-id"), ==, venture_entity_get_id(initech));
		g_assert_cmpstr(source, ==, "hubspot");
	}
	/* Dan names a company that is not in the export. */
	g_clear_pointer(&status, g_free);
	g_clear_pointer(&exception, g_free);
	status = row_status(f, batch, "contact", "204", &exception);
	g_assert_cmpstr(status, ==, "exception");
	g_assert_nonnull(strstr(exception, "999"));
}

/* DONE WHEN 3: deals carry owner, money with currency, the mapped stage,
 * close date and won/lost. What breaks if this regresses: the pipeline
 * report shows every migrated deal as a fresh lead worth nothing. */
static void
test_deals(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = manifest(f, "hubspot", hubspot_files, NULL, NULL);
	g_autoptr(VentureEntity) batch = preview(f, payload);
	g_autoptr(VentureEntity) open_deal = NULL;
	g_autoptr(VentureEntity) won = NULL;
	g_autoptr(VentureEntity) lost = NULL;
	g_autoptr(VentureMoney) value = NULL;
	g_autoptr(GDateTime) expected = NULL;
	g_autoptr(GDateTime) closed = NULL;
	g_autofree gchar *owner = NULL;
	gint stage = -1;
	(void)data;
	import(f, batch);
	open_deal = record_of(f, batch, "deal", "301", VENTURE_TYPE_DEAL);
	g_object_get(open_deal, "value", &value, "owner", &owner, "stage", &stage,
		"expected-close-at", &expected, "closed-at", &closed, NULL);
	g_assert_nonnull(value);
	g_assert_cmpint(venture_money_get_amount(value), ==, 1250000);
	g_assert_cmpstr(venture_money_get_currency(value), ==, "USD");
	g_assert_cmpstr(owner, ==, "jane");
	g_assert_cmpint(int_of(open_deal, "stage-id"), ==, stage_id(f, "proposal"));
	g_assert_cmpint(stage, ==, VENTURE_DEAL_STAGE_PROPOSAL);
	g_assert_nonnull(expected);
	g_assert_cmpint(g_date_time_get_month(expected), ==, 6);
	g_assert_cmpint(g_date_time_get_day_of_month(expected), ==, 30);
	g_assert_null(closed);
	{
		g_autoptr(VentureEntity) company = record_of(f, batch, "company", "101", VENTURE_TYPE_COMPANY);
		g_assert_cmpint(int_of(open_deal, "company-id"), ==, venture_entity_get_id(company));
	}

	won = record_of(f, batch, "deal", "302", VENTURE_TYPE_DEAL);
	g_object_get(won, "stage", &stage, "closed-at", &closed, NULL);
	g_assert_cmpint(stage, ==, VENTURE_DEAL_STAGE_WON);
	g_assert_cmpint(int_of(won, "stage-id"), ==, stage_id(f, "won"));
	g_assert_nonnull(closed);

	lost = record_of(f, batch, "deal", "303", VENTURE_TYPE_DEAL);
	g_clear_pointer(&value, venture_money_free);
	g_object_get(lost, "stage", &stage, "value", &value, NULL);
	g_assert_cmpint(stage, ==, VENTURE_DEAL_STAGE_LOST);
	g_assert_cmpint(int_of(lost, "loss-reason-id"), ==, f->loss_reason);
	g_assert_cmpstr(venture_money_get_currency(value), ==, "EUR");
	g_assert_cmpint(venture_money_get_amount(value), ==, 90050);
}

/* DONE WHEN 3, refusal: an unmapped stage is refused at preview before
 * any write. What breaks if this regresses: half an export lands and the
 * operator has to hand-fix the rest. */
static void
test_unmapped_stage_refused(Fixture *f, gconstpointer data)
{
	g_autofree gchar *map = g_strdup_printf("{\"presentationscheduled\":%" G_GINT64_FORMAT ",\"closedwon\":%" G_GINT64_FORMAT "}",
		stage_id(f, "proposal"), stage_id(f, "won"));
	g_autoptr(JsonObject) payload = manifest(f, "hubspot", hubspot_files, map, NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) batch = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	batch = venture_crm_import_service_preview(venture_crm_import_service_get(f->db), f->org, payload, &actor, &error);
	g_assert_null(batch);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "closedlost"));
	g_assert_cmpuint(count(f, VENTURE_TYPE_CRM_IMPORT), ==, 0);
	g_assert_cmpuint(count(f, VENTURE_TYPE_CRM_IMPORT_ROW), ==, 0);
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, 0);
	g_assert_cmpuint(count(f, VENTURE_TYPE_DEAL), ==, 0);

	/* A map naming a stage id that does not exist is refused too. */
	g_clear_error(&error);
	g_clear_pointer(&map, g_free);
	g_clear_pointer(&payload, json_object_unref);
	map = g_strdup_printf("{\"presentationscheduled\":%" G_GINT64_FORMAT ",\"closedwon\":%" G_GINT64_FORMAT ",\"closedlost\":424242}",
		stage_id(f, "proposal"), stage_id(f, "won"));
	payload = manifest(f, "hubspot", hubspot_files, map, NULL);
	batch = venture_crm_import_service_preview(venture_crm_import_service_get(f->db), f->org, payload, &actor, &error);
	g_assert_null(batch);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "424242"));
	g_assert_cmpuint(count(f, VENTURE_TYPE_CRM_IMPORT), ==, 0);
}

/* A lost stage needs a loss reason, a deal amount needs a currency, and
 * attachments are unsupported: each refused at preview with nothing written. */
static void
test_manifest_refusals(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) batch = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	{
		g_autoptr(JsonObject) payload = manifest(f, "hubspot", hubspot_files, NULL, NULL);
		json_object_remove_member(payload, "loss_reason_id");
		batch = venture_crm_import_service_preview(venture_crm_import_service_get(f->db), f->org, payload, &actor, &error);
		g_assert_null(batch);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_nonnull(strstr(error->message, "loss_reason_id"));
		g_clear_error(&error);
	}
	{
		g_autoptr(JsonObject) payload = manifest(f, "hubspot", hubspot_files, NULL,
			"\"columns\":{\"deals\":{\"currency\":\"Pipeline\"}}");
		batch = venture_crm_import_service_preview(venture_crm_import_service_get(f->db), f->org, payload, &actor, &error);
		g_assert_null(batch);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_nonnull(strstr(error->message, "currency"));
		g_clear_error(&error);
	}
	{
		g_autoptr(JsonObject) payload = manifest(f, "hubspot", hubspot_files, NULL, NULL);
		json_object_set_string_member(json_object_get_object_member(payload, "csv"), "attachments", "Id,File\n1,a.pdf\n");
		batch = venture_crm_import_service_preview(venture_crm_import_service_get(f->db), f->org, payload, &actor, &error);
		g_assert_null(batch);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_nonnull(strstr(error->message, "attachments"));
		g_assert_nonnull(strstr(error->message, "unsupported"));
		g_clear_error(&error);
	}
	{
		/* "All entities" is not a place a migration can land. */
		g_autoptr(JsonObject) payload = manifest(f, "hubspot", hubspot_files, NULL, NULL);
		batch = venture_crm_import_service_preview(venture_crm_import_service_get(f->db), 0, payload, &actor, &error);
		g_assert_null(batch);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_nonnull(strstr(error->message, "organization"));
		g_clear_error(&error);
	}
	{
		g_autoptr(JsonObject) payload = manifest(f, "pipedrive", hubspot_files, NULL, NULL);
		batch = venture_crm_import_service_preview(venture_crm_import_service_get(f->db), f->org, payload, &actor, &error);
		g_assert_null(batch);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_nonnull(strstr(error->message, "hubspot"));
		g_clear_error(&error);
	}
	{
		/* A required column the export does not carry is named. */
		g_autoptr(JsonObject) payload = manifest(f, "hubspot", hubspot_files, NULL,
			"\"columns\":{\"contacts\":{\"id\":\"Contact ID\"}}");
		batch = venture_crm_import_service_preview(venture_crm_import_service_get(f->db), f->org, payload, &actor, &error);
		g_assert_null(batch);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_assert_nonnull(strstr(error->message, "Contact ID"));
		g_clear_error(&error);
	}
	g_assert_cmpuint(count(f, VENTURE_TYPE_CRM_IMPORT), ==, 0);
	g_assert_cmpuint(count(f, VENTURE_TYPE_CRM_IMPORT_ROW), ==, 0);
}

/* DONE WHEN 4: notes, emails, calls and completed tasks become
 * interactions on the right record with the original timestamp and
 * author; open tasks become planned activities. What breaks if this
 * regresses: a contact's timeline starts on migration day. */
static void
test_history_and_tasks(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = manifest(f, "hubspot", hubspot_files, NULL, NULL);
	g_autoptr(VentureEntity) batch = preview(f, payload);
	g_autoptr(VentureEntity) note = NULL;
	g_autoptr(VentureEntity) mail = NULL;
	g_autoptr(VentureEntity) call = NULL;
	g_autoptr(VentureEntity) meeting = NULL;
	g_autoptr(VentureEntity) done = NULL;
	g_autoptr(VentureEntity) task = NULL;
	g_autoptr(VentureEntity) followup = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *subject = NULL;
	g_autofree gchar *owner = NULL;
	gint kind = -1;
	gint status = -1;
	(void)data;
	import(f, batch);

	note = record_of(f, batch, "note", "401", VENTURE_TYPE_INTERACTION);
	g_object_get(note, "kind", &kind, "body", &body, "subject", &subject, "occurred-at", &when, NULL);
	g_assert_cmpint(kind, ==, VENTURE_INTERACTION_KIND_NOTE);
	g_assert_cmpstr(subject, ==, "Kickoff");
	g_assert_nonnull(strstr(body, "\"phase one\""));
	g_assert_nonnull(strstr(body, "Jane Doe"));
	g_assert_cmpint(g_date_time_get_year(when), ==, 2025);
	g_assert_cmpint(g_date_time_get_month(when), ==, 1);
	g_assert_cmpint(g_date_time_get_day_of_month(when), ==, 20);
	g_assert_cmpint(g_date_time_get_hour(when), ==, 14);
	{
		g_autoptr(VentureEntity) alice = record_of(f, batch, "contact", "201", VENTURE_TYPE_CONTACT);
		g_autoptr(VentureEntity) acme = record_of(f, batch, "company", "101", VENTURE_TYPE_COMPANY);
		g_autoptr(VentureEntity) deal = record_of(f, batch, "deal", "301", VENTURE_TYPE_DEAL);
		g_assert_cmpint(int_of(note, "contact-id"), ==, venture_entity_get_id(alice));
		g_assert_cmpint(int_of(note, "company-id"), ==, venture_entity_get_id(acme));
		g_assert_cmpint(int_of(note, "deal-id"), ==, venture_entity_get_id(deal));
	}

	mail = record_of(f, batch, "note", "402", VENTURE_TYPE_INTERACTION);
	g_object_get(mail, "kind", &kind, NULL);
	g_assert_cmpint(kind, ==, VENTURE_INTERACTION_KIND_EMAIL);
	call = record_of(f, batch, "note", "403", VENTURE_TYPE_INTERACTION);
	g_object_get(call, "kind", &kind, NULL);
	g_assert_cmpint(kind, ==, VENTURE_INTERACTION_KIND_CALL);
	/* A company-only meeting attaches without inventing a contact. */
	meeting = record_of(f, batch, "note", "404", VENTURE_TYPE_INTERACTION);
	g_object_get(meeting, "kind", &kind, NULL);
	g_assert_cmpint(kind, ==, VENTURE_INTERACTION_KIND_MEETING);
	g_assert_cmpint(int_of(meeting, "contact-id"), ==, 0);
	g_assert_cmpint(int_of(meeting, "company-id"), >, 0);

	/* The completed call task is history, stamped when it was completed. */
	done = record_of(f, batch, "task", "502", VENTURE_TYPE_INTERACTION);
	g_clear_pointer(&when, g_date_time_unref);
	g_clear_pointer(&subject, g_free);
	g_clear_pointer(&body, g_free);
	g_object_get(done, "kind", &kind, "occurred-at", &when, "subject", &subject, "body", &body, NULL);
	g_assert_cmpint(kind, ==, VENTURE_INTERACTION_KIND_CALL);
	g_assert_cmpstr(subject, ==, "Call Carol");
	g_assert_nonnull(strstr(body, "Sam Lee"));
	g_assert_cmpint(g_date_time_get_day_of_month(when), ==, 19);
	g_assert_cmpint(g_date_time_get_hour(when), ==, 15);

	/* Open tasks are next actions for their owner. */
	task = record_of(f, batch, "task", "501", VENTURE_TYPE_ACTIVITY);
	g_clear_pointer(&when, g_date_time_unref);
	g_object_get(task, "status", &status, "kind", &kind, "owner", &owner, "due-at", &when, NULL);
	g_assert_cmpint(status, ==, VENTURE_ACTIVITY_STATUS_PLANNED);
	g_assert_cmpint(kind, ==, VENTURE_ACTIVITY_KIND_TASK);
	g_assert_cmpstr(owner, ==, "jane");
	g_assert_cmpint(g_date_time_get_month(when), ==, 7);
	g_assert_cmpint(int_of(task, "deal-id"), >, 0);
	g_assert_cmpint(int_of(task, "contact-id"), >, 0);
	followup = record_of(f, batch, "task", "503", VENTURE_TYPE_ACTIVITY);
	g_object_get(followup, "kind", &kind, NULL);
	g_assert_cmpint(kind, ==, VENTURE_ACTIVITY_KIND_EMAIL);
}

/* DONE WHEN 5: rollback before activation removes exactly what the import
 * created and nothing else. What breaks if this regresses: undoing a bad
 * import deletes the customers you already had. */
static void
test_rollback_exact(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureCompany) acme = venture_company_new();
	g_autoptr(VentureContact) alice = venture_contact_new();
	g_autoptr(VentureInteraction) old_note = venture_interaction_new();
	g_autoptr(VentureDeal) old_deal = venture_deal_new();
	g_autoptr(VentureActivity) old_task = venture_activity_new();
	g_autoptr(JsonObject) payload = NULL;
	g_autoptr(VentureEntity) batch = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *status = NULL;
	VentureActor actor;
	gint64 alice_id, acme_id;
	guint i;
	(void)data;
	actor_init(&actor);
	g_object_set(acme, "name", "Acme Corp", "website", "acme.com", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(acme), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(acme), NULL, &error));
	acme_id = venture_entity_get_id(VENTURE_ENTITY(acme));
	g_object_set(alice, "name", "Alice Adams", "email", "alice.adams@acme.com", "phone", "+1 555 0101",
		"company-id", acme_id, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(alice), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(alice), NULL, &error));
	alice_id = venture_entity_get_id(VENTURE_ENTITY(alice));
	g_object_set(old_note, "contact-id", alice_id, "company-id", acme_id, "kind", VENTURE_INTERACTION_KIND_NOTE,
		"subject", "Before migration", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(old_note), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(old_note), NULL, &error));
	g_object_set(old_deal, "name", "Existing deal", "company-id", acme_id, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(old_deal), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(old_deal), NULL, &error));
	g_object_set(old_task, "subject", "Existing task", "company-id", acme_id, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(old_task), f->org);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(old_task), NULL, &error));
	g_assert_no_error(error);

	payload = manifest(f, "hubspot", hubspot_files, NULL, NULL);
	batch = preview(f, payload);
	import(f, batch);
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, 3);
	g_assert_cmpuint(count(f, VENTURE_TYPE_CONTACT), ==, 4);
	g_assert_cmpuint(count(f, VENTURE_TYPE_DEAL), ==, 4);
	g_assert_cmpuint(count(f, VENTURE_TYPE_INTERACTION), ==, 6);
	g_assert_cmpuint(count(f, VENTURE_TYPE_ACTIVITY), ==, 3);

	g_assert_true(venture_crm_import_service_rollback(venture_crm_import_service_get(f->db),
		VENTURE_CRM_IMPORT(batch), &actor, &error));
	g_assert_no_error(error);
	g_object_get(batch, "state", &state, NULL);
	g_assert_cmpstr(state, ==, "rolled_back");
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, 1);
	g_assert_cmpuint(count(f, VENTURE_TYPE_CONTACT), ==, 1);
	g_assert_cmpuint(count(f, VENTURE_TYPE_DEAL), ==, 1);
	g_assert_cmpuint(count(f, VENTURE_TYPE_INTERACTION), ==, 1);
	g_assert_cmpuint(count(f, VENTURE_TYPE_ACTIVITY), ==, 1);
	{
		g_autoptr(VentureEntity) kept = venture_database_get(f->db, VENTURE_TYPE_CONTACT, alice_id, NULL);
		g_autoptr(VentureEntity) kept_company = venture_database_get(f->db, VENTURE_TYPE_COMPANY, acme_id, NULL);
		g_assert_nonnull(kept);
		g_assert_false(venture_entity_is_deleted(kept));
		g_assert_nonnull(kept_company);
		g_assert_false(venture_entity_is_deleted(kept_company));
	}
	/* A removed imported win must not remain credited toward quota. Retain
	 * both immutable booking and reversal, with exactly zero net value. */
	{
		g_autoptr(GPtrArray) credits = live(f, VENTURE_TYPE_SALES_CREDIT);
		gint64 net = 0;
		g_assert_cmpuint(credits->len, ==, 2);
		for (i = 0; i < credits->len; i++)
		{
			g_autoptr(VentureMoney) amount = NULL;
			g_object_get(g_ptr_array_index(credits, i), "value", &amount, NULL);
			g_assert_nonnull(amount);
			net += venture_money_get_amount(amount);
		}
		g_assert_cmpint(net, ==, 0);
	}
	rows = live(f, VENTURE_TYPE_CRM_IMPORT_ROW);
	g_assert_cmpuint(rows->len, ==, 17);
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *row_state = string_of(g_ptr_array_index(rows, i), "status");
		g_assert_cmpstr(row_state, ==, "rolled_back");
	}
	/* Rolling back twice is a no-op, and a rolled-back batch cannot import. */
	g_assert_true(venture_crm_import_service_rollback(venture_crm_import_service_get(f->db),
		VENTURE_CRM_IMPORT(batch), &actor, &error));
	g_assert_no_error(error);
	g_assert_false(venture_crm_import_service_import(venture_crm_import_service_get(f->db),
		VENTURE_CRM_IMPORT(batch), &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	/* A fresh batch after rollback recreates the records: the rolled-back
	 * source ids no longer count as imported. */
	{
		g_autoptr(JsonObject) again = manifest(f, "hubspot", hubspot_files, NULL, NULL);
		g_autoptr(VentureEntity) second = preview(f, again);
		import(f, second);
		status = row_status(f, second, "company", "102", NULL);
		g_assert_cmpstr(status, ==, "imported");
		g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, 3);
	}
}

/* DONE WHEN 6: the built-in column maps for Zoho CRM and Salesforce read
 * their default export headers, and a manifest override replaces a header.
 * What breaks if this regresses: every migration starts with a hand-written
 * column map. */
static void
test_vendor_maps(Fixture *f, gconstpointer data)
{
	static const gchar *const zoho_files[] = {
		"companies", "zoho-accounts.csv", "contacts", "zoho-contacts.csv", "deals", "zoho-deals.csv",
		"notes", "zoho-notes.csv", "tasks", "zoho-tasks.csv", NULL
	};
	static const gchar *const salesforce_files[] = {
		"companies", "salesforce-accounts.csv", "contacts", "salesforce-contacts.csv",
		"deals", "salesforce-opportunities.csv", "notes", "salesforce-notes.csv", "tasks", "salesforce-tasks.csv", NULL
	};
	g_autofree gchar *zoho_map = g_strdup_printf("{\"Qualification\":%" G_GINT64_FORMAT "}", stage_id(f, "qualified"));
	g_autofree gchar *salesforce_map = g_strdup_printf("{\"Closed Won\":%" G_GINT64_FORMAT "}", stage_id(f, "won"));
	g_autoptr(JsonObject) zoho = manifest(f, "zoho_crm", zoho_files, zoho_map, NULL);
	g_autoptr(JsonObject) salesforce = manifest(f, "salesforce", salesforce_files, salesforce_map,
		"\"columns\":{\"contacts\":{\"title\":\"Title\"}}");
	g_autoptr(VentureEntity) zoho_batch = NULL;
	g_autoptr(VentureEntity) salesforce_batch = NULL;
	g_autoptr(JsonObject) defaults = NULL;
	(void)data;

	defaults = venture_crm_import_default_columns("hubspot", "contacts");
	g_assert_nonnull(defaults);
	g_assert_cmpstr(json_object_get_string_member(defaults, "email"), ==, "Email");
	g_clear_pointer(&defaults, json_object_unref);
	defaults = venture_crm_import_default_columns("salesforce", "deals");
	g_assert_cmpstr(json_object_get_string_member(defaults, "stage"), ==, "StageName");
	g_clear_pointer(&defaults, json_object_unref);
	g_assert_null(venture_crm_import_default_columns("pipedrive", "deals"));

	zoho_batch = preview(f, zoho);
	import(f, zoho_batch);
	{
		g_autoptr(VentureEntity) company = record_of(f, zoho_batch, "company", "z-acc-1", VENTURE_TYPE_COMPANY);
		g_autoptr(VentureEntity) contact = record_of(f, zoho_batch, "contact", "z-con-1", VENTURE_TYPE_CONTACT);
		g_autoptr(VentureEntity) deal = record_of(f, zoho_batch, "deal", "z-deal-1", VENTURE_TYPE_DEAL);
		g_autoptr(VentureEntity) note = record_of(f, zoho_batch, "note", "z-note-1", VENTURE_TYPE_INTERACTION);
		g_autoptr(VentureEntity) task = record_of(f, zoho_batch, "task", "z-task-1", VENTURE_TYPE_ACTIVITY);
		g_autoptr(VentureMoney) value = NULL;
		g_autofree gchar *name = string_of(company, "name");
		g_autofree gchar *email = string_of(contact, "email");
		g_assert_cmpstr(name, ==, "Umbrella Ltd");
		g_assert_cmpstr(email, ==, "uma@umbrella.example");
		g_assert_cmpint(int_of(contact, "company-id"), ==, venture_entity_get_id(company));
		g_object_get(deal, "value", &value, NULL);
		g_assert_cmpstr(venture_money_get_currency(value), ==, "GBP");
		g_assert_cmpint(int_of(deal, "stage-id"), ==, stage_id(f, "qualified"));
		/* Zoho's Parent Id resolves to whichever object carries it. */
		g_assert_cmpint(int_of(note, "contact-id"), ==, venture_entity_get_id(contact));
		g_assert_cmpint(int_of(task, "deal-id"), ==, venture_entity_get_id(deal));
	}

	salesforce_batch = preview(f, salesforce);
	import(f, salesforce_batch);
	{
		g_autoptr(VentureEntity) contact = record_of(f, salesforce_batch, "contact", "003A", VENTURE_TYPE_CONTACT);
		g_autoptr(VentureEntity) deal = record_of(f, salesforce_batch, "deal", "006A", VENTURE_TYPE_DEAL);
		g_autoptr(VentureEntity) note = record_of(f, salesforce_batch, "note", "069A", VENTURE_TYPE_INTERACTION);
		g_autoptr(VentureEntity) call = record_of(f, salesforce_batch, "task", "00TA", VENTURE_TYPE_ACTIVITY);
		g_autoptr(VentureEntity) sent = record_of(f, salesforce_batch, "task", "00TB", VENTURE_TYPE_INTERACTION);
		g_autofree gchar *role = string_of(contact, "role");
		gint kind = -1;
		gint stage = -1;
		g_assert_cmpstr(role, ==, "Owner");
		g_object_get(deal, "stage", &stage, NULL);
		g_assert_cmpint(stage, ==, VENTURE_DEAL_STAGE_WON);
		g_assert_cmpint(int_of(note, "deal-id"), ==, venture_entity_get_id(deal));
		g_object_get(call, "kind", &kind, NULL);
		g_assert_cmpint(kind, ==, VENTURE_ACTIVITY_KIND_CALL);
		g_object_get(sent, "kind", &kind, NULL);
		g_assert_cmpint(kind, ==, VENTURE_INTERACTION_KIND_EMAIL);
		g_assert_cmpint(int_of(sent, "contact-id"), ==, venture_entity_get_id(contact));
	}
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, 2);
	g_assert_cmpuint(count(f, VENTURE_TYPE_CONTACT), ==, 2);
	g_assert_cmpuint(count(f, VENTURE_TYPE_DEAL), ==, 2);
}

/* Batches and rows belong to the service: a generic write is refused. */
static void
test_generic_write_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = manifest(f, "hubspot", hubspot_files, NULL, NULL);
	g_autoptr(VentureEntity) batch = preview(f, payload);
	g_autoptr(VentureCrmImportRow) row = venture_crm_import_row_new();
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(batch, "state", "active", NULL);
	g_assert_false(venture_database_save(f->db, batch, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "VentureCrmImportService"));
	g_clear_error(&error);
	g_object_set(row, "import-id", venture_entity_get_id(batch), "source-object", "company", "source-id", "x", "status", "imported", NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(row), f->org);
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(row), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_false(venture_database_delete(f->db, batch, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

/* A failure after the first write leaves nothing: the import is one
 * transaction. A note whose timestamp cannot be parsed is such a failure. */
static void
test_import_atomic(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonObject) payload = manifest(f, "hubspot", hubspot_files, NULL, NULL);
	g_autoptr(VentureEntity) batch = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	(void)data;
	actor_init(&actor);
	json_object_set_string_member(json_object_get_object_member(payload, "csv"), "notes",
		"Record ID,Activity type,Title,Body,Activity date,Activity assigned to,Associated Contact IDs,Associated Company IDs,Associated Deal IDs\n"
		"401,NOTE,Kickoff,body,not a date,Jane Doe,201,101,301\n");
	batch = preview(f, payload);
	g_assert_false(venture_crm_import_service_import(venture_crm_import_service_get(f->db),
		VENTURE_CRM_IMPORT(batch), &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "401"));
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, 0);
	g_assert_cmpuint(count(f, VENTURE_TYPE_CONTACT), ==, 0);
	g_assert_cmpuint(count(f, VENTURE_TYPE_DEAL), ==, 0);
	g_assert_cmpuint(count(f, VENTURE_TYPE_INTERACTION), ==, 0);
	{
		g_autoptr(VentureEntity) fresh = venture_database_get(f->db, VENTURE_TYPE_CRM_IMPORT, venture_entity_get_id(batch), NULL);
		g_autofree gchar *state = string_of(fresh, "state");
		g_assert_cmpstr(state, ==, "preview");
	}
}

/* Source ids are stable only within one vendor: a HubSpot company 101 and
 * a Zoho account 101 are two companies. What breaks if this regresses: a
 * second vendor's export silently lands on the first vendor's records. */
static void
test_source_ids_scoped_by_vendor(Fixture *f, gconstpointer data)
{
	static const gchar *const zoho_files[] = { "companies", "zoho-accounts.csv", NULL };
	g_autoptr(JsonObject) hubspot = manifest(f, "hubspot", hubspot_files, NULL, NULL);
	g_autoptr(JsonObject) zoho = manifest(f, "zoho_crm", zoho_files, "{}", NULL);
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *name = NULL;
	(void)data;
	first = preview(f, hubspot);
	import(f, first);
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, 3);
	/* The Zoho export reuses HubSpot's id 101 for a company that matches
	 * nothing by domain or email. */
	json_object_set_string_member(json_object_get_object_member(zoho, "csv"), "companies",
		"Record Id,Account Name,Website,Phone,Industry,Account Owner,Created Time\n"
		"101,Wayne Enterprises,wayne.example,,Holdings,Zoe Owner,2025-01-01 08:00\n");
	second = preview(f, zoho);
	import(f, second);
	status = row_status(f, second, "company", "101", NULL);
	g_assert_cmpstr(status, ==, "imported");
	company = record_of(f, second, "company", "101", VENTURE_TYPE_COMPANY);
	name = string_of(company, "name");
	g_assert_cmpstr(name, ==, "Wayne Enterprises");
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, 4);
	{
		g_autoptr(VentureEntity) row = row_for(f, second, "company", "101");
		g_autofree gchar *source = string_of(row, "source");
		g_assert_cmpstr(source, ==, "zoho_crm");
	}
}

static gint64
scalar(VentureDatabase *db, const gchar *sql)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(OrmResult) result = venture_database_query_raw(db, sql, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	return orm_row_get_integer(orm_result_get_row(result), 0);
}

/* Migration 000390 records the pair of tables on a database with the
 * module on, and succeeds without them when the module is off. */
static void
test_migration(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;
	(void)data;
	g_assert_cmpint(scalar(f->db, "SELECT CAST(COUNT(*) AS BIGINT) FROM schema_migrations WHERE version = 390"), ==, 1);
	g_assert_cmpint(scalar(f->db, "SELECT CAST(COUNT(*) AS BIGINT) FROM sqlite_master WHERE type = 'table' AND name IN ('crm_imports', 'crm_import_rows')"), ==, 2);

	venture_config_set_module_enabled(config, "crm_import", FALSE);
	db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	context = venture_context_new(config, db);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	g_assert_cmpint(scalar(db, "SELECT CAST(COUNT(*) AS BIGINT) FROM schema_migrations WHERE version = 390"), ==, 1);
	g_assert_cmpint(scalar(db, "SELECT CAST(COUNT(*) AS BIGINT) FROM sqlite_master WHERE type = 'table' AND name IN ('crm_imports', 'crm_import_rows')"), ==, 0);
	g_assert_cmpint(scalar(db, "SELECT CAST(COUNT(*) AS BIGINT) FROM sqlite_master WHERE type = 'table' AND name = 'companies'"), ==, 1);
	g_clear_object(&context);
	g_clear_object(&db);
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	g_assert_true(venture_module_registry_configure(registry, everything, NULL));
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

typedef struct
{
	gboolean done;
	GBytes *bytes;
	GError *error;
} SurfaceResult;

static void
http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	SurfaceResult *response = data;
	response->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &response->error);
	response->done = TRUE;
}

static guint
http_request(VentureWebServer *server, const gchar *method, const gchar *path,
	const gchar *content_type, const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	SurfaceResult response;
	guint status;
	memset(&response, 0, sizeof(response));
	url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body != NULL)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &response);
	while (!response.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(response.error);
	if (out != NULL)
		*out = g_strndup(g_bytes_get_data(response.bytes, NULL), g_bytes_get_size(response.bytes));
	status = soup_message_get_status(message);
	g_clear_pointer(&response.bytes, g_bytes_unref);
	return status;
}

typedef struct
{
	gboolean done;
	gchar *out;
	gchar *err;
	GError *error;
} CliReply;

static void
cli_received(GObject *source, GAsyncResult *result, gpointer data)
{
	CliReply *reply = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &reply->out, &reply->err, &reply->error);
	reply->done = TRUE;
}

static gboolean
run_cli(VentureWebServer *server, const gchar *a, const gchar *b, const gchar *c, gchar **out)
{
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GError) error = NULL;
	CliReply reply = { FALSE, NULL, NULL, NULL };
	gboolean ok;
	process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error,
		"build/debug/venturectl", "--server", venture_web_server_get_base_url(server), "-f", "json", a, b, c, NULL);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, cli_received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	g_test_message("CLI stderr: %s", reply.err != NULL ? reply.err : "");
	ok = g_subprocess_get_successful(process);
	if (out != NULL) *out = g_steal_pointer(&reply.out);
	g_free(reply.out);
	g_free(reply.err);
	return ok;
}

/* REST preview and actions, and venturectl crm preview/import/rollback
 * reading a manifest whose "files" name CSVs next to it. */
static void
test_surfaces(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	g_autofree gchar *dir = g_dir_make_tmp("venture-crm-import-XXXXXX", NULL);
	g_autofree gchar *body = NULL;
	g_autofree gchar *payload = NULL;
	g_autofree gchar *manifest_path = NULL;
	g_autofree gchar *manifest_text = NULL;
	g_autofree gchar *status = NULL;
	g_autoptr(JsonObject) object = manifest(f, "hubspot", hubspot_files, NULL, NULL);
	guint port;
	guint i;
	(void)data;
	port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	g_object_set(f->config, "state-dir", dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	{
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
		json_node_set_object(node, json_object_ref(object));
		payload = venture_json_to_string(node, FALSE);
	}
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/crm_imports/preview", "application/json", payload, &body), ==, 200);
	g_assert_nonnull(strstr(body, "\"preview\""));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/crm_imports/1/import", "application/json", "{}", &body), ==, 200);
	g_assert_nonnull(strstr(body, "\"imported\""));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, 3);
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/crm_imports/1/rollback", "application/json", "{}", &body), ==, 200);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, 0);
	/* A rolled-back batch refuses import over HTTP with the validation
	 * status, and the refusal names the service. */
	g_assert_cmpuint(http_request(server, "POST", "/api/v1/crm_imports/1/import", "application/json", "{}", &body), ==, 422);
	g_assert_nonnull(strstr(body, "VentureCrmImportService"));
	g_clear_pointer(&body, g_free);

	/* venturectl: a manifest on disk naming its CSV files. */
	for (i = 0; hubspot_files[i] != NULL; i += 2)
	{
		g_autofree gchar *text = fixture_text(hubspot_files[i + 1]);
		g_autofree gchar *path = g_build_filename(dir, hubspot_files[i + 1], NULL);
		g_assert_true(g_file_set_contents(path, text, -1, NULL));
	}
	json_object_remove_member(object, "csv");
	{
		g_autoptr(JsonObject) files = json_object_new();
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
		for (i = 0; hubspot_files[i] != NULL; i += 2)
			json_object_set_string_member(files, hubspot_files[i], hubspot_files[i + 1]);
		json_object_set_object_member(object, "files", g_steal_pointer(&files));
		json_node_set_object(node, json_object_ref(object));
		manifest_text = venture_json_to_string(node, FALSE);
	}
	manifest_path = g_build_filename(dir, "manifest.json", NULL);
	g_assert_true(g_file_set_contents(manifest_path, manifest_text, -1, NULL));
	g_assert_true(run_cli(server, "crm", "preview", manifest_path, &body));
	g_assert_nonnull(strstr(body, "\"preview\""));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, 0);
	g_assert_true(run_cli(server, "crm", "import", "2", &body));
	g_assert_nonnull(strstr(body, "\"imported\""));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, 3);
	g_assert_true(run_cli(server, "crm", "rollback", "2", &body));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, 0);
	/* crm import FILE previews and imports in one go. */
	g_assert_true(run_cli(server, "crm", "import", manifest_path, &body));
	g_assert_nonnull(strstr(body, "\"imported\""));
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(count(f, VENTURE_TYPE_COMPANY), ==, 3);
	g_assert_false(run_cli(server, "crm", "bogus", "1", NULL));
	venture_web_server_stop(server);
	g_clear_object(&server);
	venture_test_remove_tree(dir);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add("/crm-import/lifecycle-idempotent", Fixture, NULL, setup, test_lifecycle_idempotent, teardown);
	g_test_add("/crm-import/dedupe-and-links", Fixture, NULL, setup, test_dedupe_and_links, teardown);
	g_test_add("/crm-import/deals", Fixture, NULL, setup, test_deals, teardown);
	g_test_add("/crm-import/unmapped-stage-refused", Fixture, NULL, setup, test_unmapped_stage_refused, teardown);
	g_test_add("/crm-import/manifest-refusals", Fixture, NULL, setup, test_manifest_refusals, teardown);
	g_test_add("/crm-import/history-and-tasks", Fixture, NULL, setup, test_history_and_tasks, teardown);
	g_test_add("/crm-import/rollback-exact", Fixture, NULL, setup, test_rollback_exact, teardown);
	g_test_add("/crm-import/vendor-maps", Fixture, NULL, setup, test_vendor_maps, teardown);
	g_test_add("/crm-import/generic-write", Fixture, NULL, setup, test_generic_write_refused, teardown);
	g_test_add("/crm-import/import-atomic", Fixture, NULL, setup, test_import_atomic, teardown);
	g_test_add("/crm-import/source-ids-scoped-by-vendor", Fixture, NULL, setup, test_source_ids_scoped_by_vendor, teardown);
	g_test_add("/crm-import/migration", Fixture, NULL, setup, test_migration, teardown);
	g_test_add("/crm-import/surfaces", Fixture, NULL, setup, test_surfaces, teardown);
	return g_test_run();
}
