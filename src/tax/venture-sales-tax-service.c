/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
#include <json-glib/json-glib.h>

/*
 * The jurisdiction layer sits on top of tax freezing, never inside it.
 * VentureSettlementService asks this service for the rate to apply to a
 * line at first issue and freezes the answer on the line the same way it
 * freezes a tax code's levy; the return report only ever reads what was
 * frozen. A later rate change is a new window row and touches nothing
 * already issued.
 */

struct _VentureSalesTaxService
{
	GObject parent_instance;
	GWeakRef database;
};

G_DEFINE_FINAL_TYPE(VentureSalesTaxService, venture_sales_tax_service, G_TYPE_OBJECT)

static void
service_finalize(GObject *object)
{
	VentureSalesTaxService *self = VENTURE_SALES_TAX_SERVICE(object);
	g_weak_ref_clear(&self->database);
	G_OBJECT_CLASS(venture_sales_tax_service_parent_class)->finalize(object);
}

static void
venture_sales_tax_service_class_init(VentureSalesTaxServiceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = service_finalize;
}

static void
venture_sales_tax_service_init(VentureSalesTaxService *self)
{
	g_weak_ref_init(&self->database, NULL);
}

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureSalesTaxService: %s", message);
	return FALSE;
}

static gboolean
module_on(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"tax_jurisdiction") != G_TYPE_INVALID;
}

VentureSalesTaxService *
venture_sales_tax_service_get(VentureDatabase *database)
{
	VentureSalesTaxService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-sales-tax-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_SALES_TAX_SERVICE, NULL);
		g_weak_ref_set(&self->database, database);
		g_object_set_data_full(G_OBJECT(database), "venture-sales-tax-service", self, g_object_unref);
	}
	return self;
}

static VentureDatabase *
service_db(VentureSalesTaxService *self)
{
	return g_weak_ref_get(&self->database);
}

/* Address codes match exactly, after the only normalisation a code can
 * survive: surrounding whitespace and letter case. Empty is NULL. */
static gchar *
normalise(const gchar *code)
{
	g_autofree gchar *trimmed = NULL;
	if (code == NULL)
		return NULL;
	trimmed = g_strstrip(g_strdup(code));
	if (*trimmed == '\0')
		return NULL;
	return g_utf8_strup(trimmed, -1);
}

static gchar *
string_of(VentureEntity *record, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(record, field, &value, NULL);
	return value;
}

static gint64
id_of(VentureEntity *record, const gchar *field)
{
	gint64 value = 0;
	g_object_get(record, field, &value, NULL);
	return value;
}

/* venture_database_get() answers a missing row with NULL and no error;
 * every caller here wants the absence named. */
static VentureEntity *
fetch(VentureDatabase *db, GType type, gint64 id, const gchar *what, GError **error)
{
	g_autoptr(GError) local = NULL;
	VentureEntity *record = venture_database_get(db, type, id, &local);
	if (record != NULL)
		return record;
	if (local != NULL)
		g_propagate_error(error, g_steal_pointer(&local));
	else
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"VentureSalesTaxService: %s %" G_GINT64_FORMAT " does not exist", what, id);
	return NULL;
}

static GPtrArray *
find_all(VentureDatabase *db, GType type, gint64 organization_id, const gchar *field,
	const gchar *value, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	venture_query_set_organization(query, organization_id);
	if (field != NULL && !venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_EQ, value, error))
		return NULL;
	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;
	return venture_database_find(db, query, error);
}

/* Returns the window of @code in force at @date, or NULL for none. */
static VentureEntity *
window_at(VentureDatabase *db, gint64 organization_id, const gchar *code, GDateTime *date, GError **error)
{
	g_autoptr(GPtrArray) windows = find_all(db, VENTURE_TYPE_TAX_JURISDICTION, organization_id, "code", code, error);
	guint i;
	if (windows == NULL)
		return NULL;
	for (i = 0; i < windows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(windows, i);
		if (venture_tax_jurisdiction_covers(VENTURE_TAX_JURISDICTION(row), date))
			return g_object_ref(row);
	}
	return NULL;
}

/* One part of a rule matches when the rule leaves it open or names the
 * customer's code exactly. */
static gboolean
part_matches(const gchar *rule_part, const gchar *customer_part, guint *specificity)
{
	if (rule_part == NULL)
		return TRUE;
	if (g_strcmp0(rule_part, customer_part) != 0)
		return FALSE;
	(*specificity)++;
	return TRUE;
}

VentureEntity *
venture_sales_tax_service_resolve(VentureSalesTaxService *self, VentureEntity *customer,
	GDateTime *date, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(GPtrArray) rules = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *county = NULL;
	g_autofree gchar *city = NULL;
	g_autofree gchar *raw_state = NULL;
	g_autofree gchar *raw_county = NULL;
	g_autofree gchar *raw_city = NULL;
	g_autoptr(VentureEntity) named = NULL;
	g_autofree gchar *code = NULL;
	VentureEntity *best = NULL;
	guint best_specificity = 0;
	guint i;

	g_return_val_if_fail(VENTURE_IS_SALES_TAX_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_COMPANY(customer), NULL);
	g_return_val_if_fail(date != NULL, NULL);
	if (!module_on())
		return NULL;
	g_object_get(customer, "address-state", &raw_state, "address-county", &raw_county,
		"address-city", &raw_city, NULL);
	state = normalise(raw_state);
	county = normalise(raw_county);
	city = normalise(raw_city);
	if (state == NULL)
		return NULL;
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), NULL;
	rules = find_all(db, VENTURE_TYPE_TAX_RULE, venture_entity_get_organization_id(customer), NULL, NULL, error);
	if (rules == NULL)
		return NULL;
	for (i = 0; i < rules->len; i++)
	{
		VentureEntity *rule = g_ptr_array_index(rules, i);
		g_autofree gchar *rule_state = normalise(string_of(rule, "state"));
		g_autofree gchar *rule_county = normalise(string_of(rule, "county"));
		g_autofree gchar *rule_city = normalise(string_of(rule, "city"));
		gboolean active = FALSE;
		guint specificity = 0;
		/* A rule the operator switched off must stop levying. Charging tax
		 * from an inactive rule is money taken from a customer and remitted
		 * to a jurisdiction on the strength of a row nobody meant to use. */
		g_object_get(rule, "active", &active, NULL);
		if (!active)
			continue;
		if (g_strcmp0(rule_state, state) != 0)
			continue;
		specificity = 1;
		if (!part_matches(rule_county, county, &specificity) ||
			!part_matches(rule_city, city, &specificity))
			continue;
		/* Ties keep the earliest rule: deterministic and visible in the list. */
		if (best == NULL || specificity > best_specificity)
		{
			best = rule;
			best_specificity = specificity;
		}
	}
	if (best == NULL)
		return NULL;
	/* A rule left pointing at a removed window selects nothing. */
	named = venture_database_get(db, VENTURE_TYPE_TAX_JURISDICTION, id_of(best, "jurisdiction-id"), error);
	if (named == NULL)
		return NULL;
	code = string_of(named, "code");
	return window_at(db, venture_entity_get_organization_id(customer), code, date, error);
}

gboolean
venture_sales_tax_service_freeze_line(VentureSalesTaxService *self, VentureEntity *invoice,
	VentureEntity *line, GDateTime *date, gboolean customer_exempt,
	gint64 *numerator, gint64 *denominator, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureEntity) customer = NULL;
	g_autoptr(VentureEntity) jurisdiction = NULL;
	g_autoptr(GError) lookup = NULL;
	gboolean product_exempt = FALSE;
	gboolean exempt;
	gint64 jurisdiction_id = 0;
	gint64 rate = 0;

	g_return_val_if_fail(VENTURE_IS_SALES_TAX_SERVICE(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_INVOICE(invoice), FALSE);
	g_return_val_if_fail(VENTURE_IS_INVOICE_LINE(line), FALSE);
	g_return_val_if_fail(numerator != NULL && denominator != NULL, FALSE);
	if (!module_on())
		return TRUE;
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed");
	if (id_of(line, "product-id") > 0)
	{
		g_autoptr(VentureEntity) product = fetch(db, VENTURE_TYPE_PRODUCT,
			id_of(line, "product-id"), "product", error);
		if (product == NULL)
			return FALSE;
		g_object_get(product, "tax-exempt", &product_exempt, NULL);
	}
	exempt = customer_exempt || product_exempt;
	if (id_of(invoice, "company-id") > 0)
	{
		customer = fetch(db, VENTURE_TYPE_COMPANY, id_of(invoice, "company-id"), "company", error);
		if (customer == NULL)
			return FALSE;
		if (venture_entity_get_organization_id(customer) != venture_entity_get_organization_id(invoice))
			return refuse(error, VENTURE_ERROR_PERMISSION_DENIED,
				"The invoice and its customer belong to different organizations");
		jurisdiction = venture_sales_tax_service_resolve(self, customer, date, &lookup);
		if (lookup != NULL)
		{
			g_propagate_error(error, g_steal_pointer(&lookup));
			return FALSE;
		}
	}
	if (jurisdiction != NULL)
	{
		jurisdiction_id = venture_entity_get_id(jurisdiction);
		if (!exempt && !venture_tax_jurisdiction_get_rate(VENTURE_TAX_JURISDICTION(jurisdiction),
				&rate, denominator, error))
			return FALSE;
		*numerator = rate;
		if (exempt)
			*denominator = VENTURE_TAX_RATE_DENOMINATOR;
	}
	else if (product_exempt)
		*numerator = 0;
	g_object_set(line, "tax-jurisdiction-id", jurisdiction_id, "tax-rate-scaled", rate,
		"tax-exempt", exempt, NULL);
	return TRUE;
}

/* ==========================================================================
 * Write guard
 * ========================================================================== */

static gboolean
check_jurisdiction(VentureDatabase *database, VentureEntity *record, GError **error)
{
	g_autofree gchar *code = string_of(record, "code");
	g_autofree gchar *kind = string_of(record, "kind");
	g_autoptr(GDateTime) from = NULL;
	g_autoptr(GDateTime) to = NULL;
	g_autoptr(GPtrArray) others = NULL;
	gint64 rate = 0;
	guint i;

	g_object_get(record, "rate-scaled", &rate, "effective-from", &from, "effective-to", &to, NULL);
	if (venture_string_is_empty(code))
		return refuse(error, VENTURE_ERROR_VALIDATION, "A jurisdiction needs a code");
	if (rate < 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "A jurisdiction rate cannot be negative");
	if (!venture_string_is_empty(kind) && g_strcmp0(kind, "sales") != 0 && g_strcmp0(kind, "use") != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "A jurisdiction kind is sales or use");
	if (from == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "A jurisdiction rate needs an effective-from date");
	if (to != NULL && g_date_time_compare(to, from) <= 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "A jurisdiction rate's effective-to must follow effective-from");
	others = find_all(database, VENTURE_TYPE_TAX_JURISDICTION, venture_entity_get_organization_id(record),
		"code", code, error);
	if (others == NULL)
		return FALSE;
	for (i = 0; i < others->len; i++)
	{
		VentureEntity *other = g_ptr_array_index(others, i);
		g_autoptr(GDateTime) other_from = NULL;
		g_autoptr(GDateTime) other_to = NULL;
		if (venture_entity_get_id(other) == venture_entity_get_id(record))
			continue;
		g_object_get(other, "effective-from", &other_from, "effective-to", &other_to, NULL);
		if (other_from == NULL)
			continue;
		/* Half-open windows overlap when each starts before the other ends. */
		if ((to == NULL || g_date_time_compare(other_from, to) < 0) &&
			(other_to == NULL || g_date_time_compare(from, other_to) < 0))
		{
			g_autofree gchar *from_text = g_date_time_format(other_from, "%Y-%m-%d");
			g_autofree gchar *to_text = other_to != NULL ? g_date_time_format(other_to, "%Y-%m-%d") : g_strdup("open");
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
				"VentureSalesTaxService: %s already has a rate window %s to %s; close it before opening another",
				code, from_text, to_text);
			return FALSE;
		}
	}
	return TRUE;
}

static gboolean
check_rule(VentureDatabase *database, VentureEntity *record, GError **error)
{
	g_autofree gchar *raw_state = string_of(record, "state");
	g_autofree gchar *state = normalise(raw_state);
	g_autoptr(VentureEntity) jurisdiction = NULL;
	gint64 jurisdiction_id = id_of(record, "jurisdiction-id");

	if (state == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "A tax rule needs a state code");
	if (jurisdiction_id <= 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "A tax rule needs a jurisdiction");
	jurisdiction = fetch(database, VENTURE_TYPE_TAX_JURISDICTION, jurisdiction_id, "tax_jurisdiction", error);
	if (jurisdiction == NULL)
		return FALSE;
	if (venture_entity_get_organization_id(jurisdiction) != venture_entity_get_organization_id(record))
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED,
			"A tax rule and its jurisdiction belong to the same organization");
	return TRUE;
}

gboolean
venture_sales_tax_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error)
{
	if (record == NULL || database == NULL || removal)
		return TRUE;
	if (VENTURE_IS_TAX_JURISDICTION(record))
		return check_jurisdiction(database, record, error);
	if (VENTURE_IS_TAX_RULE(record))
		return check_rule(database, record, error);
	return TRUE;
}

/* ==========================================================================
 * The return
 * ========================================================================== */

typedef struct
{
	gchar *code;
	gchar *name;
	gchar *kind;
	gint64 rate;
	gboolean has_rate;
	VentureMoney *gross;
	VentureMoney *exempt;
	VentureMoney *collected;
	VentureMoney *credited;
} ReturnRow;

static void
return_row_free(gpointer data)
{
	ReturnRow *row = data;
	g_free(row->code);
	g_free(row->name);
	g_free(row->kind);
	g_clear_pointer(&row->gross, venture_money_free);
	g_clear_pointer(&row->exempt, venture_money_free);
	g_clear_pointer(&row->collected, venture_money_free);
	g_clear_pointer(&row->credited, venture_money_free);
	g_free(row);
}

static gboolean
add_signed(VentureMoney **total, const VentureMoney *amount, gboolean reversal, GError **error)
{
	g_autoptr(VentureMoney) signed_amount = NULL;
	VentureMoney *next;
	if (amount == NULL)
		return TRUE;
	signed_amount = venture_money_multiply_rational(amount, reversal ? -1 : 1, 1, error);
	if (signed_amount == NULL)
		return FALSE;
	next = venture_money_add(*total, signed_amount, error);
	if (next == NULL)
		return FALSE;
	venture_money_free(*total);
	*total = next;
	return TRUE;
}

static ReturnRow *
row_for(GHashTable *rows, const gchar *code, const gchar *name, const gchar *kind, const gchar *currency)
{
	ReturnRow *row = g_hash_table_lookup(rows, code);
	if (row == NULL)
	{
		row = g_new0(ReturnRow, 1);
		row->code = g_strdup(code);
		row->name = g_strdup(name != NULL ? name : "");
		row->kind = g_strdup(kind != NULL ? kind : "");
		row->gross = venture_money_new_zero(currency);
		row->exempt = venture_money_new_zero(currency);
		row->collected = venture_money_new_zero(currency);
		row->credited = venture_money_new_zero(currency);
		g_hash_table_insert(rows, g_strdup(code), row);
	}
	return row;
}

/* A frozen jurisdiction names its return row; a tax code's jurisdiction
 * string is the row for lines that bypassed the rules; everything else is
 * "unassigned" so percent-only tax stays visible to the filer. */
static ReturnRow *
row_for_jurisdiction(GHashTable *rows, GHashTable *cache, VentureDatabase *db,
	gint64 jurisdiction_id, gint64 tax_code_id, const gchar *currency, GError **error)
{
	if (jurisdiction_id > 0)
	{
		VentureEntity *jurisdiction = g_hash_table_lookup(cache, &jurisdiction_id);
		g_autofree gchar *code = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *kind = NULL;
		ReturnRow *row;
		if (jurisdiction == NULL)
		{
			gint64 *key = g_new(gint64, 1);
			*key = jurisdiction_id;
			jurisdiction = fetch(db, VENTURE_TYPE_TAX_JURISDICTION, jurisdiction_id, "tax_jurisdiction", error);
			if (jurisdiction == NULL)
			{
				g_free(key);
				return NULL;
			}
			g_hash_table_insert(cache, key, jurisdiction);
		}
		g_object_get(jurisdiction, "code", &code, "name", &name, "kind", &kind, NULL);
		row = row_for(rows, code, name, venture_string_is_empty(kind) ? "sales" : kind, currency);
		if (!row->has_rate)
		{
			g_object_get(jurisdiction, "rate-scaled", &row->rate, NULL);
			row->has_rate = TRUE;
		}
		return row;
	}
	if (tax_code_id > 0)
	{
		g_autoptr(VentureEntity) tax_code = fetch(db, VENTURE_TYPE_TAX_CODE, tax_code_id, "tax_code", error);
		g_autofree gchar *code = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *jurisdiction = NULL;
		if (tax_code == NULL)
			return NULL;
		g_object_get(tax_code, "code", &code, "name", &name, "jurisdiction", &jurisdiction, NULL);
		return row_for(rows, venture_string_is_empty(jurisdiction) ? code : jurisdiction, name, "tax_code", currency);
	}
	return row_for(rows, "unassigned", "No jurisdiction", "", currency);
}

static gint64
option_id(JsonObject *options, const gchar *name)
{
	return options != NULL ? venture_json_object_get_int(options, name, 0) : 0;
}

static GDateTime *
period_cutoff(VentureDateRange *period, JsonObject *options, GError **error)
{
	GDateTime *end = period != NULL ? venture_date_range_get_end(period) : NULL;
	g_autoptr(GDateTime) now = NULL;
	if (options != NULL && json_object_has_member(options, "as_of"))
	{
		g_autoptr(GDateTime) as_of = venture_period_report_as_of(options, error);
		if (as_of == NULL)
			return NULL;
		now = g_date_time_add(as_of, 1);
	}
	else
		now = venture_time_now();
	return end != NULL && g_date_time_compare(end, now) < 0 ? g_date_time_ref(end) : g_steal_pointer(&now);
}

static gint
compare_codes(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

static VentureReportResult *
sales_tax_return(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GHashTable) rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, return_row_free);
	g_autoptr(GHashTable) cache = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_object_unref);
	g_autoptr(GHashTable) invoices = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_object_unref);
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GPtrArray) credits = NULL;
	g_autoptr(GPtrArray) codes = NULL;
	g_autoptr(VentureQuery) event_query = NULL;
	GDateTime *start = period != NULL ? venture_date_range_get_start(period) : NULL;
	VentureDatabase *db = venture_context_get_database(context);
	const gchar *currency = options != NULL
		? venture_json_object_get_string(options, "currency", venture_money_get_default_currency())
		: venture_money_get_default_currency();
	gint64 organization_id = option_id(options, "organization_id");
	gint64 venture_id = option_id(options, "venture_id");
	GHashTableIter iter;
	gpointer key;
	guint i;

	if (organization_id == 0)
		organization_id = venture_context_get_default_organization_id(context);
	end = period_cutoff(period, options, error);
	if (end == NULL)
		return NULL;
	/* Issue and void events date the frozen lines, exactly as tax_liability
	 * does, so the two reports cannot disagree about which month a sale is in. */
	event_query = venture_query_new(VENTURE_TYPE_INVOICE_EVENT);
	venture_query_set_limit(event_query, 0);
	venture_query_set_include_deleted(event_query, TRUE);
	venture_query_set_organization(event_query, organization_id);
	if (venture_id != 0 &&
		!venture_query_add_filter_int(event_query, "venture-id", VENTURE_FILTER_OP_EQ, venture_id, error))
		return NULL;
	events = venture_database_find(db, event_query, error);
	if (events == NULL)
		return NULL;
	for (i = 0; i < events->len; i++)
	{
		VentureEntity *event = g_ptr_array_index(events, i);
		g_autofree gchar *kind = NULL;
		g_autoptr(GDateTime) date = NULL;
		g_autoptr(GDateTime) opening_at = NULL;
		g_autoptr(VentureQuery) line_query = NULL;
		g_autoptr(GPtrArray) lines = NULL;
		VentureEntity *invoice;
		gint64 invoice_id = 0;
		gboolean reversal;
		gboolean invoice_exempt = FALSE;
		guint j;

		g_object_get(event, "kind", &kind, "date", &date, "invoice-id", &invoice_id, NULL);
		if ((g_strcmp0(kind, "issue") != 0 && g_strcmp0(kind, "void") != 0) ||
			date == NULL || g_date_time_compare(date, end) >= 0 ||
			(start != NULL && g_date_time_compare(date, start) < 0))
			continue;
		reversal = g_strcmp0(kind, "void") == 0;
		invoice = g_hash_table_lookup(invoices, &invoice_id);
		if (invoice == NULL)
		{
			gint64 *invoice_key = g_new(gint64, 1);
			*invoice_key = invoice_id;
			invoice = fetch(db, VENTURE_TYPE_INVOICE, invoice_id, "invoice", error);
			if (invoice == NULL)
			{
				g_free(invoice_key);
				return NULL;
			}
			g_hash_table_insert(invoices, invoice_key, invoice);
		}
		/* A migrated opening invoice's tax was collected, and possibly
		 * filed, in the source system; tax_liability and the filing adapter
		 * both skip it, and a return that did not would ask the filer to
		 * remit it twice. */
		g_object_get(invoice, "opening-at", &opening_at, "tax-exempt", &invoice_exempt, NULL);
		if (opening_at != NULL)
			continue;
		line_query = venture_query_new(VENTURE_TYPE_INVOICE_LINE);
		venture_query_set_limit(line_query, 0);
		venture_query_set_organization(line_query, organization_id);
		if (!venture_query_add_filter_int(line_query, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, error))
			return NULL;
		lines = venture_database_find(db, line_query, error);
		if (lines == NULL)
			return NULL;
		for (j = 0; j < lines->len; j++)
		{
			VentureEntity *line = g_ptr_array_index(lines, j);
			g_autoptr(VentureMoney) income = NULL;
			g_autoptr(VentureMoney) tax = NULL;
			gint64 jurisdiction_id = 0, tax_code_id = 0;
			gboolean line_exempt = FALSE;
			ReturnRow *row;

			g_object_get(line, "income-amount", &income, "tax-amount", &tax,
				"tax-jurisdiction-id", &jurisdiction_id, "tax-code-id", &tax_code_id,
				"tax-exempt", &line_exempt, NULL);
			row = row_for_jurisdiction(rows, cache, db, jurisdiction_id, tax_code_id, currency, error);
			if (row == NULL)
				return NULL;
			if (!add_signed(&row->gross, income, reversal, error) ||
				((line_exempt || invoice_exempt) && !add_signed(&row->exempt, income, reversal, error)) ||
				!add_signed(&row->collected, tax, reversal, error))
				return NULL;
		}
	}
	/* Credit notes carry frozen tax and, when the operator names it, the
	 * jurisdiction being credited. A venture filter cannot attribute
	 * customer-level credits, so they are left out under one, as in
	 * tax_liability. */
	if (venture_id == 0)
	{
		g_autoptr(VentureQuery) credit_query = venture_query_new(VENTURE_TYPE_CUSTOMER_CREDIT);
		g_autofree gchar *end_text = g_date_time_format_iso8601(end);
		venture_query_set_limit(credit_query, 0);
		venture_query_set_include_deleted(credit_query, TRUE);
		venture_query_set_organization(credit_query, organization_id);
		if (!venture_query_add_filter_string(credit_query, "date", VENTURE_FILTER_OP_LT, end_text, error))
			return NULL;
		credits = venture_database_find(db, credit_query, error);
		if (credits == NULL)
			return NULL;
		for (i = 0; i < credits->len; i++)
		{
			VentureEntity *credit = g_ptr_array_index(credits, i);
			g_autoptr(GDateTime) date = NULL;
			g_autofree gchar *kind = NULL;
			g_autoptr(VentureMoney) tax = NULL;
			gint64 jurisdiction_id = 0;
			ReturnRow *row;

			g_object_get(credit, "date", &date, "kind", &kind, "tax-amount", &tax,
				"tax-jurisdiction-id", &jurisdiction_id, NULL);
			if (g_strcmp0(kind, "credit_note") != 0 || tax == NULL || venture_money_is_zero(tax) ||
				date == NULL || (start != NULL && g_date_time_compare(date, start) < 0))
				continue;
			row = row_for_jurisdiction(rows, cache, db, jurisdiction_id, 0, currency, error);
			if (row == NULL || !add_signed(&row->credited, tax, FALSE, error))
				return NULL;
		}
	}
	result = venture_report_result_new("Sales tax return by jurisdiction", period);
	venture_report_result_add_column(result, "jurisdiction", "Jurisdiction", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "name", "Name", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "kind", "Kind", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "rate_percent", "Rate %", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "gross_sales", "Gross sales", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "exempt_sales", "Exempt sales", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "taxable_sales", "Taxable sales", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "tax_collected", "Tax collected", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "tax_credited", "Tax credited", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "net_due", "Net due", VENTURE_REPORT_COLUMN_MONEY);
	codes = g_ptr_array_new();
	g_hash_table_iter_init(&iter, rows);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		g_ptr_array_add(codes, key);
	g_ptr_array_sort(codes, compare_codes);
	for (i = 0; i < codes->len; i++)
	{
		ReturnRow *row = g_hash_table_lookup(rows, g_ptr_array_index(codes, i));
		g_autoptr(VentureMoney) taxable = venture_money_subtract(row->gross, row->exempt, error);
		g_autoptr(VentureMoney) net_due = NULL;
		g_autofree gchar *rate = row->has_rate ? venture_tax_rate_percent_string(row->rate) : g_strdup("");
		if (taxable == NULL)
			return NULL;
		net_due = venture_money_subtract(row->collected, row->credited, error);
		if (net_due == NULL)
			return NULL;
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "jurisdiction", row->code);
		venture_report_result_set_text(result, "name", row->name);
		venture_report_result_set_text(result, "kind", row->kind);
		venture_report_result_set_text(result, "rate_percent", rate);
		venture_report_result_set_money(result, "gross_sales", row->gross);
		venture_report_result_set_money(result, "exempt_sales", row->exempt);
		venture_report_result_set_money(result, "taxable_sales", taxable);
		venture_report_result_set_money(result, "tax_collected", row->collected);
		venture_report_result_set_money(result, "tax_credited", row->credited);
		venture_report_result_set_money(result, "net_due", net_due);
	}
	return g_steal_pointer(&result);
}

void
venture_sales_tax_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"sales_tax_return", "Sales tax return by jurisdiction",
		"Gross, exempt and taxable sales, tax collected and credited, and net due per jurisdiction from frozen invoice lines.",
		sales_tax_return)));
}

/* ==========================================================================
 * CSV in the 1099 pack's shape
 * ========================================================================== */

static const gchar *const csv_money_keys[] = {
	"gross_sales", "exempt_sales", "taxable_sales", "tax_collected", "tax_credited", "net_due"
};

gchar *
venture_sales_tax_service_export_csv(VentureSalesTaxService *self, VentureContext *context,
	VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GString) text = NULL;
	g_autofree gchar *start_text = NULL;
	g_autofree gchar *end_text = NULL;
	VentureReport *report;
	const gchar *only = options != NULL ? venture_json_object_get_string(options, "jurisdiction", NULL) : NULL;
	guint i, k;

	g_return_val_if_fail(VENTURE_IS_SALES_TAX_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(period != NULL, NULL);
	if (!module_on())
		return refuse(error, VENTURE_ERROR_CONFIG,
			"The sales_tax module is disabled (modules.sales_tax.enabled)"), NULL;
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), NULL;
	if (!venture_string_is_empty(only))
	{
		gint64 organization_id = option_id(options, "organization_id");
		g_autoptr(GPtrArray) windows = NULL;
		if (organization_id == 0)
			organization_id = venture_context_get_default_organization_id(context);
		windows = find_all(db, VENTURE_TYPE_TAX_JURISDICTION, organization_id, "code", only, error);
		if (windows == NULL)
			return NULL;
		if (windows->len == 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
				"VentureSalesTaxService: there is no jurisdiction coded %s", only);
			return NULL;
		}
	}
	report = venture_report_registry_lookup(venture_context_get_report_registry(context), "sales_tax_return");
	if (report == NULL)
		return refuse(error, VENTURE_ERROR_CONFIG, "The sales_tax_return report is not registered"), NULL;
	result = venture_report_generate(report, context, period, options, error);
	if (result == NULL)
		return NULL;
	start_text = g_date_time_format(venture_date_range_get_start(period), "%Y-%m-%d");
	end_text = g_date_time_format(venture_date_range_get_end(period), "%Y-%m-%d");
	text = g_string_new("report,period_start,period_end,jurisdiction,name,kind,rate_percent,"
		"gross_sales,exempt_sales,taxable_sales,tax_collected,tax_credited,net_due\n");
	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		static const gchar *const text_keys[] = { "jurisdiction", "name", "kind", "rate_percent" };
		const gchar *code = g_value_get_string(venture_report_result_get_cell(result, i, "jurisdiction"));
		g_autofree gchar *report_cell = venture_tax_filing_csv_cell("sales_tax_return");
		g_autofree gchar *start_cell = venture_tax_filing_csv_cell(start_text);
		g_autofree gchar *end_cell = venture_tax_filing_csv_cell(end_text);
		if (!venture_string_is_empty(only) && g_strcmp0(code, only) != 0)
			continue;
		g_string_append_printf(text, "%s,%s,%s", report_cell, start_cell, end_cell);
		for (k = 0; k < G_N_ELEMENTS(text_keys); k++)
		{
			g_autofree gchar *cell = venture_tax_filing_csv_cell(
				g_value_get_string(venture_report_result_get_cell(result, i, text_keys[k])));
			g_string_append_c(text, ',');
			g_string_append(text, cell);
		}
		for (k = 0; k < G_N_ELEMENTS(csv_money_keys); k++)
		{
			const VentureMoney *money = g_value_get_boxed(venture_report_result_get_cell(result, i, csv_money_keys[k]));
			g_autofree gchar *money_text = venture_money_to_string(money);
			g_autofree gchar *cell = venture_tax_filing_csv_cell(money_text);
			g_string_append_c(text, ',');
			g_string_append(text, cell);
		}
		g_string_append_c(text, '\n');
	}
	return g_string_free(g_steal_pointer(&text), FALSE);
}
