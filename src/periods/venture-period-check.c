/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

G_DEFINE_INTERFACE(VenturePeriodCheck, venture_period_check, G_TYPE_OBJECT)
static void venture_period_check_default_init(VenturePeriodCheckInterface *iface) { }
const gchar *venture_period_check_get_name(VenturePeriodCheck *self)
{ return VENTURE_PERIOD_CHECK_GET_IFACE(self)->get_name(self); }
gboolean venture_period_check_run(VenturePeriodCheck *self, VentureDatabase *database,
	VentureEntity *period, GError **error)
{ return VENTURE_PERIOD_CHECK_GET_IFACE(self)->run(self, database, period, error); }

typedef struct { GObject parent_instance; gboolean invoices; } VentureBuiltinPeriodCheck;
typedef struct { GObjectClass parent_class; } VentureBuiltinPeriodCheckClass;
GType venture_builtin_period_check_get_type(void);

static const gchar *
builtin_name(VenturePeriodCheck *check)
{
	return ((VentureBuiltinPeriodCheck *)check)->invoices ? "draft invoices" : "unbalanced ledger batches";
}

static gboolean
builtin_run(VenturePeriodCheck *check, VentureDatabase *database, VentureEntity *period, GError **error)
{
	VentureBuiltinPeriodCheck *self = (VentureBuiltinPeriodCheck *)check;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(VentureDateRange) range = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GHashTable) totals = NULL;
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	guint i;

	if (self->invoices && (G_TYPE_INVALID == venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "invoice")))
		return TRUE;
	g_object_get(period, "start-at", &start, "end-at", &end, NULL);
	range = venture_date_range_new(start, end);
	query = venture_query_new(self->invoices ? VENTURE_TYPE_INVOICE : VENTURE_TYPE_LEDGER_ENTRY);
	venture_query_set_organization(query, venture_entity_get_organization_id(period));
	venture_query_set_date_range(query, self->invoices ? "issued-at" : "occurred-at", range, NULL);
	if (self->invoices)
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, "draft", NULL);
	rows = venture_database_find(database, query, error);
	if (NULL == rows)
		return FALSE;
	if (self->invoices)
	{
		if (rows->len > 0)
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"%u invoice(s) remain in draft", rows->len);
		return 0 == rows->len;
	}
	totals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)venture_money_free);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *entry = g_ptr_array_index(rows, i);
		g_autofree gchar *batch = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) zero = NULL;
		g_autoptr(VentureMoney) next = NULL;
		VentureMoney *total;
		gint side;
		g_object_get(entry, "transaction-id", &batch, "amount", &amount, "side", &side, NULL);
		if ((NULL == amount) || (venture_money_get_amount(amount) < 0))
		{
			venture_set_error_validation(error, NULL, "Batch %s has an invalid amount", batch);
			return FALSE;
		}
		total = g_hash_table_lookup(totals, batch);
		if (NULL == total)
		{
			zero = venture_money_new(0, venture_money_get_currency(amount), venture_money_get_exponent(amount));
			total = zero;
		}
		next = (VENTURE_LEDGER_SIDE_DEBIT == side)
			? venture_money_add(total, amount, error) : venture_money_subtract(total, amount, error);
		if (NULL == next)
			return FALSE;
		g_hash_table_replace(totals, g_steal_pointer(&batch), g_steal_pointer(&next));
	}
	g_hash_table_iter_init(&iter, totals);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		if (0 != venture_money_get_amount(value))
		{
			venture_set_error_validation(error, NULL, "Batch %s is unbalanced", (gchar *)key);
			return FALSE;
		}
	}
	return TRUE;
}

static void builtin_iface_init(VenturePeriodCheckInterface *iface)
{ iface->get_name = builtin_name; iface->run = builtin_run; }
G_DEFINE_TYPE_WITH_CODE(VentureBuiltinPeriodCheck, venture_builtin_period_check, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_PERIOD_CHECK, builtin_iface_init))
static void builtin_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	if (1 == id) ((VentureBuiltinPeriodCheck *)object)->invoices = g_value_get_boolean(value);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void builtin_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	if (1 == id) g_value_set_boolean(value, ((VentureBuiltinPeriodCheck *)object)->invoices);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void venture_builtin_period_check_class_init(VentureBuiltinPeriodCheckClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->set_property = builtin_set_property;
	object_class->get_property = builtin_get_property;
	g_object_class_install_property(object_class, 1,
		g_param_spec_boolean("invoices", "Invoices", "Check drafts instead of ledger batches", FALSE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}
static void venture_builtin_period_check_init(VentureBuiltinPeriodCheck *self) { }

struct _VenturePeriodChecklist { GObject parent_instance; GPtrArray *checks; };
G_DEFINE_FINAL_TYPE(VenturePeriodChecklist, venture_period_checklist, G_TYPE_OBJECT)
static void checklist_finalize(GObject *object)
{
	g_ptr_array_unref(VENTURE_PERIOD_CHECKLIST(object)->checks);
	G_OBJECT_CLASS(venture_period_checklist_parent_class)->finalize(object);
}
static void venture_period_checklist_class_init(VenturePeriodChecklistClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = checklist_finalize;
	/**
	 * VenturePeriodChecklist::collect-checks:
	 * @self: the checklist
	 * @checks: (element-type VenturePeriodCheck): checks for this close;
	 *   append a full reference to add a plugin check
	 *
	 * RUN_LAST, emitted before checking, inside the closing transaction.
	 * Handlers append; the service runs every check and combines failures.
	 */
	g_signal_new("collect-checks", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_PTR_ARRAY);
}
static void venture_period_checklist_init(VenturePeriodChecklist *self)
{ self->checks = g_ptr_array_new_with_free_func(g_object_unref); }

VenturePeriodChecklist *
venture_period_checklist_new(void)
{
	VenturePeriodChecklist *self = g_object_new(VENTURE_TYPE_PERIOD_CHECKLIST, NULL);
	venture_period_checklist_add(self, g_object_new(venture_builtin_period_check_get_type(), NULL));
	venture_period_checklist_add(self, g_object_new(venture_builtin_period_check_get_type(), "invoices", TRUE, NULL));
	return self;
}
void venture_period_checklist_add(VenturePeriodChecklist *self, VenturePeriodCheck *check)
{
	g_return_if_fail(VENTURE_IS_PERIOD_CHECK(check));
	g_ptr_array_add(self->checks, check);
}

gboolean
venture_period_checklist_run(VenturePeriodChecklist *self, VentureDatabase *database,
	VentureEntity *period, GError **error)
{
	g_autoptr(GPtrArray) checks = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GString) failures = g_string_new(NULL);
	guint i;
	for (i = 0; i < self->checks->len; i++)
		g_ptr_array_add(checks, g_object_ref(g_ptr_array_index(self->checks, i)));
	g_signal_emit_by_name(self, "collect-checks", checks);
	for (i = 0; i < checks->len; i++)
	{
		VenturePeriodCheck *check = g_ptr_array_index(checks, i);
		g_autoptr(GError) failure = NULL;
		if (!venture_period_check_run(check, database, period, &failure))
			g_string_append_printf(failures, "%s%s: %s", failures->len ? "; " : "",
				venture_period_check_get_name(check), (NULL != failure) ? failure->message : "failed");
	}
	if (failures->len > 0)
	{
		venture_set_error_validation(error, "state", "Period close refused: %s", failures->str);
		return FALSE;
	}
	return TRUE;
}
