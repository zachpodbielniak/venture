/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
#include <json-glib/json-glib.h>

struct _VentureTaxFilingService
{
	GObject parent_instance;
	GWeakRef database;
	VentureTaxFilingAdapterRegistry *adapters;
	VentureEntity *writing;
	gboolean busy;
};

G_DEFINE_FINAL_TYPE(VentureTaxFilingService, venture_tax_filing_service, G_TYPE_OBJECT)

static void
service_finalize(GObject *object)
{
	VentureTaxFilingService *self = VENTURE_TAX_FILING_SERVICE(object);
	g_weak_ref_clear(&self->database);
	g_clear_object(&self->adapters);
	G_OBJECT_CLASS(venture_tax_filing_service_parent_class)->finalize(object);
}

static void
venture_tax_filing_service_class_init(VentureTaxFilingServiceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = service_finalize;
}

static void
venture_tax_filing_service_init(VentureTaxFilingService *self)
{
	g_weak_ref_init(&self->database, NULL);
	self->adapters = venture_tax_filing_adapter_registry_new();
	venture_tax_filing_adapter_registry_add(self->adapters, venture_us_sales_tax_adapter_new());
}

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureTaxFilingService: %s", message);
	return FALSE;
}

static gboolean
module_on(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"tax_filing") != G_TYPE_INVALID;
}

VentureTaxFilingService *
venture_tax_filing_service_get(VentureDatabase *database)
{
	VentureTaxFilingService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-tax-filing-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_TAX_FILING_SERVICE, NULL);
		g_weak_ref_set(&self->database, database);
		g_object_set_data_full(G_OBJECT(database), "venture-tax-filing-service", self, g_object_unref);
	}
	return self;
}

VentureTaxFilingAdapterRegistry *
venture_tax_filing_service_get_adapters(VentureTaxFilingService *self)
{
	g_return_val_if_fail(VENTURE_IS_TAX_FILING_SERVICE(self), NULL);
	return self->adapters;
}

static VentureDatabase *
service_db(VentureTaxFilingService *self)
{
	return g_weak_ref_get(&self->database);
}

static gboolean
save_internal(VentureTaxFilingService *self, VentureDatabase *db, VentureEntity *entity,
	const VentureActor *actor, GError **error)
{
	VentureEntity *previous = self->writing;
	gboolean ok;
	self->writing = entity;
	ok = venture_database_save(db, entity, actor, error);
	self->writing = previous;
	return ok;
}

static gboolean
begin_op(VentureTaxFilingService *self, VentureDatabase *db, GError **error)
{
	if (!module_on())
		return refuse(error, VENTURE_ERROR_CONFIG,
			"The tax_filing module is disabled (modules.tax_filing.enabled)");
	if (self->busy)
		return refuse(error, VENTURE_ERROR_CONFLICT, "A tax filing operation is already in progress");
	if (!venture_database_begin(db, error))
		return FALSE;
	self->busy = TRUE;
	return TRUE;
}

static gboolean
finish_op(VentureTaxFilingService *self, VentureDatabase *db, gboolean ok, GError **error)
{
	if (ok)
		ok = venture_database_commit(db, error);
	else
		venture_database_rollback(db);
	self->busy = FALSE;
	return ok;
}

static gchar *
status_of(VentureEntity *entity)
{
	gchar *status = NULL;
	g_object_get(entity, "status", &status, NULL);
	return status;
}

static gboolean
require_status(VentureEntity *entity, const gchar *wanted, GError **error)
{
	g_autofree gchar *status = status_of(entity);
	if (g_strcmp0(status, wanted) == 0)
		return TRUE;
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		"VentureTaxFilingService: expected status %s", wanted);
	return FALSE;
}

static gboolean
load_period_dates(VentureDatabase *db, gint64 period_id, GDateTime **start, GDateTime **end, GError **error)
{
	g_autoptr(VentureEntity) period = NULL;
	if (period_id <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureTaxFilingService: a fiscal period or date range is required");
		return FALSE;
	}
	period = venture_database_get(db, VENTURE_TYPE_FISCAL_PERIOD, period_id, error);
	if (period == NULL)
		return FALSE;
	g_object_get(period, "start-at", start, "end-at", end, NULL);
	return *start != NULL && *end != NULL;
}

VentureEntity *
venture_tax_filing_service_prepare(VentureTaxFilingService *self, gint64 organization_id,
	const gchar *country, const gchar *jurisdiction, gint64 fiscal_period_id,
	GDateTime *period_start, GDateTime *period_end, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureEntity) filing = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autofree gchar *name = NULL;
	VentureTaxFilingAdapter *adapter;
	const gchar *place;
	g_return_val_if_fail(VENTURE_IS_TAX_FILING_SERVICE(self), NULL);
	place = (jurisdiction != NULL && *jurisdiction != '\0') ? jurisdiction :
		(country != NULL && *country != '\0') ? country : "(unnamed)";
	if (country == NULL || *country == '\0')
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureTaxFilingService: Missing country for jurisdiction %s", place);
		return NULL;
	}
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), NULL;
	adapter = venture_tax_filing_adapter_registry_lookup(self->adapters, country);
	if (adapter == NULL)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"VentureTaxFilingService: No tax filing adapter for jurisdiction %s", place);
		return NULL;
	}
	if (period_start != NULL && period_end != NULL)
	{
		start = g_date_time_ref(period_start);
		end = g_date_time_ref(period_end);
	}
	else if (!load_period_dates(db, fiscal_period_id, &start, &end, error))
		return NULL;
	if (!begin_op(self, db, error))
		return NULL;
	{
		g_autofree gchar *stamp = g_date_time_format(start, "%Y-%m");
		name = g_strdup_printf("%s %s", place, stamp != NULL ? stamp : "");
	}
	filing = VENTURE_ENTITY(venture_tax_filing_new());
	g_object_set(filing, "name", name, "country", country,
		"jurisdiction", jurisdiction != NULL && *jurisdiction != '\0' ? jurisdiction : country,
		"fiscal-period-id", fiscal_period_id, "period-start", start, "period-end", end,
		"adapter", country, "rule-id", venture_tax_filing_adapter_get_rule_id(adapter),
		"status", "draft", "currency", "USD", NULL);
	venture_entity_set_organization_id(filing, organization_id);
	if (!venture_tax_filing_adapter_prepare(adapter, db, filing, error) ||
		!save_internal(self, db, filing, actor, error))
		return finish_op(self, db, FALSE, error), NULL;
	if (!finish_op(self, db, TRUE, error))
		return NULL;
	return g_steal_pointer(&filing);
}

static gboolean
transition(VentureTaxFilingService *self, VentureEntity *filing, const gchar *from,
	const gchar *to, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_return_val_if_fail(VENTURE_IS_TAX_FILING_SERVICE(self), FALSE);
	if (!VENTURE_IS_TAX_FILING(filing))
		return refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "A tax_filing record is required");
	if (!require_status(filing, from, error))
		return FALSE;
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed");
	if (!begin_op(self, db, error))
		return FALSE;
	g_object_set(filing, "status", to, NULL);
	if (!save_internal(self, db, filing, actor, error))
		return finish_op(self, db, FALSE, error);
	return finish_op(self, db, TRUE, error);
}

gboolean
venture_tax_filing_service_review(VentureTaxFilingService *self, VentureEntity *filing,
	const VentureActor *actor, GError **error)
{
	return transition(self, filing, "draft", "reviewed", actor, error);
}

gboolean
venture_tax_filing_service_submit(VentureTaxFilingService *self, VentureEntity *filing,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *json = NULL;
	g_autofree gchar *csv = NULL;
	if (!VENTURE_IS_TAX_FILING(filing))
		return refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "A tax_filing record is required");
	g_object_get(filing, "json-pack", &json, "csv-pack", &csv, NULL);
	if (json == NULL || *json == '\0' || csv == NULL)
		return refuse(error, VENTURE_ERROR_VALIDATION, "A prepared JSON and CSV pack is required to submit");
	return transition(self, filing, "reviewed", "submitted", actor, error);
}

gboolean
venture_tax_filing_service_acknowledge(VentureTaxFilingService *self, VentureEntity *filing,
	const gchar *acknowledgment_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	if (!VENTURE_IS_TAX_FILING(filing))
		return refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "A tax_filing record is required");
	if (acknowledgment_id == NULL || *acknowledgment_id == '\0')
		return refuse(error, VENTURE_ERROR_VALIDATION, "An acknowledgment id is required");
	if (!require_status(filing, "submitted", error))
		return FALSE;
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed");
	if (!begin_op(self, db, error))
		return FALSE;
	g_object_set(filing, "status", "acknowledged", "acknowledgment-id", acknowledgment_id, NULL);
	if (!save_internal(self, db, filing, actor, error))
		return finish_op(self, db, FALSE, error);
	return finish_op(self, db, TRUE, error);
}

VentureEntity *
venture_tax_filing_service_amend(VentureTaxFilingService *self, VentureEntity *filing,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *country = NULL;
	g_autofree gchar *jurisdiction = NULL;
	g_autofree gchar *status = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	gint64 period_id = 0;
	g_return_val_if_fail(VENTURE_IS_TAX_FILING_SERVICE(self), NULL);
	if (!VENTURE_IS_TAX_FILING(filing))
		return refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "A tax_filing record is required"), NULL;
	g_object_get(filing, "status", &status, "country", &country, "jurisdiction", &jurisdiction,
		"fiscal-period-id", &period_id, "period-start", &start, "period-end", &end, NULL);
	if (g_strcmp0(status, "submitted") != 0 && g_strcmp0(status, "acknowledged") != 0)
		return refuse(error, VENTURE_ERROR_CONFLICT, "Only submitted or acknowledged packs can be amended"), NULL;
	{
		g_autoptr(VentureEntity) next = venture_tax_filing_service_prepare(self,
			venture_entity_get_organization_id(filing), country, jurisdiction, period_id,
			start, end, actor, error);
		if (next == NULL)
			return NULL;
		g_object_set(next, "amended-from-id", venture_entity_get_id(filing), NULL);
		{
			g_autoptr(VentureDatabase) db = service_db(self);
			if (db == NULL || !begin_op(self, db, error))
				return NULL;
			if (!save_internal(self, db, next, actor, error))
				return finish_op(self, db, FALSE, error), NULL;
			if (!finish_op(self, db, TRUE, error))
				return NULL;
		}
		return g_steal_pointer(&next);
	}
}

static VentureEntity *
find_form(VentureDatabase *db, gint64 org, gint64 vendor_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTRACTOR_TAX_FORM);
	venture_query_set_organization(query, org);
	if (!venture_query_add_filter_int(query, "vendor-id", VENTURE_FILTER_OP_EQ, vendor_id, error))
		return NULL;
	return venture_database_find_one(db, query, error);
}

static VentureEntity *
find_pack(VentureDatabase *db, gint64 org, const gchar *key, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTRACTOR_TAX_PACK);
	venture_query_set_organization(query, org);
	if (!venture_query_add_filter_string(query, "filing-key", VENTURE_FILTER_OP_EQ, key, error))
		return NULL;
	return venture_database_find_one(db, query, error);
}

static gboolean
year_bounds(gint year, GDateTime **start, GDateTime **end, GError **error)
{
	if (year < 1900 || year > 9999)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"VentureTaxFilingService: a calendar year is required");
		return FALSE;
	}
	*start = g_date_time_new_utc(year, 1, 1, 0, 0, 0);
	*end = g_date_time_new_utc(year + 1, 1, 1, 0, 0, 0);
	return *start != NULL && *end != NULL;
}

static VentureMoney *
paid_in_year(VentureDatabase *db, gint64 org, gint64 vendor_id, gint year, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) payments = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(VentureMoney) total = NULL;
	guint i;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "bill_payment") == G_TYPE_INVALID)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			"VentureTaxFilingService: paid contractor totals require the payables module");
		return NULL;
	}
	if (!year_bounds(year, &start, &end, error))
		return NULL;
	query = venture_query_new(VENTURE_TYPE_BILL_PAYMENT);
	venture_query_set_limit(query, 0);
	venture_query_set_organization(query, org);
	if (!venture_query_add_filter_int(query, "vendor-id", VENTURE_FILTER_OP_EQ, vendor_id, error))
		return NULL;
	payments = venture_database_find(db, query, error);
	if (payments == NULL)
		return NULL;
	for (i = 0; i < payments->len; i++)
	{
		VentureEntity *payment = g_ptr_array_index(payments, i);
		g_autoptr(GDateTime) when = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) next = NULL;
		if (venture_entity_is_deleted(payment))
			continue;
		g_object_get(payment, "date", &when, "amount", &amount, NULL);
		if (when == NULL || amount == NULL)
			continue;
		if (g_date_time_compare(when, start) < 0 || g_date_time_compare(when, end) >= 0)
			continue;
		if (total == NULL)
			total = venture_money_copy(amount);
		else
		{
			next = venture_money_add(total, amount, error);
			if (next == NULL)
				return NULL;
			g_clear_pointer(&total, venture_money_free);
			total = g_steal_pointer(&next);
		}
	}
	return g_steal_pointer(&total);
}

static gchar *
csv_cell(const gchar *value)
{
	GString *cell = g_string_new("\"");
	const gchar *p;
	/* RFC 4180 quoting preserves commas, line breaks and literal quotation marks. */
	for (p = value != NULL ? value : ""; *p != '\0'; p++)
	{
		if (*p == '"')
			g_string_append_c(cell, '"');
		g_string_append_c(cell, *p);
	}
	g_string_append_c(cell, '"');
	return g_string_free(cell, FALSE);
}

static gchar *
build_1099_csv(VentureEntity *form, VentureEntity *vendor, gint year, const VentureMoney *amount)
{
	g_autofree gchar *tin = NULL;
	g_autofree gchar *legal = NULL;
	g_autofree gchar *vendor_name = NULL;
	g_autofree gchar *money = amount != NULL ? venture_money_to_string(amount) : g_strdup("0 USD");
	g_autofree gchar *legal_cell = NULL;
	g_autofree gchar *tin_cell = NULL;
	g_autofree gchar *amount_cell = NULL;
	g_object_get(form, "tin", &tin, "legal-name", &legal, NULL);
	g_object_get(vendor, "name", &vendor_name, NULL);
	legal_cell = csv_cell(legal != NULL && *legal != '\0' ? legal : vendor_name);
	tin_cell = csv_cell(tin);
	amount_cell = csv_cell(money);
	return g_strdup_printf("form,year,vendor,tin,amount\n1099-NEC,%d,%s,%s,%s\n",
		year, legal_cell, tin_cell, amount_cell);
}

VentureEntity *
venture_tax_filing_service_prepare_1099(VentureTaxFilingService *self, gint64 organization_id,
	gint64 vendor_id, gint year, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureEntity) form = NULL;
	g_autoptr(VentureEntity) existing = NULL;
	g_autoptr(VentureEntity) pack = NULL;
	g_autoptr(VentureEntity) vendor = NULL;
	g_autoptr(VentureMoney) paid = NULL;
	g_autoptr(VentureMoney) threshold = NULL;
	g_autofree gchar *key = NULL;
	g_return_val_if_fail(VENTURE_IS_TAX_FILING_SERVICE(self), NULL);
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), NULL;
	if (!module_on())
		return refuse(error, VENTURE_ERROR_CONFIG,
			"The tax_filing module is disabled (modules.tax_filing.enabled)"), NULL;
	/* IRS 2026 instructions raise NEC's general threshold; later inflation values need a rule update. */
	if (year < 2020 || year > 2026)
		return refuse(error, VENTURE_ERROR_VALIDATION,
			"1099-NEC bookkeeping rules support 2020 through 2026; update the year-specific threshold for later years"), NULL;
	key = g_strdup_printf("%" G_GINT64_FORMAT ":%d:1099-NEC", vendor_id, year);
	existing = find_pack(db, organization_id, key, error);
	if (error != NULL && *error != NULL)
		return NULL;
	if (existing != NULL)
		return g_steal_pointer(&existing);
	form = find_form(db, organization_id, vendor_id, error);
	if (error != NULL && *error != NULL)
		return NULL;
	if (form == NULL)
		return refuse(error, VENTURE_ERROR_NOT_FOUND,
			"A contractor tax form is required before preparing 1099-NEC"), NULL;
	paid = paid_in_year(db, organization_id, vendor_id, year, error);
	if (paid == NULL && (error == NULL || *error == NULL))
		paid = venture_money_from_string("0 USD", "USD", NULL);
	if (paid == NULL)
		return NULL;
	if (g_strcmp0(venture_money_get_currency(paid), "USD") != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "1099-NEC bookkeeping totals must be in USD"), NULL;
	threshold = venture_money_from_string(year == 2026 ? "2000 USD" : "600 USD", "USD", error);
	if (threshold == NULL)
		return NULL;
	if (venture_money_compare(paid, threshold) < 0)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"1099-NEC bookkeeping pack requires calendar-year payments of at least %s USD for %d",
			year == 2026 ? "2000" : "600", year);
		return NULL;
	}
	vendor = venture_database_get(db, VENTURE_TYPE_COMPANY, vendor_id, error);
	if (vendor == NULL)
		return NULL;
	if (!begin_op(self, db, error))
		return NULL;
	pack = VENTURE_ENTITY(venture_contractor_tax_pack_new());
	g_object_set(pack, "vendor-id", vendor_id, "form-id", venture_entity_get_id(form),
		"year", (gint64)year, "form-kind", "1099-NEC", "filing-key", key,
		"status", "draft", "amount", paid, NULL);
	venture_entity_set_organization_id(pack, organization_id);
	if (!save_internal(self, db, pack, actor, error))
		return finish_op(self, db, FALSE, error), NULL;
	if (!finish_op(self, db, TRUE, error))
		return NULL;
	return g_steal_pointer(&pack);
}

static gboolean
transition_1099(VentureTaxFilingService *self, VentureEntity *pack, const gchar *from,
	const gchar *to, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	if (!VENTURE_IS_CONTRACTOR_TAX_PACK(pack))
		return refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "A contractor_tax_pack is required");
	if (!require_status(pack, from, error))
		return FALSE;
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed");
	if (!begin_op(self, db, error))
		return FALSE;
	g_object_set(pack, "status", to, NULL);
	if (!save_internal(self, db, pack, actor, error))
		return finish_op(self, db, FALSE, error);
	return finish_op(self, db, TRUE, error);
}

gboolean
venture_tax_filing_service_review_1099(VentureTaxFilingService *self, VentureEntity *pack,
	const VentureActor *actor, GError **error)
{
	return transition_1099(self, pack, "draft", "reviewed", actor, error);
}

gboolean
venture_tax_filing_service_approve_1099(VentureTaxFilingService *self, VentureEntity *pack,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureEntity) form = NULL;
	g_autoptr(VentureEntity) vendor = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *csv = NULL;
	gint64 form_id = 0;
	gint64 vendor_id = 0;
	gint64 year = 0;
	if (!VENTURE_IS_CONTRACTOR_TAX_PACK(pack))
		return refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "A contractor_tax_pack is required");
	if (!require_status(pack, "reviewed", error))
		return FALSE;
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed");
	g_object_get(pack, "form-id", &form_id, "vendor-id", &vendor_id, "year", &year, "amount", &amount, NULL);
	form = venture_database_get(db, VENTURE_TYPE_CONTRACTOR_TAX_FORM, form_id, error);
	if (form == NULL)
		return FALSE;
	vendor = venture_database_get(db, VENTURE_TYPE_COMPANY, vendor_id, error);
	if (vendor == NULL)
		return FALSE;
	csv = build_1099_csv(form, vendor, (gint)year, amount);
	if (!begin_op(self, db, error))
		return FALSE;
	g_object_set(pack, "status", "approved", "csv-pack", csv, NULL);
	if (!save_internal(self, db, pack, actor, error))
		return finish_op(self, db, FALSE, error);
	return finish_op(self, db, TRUE, error);
}

gchar *
venture_tax_filing_service_export_1099(VentureTaxFilingService *self, VentureEntity *pack,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *status = NULL;
	g_autofree gchar *csv = NULL;
	g_autoptr(VentureDatabase) db = NULL;
	if (!VENTURE_IS_CONTRACTOR_TAX_PACK(pack))
		return refuse(error, VENTURE_ERROR_INVALID_ARGUMENT, "A contractor_tax_pack is required"), NULL;
	g_object_get(pack, "status", &status, "csv-pack", &csv, NULL);
	if (g_strcmp0(status, "exported") == 0 && csv != NULL)
		return g_steal_pointer(&csv);
	if (g_strcmp0(status, "approved") != 0)
		return refuse(error, VENTURE_ERROR_CONFLICT,
			"Review and approve the 1099-NEC pack before export"), NULL;
	if (csv == NULL || *csv == '\0')
		return refuse(error, VENTURE_ERROR_VALIDATION, "The approved pack has no CSV"), NULL;
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), NULL;
	if (!begin_op(self, db, error))
		return NULL;
	g_object_set(pack, "status", "exported", NULL);
	/* A stale export must release the transaction and the service's reentrancy guard. */
	if (!save_internal(self, db, pack, actor, error))
		return finish_op(self, db, FALSE, error), NULL;
	if (!finish_op(self, db, TRUE, error))
		return NULL;
	return g_steal_pointer(&csv);
}

gboolean
venture_tax_filing_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error)
{
	VentureTaxFilingService *self;
	if (record == NULL || database == NULL)
		return TRUE;
	if (!VENTURE_IS_TAX_FILING(record) && !VENTURE_IS_CONTRACTOR_TAX_PACK(record) &&
		!VENTURE_IS_CONTRACTOR_TAX_FORM(record))
		return TRUE;
	self = venture_tax_filing_service_get(database);
	if (self->writing == record)
		return TRUE;
	if (removal && (VENTURE_IS_TAX_FILING(record) || VENTURE_IS_CONTRACTOR_TAX_PACK(record)))
	{
		g_autofree gchar *status = status_of(record);
		if (g_strcmp0(status, "submitted") == 0 || g_strcmp0(status, "acknowledged") == 0 ||
			g_strcmp0(status, "exported") == 0)
			return refuse(error, VENTURE_ERROR_PERMISSION_DENIED,
				"Submitted tax packs are retained");
	}
	return TRUE;
}

gboolean
venture_tax_filing_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error)
{
	VentureTaxFilingService *self;
	(void)actor;
	*handled = FALSE;
	if (!VENTURE_IS_TAX_FILING(record) && !VENTURE_IS_CONTRACTOR_TAX_PACK(record))
		return TRUE;
	if (!module_on())
		return refuse(error, VENTURE_ERROR_CONFIG,
			"The tax_filing module is disabled (modules.tax_filing.enabled)");
	self = venture_tax_filing_service_get(database);
	if (self->writing == record)
		return TRUE;
	return refuse(error, VENTURE_ERROR_PERMISSION_DENIED,
		"Tax filing packs are written only by VentureTaxFilingService");
}

static gboolean
filing_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	g_autofree gchar *name = NULL;
	g_autofree gchar *status = NULL;
	(void)actor;
	g_object_get(action, "name", &name, NULL);
	g_object_get(entity, "status", &status, NULL);
	if (g_strcmp0(name, "review") == 0 && g_strcmp0(status, "draft") == 0)
		return TRUE;
	if (g_strcmp0(name, "submit") == 0 && g_strcmp0(status, "reviewed") == 0)
		return TRUE;
	if (g_strcmp0(name, "acknowledge") == 0 && g_strcmp0(status, "submitted") == 0)
		return TRUE;
	if (g_strcmp0(name, "amend") == 0 &&
		(g_strcmp0(status, "submitted") == 0 || g_strcmp0(status, "acknowledged") == 0))
		return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		"VentureTaxFilingService does not allow that action in this state");
	return FALSE;
}

static VentureEntity *
filing_review(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	(void)params;
	if (!venture_tax_filing_service_review(venture_action_get_data(action), entity, actor, error))
		return NULL;
	return g_object_ref(entity);
}
static VentureEntity *
filing_submit(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	(void)params;
	if (!venture_tax_filing_service_submit(venture_action_get_data(action), entity, actor, error))
		return NULL;
	return g_object_ref(entity);
}
static VentureEntity *
filing_ack(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	JsonNode *id = params != NULL ? g_hash_table_lookup(params, "acknowledgment_id") : NULL;
	const gchar *text = id && JSON_NODE_HOLDS_VALUE(id) ? json_node_get_string(id) : NULL;
	if (!venture_tax_filing_service_acknowledge(venture_action_get_data(action), entity, text, actor, error))
		return NULL;
	return g_object_ref(entity);
}
static VentureEntity *
filing_amend(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	(void)params;
	return venture_tax_filing_service_amend(venture_action_get_data(action), entity, actor, error);
}

static gboolean
pack_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	g_autofree gchar *name = NULL;
	g_autofree gchar *status = NULL;
	(void)actor;
	g_object_get(action, "name", &name, NULL);
	g_object_get(entity, "status", &status, NULL);
	if (g_strcmp0(name, "review") == 0 && g_strcmp0(status, "draft") == 0)
		return TRUE;
	if (g_strcmp0(name, "approve") == 0 && g_strcmp0(status, "reviewed") == 0)
		return TRUE;
	if (g_strcmp0(name, "export") == 0 &&
		(g_strcmp0(status, "approved") == 0 || g_strcmp0(status, "exported") == 0))
		return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		"VentureTaxFilingService does not allow that 1099 action in this state");
	return FALSE;
}
static VentureEntity *
pack_review(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	(void)params;
	if (!venture_tax_filing_service_review_1099(venture_action_get_data(action), entity, actor, error))
		return NULL;
	return g_object_ref(entity);
}
static VentureEntity *
pack_approve(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	(void)params;
	if (!venture_tax_filing_service_approve_1099(venture_action_get_data(action), entity, actor, error))
		return NULL;
	return g_object_ref(entity);
}
static VentureEntity *
pack_export(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *csv = NULL;
	(void)params;
	csv = venture_tax_filing_service_export_1099(venture_action_get_data(action), entity, actor, error);
	if (csv == NULL)
		return NULL;
	return g_object_ref(entity);
}

void
venture_tax_filing_actions_register(VentureDatabase *database)
{
	VentureActionRegistry *registry = venture_database_get_action_registry(database);
	VentureTaxFilingService *service = venture_tax_filing_service_get(database);
	g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(VentureAction) review = NULL;
	g_autoptr(VentureAction) submit = NULL;
	g_autoptr(VentureAction) ack = NULL;
	g_autoptr(VentureAction) amend = NULL;
	g_autoptr(VentureAction) preview = NULL;
	g_autoptr(VentureAction) approve = NULL;
	g_autoptr(VentureAction) export_action = NULL;
	g_autoptr(GError) error = NULL;
	VentureFieldSpec *ack_spec;
	review = g_object_new(VENTURE_TYPE_ACTION, "type-name", "tax_filing", "name", "review",
		"label", "Review", "description", "Mark this filing pack reviewed",
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);
	submit = g_object_new(VENTURE_TYPE_ACTION, "type-name", "tax_filing", "name", "submit",
		"label", "Submit", "description", "Preserve JSON and CSV bytes and mark submitted",
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);
	ack_spec = venture_field_spec_new("acknowledgment_id", "Acknowledgment id", VENTURE_FIELD_KIND_STRING);
	ack_spec->required = TRUE;
	g_ptr_array_add(parameters, ack_spec);
	ack = g_object_new(VENTURE_TYPE_ACTION, "type-name", "tax_filing", "name", "acknowledge",
		"label", "Acknowledge", "description", "Store the filing acknowledgment id",
		"parameters", parameters, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	amend = g_object_new(VENTURE_TYPE_ACTION, "type-name", "tax_filing", "name", "amend",
		"label", "Amend", "description", "Prepare a new draft and keep the prior pack",
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);
	preview = g_object_new(VENTURE_TYPE_ACTION, "type-name", "contractor_tax_pack", "name", "review",
		"label", "Review", "description", "Mark this 1099-NEC pack reviewed",
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);
	approve = g_object_new(VENTURE_TYPE_ACTION, "type-name", "contractor_tax_pack", "name", "approve",
		"label", "Approve", "description", "Approve and freeze the 1099-NEC CSV",
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);
	export_action = g_object_new(VENTURE_TYPE_ACTION, "type-name", "contractor_tax_pack", "name", "export",
		"label", "Export", "description", "Export the approved 1099-NEC CSV",
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(registry, review, filing_allowed, filing_review,
			g_object_ref(service), g_object_unref, &error) ||
		!venture_action_registry_register(registry, submit, filing_allowed, filing_submit,
			g_object_ref(service), g_object_unref, &error) ||
		!venture_action_registry_register(registry, ack, filing_allowed, filing_ack,
			g_object_ref(service), g_object_unref, &error) ||
		!venture_action_registry_register(registry, amend, filing_allowed, filing_amend,
			g_object_ref(service), g_object_unref, &error) ||
		!venture_action_registry_register(registry, preview, pack_allowed, pack_review,
			g_object_ref(service), g_object_unref, &error) ||
		!venture_action_registry_register(registry, approve, pack_allowed, pack_approve,
			g_object_ref(service), g_object_unref, &error) ||
		!venture_action_registry_register(registry, export_action, pack_allowed, pack_export,
			g_object_ref(service), g_object_unref, &error))
		g_error("Tax filing action registration: %s", error->message);
}
