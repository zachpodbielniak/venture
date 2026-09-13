/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
#include <stdio.h>

struct _VentureBankMatchService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
};
G_DEFINE_FINAL_TYPE(VentureBankMatchService, venture_bank_match_service, G_TYPE_OBJECT)

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	VentureBankMatchService *self = VENTURE_BANK_MATCH_SERVICE(object);
	if (id == 1) g_value_set_object(value, self->database);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureBankMatchService *self = VENTURE_BANK_MATCH_SERVICE(object);
	if (id == 1)
	{
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	}
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VentureBankMatchService *self = VENTURE_BANK_MATCH_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_bank_match_service_parent_class)->finalize(object);
}

static void
venture_bank_match_service_class_init(VentureBankMatchServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;
	g_object_class_install_property(object_class, 1,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_bank_match_service_init(VentureBankMatchService *self)
{
	(void)self;
}

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"VentureBankMatchService: %s", message);
	return FALSE;
}

gboolean
venture_bank_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error)
{
	const gchar *name = venture_entity_get_entity_name(record);
	VentureBankMatchService *self;
	if (!g_str_has_prefix(name, "bank_") && strcmp(name, "reconciliation"))
		return TRUE;
	self = venture_database_get_bank_match_service(database);
	if (self->writing == record)
	{
		self->writing = NULL;
		return TRUE;
	}
	if (!strcmp(name, "bank_account") && !removal)
	{
		g_autoptr(VentureEntity) account = NULL;
		g_autofree gchar *currency = NULL;
		gint64 id = 0;
		g_object_get(record, "account-id", &id, "currency", &currency, NULL);
		account = venture_database_get(database, VENTURE_TYPE_ACCOUNT, id, error);
		if (account == NULL) return FALSE;
		if (venture_entity_get_organization_id(account) != venture_entity_get_organization_id(record))
			return refuse(error, "ledger account belongs to another organization");
		if (currency == NULL || strlen(currency) != 3)
			return refuse(error, "bank currency is required");
		return TRUE;
	}
	return refuse(error, "statement evidence must be changed through the service");
}

static gint64
number(VentureEntity *record, const gchar *field)
{
	gint64 value = 0;
	g_object_get(record, field, &value, NULL);
	return value;
}

static GPtrArray *
rows(VentureDatabase *db, GType type, gint64 org, const gchar *field, gint64 id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	venture_query_set_organization(query, org);
	if (field != NULL && !venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, error))
		return NULL;
	return venture_database_find(db, query, error);
}

static VentureEntity *
new_record(GType type, gint64 org)
{
	VentureEntity *record = g_object_new(type, NULL);
	venture_entity_set_organization_id(record, org);
	return record;
}

static gboolean
save_owned(VentureBankMatchService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
	return ok;
}

static const gchar *
option(JsonObject *args, const gchar *name)
{
	JsonNode *node = args != NULL ? json_object_get_member(args, name) : NULL;
	return node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
		? json_node_get_string(node) : NULL;
}

static GDateTime *
date_parse(const gchar *text)
{
	g_autofree gchar *iso = NULL;
	if (text == NULL) return NULL;
	iso = strlen(text) == 10 ? g_strconcat(text, "T00:00:00Z", NULL) : g_strdup(text);
	return g_date_time_new_from_iso8601(iso, NULL);
}

static gboolean
state_is(VentureEntity *record, const gchar *wanted)
{
	g_autofree gchar *state = NULL;
	g_object_get(record, "state", &state, NULL);
	return g_strcmp0(state, wanted) == 0;
}

static gboolean
unlocked(VentureBankMatchService *self, VentureEntity *transaction, GError **error)
{
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GDateTime) date = NULL;
	guint i;
	found = rows(self->database, VENTURE_TYPE_RECONCILIATION, venture_entity_get_organization_id(transaction),
		"bank-account-id", number(transaction, "bank-account-id"), error);
	if (found == NULL) return FALSE;
	g_object_get(transaction, "date", &date, NULL);
	for (i = 0; i < found->len; i++)
	{
		VentureEntity *rec = g_ptr_array_index(found, i);
		g_autoptr(GDateTime) end = NULL;
		g_object_get(rec, "period-end", &end, NULL);
		if (state_is(rec, "reconciled") && date != NULL && end != NULL && g_date_time_compare(date, end) <= 0)
			return refuse(error, "reconciled transactions and matches are immutable");
	}
	return TRUE;
}

/* An adapter selects only registered cash documents. New vendor payment
 * records participate by the public record name and amount/date metadata. */
static VentureMoney *
candidate_amount(VentureEntity *record)
{
	const gchar *name = venture_entity_get_entity_name(record);
	VentureMoney *amount = NULL;
	if (!strcmp(name, "sale")) return venture_sale_get_net(VENTURE_SALE(record), NULL);
	if (strcmp(name, "expense") && strcmp(name, "payment") && strcmp(name, "vendor_bill_payment") && strcmp(name, "bill_payment"))
		return NULL;
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(record), "amount") == NULL)
		return NULL;
	g_object_get(record, "amount", &amount, NULL);
	if (amount != NULL && strcmp(name, "payment"))
	{
		VentureMoney *negative = venture_money_multiply_int(amount, -1, NULL);
		venture_money_free(amount);
		amount = negative;
	}
	return amount;
}

static GDateTime *
candidate_date(VentureEntity *record)
{
	GDateTime *date = NULL;
	const gchar *field = g_object_class_find_property(G_OBJECT_GET_CLASS(record), "occurred-at") != NULL
		? "occurred-at" : "date";
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(record), field) != NULL)
		g_object_get(record, field, &date, NULL);
	return date;
}

GPtrArray *
venture_bank_transaction_candidates(VentureDatabase *db, VentureEntity *transaction, GError **error)
{
	g_auto(GStrv) names = venture_entity_registry_list_names(venture_entity_registry_get_default());
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autoptr(GPtrArray) matches = NULL;
	g_autoptr(GPtrArray) result = g_ptr_array_new_with_free_func(g_object_unref);
	gint64 org = venture_entity_get_organization_id(transaction);
	guint i, j;
	g_object_get(transaction, "amount", &amount, "date", &date, NULL);
	if (org <= 0 || amount == NULL || date == NULL)
	{
		refuse(error, "candidate search needs organization, amount and date");
		return NULL;
	}
	matches = rows(db, VENTURE_TYPE_BANK_MATCH, org, NULL, 0, error);
	if (matches == NULL) return NULL;
	for (i = 0; names[i] != NULL; i++)
	{
		GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), names[i]);
		g_autoptr(GPtrArray) records = NULL;
		/* Probe the record contract without querying unrelated tables. */
		if (strcmp(names[i], "sale") && strcmp(names[i], "expense") && strcmp(names[i], "payment") &&
			strcmp(names[i], "vendor_bill_payment") && strcmp(names[i], "bill_payment")) continue;
		records = rows(db, type, org, NULL, 0, error);
		if (records == NULL) return NULL;
		for (j = 0; j < records->len; j++)
		{
			VentureEntity *record = g_ptr_array_index(records, j);
			g_autoptr(VentureMoney) value = candidate_amount(record);
			g_autoptr(GDateTime) when = candidate_date(record);
			g_autoptr(VentureMoney) delta = NULL;
			gboolean used = FALSE;
			guint k;
			if (value == NULL || when == NULL || strcmp(venture_money_get_currency(value), venture_money_get_currency(amount))) continue;
			if (g_date_time_difference(when, date) > 5 * G_TIME_SPAN_DAY || g_date_time_difference(date, when) > 5 * G_TIME_SPAN_DAY) continue;
			delta = venture_money_subtract(value, amount, error);
			if (delta == NULL) return NULL;
			/* Near means at most one currency unit, never a percentage float. */
			if (venture_money_get_amount(delta) > 100 || venture_money_get_amount(delta) < -100) continue;
			for (k = 0; k < matches->len; k++)
			{
				VentureEntity *match = g_ptr_array_index(matches, k);
				g_autofree gchar *record_type = NULL;
				g_object_get(match, "record-type", &record_type, NULL);
				if (!g_strcmp0(record_type, names[i]) && number(match, "record-id") == venture_entity_get_id(record)) used = TRUE;
			}
			if (!used) g_ptr_array_add(result, g_object_ref(record));
		}
	}
	return g_steal_pointer(&result);
}

static gboolean
match_records(VentureBankMatchService *self, VentureEntity *transaction, JsonArray *parts, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) sum = NULL;
	g_autoptr(GPtrArray) pending = g_ptr_array_new_with_free_func(g_object_unref);
	guint i;
	gint64 first = 0, org = venture_entity_get_organization_id(transaction);
	if (!unlocked(self, transaction, error)) return FALSE;
	if (!state_is(transaction, "unmatched") || parts == NULL || json_array_get_length(parts) == 0)
		return refuse(error, "matching requires an unmatched transaction and at least one record");
	g_object_get(transaction, "amount", &amount, NULL);
	sum = venture_money_new_zero(venture_money_get_currency(amount));
	for (i = 0; i < json_array_get_length(parts); i++)
	{
		JsonNode *node = json_array_get_element(parts, i);
		JsonObject *part;
		const gchar *name;
		GType type;
		g_autoptr(VentureEntity) target = NULL;
		g_autoptr(VentureEntity) match = NULL;
		g_autoptr(VentureMoney) value = NULL;
		g_autoptr(VentureMoney) contribution = NULL;
		g_autoptr(VentureMoney) next = NULL;
		g_autoptr(GPtrArray) existing = NULL;
		guint j;
		if (!JSON_NODE_HOLDS_OBJECT(node)) return refuse(error, "match parts must be objects");
		part = json_node_get_object(node);
		name = option(part, "type");
		if (name == NULL || !json_object_has_member(part, "id")) return refuse(error, "match requires type and id");
		type = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
		if (type == G_TYPE_INVALID) return refuse(error, "match record type is unavailable");
		target = venture_database_get(self->database, type, json_object_get_int_member(part, "id"), error);
		if (target == NULL) return FALSE;
		if (venture_entity_get_organization_id(target) != org) return refuse(error, "match crosses organizations");
		value = candidate_amount(target);
		if (value == NULL) return refuse(error, "record is not a supported cash document");
		contribution = option(part, "amount") != NULL ? venture_money_from_string(option(part, "amount"), venture_money_get_currency(value), error) : venture_money_copy(value);
		if (contribution == NULL) return FALSE;
		if (strcmp(venture_money_get_currency(contribution), venture_money_get_currency(value))) return refuse(error, "match crosses currencies");
		if ((venture_money_get_amount(value) >= 0 && (venture_money_get_amount(contribution) <= 0 || venture_money_compare(contribution, value) > 0)) ||
			(venture_money_get_amount(value) < 0 && (venture_money_get_amount(contribution) >= 0 || venture_money_compare(contribution, value) < 0)))
			return refuse(error, "match amount exceeds the document");
		existing = rows(self->database, VENTURE_TYPE_BANK_MATCH, org, "record-id", venture_entity_get_id(target), error);
		if (existing == NULL) return FALSE;
		for (j = 0; j < existing->len + pending->len; j++)
		{
			VentureEntity *old = j < existing->len ? g_ptr_array_index(existing, j) : g_ptr_array_index(pending, j - existing->len);
			g_autofree gchar *old_type = NULL;
			g_object_get(old, "record-type", &old_type, NULL);
			if (!g_strcmp0(old_type, name) && number(old, "record-id") == venture_entity_get_id(target))
				return refuse(error, "record already matched");
		}
		next = venture_money_add(sum, contribution, error);
		if (next == NULL) return FALSE;
		g_clear_pointer(&sum, venture_money_free);
		sum = g_steal_pointer(&next);
		match = new_record(VENTURE_TYPE_BANK_MATCH, org);
		g_object_set(match, "transaction-id", venture_entity_get_id(transaction), "record-type", name,
			"record-id", venture_entity_get_id(target), "amount", contribution,
			"kind", json_array_get_length(parts) > 1 ? "split" : venture_money_equal(value, contribution) ? "exact" : "partial",
			"created-by", actor != NULL ? actor->name : "system", NULL);
		g_ptr_array_add(pending, g_steal_pointer(&match));
	}
	if (!venture_money_equal(sum, amount)) return refuse(error, "split amounts must equal the statement transaction");
	for (i = 0; i < pending->len; i++)
	{
		VentureEntity *match = g_ptr_array_index(pending, i);
		if (!save_owned(self, match, actor, error)) return FALSE;
		if (i == 0) first = venture_entity_get_id(match);
	}
	g_object_set(transaction, "state", "matched", "match-id", first, NULL);
	return save_owned(self, transaction, actor, error);
}

static GPtrArray *
parse_csv(const gchar *data, GError **error)
{
	g_autoptr(GPtrArray) result = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
	g_autoptr(GPtrArray) row = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GString) field = g_string_new(NULL);
	gboolean quoted = FALSE, closed = FALSE;
	const gchar *p;
	for (p = data; ; p++)
	{
		gchar c = *p;
		if (quoted)
		{
			if (c == '\0') { refuse(error, "unterminated CSV quote"); return NULL; }
			if (c == '"' && p[1] == '"') { g_string_append_c(field, '"'); p++; }
			else if (c == '"') { quoted = FALSE; closed = TRUE; }
			else g_string_append_c(field, c);
			continue;
		}
		if (c == '"' && field->len == 0 && !closed) { quoted = TRUE; continue; }
		if (c == ',' || c == '\r' || c == '\n' || c == '\0')
		{
			g_ptr_array_add(row, g_strdup(field->str));
			g_string_truncate(field, 0);
			closed = FALSE;
			if (c != ',')
			{
				if (row->len > 1 || *(gchar *)g_ptr_array_index(row, 0) != '\0')
				{
					g_ptr_array_add(row, NULL);
					g_ptr_array_add(result, g_ptr_array_free(g_steal_pointer(&row), FALSE));
					row = g_ptr_array_new_with_free_func(g_free);
				}
				else g_ptr_array_set_size(row, 0);
				if (c == '\r' && p[1] == '\n') p++;
			}
			if (c == '\0') break;
		}
		else if (closed || c == '"') { refuse(error, "malformed CSV field"); return NULL; }
		else g_string_append_c(field, c);
	}
	return g_steal_pointer(&result);
}

static gchar *
ofx_value(const gchar *block, const gchar *tag)
{
	g_autofree gchar *open = g_strdup_printf("<%s>", tag);
	const gchar *start = strstr(block, open);
	const gchar *end;
	gchar *value;
	if (start == NULL) return NULL;
	start += strlen(open);
	end = start;
	while (*end && *end != '<' && *end != '\r' && *end != '\n') end++;
	value = g_strndup(start, end - start);
	return g_strstrip(value);
}

static gint
column(gchar **header, const gchar *name)
{
	gint i;
	for (i = 0; header[i] != NULL; i++)
		if (!g_strcmp0(header[i], name)) return i;
	return -1;
}

static GDateTime *
bank_date(const gchar *text, const gchar *format)
{
	gint y = 0, m = 0, d = 0;
	gchar tail;
	if (text == NULL) return NULL;
	if (!g_strcmp0(format, "%Y-%m-%d")) return date_parse(text);
	if (!g_strcmp0(format, "%m/%d/%Y") && sscanf(text, "%d/%d/%d%c", &m, &d, &y, &tail) != 3) return NULL;
	if (!g_strcmp0(format, "%d/%m/%Y") && sscanf(text, "%d/%d/%d%c", &d, &m, &y, &tail) != 3) return NULL;
	if (!g_strcmp0(format, "ofx"))
	{
		g_autofree gchar *iso = strlen(text) >= 8 ? g_strdup_printf("%.4s-%.2s-%.2s", text, text + 4, text + 6) : NULL;
		return date_parse(iso);
	}
	if (!g_date_valid_dmy(d, m, y)) return NULL;
	return g_date_time_new_utc(y, m, d, 0, 0, 0);
}

static gboolean
import_line(VentureBankMatchService *self, VentureEntity *bank, VentureEntity *statement,
	const gchar *date_text, const gchar *amount_text, const gchar *description, const gchar *reference,
	const gchar *external, const gchar *format, gboolean invert, const VentureActor *actor, GError **error)
{
	g_autofree gchar *currency = NULL;
	g_autofree gchar *key = NULL;
	g_autoptr(GDateTime) date = bank_date(date_text, format);
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureEntity) transaction = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BANK_TRANSACTION);
	g_autoptr(GPtrArray) duplicates = NULL;
	gint64 org = venture_entity_get_organization_id(bank);
	g_object_get(bank, "currency", &currency, NULL);
	g_object_get(statement, "period-start", &start, "period-end", &end, NULL);
	if (date == NULL || amount_text == NULL || external == NULL || *external == '\0')
		return refuse(error, "every imported line requires valid date, amount and external ID");
	if (g_date_time_compare(date, start) < 0 || g_date_time_compare(date, end) > 0)
		return refuse(error, "transaction date is outside the statement");
	amount = venture_money_from_string(amount_text, currency, error);
	if (amount == NULL) return FALSE;
	if (strcmp(venture_money_get_currency(amount), currency)) return refuse(error, "statement currency mismatch");
	if (invert)
	{
		VentureMoney *negative = venture_money_multiply_int(amount, -1, error);
		if (negative == NULL) return FALSE;
		g_clear_pointer(&amount, venture_money_free);
		amount = negative;
	}
	key = g_strdup_printf("%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%s", org, venture_entity_get_id(bank), external);
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	if (!venture_query_add_filter_string(query, "external-key", VENTURE_FILTER_OP_EQ, key, error)) return FALSE;
	duplicates = venture_database_find(self->database, query, error);
	if (duplicates == NULL) return FALSE;
	if (duplicates->len != 0) return refuse(error, "external ID already imported for this bank account");
	transaction = new_record(VENTURE_TYPE_BANK_TRANSACTION, org);
	g_object_set(transaction, "statement-id", venture_entity_get_id(statement), "bank-account-id", venture_entity_get_id(bank),
		"date", date, "amount", amount, "description", description != NULL && *description ? description : "Bank transaction",
		"reference", reference, "external-id", external, "external-key", key, "state", "unmatched", NULL);
	if (!unlocked(self, transaction, error)) return FALSE;
	return save_owned(self, transaction, actor, error);
}

static VentureEntity *
import_statement(VentureBankMatchService *self, VentureEntity *bank, JsonObject *args, const VentureActor *actor, GError **error)
{
	const gchar *data = option(args, "data"), *format = option(args, "format");
	g_autoptr(GDateTime) start = date_parse(option(args, "period_start"));
	g_autoptr(GDateTime) end = date_parse(option(args, "period_end"));
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autoptr(VentureMoney) opening = NULL, closing = NULL;
	g_autofree gchar *currency = NULL, *hash = NULL;
	g_autoptr(VentureEntity) statement = NULL;
	guint count = 0;
	if (data == NULL || start == NULL || end == NULL || g_date_time_compare(start, end) > 0)
	{
		refuse(error, "import needs data and valid period boundaries"); return NULL;
	}
	if (strlen(option(args, "period_end")) == 10)
	{
		GDateTime *inclusive = g_date_time_add(end, G_TIME_SPAN_DAY - 1);
		g_date_time_unref(end); end = inclusive;
	}
	g_object_get(bank, "currency", &currency, NULL);
	if (option(args, "opening_balance") == NULL || option(args, "closing_balance") == NULL)
	{
		refuse(error, "statement balances are required"); return NULL;
	}
	opening = venture_money_from_string(option(args, "opening_balance"), currency, error);
	if (opening == NULL) return NULL;
	closing = venture_money_from_string(option(args, "closing_balance"), currency, error);
	if (closing == NULL) return NULL;
	if (strcmp(venture_money_get_currency(opening), currency) || strcmp(venture_money_get_currency(closing), currency))
	{
		refuse(error, "statement balances must use bank currency"); return NULL;
	}
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, data, -1);
	statement = new_record(VENTURE_TYPE_BANK_STATEMENT, venture_entity_get_organization_id(bank));
	g_object_set(statement, "bank-account-id", venture_entity_get_id(bank), "period-start", start, "period-end", end,
		"opening-balance", opening, "closing-balance", closing, "source-file-hash", hash, "imported-at", now, NULL);
	if (!save_owned(self, statement, actor, error)) return NULL;
	if (!g_strcmp0(format, "csv"))
	{
		g_autoptr(GPtrArray) parsed = parse_csv(data, error);
		g_autofree gchar *dc = NULL, *ac = NULL, *mc = NULL, *rc = NULL, *ic = NULL, *df = NULL, *sign = NULL;
		gint d, a, m, r, x;
		guint i;
		if (parsed == NULL) return NULL;
		g_object_get(bank, "date-column", &dc, "amount-column", &ac, "description-column", &mc,
			"reference-column", &rc, "external-id-column", &ic, "date-format", &df, "sign-convention", &sign, NULL);
		if (parsed->len < 2) { refuse(error, "CSV requires header and data"); return NULL; }
		d = column(g_ptr_array_index(parsed, 0), dc); a = column(g_ptr_array_index(parsed, 0), ac);
		m = column(g_ptr_array_index(parsed, 0), mc); r = column(g_ptr_array_index(parsed, 0), rc);
		x = column(g_ptr_array_index(parsed, 0), ic);
		if (d < 0 || a < 0 || m < 0 || r < 0 || x < 0 || (g_strcmp0(sign, "normal") && g_strcmp0(sign, "invert")))
		{ refuse(error, "CSV column mapping or sign convention is invalid"); return NULL; }
		for (i = 1; i < parsed->len; i++)
		{
			gchar **line = g_ptr_array_index(parsed, i);
			gint n = g_strv_length(line);
			if (d >= n || a >= n || m >= n || r >= n || x >= n)
			{ refuse(error, "CSV row is missing mapped columns"); return NULL; }
			if (!import_line(self, bank, statement, line[d], line[a], line[m], line[r], line[x], df,
				!g_strcmp0(sign, "invert"), actor, error)) return NULL;
			count++;
		}
	}
	else if (!g_strcmp0(format, "ofx") || !g_strcmp0(format, "qfx"))
	{
		const gchar *p = data;
		while ((p = strstr(p, "<STMTTRN>")) != NULL)
		{
			const gchar *end_block = strstr(p, "</STMTTRN>");
			g_autofree gchar *block = NULL, *date = NULL, *amount = NULL, *memo = NULL, *ref = NULL, *id = NULL;
			if (end_block == NULL) { refuse(error, "unterminated OFX transaction"); return NULL; }
			block = g_strndup(p, end_block - p);
			date = ofx_value(block, "DTPOSTED"); amount = ofx_value(block, "TRNAMT");
			memo = ofx_value(block, "MEMO"); ref = ofx_value(block, "CHECKNUM"); id = ofx_value(block, "FITID");
			if (!import_line(self, bank, statement, date, amount, memo, ref, id, "ofx", FALSE, actor, error)) return NULL;
			count++; p = end_block + strlen("</STMTTRN>");
		}
	}
	else { refuse(error, "supported formats are csv, ofx and qfx"); return NULL; }
	if (count == 0) { refuse(error, "statement has no transactions"); return NULL; }
	g_object_set(bank, "last-statement-balance", closing, "last-statement-date", end, NULL);
	if (!save_owned(self, bank, actor, error)) return NULL;
	return g_steal_pointer(&statement);
}

static VentureEntity *
reconcile(VentureBankMatchService *self, VentureEntity *statement, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) bank = venture_database_get(self->database, VENTURE_TYPE_BANK_ACCOUNT, number(statement, "bank-account-id"), error);
	g_autoptr(GPtrArray) transactions = NULL;
	g_autoptr(GDateTime) end = NULL, now = g_date_time_new_now_utc();
	g_autoptr(VentureMoney) closing = NULL, book = NULL, difference = NULL;
	g_autofree gchar *currency = NULL;
	g_autoptr(VentureEntity) result = NULL;
	gint64 org = venture_entity_get_organization_id(statement);
	guint i;
	if (bank == NULL) return NULL;
	g_object_get(statement, "period-end", &end, "closing-balance", &closing, NULL);
	g_object_get(bank, "currency", &currency, NULL);
	transactions = rows(self->database, VENTURE_TYPE_BANK_TRANSACTION, org, "bank-account-id", venture_entity_get_id(bank), error);
	if (transactions == NULL) return NULL;
	for (i = 0; i < transactions->len; i++)
	{
		VentureEntity *transaction = g_ptr_array_index(transactions, i);
		g_autoptr(GDateTime) date = NULL;
		g_object_get(transaction, "date", &date, NULL);
		if (g_date_time_compare(date, end) <= 0 && state_is(transaction, "unmatched"))
		{ refuse(error, "unmatched bank transactions remain through the period end"); return NULL; }
	}
	book = venture_posting_service_account_balance(venture_database_get_posting_service(self->database),
		number(bank, "account-id"), org, currency, end, error);
	if (book == NULL) return NULL;
	difference = venture_money_subtract(closing, book, error);
	if (difference == NULL) return NULL;
	if (venture_money_get_amount(difference) != 0)
	{ refuse(error, "statement and posted book balance differ"); return NULL; }
	result = new_record(VENTURE_TYPE_RECONCILIATION, org);
	g_object_set(result, "bank-account-id", venture_entity_get_id(bank), "period-end", end,
		"statement-balance", closing, "book-balance", book, "difference", difference, "state", "reconciled",
		"reconciled-by", actor != NULL ? actor->name : "system", "reconciled-at", now, NULL);
	if (!save_owned(self, result, actor, error)) return NULL;
	return g_steal_pointer(&result);
}

static JsonArray *
parts_for(VentureEntity *record)
{
	JsonArray *parts = json_array_new();
	JsonObject *part = json_object_new();
	json_object_set_string_member(part, "type", venture_entity_get_entity_name(record));
	json_object_set_int_member(part, "id", venture_entity_get_id(record));
	json_array_add_object_element(parts, part);
	return parts;
}

static gboolean
auto_match(VentureBankMatchService *self, VentureEntity *statement, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) transactions = rows(self->database, VENTURE_TYPE_BANK_TRANSACTION,
		venture_entity_get_organization_id(statement), "statement-id", venture_entity_get_id(statement), error);
	guint i;
	if (transactions == NULL) return FALSE;
	for (i = 0; i < transactions->len; i++)
	{
		VentureEntity *transaction = g_ptr_array_index(transactions, i);
		g_autoptr(GPtrArray) candidates = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(GDateTime) date = NULL;
		guint j, count = 0;
		VentureEntity *only = NULL;
		if (!state_is(transaction, "unmatched")) continue;
		candidates = venture_bank_transaction_candidates(self->database, transaction, error);
		if (candidates == NULL) return FALSE;
		g_object_get(transaction, "amount", &amount, "date", &date, NULL);
		for (j = 0; j < candidates->len; j++)
		{
			VentureEntity *candidate = g_ptr_array_index(candidates, j);
			g_autoptr(VentureMoney) value = candidate_amount(candidate);
			g_autoptr(GDateTime) when = candidate_date(candidate);
			if (venture_money_equal(value, amount) &&
				g_date_time_difference(when, date) <= 3 * G_TIME_SPAN_DAY &&
				g_date_time_difference(date, when) <= 3 * G_TIME_SPAN_DAY)
			{ only = candidate; count++; }
		}
		if (count == 1)
		{
			g_autoptr(JsonArray) parts = parts_for(only);
			if (!match_records(self, transaction, parts, actor, error)) return FALSE;
		}
	}
	return TRUE;
}

static gboolean
create_document(VentureBankMatchService *self, VentureEntity *transaction, JsonObject *args, const VentureActor *actor, GError **error)
{
	const gchar *type = option(args, "type");
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureEntity) bank = NULL;
	g_autoptr(VentureMoney) signed_amount = NULL, amount = NULL;
	g_autoptr(GDateTime) date = NULL;
	g_autofree gchar *description = NULL;
	g_autoptr(JsonArray) parts = NULL;
	gint64 org = venture_entity_get_organization_id(transaction);
	if (!unlocked(self, transaction, error) || !state_is(transaction, "unmatched"))
		return refuse(error, "creation requires an unreconciled unmatched transaction");
	g_object_get(transaction, "amount", &signed_amount, "date", &date, "description", &description, NULL);
	if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(self->database)),
		self->database, org, date, error)) return FALSE;
	bank = venture_database_get(self->database, VENTURE_TYPE_BANK_ACCOUNT, number(transaction, "bank-account-id"), error);
	if (bank == NULL) return FALSE;
	if (!g_strcmp0(type, "expense") && venture_money_get_amount(signed_amount) < 0)
	{
		amount = venture_money_multiply_int(signed_amount, -1, error);
		if (amount == NULL) return FALSE;
		record = new_record(VENTURE_TYPE_EXPENSE, org);
		g_object_set(record, "amount", amount, "occurred-at", date, "description", description, NULL);
		if (!venture_database_save(self->database, record, actor, error)) return FALSE;
	}
	else if ((!g_strcmp0(type, "receipt") || !g_strcmp0(type, "payment")) && venture_money_get_amount(signed_amount) > 0)
	{
		if (!json_object_has_member(args, "customer_id")) return refuse(error, "receipt requires customer_id");
		if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "payment") == G_TYPE_INVALID)
			return refuse(error, "receivables module is disabled");
		record = new_record(VENTURE_TYPE_PAYMENT, org);
		g_object_set(record, "amount", signed_amount, "date", date, "method", "bank",
			"customer-id", json_object_get_int_member(args, "customer_id"), "reference", description, NULL);
		/* The canonical service owns the write permit; use its configured cash
		 * account and restore the property before returning to the caller. */
		{
			VentureSettlementService *canonical = venture_settlement_service_get(self->database);
			gint64 previous_cash = 0;
			gboolean ok;
			g_object_get(canonical, "cash-account-id", &previous_cash, NULL);
			g_object_set(canonical, "cash-account-id", number(bank, "account-id"), NULL);
			ok = venture_settlement_service_apply_payment(canonical, VENTURE_PAYMENT(record), NULL, actor, error);
			g_object_set(canonical, "cash-account-id", previous_cash, NULL);
			if (!ok) return FALSE;
		}
	}
	else return refuse(error, "negative lines create expenses; positive lines create receipts");
	parts = parts_for(record);
	return match_records(self, transaction, parts, actor, error);
}

VentureEntity *
venture_bank_match_service_execute(VentureBankMatchService *self, const gchar *action, gint64 id,
	JsonObject *args, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) record = NULL, result = NULL;
	GType type;
	gboolean ok = FALSE;
	if (self->database == NULL) { refuse(error, "database is unavailable"); return NULL; }
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "bank_transaction") == G_TYPE_INVALID)
	{ refuse(error, "banking module is disabled"); return NULL; }
	if (action == NULL) { refuse(error, "action is required"); return NULL; }
	type = !strcmp(action, "import") ? VENTURE_TYPE_BANK_ACCOUNT :
		!strcmp(action, "auto") || !strcmp(action, "reconcile") ? VENTURE_TYPE_BANK_STATEMENT : VENTURE_TYPE_BANK_TRANSACTION;
	if (!venture_database_begin(self->database, error)) return NULL;
	record = venture_database_get(self->database, type, id, error);
	if (record == NULL) goto finish;
	if (!strcmp(action, "import"))
	{
		result = import_statement(self, record, args, actor, error);
		ok = result != NULL;
	}
	else if (!strcmp(action, "reconcile"))
	{
		result = reconcile(self, record, actor, error);
		ok = result != NULL;
	}
	else if (!strcmp(action, "auto")) ok = auto_match(self, record, actor, error);
	else if (!unlocked(self, record, error)) goto finish;
	else if (!strcmp(action, "match"))
	{
		JsonNode *parts = args != NULL ? json_object_get_member(args, "parts") : NULL;
		ok = match_records(self, record, parts != NULL && JSON_NODE_HOLDS_ARRAY(parts) ? json_node_get_array(parts) : NULL, actor, error);
	}
	else if (!strcmp(action, "unmatch") || !strcmp(action, "exclude"))
	{
		g_autoptr(GPtrArray) matches = NULL;
		guint i;
		const gchar *reason = option(args, "reason");
		if (!strcmp(action, "exclude") && (reason == NULL || *reason == '\0'))
		{ refuse(error, "exclusion requires a reason"); goto finish; }
		matches = rows(self->database, VENTURE_TYPE_BANK_MATCH, venture_entity_get_organization_id(record), "transaction-id", id, error);
		if (matches == NULL) goto finish;
		for (i = 0; i < matches->len; i++)
		{
			VentureEntity *match = g_ptr_array_index(matches, i);
			self->writing = match;
			ok = venture_database_delete(self->database, match, actor, error);
			self->writing = NULL;
			if (!ok) goto finish;
		}
		g_object_set(record, "state", !strcmp(action, "exclude") ? "excluded" : "unmatched",
			"match-id", (gint64)0, "exclusion-reason", !strcmp(action, "exclude") ? reason : NULL, NULL);
		ok = save_owned(self, record, actor, error);
	}
	else if (!strcmp(action, "create")) ok = create_document(self, record, args, actor, error);
	else refuse(error, "unknown bank action");
finish:
	if (!ok)
	{
		venture_database_rollback(self->database);
		return NULL;
	}
	if (!venture_database_commit(self->database, error)) return NULL;
	return result != NULL ? g_steal_pointer(&result) : g_steal_pointer(&record);
}
