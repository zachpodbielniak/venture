/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
#include <json-glib/json-glib.h>

G_DEFINE_INTERFACE(VentureTaxFilingAdapter, venture_tax_filing_adapter, G_TYPE_OBJECT)
static void
venture_tax_filing_adapter_default_init(VentureTaxFilingAdapterInterface *iface)
{
	(void)iface;
}

const gchar *
venture_tax_filing_adapter_get_name(VentureTaxFilingAdapter *self)
{
	g_return_val_if_fail(VENTURE_IS_TAX_FILING_ADAPTER(self), NULL);
	return VENTURE_TAX_FILING_ADAPTER_GET_IFACE(self)->get_name(self);
}

const gchar *
venture_tax_filing_adapter_get_rule_id(VentureTaxFilingAdapter *self)
{
	g_return_val_if_fail(VENTURE_IS_TAX_FILING_ADAPTER(self), NULL);
	if (NULL == VENTURE_TAX_FILING_ADAPTER_GET_IFACE(self)->get_rule_id)
		return NULL;
	return VENTURE_TAX_FILING_ADAPTER_GET_IFACE(self)->get_rule_id(self);
}

const gchar *
venture_tax_filing_adapter_get_country(VentureTaxFilingAdapter *self)
{
	g_return_val_if_fail(VENTURE_IS_TAX_FILING_ADAPTER(self), NULL);
	if (NULL == VENTURE_TAX_FILING_ADAPTER_GET_IFACE(self)->get_country)
		return NULL;
	return VENTURE_TAX_FILING_ADAPTER_GET_IFACE(self)->get_country(self);
}

gboolean
venture_tax_filing_adapter_prepare(VentureTaxFilingAdapter *self, VentureDatabase *database,
	VentureEntity *filing, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_TAX_FILING_ADAPTER(self), FALSE);
	if (NULL == VENTURE_TAX_FILING_ADAPTER_GET_IFACE(self)->prepare)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
			"The tax filing adapter cannot prepare a pack");
		return FALSE;
	}
	return VENTURE_TAX_FILING_ADAPTER_GET_IFACE(self)->prepare(self, database, filing, error);
}

struct _VentureTaxFilingAdapterRegistry
{
	GObject parent_instance;
	GHashTable *adapters;
};
G_DEFINE_FINAL_TYPE(VentureTaxFilingAdapterRegistry, venture_tax_filing_adapter_registry, G_TYPE_OBJECT)

static void
venture_tax_filing_adapter_registry_finalize(GObject *object)
{
	g_hash_table_unref(VENTURE_TAX_FILING_ADAPTER_REGISTRY(object)->adapters);
	G_OBJECT_CLASS(venture_tax_filing_adapter_registry_parent_class)->finalize(object);
}
static void
venture_tax_filing_adapter_registry_class_init(VentureTaxFilingAdapterRegistryClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_tax_filing_adapter_registry_finalize;
}
static void
venture_tax_filing_adapter_registry_init(VentureTaxFilingAdapterRegistry *self)
{
	self->adapters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
}
VentureTaxFilingAdapterRegistry *
venture_tax_filing_adapter_registry_new(void)
{
	return g_object_new(VENTURE_TYPE_TAX_FILING_ADAPTER_REGISTRY, NULL);
}
void
venture_tax_filing_adapter_registry_add(VentureTaxFilingAdapterRegistry *self,
	VentureTaxFilingAdapter *adapter)
{
	const gchar *name;
	g_return_if_fail(VENTURE_IS_TAX_FILING_ADAPTER_REGISTRY(self));
	g_return_if_fail(VENTURE_IS_TAX_FILING_ADAPTER(adapter));
	name = venture_tax_filing_adapter_get_name(adapter);
	g_return_if_fail(NULL != name && '\0' != *name);
	g_hash_table_replace(self->adapters, g_strdup(name), adapter);
}
VentureTaxFilingAdapter *
venture_tax_filing_adapter_registry_lookup(VentureTaxFilingAdapterRegistry *self, const gchar *name)
{
	g_return_val_if_fail(VENTURE_IS_TAX_FILING_ADAPTER_REGISTRY(self), NULL);
	g_return_val_if_fail(NULL != name, NULL);
	return g_hash_table_lookup(self->adapters, name);
}
gboolean
venture_tax_filing_adapter_registry_remove(VentureTaxFilingAdapterRegistry *self, const gchar *name)
{
	g_return_val_if_fail(VENTURE_IS_TAX_FILING_ADAPTER_REGISTRY(self), FALSE);
	return g_hash_table_remove(self->adapters, name);
}
static gint
compare_adapters(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(venture_tax_filing_adapter_get_name(*(VentureTaxFilingAdapter *const *)a),
		venture_tax_filing_adapter_get_name(*(VentureTaxFilingAdapter *const *)b));
}
GPtrArray *
venture_tax_filing_adapter_registry_list(VentureTaxFilingAdapterRegistry *self)
{
	GPtrArray *result = g_ptr_array_new();
	GHashTableIter iter;
	gpointer value;

	g_hash_table_iter_init(&iter, self->adapters);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(result, value);
	g_ptr_array_sort(result, compare_adapters);
	return result;
}

typedef struct
{
	gchar *code;
	gchar *jurisdiction;
	VentureMoney *taxable;
	VentureMoney *tax;
} TaxLine;

static void
tax_line_free(gpointer data)
{
	TaxLine *row = data;
	g_free(row->code);
	g_free(row->jurisdiction);
	g_clear_pointer(&row->taxable, venture_money_free);
	g_clear_pointer(&row->tax, venture_money_free);
	g_free(row);
}

static const gchar *
country_of(const gchar *jurisdiction)
{
	static gchar buf[8];
	const gchar *dash;
	gsize n;
	if (jurisdiction == NULL || *jurisdiction == '\0')
		return "";
	dash = strchr(jurisdiction, '-');
	n = dash != NULL ? (gsize)(dash - jurisdiction) : strlen(jurisdiction);
	if (n == 0 || n >= sizeof(buf))
		return jurisdiction;
	memcpy(buf, jurisdiction, n);
	buf[n] = '\0';
	return buf;
}

static gboolean
jurisdiction_matches(const gchar *wanted, const gchar *actual)
{
	if (wanted == NULL || *wanted == '\0')
		return TRUE;
	if (actual == NULL)
		return FALSE;
	if (g_strcmp0(wanted, actual) == 0)
		return TRUE;
	if (g_str_has_prefix(actual, wanted) && actual[strlen(wanted)] == '-')
		return TRUE;
	return FALSE;
}

static gboolean
in_period(GDateTime *when, GDateTime *start, GDateTime *end)
{
	if (when == NULL || start == NULL || end == NULL)
		return FALSE;
	return g_date_time_compare(when, start) >= 0 && g_date_time_compare(when, end) < 0;
}

static gboolean
add_amounts(VentureMoney **slot, const VentureMoney *add, const gchar *currency, GError **error)
{
	g_autoptr(VentureMoney) next = NULL;
	if (add == NULL)
		return TRUE;
	if (*slot == NULL)
	{
		*slot = venture_money_copy(add);
		return *slot != NULL;
	}
	next = venture_money_add(*slot, add, error);
	if (next == NULL)
		return FALSE;
	(void)currency;
	venture_money_free(*slot);
	*slot = g_steal_pointer(&next);
	return TRUE;
}

static gboolean
us_prepare(VentureTaxFilingAdapter *self, VentureDatabase *database, VentureEntity *filing, GError **error)
{
	g_autofree gchar *country = NULL;
	g_autofree gchar *jurisdiction = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(GHashTable) grouped = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, tax_line_free);
	g_autoptr(VentureQuery) events_query = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GString) csv = g_string_new("code,jurisdiction,taxable,tax\n");
	g_autoptr(VentureMoney) total_taxable = NULL;
	g_autoptr(VentureMoney) total_tax = NULL;
	g_autoptr(VentureMoney) event_tax = NULL;
	g_autofree gchar *json = NULL;
	GHashTableIter iter;
	TaxLine *row;
	const gchar *currency = "USD";
	gint64 org;
	guint i;

	(void)self;
	g_object_get(filing, "country", &country, "jurisdiction", &jurisdiction,
		"period-start", &start, "period-end", &end, NULL);
	org = venture_entity_get_organization_id(filing);
	if (country == NULL || *country == '\0')
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Missing country for jurisdiction %s",
			jurisdiction != NULL && *jurisdiction != '\0' ? jurisdiction : "(unnamed)");
		return FALSE;
	}
	if (g_strcmp0(country, "US") != 0)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"The US sales-tax adapter cannot file jurisdiction %s",
			jurisdiction != NULL ? jurisdiction : country);
		return FALSE;
	}
	events_query = venture_query_new(VENTURE_TYPE_INVOICE_EVENT);
	if (events_query == NULL)
		return FALSE;
	venture_query_set_limit(events_query, 0);
	venture_query_set_organization(events_query, org);
	events = venture_database_find(database, events_query, error);
	if (events == NULL)
		return FALSE;
	for (i = 0; i < events->len; i++)
	{
		VentureEntity *event = g_ptr_array_index(events, i);
		g_autofree gchar *kind = NULL;
		g_autoptr(GDateTime) when = NULL;
		g_autoptr(VentureMoney) frozen = NULL;
		g_autoptr(VentureQuery) lines_query = NULL;
		g_autoptr(GPtrArray) lines = NULL;
		gint64 invoice_id = 0;
		guint j;
		if (venture_entity_is_deleted(event))
			continue;
		g_object_get(event, "kind", &kind, "date", &when, "invoice-id", &invoice_id,
			"tax-amount", &frozen, NULL);
		if (g_strcmp0(kind, "issue") != 0 || !in_period(when, start, end))
			continue;
		if (!add_amounts(&event_tax, frozen, currency, error))
			return FALSE;
		lines_query = venture_query_new(VENTURE_TYPE_INVOICE_LINE);
		venture_query_set_limit(lines_query, 0);
		if (!venture_query_add_filter_int(lines_query, "invoice-id", VENTURE_FILTER_OP_EQ,
			invoice_id, error))
			return FALSE;
		lines = venture_database_find(database, lines_query, error);
		if (lines == NULL)
			return FALSE;
		for (j = 0; j < lines->len; j++)
		{
			VentureEntity *line = g_ptr_array_index(lines, j);
			g_autoptr(VentureMoney) taxable = NULL;
			g_autoptr(VentureMoney) tax = NULL;
			g_autoptr(VentureEntity) code = NULL;
			g_autofree gchar *code_name = NULL;
			g_autofree gchar *code_jurisdiction = NULL;
			gboolean recoverable = FALSE;
			gint64 tax_code_id = 0;
			TaxLine *slot;
			if (venture_entity_is_deleted(line))
				continue;
			g_object_get(line, "income-amount", &taxable, "tax-amount", &tax,
				"tax-code-id", &tax_code_id, NULL);
			if (tax_code_id == 0)
				continue;
			code = venture_database_get(database, VENTURE_TYPE_TAX_CODE, tax_code_id, error);
			if (code == NULL)
				return FALSE;
			g_object_get(code, "code", &code_name, "jurisdiction", &code_jurisdiction,
				"recoverable", &recoverable, NULL);
			if (code_jurisdiction == NULL || *code_jurisdiction == '\0')
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
					"Missing country for jurisdiction %s",
					code_name != NULL ? code_name : "(unnamed)");
				return FALSE;
			}
			if (g_strcmp0(country_of(code_jurisdiction), "US") != 0)
				continue;
			if (!jurisdiction_matches(jurisdiction, code_jurisdiction))
				continue;
			if (recoverable)
				continue;
			slot = g_hash_table_lookup(grouped, GINT_TO_POINTER((gint)tax_code_id));
			if (slot == NULL)
			{
				slot = g_new0(TaxLine, 1);
				slot->code = g_strdup(code_name);
				slot->jurisdiction = g_strdup(code_jurisdiction);
				g_hash_table_insert(grouped, GINT_TO_POINTER((gint)tax_code_id), slot);
			}
			if (!add_amounts(&slot->taxable, taxable, currency, error) ||
				!add_amounts(&slot->tax, tax, currency, error) ||
				!add_amounts(&total_taxable, taxable, currency, error) ||
				!add_amounts(&total_tax, tax, currency, error))
				return FALSE;
		}
	}
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "rule_id");
	json_builder_add_string_value(builder, "us-sales-tax:1");
	json_builder_set_member_name(builder, "country");
	json_builder_add_string_value(builder, country);
	json_builder_set_member_name(builder, "jurisdiction");
	json_builder_add_string_value(builder, jurisdiction != NULL ? jurisdiction : country);
	json_builder_set_member_name(builder, "lines");
	json_builder_begin_array(builder);
	g_hash_table_iter_init(&iter, grouped);
	while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&row))
	{
		g_autofree gchar *taxable_text = row->taxable != NULL ? venture_money_to_string(row->taxable) : g_strdup("0 USD");
		g_autofree gchar *tax_text = row->tax != NULL ? venture_money_to_string(row->tax) : g_strdup("0 USD");
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "code");
		json_builder_add_string_value(builder, row->code);
		json_builder_set_member_name(builder, "jurisdiction");
		json_builder_add_string_value(builder, row->jurisdiction);
		json_builder_set_member_name(builder, "taxable");
		json_builder_add_string_value(builder, taxable_text);
		json_builder_set_member_name(builder, "tax");
		json_builder_add_string_value(builder, tax_text);
		json_builder_end_object(builder);
		g_string_append_printf(csv, "%s,%s,%s,%s\n", row->code, row->jurisdiction, taxable_text, tax_text);
	}
	json_builder_end_array(builder);
	{
		g_autofree gchar *taxable_total = total_taxable != NULL ? venture_money_to_string(total_taxable) : g_strdup("0 USD");
		g_autofree gchar *tax_total = total_tax != NULL ? venture_money_to_string(total_tax) : g_strdup("0 USD");
		json_builder_set_member_name(builder, "taxable");
		json_builder_add_string_value(builder, taxable_total);
		json_builder_set_member_name(builder, "tax");
		json_builder_add_string_value(builder, tax_total);
	}
	if (event_tax != NULL)
	{
		g_autofree gchar *frozen = venture_money_to_string(event_tax);
		json_builder_set_member_name(builder, "invoice_event_tax");
		json_builder_add_string_value(builder, frozen);
	}
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	generator = json_generator_new();
	json_generator_set_root(generator, root);
	json = json_generator_to_data(generator, NULL);
	g_object_set(filing, "json-pack", json, "csv-pack", csv->str, "rule-id", "us-sales-tax:1",
		"adapter", "US", "taxable", total_taxable, "tax", total_tax, "currency", currency, NULL);
	return TRUE;
}

G_DECLARE_FINAL_TYPE(VentureUsSalesTaxAdapter, venture_us_sales_tax_adapter,
	VENTURE, US_SALES_TAX_ADAPTER, GObject)
struct _VentureUsSalesTaxAdapter { GObject parent_instance; };
static void us_iface_init(VentureTaxFilingAdapterInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureUsSalesTaxAdapter, venture_us_sales_tax_adapter, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_TAX_FILING_ADAPTER, us_iface_init))
static void venture_us_sales_tax_adapter_init(VentureUsSalesTaxAdapter *self) { (void)self; }
static void venture_us_sales_tax_adapter_class_init(VentureUsSalesTaxAdapterClass *klass) { (void)klass; }
static const gchar *us_name(VentureTaxFilingAdapter *self) { (void)self; return "US"; }
static const gchar *us_rule(VentureTaxFilingAdapter *self) { (void)self; return "us-sales-tax:1"; }
static const gchar *us_country(VentureTaxFilingAdapter *self) { (void)self; return "US"; }
static void
us_iface_init(VentureTaxFilingAdapterInterface *iface)
{
	iface->get_name = us_name;
	iface->get_rule_id = us_rule;
	iface->get_country = us_country;
	iface->prepare = us_prepare;
}

VentureTaxFilingAdapter *
venture_us_sales_tax_adapter_new(void)
{
	return g_object_new(venture_us_sales_tax_adapter_get_type(), NULL);
}
