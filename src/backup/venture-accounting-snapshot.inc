/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "db/venture-database-snapshot-private.h"

/* Module membership selects the archive roots; property metadata supplies
 * their fields and ordinary edges. Provider credentials and access management
 * are deliberately outside an accounting archive. */
static gboolean
snapshot_type_allowed(const gchar *name)
{
	static const gchar *const modules[] = {
		"finance", "sales", "invoicing", "receivables", "payables", "ledger",
		"periods", "assets", "banking", "goods", "setup", "autojournal",
		"cutover", "statements", "tax_filing", "budgets", "equity", "group",
		"claims", "payroll", "projects", "progress", "billing", "close", NULL
	};
	const gchar *module;
	guint i;
	if (g_str_equal(name, "company") || g_str_equal(name, "contact") ||
		g_str_equal(name, "venture") || g_str_equal(name, "accounting_custom_field") ||
		g_str_equal(name, "accounting_layout"))
		return TRUE;
	module = venture_entity_registry_get_type_module(venture_entity_registry_get_default(), name);
	if (module == NULL)
	{
		/* A library caller may never construct VentureContext. Read the same
		 * built-in ownership table without applying or changing module masks. */
		const VentureModuleInfo *infos;
		GType type = venture_entity_registry_lookup_any(venture_entity_registry_get_default(), name);
		gsize count, item, member;
		infos = venture_module_registry_get_builtin_infos(&count);
		for (item = 0; item < count && module == NULL; item++)
			for (member = 0; infos[item].entity_types != NULL && infos[item].entity_types[member] != NULL; member++)
				if (infos[item].entity_types[member]() == type)
				{
					module = infos[item].name;
					break;
				}
	}
	for (i = 0; modules[i] != NULL; i++)
		if (g_strcmp0(module, modules[i]) == 0)
			return TRUE;
	return FALSE;
}

static gint
snapshot_table_exists(VentureDatabase *database, GType type, GError **error)
{
	g_autoptr(VentureEntity) prototype = g_object_new(type, NULL);
	g_autoptr(GHashTable) columns = venture_schema_get_existing_columns(
		venture_database_get_connection(database), venture_entity_get_table_name(prototype), error);
	return columns == NULL ? -1 : g_hash_table_size(columns) != 0;
}

static gchar *
snapshot_key(const gchar *type, gint64 id)
{
	return g_strdup_printf("%s:%" G_GINT64_FORMAT, type, id);
}

/* Polymorphic edges use paired <subject>-id and <subject>-type fields.
 * Declared references take precedence over that existing naming contract. */
static const gchar *
snapshot_reference(VentureEntity *record, const gchar *field, gchar **owned)
{
	g_autofree gchar *type_field = NULL;
	GParamSpec *property;
	const gchar *reference = venture_entity_class_get_reference(VENTURE_ENTITY_GET_CLASS(record), field);
	*owned = NULL;
	if (reference != NULL)
		return reference;
	if (!g_str_has_suffix(field, "-id"))
		return NULL;
	type_field = g_strdup_printf("%.*s-type", (gint)(strlen(field) - 3), field);
	property = g_object_class_find_property(G_OBJECT_GET_CLASS(record), type_field);
	if (property != NULL && property->value_type == G_TYPE_STRING)
	{
		g_object_get(record, type_field, owned, NULL);
		return *owned;
	}
	return NULL;
}

static gboolean
snapshot_add(VentureEntity *record, GPtrArray *rows, GHashTable *index)
{
	g_autofree gchar *key = snapshot_key(venture_entity_get_entity_name(record), venture_entity_get_id(record));
	if (g_hash_table_contains(index, key))
		return FALSE;
	g_ptr_array_add(rows, g_object_ref(record));
	g_hash_table_insert(index, g_steal_pointer(&key), record);
	return TRUE;
}

static JsonNode *
snapshot_export(VentureBackupService *self, gint64 org, GError **error)
{
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	g_auto(GStrv) names = venture_entity_registry_list_all_names(registry);
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GHashTable) index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) external_index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonArray) external = json_array_new();
	g_autoptr(VentureEntity) organization = NULL;
	guint i;
	gint64 source_org;
	g_autofree gchar *currency = NULL;
	gint64 fiscal_month = 0;

	if (venture_database_has_transaction(self->database))
	{
		refuse(error, "export needs its own consistent snapshot transaction");
		return NULL;
	}
	if (!venture_database_begin(self->database, error))
		return NULL;
	if (venture_database_get_backend(self->database) == VENTURE_DATABASE_BACKEND_POSTGRES &&
		!venture_database_execute(self->database, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ", NULL, error))
		goto fail;
	organization = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, org, error);
	if (organization == NULL)
		goto fail;
	g_object_get(organization, "default-currency", &currency, "fiscal-year-start-month", &fiscal_month, NULL);
	for (i = 0; names[i] != NULL; i++)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) found = NULL;
		GType type;
		guint j;
		if (!snapshot_type_allowed(names[i]))
			continue;
		type = venture_entity_registry_lookup_any(registry, names[i]);
		{
			gint exists = snapshot_table_exists(self->database, type, error);
			if (exists < 0)
				goto fail;
			if (!exists)
				continue;
		}
		query = venture_query_new(type);
		venture_query_set_organization(query, org);
		venture_query_set_include_deleted(query, TRUE);
		venture_query_set_limit(query, 0);
		found = venture_database_find(self->database, query, error);
		if (found == NULL)
			goto fail;
		for (j = 0; j < found->len; j++)
			snapshot_add(g_ptr_array_index(found, j), rows, index);
	}
	/* Walk to closure, including historical referents that were soft deleted
	 * or shared defaults. Cross-organization/access records are identities to
	 * resolve explicitly, never an excuse to copy another tenant's data. */
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *record = g_ptr_array_index(rows, i);
		g_autofree GParamSpec **properties = NULL;
		guint n, j;
		properties = venture_entity_class_list_persistent_properties(VENTURE_ENTITY_GET_CLASS(record), &n);
		for (j = 0; j < n; j++)
		{
			g_autofree gchar *owned = NULL, *key = NULL;
			g_autoptr(VentureEntity) target = NULL;
			const gchar *reference = snapshot_reference(record, properties[j]->name, &owned);
			gint64 id = 0;
			GType type;
			if (reference == NULL || *reference == '\0' || properties[j]->value_type != G_TYPE_INT64)
				continue;
			g_object_get(record, properties[j]->name, &id, NULL);
			if (id == 0 || (g_str_equal(reference, "organization") && id == org))
				continue;
			key = snapshot_key(reference, id);
			if (g_hash_table_contains(index, key) || g_hash_table_contains(external_index, key))
				continue;
			type = venture_entity_registry_lookup_any(registry, reference);
			if (type == G_TYPE_INVALID)
			{
				refuse(error, "archive contains an unknown reference type");
				goto fail;
			}
			target = venture_database_get(self->database, type, id, error);
			if (target == NULL)
				goto fail;
			source_org = venture_entity_get_organization_id(target);
			if (snapshot_type_allowed(reference) && (source_org == org || source_org == 0))
				snapshot_add(target, rows, index);
			else
			{
				JsonObject *identity = json_object_new();
				json_object_set_string_member(identity, "type", reference);
				json_object_set_int_member(identity, "id", id);
				json_object_set_string_member(identity, "uuid", venture_entity_get_uuid(target));
				json_array_add_object_element(external, identity);
				g_hash_table_add(external_index, g_steal_pointer(&key));
			}
		}
	}
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "version");
	json_builder_add_int_value(builder, 4);
	json_builder_set_member_name(builder, "organization_id");
	json_builder_add_int_value(builder, org);
	json_builder_set_member_name(builder, "currency");
	json_builder_add_string_value(builder, currency);
	json_builder_set_member_name(builder, "fiscal_year_start_month");
	json_builder_add_int_value(builder, fiscal_month);
	json_builder_set_member_name(builder, "external_references");
	{
		JsonNode *node = json_node_new(JSON_NODE_ARRAY);
		json_node_take_array(node, g_steal_pointer(&external));
		json_builder_add_value(builder, node);
	}
	json_builder_set_member_name(builder, "records");
	json_builder_begin_array(builder);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *record = g_ptr_array_index(rows, i);
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "type");
		json_builder_add_string_value(builder, venture_entity_get_entity_name(record));
		json_builder_set_member_name(builder, "record");
		json_builder_add_value(builder, venture_serializable_to_json(VENTURE_SERIALIZABLE(record), FALSE));
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	if (!venture_database_commit(self->database, error))
		return NULL;
	return json_builder_get_root(builder);
fail:
	venture_database_rollback(self->database);
	return NULL;
}

static JsonArray *
snapshot_array(JsonObject *object, const gchar *name, GError **error)
{
	JsonNode *node = json_object_get_member(object, name);
	if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
	{
		refuse(error, "archive is missing a required array");
		return NULL;
	}
	return json_node_get_array(node);
}

static gboolean
snapshot_remap(VentureEntity *record, GHashTable *mapping, GError **error)
{
	g_autofree GParamSpec **properties = NULL;
	guint n, i;
	properties = venture_entity_class_list_persistent_properties(VENTURE_ENTITY_GET_CLASS(record), &n);
	for (i = 0; i < n; i++)
	{
		g_autofree gchar *owned = NULL, *key = NULL;
		const gchar *reference = snapshot_reference(record, properties[i]->name, &owned);
		VentureEntity *target;
		gint64 id = 0;
		if (reference == NULL || *reference == '\0' || properties[i]->value_type != G_TYPE_INT64)
			continue;
		g_object_get(record, properties[i]->name, &id, NULL);
		if (id == 0)
			continue;
		key = snapshot_key(reference, id);
		target = g_hash_table_lookup(mapping, key);
		if (target == NULL)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"Archive has an unresolved %s.%s reference", venture_entity_get_entity_name(record), properties[i]->name);
			return FALSE;
		}
		g_object_set(record, properties[i]->name, venture_entity_get_id(target), NULL);
	}
	return TRUE;
}

static gchar *
snapshot_transfer_key(VentureEntity *transfer, const gchar *prefix)
{
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *day = NULL;
	gint64 from = 0, to = 0;
	g_object_get(transfer, "from-bank-account-id", &from, "to-bank-account-id", &to,
		"date", &date, "amount", &amount, NULL);
	day = date != NULL ? g_date_time_format(date, "%Y-%m-%d") : g_strdup("");
	return g_strdup_printf("%s%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%s:%" G_GINT64_FORMAT,
		prefix, venture_entity_get_organization_id(transfer), from, to, day, amount != NULL ? venture_money_get_amount(amount) : 0);
}

/* Non-organization UNIQUE fields encode a scope in their value. Rebuild
 * known operational keys so later imports still recognize duplicates. */
static void
snapshot_keys(VentureEntity *record, gint64 old_org, gint64 org, gboolean temporary)
{
	g_autofree GParamSpec **properties = NULL;
	guint n, i;
	properties = venture_entity_class_list_persistent_properties(VENTURE_ENTITY_GET_CLASS(record), &n);
	for (i = 0; i < n; i++)
	{
		VentureColumnFlags flags = venture_entity_class_get_column_flags(VENTURE_ENTITY_GET_CLASS(record), properties[i]->name);
		g_autofree gchar *value = NULL, *replacement = NULL, *prefix = NULL;
		const gchar *tail;
		if (!(flags & (VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION)) ||
			(!temporary && !(flags & VENTURE_COLUMN_FLAG_UNIQUE)) || properties[i]->value_type != G_TYPE_STRING ||
			g_str_equal(properties[i]->name, "uuid"))
			continue;
		g_object_get(record, properties[i]->name, &value, NULL);
		if (value == NULL || *value == '\0')
			continue;
		if (temporary)
			replacement = g_strdup_printf("restore:%s:%s", venture_entity_get_uuid(record), properties[i]->name);
		else
		{
			prefix = g_strdup_printf("%" G_GINT64_FORMAT ":", old_org);
			tail = g_str_has_prefix(value, prefix) ? value + strlen(prefix) : value;
			replacement = g_strdup_printf("%" G_GINT64_FORMAT ":%s", org, tail);
			if (VENTURE_IS_BANK_TRANSFER(record) && g_str_equal(properties[i]->name, "transfer-key"))
			{
				g_free(replacement);
				replacement = snapshot_transfer_key(record, "");
			}
			if (VENTURE_IS_BANK_TRANSACTION(record) && g_str_equal(properties[i]->name, "external-key"))
			{
				g_autofree gchar *external = NULL;
				gint64 bank = 0;
				g_object_get(record, "bank-account-id", &bank, "external-id", &external, NULL);
				g_free(replacement);
				replacement = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%s", org, bank, external ? external : "");
			}
		}
		g_object_set(record, properties[i]->name, replacement, NULL);
	}
}

static gboolean
snapshot_decode(VentureEntity *record, JsonNode *node, GError **error)
{
	g_autofree GParamSpec **properties = NULL;
	JsonObject *object = json_node_get_object(node);
	g_autoptr(GList) members = json_object_get_members(object);
	GList *iter;
	guint n, i;
	for (iter = members; iter != NULL; iter = iter->next)
	{
		const gchar *name = iter->data;
		GParamSpec *property;
		if (g_str_equal(name, "attributes"))
		{
			JsonNode *attributes = json_object_get_member(object, name);
			g_autoptr(GList) keys = NULL;
			GList *key;
			if (!JSON_NODE_HOLDS_OBJECT(attributes))
				return refuse(error, "archive attributes must be a string map");
			keys = json_object_get_members(json_node_get_object(attributes));
			for (key = keys; key != NULL; key = key->next)
			{
				JsonNode *value = json_object_get_member(json_node_get_object(attributes), key->data);
				GParamSpec *shadow = g_object_class_find_property(G_OBJECT_GET_CLASS(record), key->data);
				if (json_node_get_value_type(value) != G_TYPE_STRING || (shadow != NULL &&
					(venture_entity_class_get_column_flags(VENTURE_ENTITY_GET_CLASS(record), shadow->name) & VENTURE_COLUMN_FLAG_SENSITIVE)))
					return refuse(error, "archive attributes contain an invalid or sensitive value");
			}
			continue;
		}
		if (g_str_equal(name, "type") || g_str_equal(name, "display_name"))
			continue;
		property = g_object_class_find_property(G_OBJECT_GET_CLASS(record), name);
		if (property == NULL || (venture_entity_class_get_column_flags(VENTURE_ENTITY_GET_CLASS(record),
			property->name) & (VENTURE_COLUMN_FLAG_SENSITIVE | VENTURE_COLUMN_FLAG_TRANSIENT)))
			return refuse(error, "archive contains an unknown or excluded field");
	}
	properties = venture_entity_class_list_persistent_properties(VENTURE_ENTITY_GET_CLASS(record), &n);
	for (i = 0; i < n; i++)
	{
		g_autofree gchar *column = venture_entity_property_to_column(properties[i]->name);
		g_auto(GValue) value = G_VALUE_INIT;
		JsonNode *member;
		if (g_str_equal(column, "attributes") || (venture_entity_class_get_column_flags(
			VENTURE_ENTITY_GET_CLASS(record), properties[i]->name) & VENTURE_COLUMN_FLAG_SENSITIVE))
			continue;
		member = json_object_get_member(object, column);
		if (member == NULL)
			return refuse(error, "archive record is missing a persistent field");
		if (!venture_json_value_from_node(member, properties[i]->value_type, &value, error))
			return FALSE;
		if (g_param_value_validate(properties[i], &value))
			return refuse(error, "archive field is outside its declared range");
	}
	return venture_serializable_from_json(VENTURE_SERIALIZABLE(record), node, error) &&
		venture_entity_validate(record, error);
}

typedef struct
{
	VentureMoney *debit;
	VentureMoney *credit;
	guint lines;
} SnapshotBalance;

static void
snapshot_balance_free(gpointer data)
{
	SnapshotBalance *balance = data;
	g_clear_pointer(&balance->debit, venture_money_free);
	g_clear_pointer(&balance->credit, venture_money_free);
	g_free(balance);
}

static gboolean
snapshot_balanced(GPtrArray *rows, GError **error)
{
	g_autoptr(GHashTable) balances = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, snapshot_balance_free);
	GHashTableIter iter;
	gpointer value;
	guint i;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *journal = g_ptr_array_index(rows, i);
		g_autofree gchar *currency = NULL;
		gint state = 0;
		gint64 *id;
		SnapshotBalance *balance;
		if (!VENTURE_IS_JOURNAL(journal))
			continue;
		g_object_get(journal, "state", &state, "currency", &currency, NULL);
		if (state == VENTURE_JOURNAL_DRAFT)
			continue;
		if (!venture_currency_is_valid(currency))
			return refuse(error, "posted journal needs a valid book currency");
		balance = g_new0(SnapshotBalance, 1);
		balance->debit = venture_money_new_zero(currency);
		balance->credit = venture_money_new_zero(currency);
		id = g_new(gint64, 1);
		*id = venture_entity_get_id(journal);
		g_hash_table_insert(balances, id, balance);
	}
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(rows, i);
		g_autoptr(VentureMoney) amount = NULL, sum = NULL;
		SnapshotBalance *balance;
		VentureMoney **total;
		gint64 journal = 0;
		gint side;
		if (!VENTURE_IS_JOURNAL_LINE(line))
			continue;
		g_object_get(line, "journal-id", &journal, NULL);
		balance = g_hash_table_lookup(balances, &journal);
		if (balance == NULL)
			continue;
		g_object_get(line, "side", &side, "book-amount", &amount, NULL);
		if (amount == NULL || venture_money_get_amount(amount) < 0 ||
			(side != VENTURE_LEDGER_SIDE_DEBIT && side != VENTURE_LEDGER_SIDE_CREDIT))
			return refuse(error, "archive contains an invalid posted journal line");
		total = side == VENTURE_LEDGER_SIDE_DEBIT ? &balance->debit : &balance->credit;
		sum = venture_money_add(*total, amount, error);
		if (sum == NULL)
			return FALSE;
		g_clear_pointer(total, venture_money_free);
		*total = g_steal_pointer(&sum);
		balance->lines++;
	}
	g_hash_table_iter_init(&iter, balances);
	while (g_hash_table_iter_next(&iter, NULL, &value))
	{
		SnapshotBalance *balance = value;
		if (balance->lines < 2 || !venture_money_equal(balance->debit, balance->credit))
			return refuse(error, "archive contains an unbalanced posted journal");
	}
	return TRUE;
}

/* A balanced header alone cannot detect a truncated ledger projection. Each
 * posted line must have exactly one matching projection, including reversals. */
static gboolean
snapshot_projections(GPtrArray *rows, GError **error)
{
	g_autoptr(GHashTable) index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) seen = g_hash_table_new(g_direct_hash, g_direct_equal);
	guint i;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_hash_table_insert(index, snapshot_key(venture_entity_get_entity_name(row), venture_entity_get_id(row)), row);
	}
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *entry = g_ptr_array_index(rows, i), *line, *journal;
		g_autofree gchar *key = NULL, *journal_key = NULL, *transaction = NULL;
		g_autoptr(VentureMoney) amount = NULL, book = NULL;
		g_autoptr(GDateTime) date = NULL, occurred = NULL;
		gint64 line_id = 0, journal_id = 0, account = 0, projected_account = 0;
		gint side = 0, projected_side = 0, state = 0;
		if (!VENTURE_IS_LEDGER_ENTRY(entry))
			continue;
		g_object_get(entry, "journal-line-id", &line_id, "account-id", &projected_account,
			"side", &projected_side, "amount", &amount, "transaction-id", &transaction,
			"occurred-at", &occurred, NULL);
		key = snapshot_key("journal_line", line_id);
		line = g_hash_table_lookup(index, key);
		if (line == NULL || g_hash_table_contains(seen, line))
			return refuse(error, "missing or duplicate archive ledger projection");
		g_hash_table_add(seen, line);
		g_object_get(line, "journal-id", &journal_id, "account-id", &account,
			"side", &side, "book-amount", &book, NULL);
		journal_key = snapshot_key("journal", journal_id);
		journal = g_hash_table_lookup(index, journal_key);
		if (journal == NULL)
			return refuse(error, "archive projection has no journal");
		g_object_get(journal, "state", &state, "occurred-at", &date, NULL);
		if (state == VENTURE_JOURNAL_DRAFT || account != projected_account || side != projected_side ||
			amount == NULL || book == NULL || !venture_money_equal(amount, book) ||
			g_strcmp0(transaction, venture_entity_get_uuid(journal)) != 0 ||
			(date == NULL) != (occurred == NULL) || (date != NULL && !g_date_time_equal(date, occurred)))
			return refuse(error, "archive ledger projection differs from its journal line");
	}
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(rows, i), *journal;
		g_autofree gchar *key = NULL;
		gint64 id = 0;
		gint state = 0;
		if (!VENTURE_IS_JOURNAL_LINE(line))
			continue;
		g_object_get(line, "journal-id", &id, NULL);
		key = snapshot_key("journal", id);
		journal = g_hash_table_lookup(index, key);
		if (journal == NULL)
			return refuse(error, "archive line has no journal");
		g_object_get(journal, "state", &state, NULL);
		if (state != VENTURE_JOURNAL_DRAFT && !g_hash_table_contains(seen, line))
			return refuse(error, "archive posted line has no ledger projection");
	}
	return TRUE;
}

static gboolean
snapshot_restore(VentureBackupService *self, gint64 org, JsonObject *archive,
	const VentureActor *actor, GError **error)
{
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	g_autoptr(GPtrArray) originals = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) restored = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GPtrArray) external_rows = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GHashTable) schemas = g_hash_table_new(g_direct_hash, g_direct_equal);
	g_autoptr(GHashTable) uuids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) mapping = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(VentureEntity) organization = NULL;
	g_auto(GStrv) names = venture_entity_registry_list_all_names(registry);
	JsonArray *records, *external;
	gint64 old_org, fiscal_month, organization_version;
	const gchar *currency;
	guint i;
	JsonNode *org_node = json_object_get_member(archive, "organization_id");
	if (org_node == NULL || json_node_get_value_type(org_node) != G_TYPE_INT64 || json_node_get_int(org_node) <= 0)
		return refuse(error, "archive needs a source organization");
	old_org = json_node_get_int(org_node);
	currency = venture_json_object_get_string(archive, "currency", "");
	org_node = json_object_get_member(archive, "fiscal_year_start_month");
	if (!venture_currency_is_valid(currency) || org_node == NULL || json_node_get_value_type(org_node) != G_TYPE_INT64)
		return refuse(error, "archive needs valid book currency and fiscal settings");
	fiscal_month = json_node_get_int(org_node);
	if (fiscal_month < 1 || fiscal_month > 12)
		return refuse(error, "invalid archive fiscal month");
	records = snapshot_array(archive, "records", error);
	if (records == NULL)
		return FALSE;
	external = snapshot_array(archive, "external_references", error);
	if (external == NULL)
		return FALSE;
	if (!venture_database_begin(self->database, error))
		return FALSE;
	organization = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, org, error);
	if (organization == NULL)
		goto fail;
	organization_version = venture_entity_get_version(organization);
	g_object_set(organization, "default-currency", currency, "fiscal-year-start-month", fiscal_month,
		"version", organization_version + 1, NULL);
	/* Updating the destination row serializes two concurrent PostgreSQL
	 * restores before either one checks emptiness. Rollback restores settings. */
	if (!venture_database_snapshot_update(self->database, organization, organization_version, error))
		goto fail;
	g_hash_table_insert(mapping, snapshot_key("organization", old_org), organization);
	/* An empty ledger is insufficient: unpaid drafts and bank evidence are
	 * books too. Never mix an archive with pre-existing destination records. */
	for (i = 0; names[i] != NULL; i++)
	{
		g_autoptr(VentureQuery) query = NULL;
		gint64 count;
		if (!snapshot_type_allowed(names[i]))
			continue;
		{
			gint exists = snapshot_table_exists(self->database, venture_entity_registry_lookup_any(registry, names[i]), error);
			if (exists < 0)
				goto fail;
			if (!exists)
				continue;
		}
		query = venture_query_new(venture_entity_registry_lookup_any(registry, names[i]));
		venture_query_set_organization(query, org);
		venture_query_set_include_deleted(query, TRUE);
		count = venture_database_count(self->database, query, error);
		if (count < 0)
			goto fail;
		if (count != 0)
		{
			refuse(error, "historical restore requires an empty destination organization");
			goto fail;
		}
	}
	for (i = 0; i < json_array_get_length(external); i++)
	{
		JsonNode *node = json_array_get_element(external, i);
		JsonObject *identity;
		const gchar *name, *uuid;
		gint64 id;
		GType type;
		g_autofree gchar *key = NULL;
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(VentureEntity) target = NULL;
		if (!JSON_NODE_HOLDS_OBJECT(node))
		{
			refuse(error, "invalid external archive identity");
			goto fail;
		}
		identity = json_node_get_object(node);
		name = venture_json_object_get_string(identity, "type", "");
		uuid = venture_json_object_get_string(identity, "uuid", "");
		type = venture_entity_registry_lookup_any(registry, name);
		node = json_object_get_member(identity, "id");
		if (type == G_TYPE_INVALID || node == NULL || json_node_get_value_type(node) != G_TYPE_INT64 || json_node_get_int(node) <= 0 || !g_uuid_string_is_valid(uuid))
		{
			refuse(error, "invalid external archive identity");
			goto fail;
		}
		id = json_node_get_int(node);
		key = snapshot_key(name, id);
		if (g_hash_table_contains(mapping, key))
		{
			refuse(error, "duplicate archive identity");
			goto fail;
		}
		query = venture_query_new(type);
		venture_query_set_include_deleted(query, TRUE);
		venture_query_add_filter_string(query, "uuid", VENTURE_FILTER_OP_EQ, uuid, NULL);
		target = venture_database_find_one(self->database, query, error);
		if (target == NULL)
			goto fail;
		g_hash_table_insert(mapping, g_steal_pointer(&key), target);
		g_ptr_array_add(external_rows, g_steal_pointer(&target));
	}
	for (i = 0; i < json_array_get_length(records); i++)
	{
		JsonNode *entry = json_array_get_element(records, i), *node;
		JsonObject *object;
		const gchar *name;
		GType type;
		g_autoptr(VentureEntity) original = NULL, copy = NULL;
		g_autofree gchar *key = NULL, *uuid = NULL;
		if (!JSON_NODE_HOLDS_OBJECT(entry))
		{
			refuse(error, "invalid archive record");
			goto fail;
		}
		object = json_node_get_object(entry);
		name = venture_json_object_get_string(object, "type", "");
		type = venture_entity_registry_lookup_any(registry, name);
		node = json_object_get_member(object, "record");
		if (!snapshot_type_allowed(name) || type == G_TYPE_INVALID || node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
		{
			refuse(error, "archive contains an unsupported record type");
			goto fail;
		}
		original = g_object_new(type, NULL);
		if (!snapshot_decode(original, node, error))
			goto fail;
		if (venture_entity_get_id(original) <= 0 || (venture_entity_get_organization_id(original) != old_org &&
			venture_entity_get_organization_id(original) != 0))
		{
			refuse(error, "invalid archive record identity or organization");
			goto fail;
		}
		key = snapshot_key(name, venture_entity_get_id(original));
		if (g_hash_table_contains(mapping, key) || venture_entity_get_uuid(original) == NULL ||
			!g_uuid_string_is_valid(venture_entity_get_uuid(original)) ||
			g_hash_table_contains(uuids, venture_entity_get_uuid(original)))
		{
			refuse(error, "duplicate or invalid archive record identity");
			goto fail;
		}
		copy = g_object_new(type, NULL);
		venture_entity_copy_properties_from(copy, original, FALSE);
		uuid = g_uuid_string_random();
		g_object_set(copy, "id", (gint64)0, "uuid", uuid, "organization-id", org, NULL);
		g_hash_table_insert(mapping, g_steal_pointer(&key), copy);
		g_hash_table_insert(uuids, g_strdup(venture_entity_get_uuid(original)), copy);
		g_ptr_array_add(originals, g_steal_pointer(&original));
		g_ptr_array_add(restored, g_steal_pointer(&copy));
	}
	if (!snapshot_balanced(originals, error) || !snapshot_projections(originals, error))
		goto fail;
	/* Reserve identities using the schema binder. No save hooks run: these
	 * are historical states, not commands to issue/pay/ship them again. */
	for (i = 0; i < restored->len; i++)
	{
		VentureEntity *copy = g_ptr_array_index(restored, i);
		if (!g_hash_table_contains(schemas, GSIZE_TO_POINTER(G_OBJECT_TYPE(copy))))
		{
			if (!venture_schema_create_table(venture_database_get_connection(self->database), G_OBJECT_TYPE(copy), error))
				goto fail;
			g_hash_table_add(schemas, GSIZE_TO_POINTER(G_OBJECT_TYPE(copy)));
		}
		snapshot_keys(copy, old_org, org, TRUE);
		if (!venture_database_snapshot_insert(self->database, copy, error))
			goto fail;
	}
	for (i = 0; i < restored->len; i++)
	{
		VentureEntity *copy = g_ptr_array_index(restored, i);
		VentureEntity *original = g_ptr_array_index(originals, i);
		gint64 id = venture_entity_get_id(copy);
		g_autofree gchar *uuid = g_strdup(venture_entity_get_uuid(copy));
		venture_entity_copy_properties_from(copy, original, FALSE);
		g_object_set(copy, "id", id, "uuid", uuid, NULL);
		if (!snapshot_remap(copy, mapping, error))
			goto fail;
		venture_entity_set_organization_id(copy, org);
	}
	for (i = 0; i < restored->len; i++)
	{
		VentureEntity *copy = g_ptr_array_index(restored, i);
		VentureEntity *original = g_ptr_array_index(originals, i);
		if (VENTURE_IS_LEDGER_ENTRY(copy))
		{
			g_autofree gchar *transaction = NULL;
			VentureEntity *journal;
			g_object_get(original, "transaction-id", &transaction, NULL);
			journal = transaction != NULL ? g_hash_table_lookup(uuids, transaction) : NULL;
			if (journal != NULL && VENTURE_IS_JOURNAL(journal))
				g_object_set(copy, "transaction-id", venture_entity_get_uuid(journal), NULL);
		}
		snapshot_keys(copy, old_org, org, FALSE);
		/* This hook only derives the map key from its remapped subject. Reuse
		 * the record's definition instead of duplicating that key format. */
		if (VENTURE_IS_ACCOUNTING_CONTROL_MAP(copy) && !venture_entity_before_save(copy, error))
			goto fail;
		if (VENTURE_IS_BILLING_NOTICE(copy))
		{
			g_autoptr(GDateTime) past_due = NULL;
			g_autofree gchar *stamp = NULL, *key = NULL;
			gint64 subscription = 0, step = 0;
			g_object_get(copy, "subscription-id", &subscription, "dunning-step-id", &step, "past-due-at", &past_due, NULL);
			if (past_due != NULL)
			{
				stamp = g_date_time_format_iso8601(past_due);
				key = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%s", subscription, step, stamp);
				g_object_set(copy, "delivery-key", key, NULL);
			}
		}
		if (VENTURE_IS_JOURNAL(copy))
		{
			g_autofree gchar *source = NULL, *old_key = NULL;
			gint64 source_id = 0;
			g_object_get(original, "source-type", &source, "source-id", &source_id, "posting-key", &old_key, NULL);
			if (g_strcmp0(source, "bank_transfer") == 0 && old_key != NULL && g_str_has_prefix(old_key, "bank_transfer:"))
			{
				g_autofree gchar *key = snapshot_key(source, source_id);
				VentureEntity *transfer = g_hash_table_lookup(mapping, key);
				g_autofree gchar *posting_key = snapshot_transfer_key(transfer, "bank_transfer:");
				g_object_set(copy, "posting-key", posting_key, NULL);
			}
			else if (g_strcmp0(source, "sale") == 0 && old_key != NULL && g_str_has_prefix(old_key, "sale:"))
			{
				const gchar *suffix = strchr(old_key + strlen("sale:"), ':');
				if (suffix != NULL)
				{
					g_autofree gchar *posting_key = NULL;
					g_object_get(copy, "source-id", &source_id, NULL);
					posting_key = g_strdup_printf("sale:%" G_GINT64_FORMAT "%s", source_id, suffix);
					g_object_set(copy, "posting-key", posting_key, NULL);
				}
			}
		}
		if (!venture_database_snapshot_update(self->database, copy, venture_entity_get_version(copy), error))
			goto fail;
	}
	for (i = 0; i < restored->len; i++)
		if (!venture_custom_fields_sync(self->database, g_ptr_array_index(restored, i), actor, error))
			goto fail;
	{
		g_autoptr(VentureAccountingBackup) evidence = venture_accounting_backup_new();
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
		g_autofree gchar *payload = NULL, *checksum = NULL;
		JsonArray *identities = json_array_new();
		JsonObject *result = json_object_new();
		json_node_take_object(node, result);
		json_object_set_int_member(result, "version", 4);
		json_object_set_int_member(result, "source_organization_id", old_org);
		json_object_set_array_member(result, "identities", identities);
		for (i = 0; i < restored->len; i++)
		{
			VentureEntity *old = g_ptr_array_index(originals, i), *copy = g_ptr_array_index(restored, i);
			JsonObject *identity = json_object_new();
			json_object_set_string_member(identity, "type", venture_entity_get_entity_name(old));
			json_object_set_int_member(identity, "source_id", venture_entity_get_id(old));
			json_object_set_string_member(identity, "source_uuid", venture_entity_get_uuid(old));
			json_object_set_int_member(identity, "id", venture_entity_get_id(copy));
			json_object_set_string_member(identity, "uuid", venture_entity_get_uuid(copy));
			json_array_add_object_element(identities, identity);
		}
		payload = venture_json_to_string(node, FALSE);
		checksum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, payload, -1);
		g_object_set(evidence, "organization-id", org, "format", "json", "state", "restored",
			"payload", payload, "checksum", checksum, NULL);
		if (!save_owned(self, VENTURE_ENTITY(evidence), actor, error))
			goto fail;
	}
	return venture_database_commit(self->database, error);
fail:
	venture_database_rollback(self->database);
	return FALSE;
}
