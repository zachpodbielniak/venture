/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include <venture.h>

/* Every opt-in PostgreSQL fixture owns a unique schema. It never resets the
 * public schema or another fixture's data, even when suites run together. */
static inline VentureDatabase *
venture_test_accounting_database(GError **error)
{
	const gchar *uri = g_getenv("VENTURE_TEST_ACCOUNTING_POSTGRES_URI");
	g_autoptr(VentureDatabase) database = venture_database_new(uri ? uri : "sqlite://:memory:", error);
	if (database == NULL)
		return NULL;
	if (uri != NULL)
	{
		g_autofree gchar *uuid = g_uuid_string_random();
		g_autofree gchar *schema = NULL, *sql = NULL;
		g_strdelimit(uuid, "-", '_');
		schema = g_strconcat("venture_accounting_test_", uuid, NULL);
		sql = g_strdup_printf("CREATE SCHEMA %s; SET search_path TO %s", schema, schema);
		if (!venture_database_execute(database, sql, NULL, error))
			return NULL;
		g_object_set_data_full(G_OBJECT(database), "accounting-test-schema", g_steal_pointer(&schema), g_free);
	}
	return g_steal_pointer(&database);
}

static inline void
venture_test_accounting_database_cleanup(VentureDatabase *database)
{
	const gchar *schema = g_object_get_data(G_OBJECT(database), "accounting-test-schema");
	if (schema != NULL)
	{
		g_autofree gchar *sql = g_strdup_printf("DROP SCHEMA %s CASCADE", schema);
		g_autoptr(GError) error = NULL;
		g_assert_true(venture_database_execute(database, sql, NULL, &error));
		g_assert_no_error(error);
	}
}

static inline gchar *
venture_test_archive_key(const gchar *type, gint64 id)
{
	return g_strdup_printf("%s:%" G_GINT64_FORMAT, type, id);
}

/* Compare the entire public historical record, not just totals or lower-bound
 * row counts. Only newly assigned identities and scope-derived unique keys
 * differ. Independently ask the ledger/subledger APIs for their balances. */
static inline gint64
venture_test_accounting_roundtrip_into(VentureDatabase *database, VentureDatabase *target_database, gint64 org)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) backup = NULL, evidence = NULL;
	g_autoptr(VentureOrganization) destination = venture_organization_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNTING_BACKUP);
	g_autofree gchar *payload = NULL, *manifest = NULL;
	g_autoptr(JsonNode) archive = NULL, mapping_node = NULL;
	g_autoptr(GHashTable) mapping = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autoptr(GHashTable) counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) targets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	JsonArray *records, *identities;
	gint64 dest;
	gboolean ok;
	guint i;
	backup = venture_backup_service_export(venture_backup_service_get(database), org, "json", NULL, &error);
	g_assert_no_error(error);
	g_object_get(backup, "payload", &payload, NULL);
	archive = venture_json_parse(payload, &error);
	g_assert_no_error(error);
	/* Archive ordering must not determine whether references can resolve. */
	{
		JsonArray *forward = json_object_get_array_member(json_node_get_object(archive), "records");
		JsonArray *reverse = json_array_new();
		guint remaining = json_array_get_length(forward);
		while (remaining > 0)
			json_array_add_element(reverse, json_node_copy(json_array_get_element(forward, --remaining)));
		json_object_set_array_member(json_node_get_object(archive), "records", reverse);
		g_free(payload);
		payload = venture_json_to_string(archive, FALSE);
	}
	g_object_set(destination, "name", "Restored historical books", "default-currency", "USD", NULL);
	g_assert_true(venture_database_save(target_database, VENTURE_ENTITY(destination), NULL, &error));
	g_assert_no_error(error);
	dest = venture_entity_get_id(VENTURE_ENTITY(destination));
	ok = venture_backup_service_restore(venture_backup_service_get(target_database), dest, payload, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
	venture_query_set_organization(query, dest);
	venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_EQ, "restored", NULL);
	evidence = venture_database_find_one(target_database, query, &error);
	g_assert_no_error(error);
	g_object_get(evidence, "payload", &manifest, NULL);
	mapping_node = venture_json_parse(manifest, &error);
	g_assert_no_error(error);
	identities = json_object_get_array_member(json_node_get_object(mapping_node), "identities");
	records = json_object_get_array_member(json_node_get_object(archive), "records");
	g_assert_cmpuint(json_array_get_length(records), ==, json_array_get_length(identities));
	for (i = 0; i < json_array_get_length(identities); i++)
	{
		JsonObject *item = json_array_get_object_element(identities, i);
		const gchar *type = json_object_get_string_member(item, "type");
		gint64 *id = g_new(gint64, 1);
		g_autofree gchar *key = NULL, *target = NULL;
		*id = json_object_get_int_member(item, "id");
		key = venture_test_archive_key(type, json_object_get_int_member(item, "source_id"));
		target = venture_test_archive_key(type, *id);
		g_assert_false(g_hash_table_contains(mapping, key));
		g_assert_false(g_hash_table_contains(targets, target));
		g_hash_table_add(targets, g_steal_pointer(&target));
		g_hash_table_insert(mapping, g_steal_pointer(&key), id);
	}
	for (i = 0; i < json_array_get_length(records); i++)
	{
		JsonObject *entry = json_array_get_object_element(records, i);
		const gchar *name = json_object_get_string_member(entry, "type");
		g_autoptr(JsonNode) expected = json_node_copy(json_object_get_member(entry, "record")), actual = NULL;
		JsonObject *old = json_node_get_object(expected), *current;
		gint64 old_id = json_object_get_int_member(old, "id");
		g_autofree gchar *key = venture_test_archive_key(name, old_id);
		gint64 *id = g_hash_table_lookup(mapping, key);
		g_autoptr(VentureEntity) record = NULL;
		g_autofree GParamSpec **properties = NULL;
		guint n, j;
		g_assert_nonnull(id);
		record = venture_database_get(target_database, venture_entity_registry_lookup_any(venture_entity_registry_get_default(), name), *id, &error);
		g_assert_no_error(error);
		actual = venture_serializable_to_json(VENTURE_SERIALIZABLE(record), FALSE);
		current = json_node_get_object(actual);
		json_object_remove_member(old, "display_name");
		json_object_remove_member(current, "display_name");
		g_assert_cmpint(venture_entity_get_organization_id(record), ==, dest);
		g_hash_table_replace(counts, g_strdup(name), GUINT_TO_POINTER(GPOINTER_TO_UINT(g_hash_table_lookup(counts, name)) + 1));
		properties = venture_entity_class_list_persistent_properties(VENTURE_ENTITY_GET_CLASS(record), &n);
		for (j = 0; j < n; j++)
		{
			g_autofree gchar *column = venture_entity_property_to_column(properties[j]->name);
			const gchar *reference = venture_entity_class_get_reference(VENTURE_ENTITY_GET_CLASS(record), properties[j]->name);
			VentureColumnFlags flags = venture_entity_class_get_column_flags(VENTURE_ENTITY_GET_CLASS(record), properties[j]->name);
			if (g_str_equal(column, "id") || g_str_equal(column, "uuid") || g_str_equal(column, "organization_id") ||
				(flags & VENTURE_COLUMN_FLAG_UNIQUE) ||
				(VENTURE_IS_ACCOUNTING_CONTROL_MAP(record) && g_str_equal(column, "map_key")) ||
				(VENTURE_IS_BILLING_NOTICE(record) && g_str_equal(column, "delivery_key")) || (VENTURE_IS_LEDGER_ENTRY(record) && g_str_equal(column, "transaction_id")))
			{
				json_object_remove_member(old, column);
				json_object_remove_member(current, column);
				continue;
			}
			if (g_str_equal(column, "source_id") && json_object_has_member(old, "source_type"))
				reference = json_object_get_string_member(old, "source_type");
			if (g_str_equal(column, "record_id") && json_object_has_member(old, "record_type"))
				reference = json_object_get_string_member(old, "record_type");
			if (g_str_equal(column, "subject_id") && json_object_has_member(old, "subject_type"))
				reference = json_object_get_string_member(old, "subject_type");
			if (g_str_equal(column, "correction_id") && json_object_has_member(old, "correction_type"))
				reference = json_object_get_string_member(old, "correction_type");
			if (reference != NULL && json_object_has_member(old, column))
			{
				gint64 referenced = json_object_get_int_member(old, column);
				g_autofree gchar *refkey = venture_test_archive_key(reference, referenced);
				gint64 *mapped = g_hash_table_lookup(mapping, refkey);
				if (g_str_equal(reference, "organization") && referenced == org)
					json_object_set_int_member(old, column, dest);
				else if (mapped != NULL)
					json_object_set_int_member(old, column, *mapped);
			}
		}
		if (!json_node_equal(expected, actual))
			g_test_message("Historical field mismatch in %s", name);
		g_assert_true(json_node_equal(expected, actual));
		if (VENTURE_IS_ACCOUNT(record) || VENTURE_IS_INVOICE(record) || VENTURE_IS_VENDOR_BILL(record))
		{
			g_autoptr(VentureMoney) before = NULL, after = NULL;
			g_autoptr(GDateTime) cutoff = g_date_time_new_utc(2100, 1, 1, 0, 0, 0);
			if (VENTURE_IS_ACCOUNT(record))
			{
				before = venture_posting_service_account_balance(venture_database_get_posting_service(database), old_id, org, "USD", cutoff, &error);
				after = venture_posting_service_account_balance(venture_database_get_posting_service(target_database), *id, dest, "USD", cutoff, &error);
			}
			else if (VENTURE_IS_INVOICE(record))
			{
				before = venture_settlement_service_invoice_balance(venture_settlement_service_get(database), old_id, NULL, &error);
				after = venture_settlement_service_invoice_balance(venture_settlement_service_get(target_database), *id, NULL, &error);
			}
			else
			{
				before = venture_payables_service_bill_balance(venture_payables_service_get(database), old_id, NULL, &error);
				after = venture_payables_service_bill_balance(venture_payables_service_get(target_database), *id, NULL, &error);
			}
			g_assert_no_error(error);
			g_assert_true(venture_money_equal(before, after));
		}
	}
	{
		GHashTableIter iter;
		gpointer name, count;
		g_hash_table_iter_init(&iter, counts);
		while (g_hash_table_iter_next(&iter, &name, &count))
		{
			g_autoptr(VentureQuery) rows = venture_query_new(venture_entity_registry_lookup_any(venture_entity_registry_get_default(), name));
			venture_query_set_organization(rows, dest);
			venture_query_set_include_deleted(rows, TRUE);
			g_assert_cmpint(venture_database_count(target_database, rows, &error), ==, GPOINTER_TO_UINT(count));
			g_assert_no_error(error);
		}
	}
	return dest;
}

static inline gint64
venture_test_accounting_roundtrip(VentureDatabase *database, gint64 org)
{
	return venture_test_accounting_roundtrip_into(database, database, org);
}
