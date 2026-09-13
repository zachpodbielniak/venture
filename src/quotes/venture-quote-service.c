/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <sys/random.h>
#include <errno.h>
#include <string.h>

struct _VentureQuoteService {
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
	VentureEntity *removing;
	gboolean busy;
};
G_DEFINE_FINAL_TYPE(VentureQuoteService, venture_quote_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureQuoteService: %s", message);
	return FALSE;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	VentureQuoteService *self = VENTURE_QUOTE_SERVICE(object);
	if (id == 1) g_value_set_object(value, self->database);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureQuoteService *self = VENTURE_QUOTE_SERVICE(object);
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
	VentureQuoteService *self = VENTURE_QUOTE_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_quote_service_parent_class)->finalize(object);
}

static gboolean
first_error(GSignalInvocationHint *hint, GValue *accumulator, const GValue *value, gpointer data)
{
	(void)hint;
	(void)data;
	if (g_value_get_boxed(value) == NULL) return TRUE;
	g_value_copy(value, accumulator);
	return FALSE;
}

static void
venture_quote_service_class_init(VentureQuoteServiceClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->get_property = get_property;
	object->set_property = set_property;
	object->finalize = finalize;
	/**
	 * VentureQuoteService::before-action:
	 * @self: the service
	 * @quote: detached snapshot of the current quote
	 * @action: the requested business action
	 *
	 * Emitted inside the transaction after concurrency validation, before
	 * evidence or financial writes. The first error vetoes the action.
	 * Returns: (transfer full) (nullable): an error to veto, or NULL
	 */
	g_signal_new("before-action", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		0, first_error, NULL, NULL, G_TYPE_ERROR, 2, VENTURE_TYPE_ENTITY, G_TYPE_STRING);
	g_object_class_install_property(object, 1,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_quote_service_init(VentureQuoteService *self)
{
	(void)self;
}

static gint64
integer(VentureEntity *r, const gchar *field)
{
	gint64 value;
	g_object_get(r, field, &value, NULL);
	return value;
}

static gint
state(VentureEntity *r)
{
	gint value;
	g_object_get(r, "status", &value, NULL);
	return value;
}

static GPtrArray *
find(VentureQuoteService *self, GType type, gint64 org, const gchar *field, gint64 id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	if (field != NULL && !venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, error))
		return NULL;
	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;
	return venture_database_find(self->database, query, error);
}

static VentureEntity *
get(VentureQuoteService *self, GType type, gint64 org, gint64 id, GError **error)
{
	VentureEntity *r = venture_database_get(self->database, type, id, error);
	if (r == NULL && (error == NULL || *error == NULL))
		refuse(error, VENTURE_ERROR_NOT_FOUND, "record not found in this organization");
	if (r != NULL && (venture_entity_get_organization_id(r) != org || venture_entity_is_deleted(r)))
	{
		g_object_unref(r);
		refuse(error, VENTURE_ERROR_NOT_FOUND, "record not found in this organization");
		return NULL;
	}
	return r;
}

/* The permit is consumed before any save callback can reenter the service. */
static gboolean
write_record(VentureQuoteService *self, VentureEntity *r, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) snapshot = g_object_new(G_OBJECT_TYPE(r), NULL);
	g_autoptr(JsonNode) diff = NULL;
	gboolean ok;
	venture_entity_copy_properties_from(snapshot, r, FALSE);
	self->writing = r;
	ok = venture_database_save(self->database, r, actor, error);
	self->writing = NULL;
	if (ok)
	{
		diff = venture_entity_diff(snapshot, r);
		if (json_object_get_size(json_node_get_object(diff)) != 0 ||
			venture_entity_get_organization_id(snapshot) != venture_entity_get_organization_id(r))
			ok = refuse(error, VENTURE_ERROR_VALIDATION, "a callback changed service-owned fields during persistence");
	}
	if (!ok) venture_entity_copy_properties_from(r, snapshot, FALSE);
	return ok;
}

static gboolean
add(VentureMoney **sum, VentureMoney *value, GError **error)
{
	VentureMoney *next = venture_money_add(*sum, value, error);
	if (next == NULL) return FALSE;
	venture_money_free(*sum);
	*sum = next;
	return TRUE;
}

static gboolean
line_amounts(VentureEntity *line, VentureMoney **subtotal, VentureMoney **discount,
	VentureMoney **tax, VentureMoney **total, GError **error)
{
	g_autoptr(VentureMoney) unit = NULL;
	g_autoptr(VentureMoney) net = NULL;
	gint64 quantity = integer(line, "quantity");
	gint64 dp = integer(line, "discount-percent");
	gint64 tp = integer(line, "tax-percent");
	g_object_get(line, "unit-price", &unit, NULL);
	if (quantity <= 0 || quantity > 1000000000 || dp < 0 || dp > 100 || tp < 0 || tp > 100 ||
		unit == NULL || venture_money_get_amount(unit) < 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "positive quantity and price, and percentages 0..100 required");
	*subtotal = venture_money_multiply_int(unit, quantity, error);
	if (*subtotal == NULL) return FALSE;
	*discount = venture_money_multiply_rational(*subtotal, dp, 100, error);
	if (*discount == NULL) return FALSE;
	net = venture_money_subtract(*subtotal, *discount, error);
	if (net == NULL) return FALSE;
	*tax = venture_money_multiply_rational(net, tp, 100, error);
	if (*tax == NULL) return FALSE;
	*total = venture_money_add(net, *tax, error);
	return *total != NULL;
}

static gboolean
compute(VentureQuoteService *self, VentureEntity *q, GError **error)
{
	g_autofree gchar *currency = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureMoney) subtotal = NULL;
	g_autoptr(VentureMoney) discount = NULL;
	g_autoptr(VentureMoney) tax = NULL;
	g_autoptr(VentureMoney) total = NULL;
	guint i;
	g_object_get(q, "currency", &currency, NULL);
	if (currency == NULL || strlen(currency) != 3)
		return refuse(error, VENTURE_ERROR_VALIDATION, "an ISO currency is required");
	for (i = 0; i < 3; i++)
		if (!g_ascii_isupper(currency[i]))
			return refuse(error, VENTURE_ERROR_VALIDATION, "currency must be uppercase");
	subtotal = venture_money_new_zero(currency);
	discount = venture_money_new_zero(currency);
	tax = venture_money_new_zero(currency);
	total = venture_money_new_zero(currency);
	lines = find(self, VENTURE_TYPE_QUOTE_LINE, venture_entity_get_organization_id(q),
		"quote-id", venture_entity_get_id(q), error);
	if (lines == NULL) return FALSE;
	for (i = 0; i < lines->len; i++)
	{
		g_autoptr(VentureMoney) s = NULL;
		g_autoptr(VentureMoney) d = NULL;
		g_autoptr(VentureMoney) t = NULL;
		g_autoptr(VentureMoney) a = NULL;
		if (!line_amounts(g_ptr_array_index(lines, i), &s, &d, &t, &a, error) ||
			!add(&subtotal, s, error) || !add(&discount, d, error) ||
			!add(&tax, t, error) || !add(&total, a, error)) return FALSE;
	}
	g_object_set(q, "subtotal", subtotal, "discount", discount, "tax", tax, "total", total, NULL);
	return TRUE;
}

static gboolean
check_refs(VentureQuoteService *self, VentureEntity *r, GError **error)
{
	g_autoptr(GPtrArray) specs = venture_entity_get_field_specs(r);
	guint i;
	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);
		const gchar *target = venture_field_spec_get_reference_type(spec);
		gint64 id;
		GType type;
		g_autoptr(VentureEntity) other = NULL;
		if (target == NULL || g_strcmp0(target, "organization") == 0) continue;
		id = integer(r, venture_field_spec_get_name(spec));
		if (id == 0) continue;
		type = venture_entity_registry_lookup(venture_entity_registry_get_default(), target);
		if (type == G_TYPE_INVALID)
			return refuse(error, VENTURE_ERROR_VALIDATION, "referenced module is disabled");
		other = get(self, type, venture_entity_get_organization_id(r), id, error);
		if (other == NULL) return FALSE;
	}
	return TRUE;
}

static gboolean
price(VentureQuoteService *self, VentureEntity *line, VentureEntity *q, GError **error)
{
	g_autoptr(VentureMoney) unit = NULL;
	g_autoptr(GPtrArray) lists = NULL;
	g_autofree gchar *currency = NULL;
	gint64 product = integer(line, "product-id");
	gint64 org = venture_entity_get_organization_id(q);
	gint64 best = -1;
	gint64 selected = 0;
	guint i;
	g_object_get(line, "unit-price", &unit, NULL);
	if (unit != NULL || product == 0) return TRUE;
	g_object_get(q, "currency", &currency, NULL);
	if (integer(q, "company-id") != 0)
	{
		g_autoptr(VentureEntity) company = get(self, VENTURE_TYPE_COMPANY, org, integer(q, "company-id"), error);
		if (company == NULL) return FALSE;
		selected = integer(company, "default-price-list-id");
		if (selected != 0)
		{
			g_autoptr(VentureEntity) chosen = get(self, VENTURE_TYPE_PRICE_LIST, org, selected, error);
			g_autofree gchar *chosen_currency = NULL;
			if (chosen == NULL) return FALSE;
			g_object_get(chosen, "currency", &chosen_currency, NULL);
			if (g_strcmp0(chosen_currency, currency) != 0)
				return refuse(error, VENTURE_ERROR_VALIDATION, "customer price list currency differs from the quote");
		}
	}
	lists = find(self, VENTURE_TYPE_PRICE_LIST, org, NULL, 0, error);
	if (lists == NULL) return FALSE;
	for (i = 0; i < lists->len; i++)
	{
		VentureEntity *list = g_ptr_array_index(lists, i);
		gboolean is_default;
		g_autofree gchar *lc = NULL;
		g_autoptr(GPtrArray) items = NULL;
		guint j;
		g_object_get(list, "is-default", &is_default, "currency", &lc, NULL);
		if ((selected != 0 ? venture_entity_get_id(list) != selected : !is_default) || g_strcmp0(lc, currency) != 0) continue;
		items = find(self, VENTURE_TYPE_PRICE_LIST_ITEM, org, "price-list-id", venture_entity_get_id(list), error);
		if (items == NULL) return FALSE;
		for (j = 0; j < items->len; j++)
		{
			VentureEntity *item = g_ptr_array_index(items, j);
			gint64 min = integer(item, "min-quantity");
			if (integer(item, "product-id") == product && min <= integer(line, "quantity") && min > best)
			{
				g_clear_pointer(&unit, venture_money_free);
				g_object_get(item, "unit-price", &unit, NULL);
				best = min;
			}
		}
	}
	if (unit == NULL)
	{
		g_autoptr(VentureEntity) p = get(self, VENTURE_TYPE_PRODUCT, org, product, error);
		if (p == NULL) return FALSE;
		g_object_get(p, "list-price", &unit, NULL);
	}
	g_object_set(line, "unit-price", unit, NULL);
	return TRUE;
}

static gboolean
save_draft(VentureQuoteService *self, VentureEntity *r, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) previous = NULL;
	g_autoptr(VentureEntity) q = NULL;
	gboolean ok = FALSE;
	if (!venture_database_begin(self->database, error)) return FALSE;
	if (venture_entity_is_persisted(r))
	{
		previous = get(self, G_OBJECT_TYPE(r), venture_entity_get_organization_id(r), venture_entity_get_id(r), error);
		if (previous == NULL) goto done;
	}
	if (!check_refs(self, r, error)) goto done;
	if (VENTURE_IS_QUOTE(r))
	{
		if (state(r) != VENTURE_QUOTE_DRAFT || (previous != NULL && state(previous) != VENTURE_QUOTE_DRAFT))
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "status and sent proposals are service-owned");
			goto done;
		}
		if (previous == NULL)
		{
			g_object_set(r, "revision", (gint64)1, "parent-id", (gint64)0,
				"invoice-id", (gint64)0, "acceptance-token", NULL, "issued-at", NULL, NULL);
		}
		else if (integer(r, "revision") != integer(previous, "revision") ||
			integer(r, "parent-id") != integer(previous, "parent-id") || integer(r, "invoice-id") != 0)
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "revision and handoff fields are service-owned");
			goto done;
		}
		if (!compute(self, r, error) || !write_record(self, r, actor, error)) goto done;
	}
	else
	{
		q = get(self, VENTURE_TYPE_QUOTE, venture_entity_get_organization_id(r), integer(r, "quote-id"), error);
		if (q == NULL) goto done;
		if (state(q) != VENTURE_QUOTE_DRAFT || (previous != NULL && integer(previous, "quote-id") != integer(r, "quote-id")))
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "lines of sent proposals are frozen; moving lines is refused");
			goto done;
		}
		if (!price(self, r, q, error) || !write_record(self, r, actor, error) ||
			!compute(self, q, error) || !write_record(self, q, actor, error)) goto done;
	}
	ok = TRUE;
done:
	if (ok) return venture_database_commit(self->database, error);
	venture_database_rollback(self->database);
	return FALSE;
}

static gchar *
secret(GError **error)
{
	guchar bytes[32];
	gchar *result;
	gsize used = 0;
	guint i;
	while (used < sizeof(bytes))
	{
		ssize_t n = getrandom(bytes + used, sizeof(bytes) - used, 0);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0)
		{
			refuse(error, VENTURE_ERROR_FAILED, "cannot obtain secure random bytes");
			return NULL;
		}
		used += (gsize)n;
	}
	result = g_malloc(65);
	for (i = 0; i < sizeof(bytes); i++) g_snprintf(result + 2 * i, 3, "%02x", bytes[i]);
	return result;
}

static gboolean
event(VentureQuoteService *self, VentureEntity *q, const gchar *kind, VentureEntity *request,
	const gchar *method, const gchar *ip, GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) r = VENTURE_ENTITY(venture_quote_event_new());
	const gchar *fields[] = { "subtotal", "discount", "tax", "total" };
	g_autofree gchar *by = NULL;
	g_autofree gchar *reason = NULL;
	guint i;
	venture_entity_set_organization_id(r, venture_entity_get_organization_id(q));
	g_object_get(request, "accepted-by", &by, "reason", &reason, NULL);
	g_object_set(r, "quote-id", venture_entity_get_id(q), "kind", kind, "occurred-at", now,
		"reason", reason, NULL);
	if (g_strcmp0(kind, "accept") == 0)
		g_object_set(r, "accepted-by", by, "accepted-at", now, "method", method, "ip", ip, NULL);
	for (i = 0; i < G_N_ELEMENTS(fields); i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		g_object_get(q, fields[i], &amount, NULL);
		g_object_set(r, fields[i], amount, NULL);
	}
	return write_record(self, r, actor, error);
}

static gboolean
handoff(VentureQuoteService *self, VentureEntity *q, GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) invoice = VENTURE_ENTITY(venture_invoice_new());
	g_autoptr(GPtrArray) lines = NULL;
	g_autofree gchar *number = NULL;
	g_autofree gchar *invoice_number = NULL;
	g_autofree gchar *terms = NULL;
	g_autoptr(GDateTime) due = NULL;
	gint64 org = venture_entity_get_organization_id(q);
	guint i;
	venture_entity_set_organization_id(invoice, org);
	g_object_get(q, "number", &number, "terms", &terms, "valid-until", &due, NULL);
	invoice_number = g_strdup_printf("QUOTE-%s", number);
	g_object_set(invoice, "number", invoice_number, "company-id", integer(q, "company-id"),
		"contact-id", integer(q, "contact-id"), "venture-id", integer(q, "venture-id"),
		"issued-at", now, "terms", terms, NULL);
	if (!venture_database_save(self->database, invoice, actor, error)) return FALSE;
	lines = find(self, VENTURE_TYPE_QUOTE_LINE, org, "quote-id", venture_entity_get_id(q), error);
	if (lines == NULL) return FALSE;
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *l = g_ptr_array_index(lines, i);
		g_autoptr(VentureEntity) r = VENTURE_ENTITY(venture_invoice_line_new());
		g_autofree gchar *description = NULL;
		g_autoptr(VentureMoney) unit = NULL;
		g_object_get(l, "description", &description, "unit-price", &unit, NULL);
		venture_entity_set_organization_id(r, org);
		g_object_set(r, "invoice-id", venture_entity_get_id(invoice), "description", description,
			"quantity", (gdouble)integer(l, "quantity"), "unit-price", unit,
			"position", integer(l, "position"), "discount-percent", integer(l, "discount-percent"),
			"tax-percent", integer(l, "tax-percent"), NULL);
		if (!venture_database_save(self->database, r, actor, error)) return FALSE;
	}
	if (!venture_settlement_service_transition(venture_settlement_service_get(self->database),
		VENTURE_INVOICE(invoice), "sent", now, actor, error)) return FALSE;
	if (integer(q, "deal-id") != 0)
	{
		g_autoptr(VentureEntity) deal = get(self, VENTURE_TYPE_DEAL, org, integer(q, "deal-id"), error);
		if (deal == NULL) return FALSE;
		if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "pipeline") != G_TYPE_INVALID)
		{
			g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PIPELINE_STAGE);
			g_autoptr(VentureEntity) won = NULL;
			g_autoptr(VentureDeal) moved = NULL;
			/* Acceptance uses the configured process, including required fields
			 * and history, inside the invoice transaction. */
			venture_query_set_organization(query, org);
			venture_query_add_filter_int(query, "pipeline-id", VENTURE_FILTER_OP_EQ, integer(deal, "pipeline-id"), NULL);
			venture_query_add_filter_string(query, "kind", VENTURE_FILTER_OP_EQ, "won", NULL);
			venture_query_add_order(query, "position", VENTURE_SORT_ASCENDING, NULL);
			venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
			won = venture_database_find_one(self->database, query, error);
			if (won == NULL)
			{
				if (error == NULL || *error == NULL)
					refuse(error, VENTURE_ERROR_VALIDATION, "deal pipeline has no won stage");
				return FALSE;
			}
			moved = venture_deal_service_move_stage(venture_database_get_deal_service(self->database),
				VENTURE_DEAL(deal), venture_entity_get_id(won), "Quote accepted", actor, error);
			if (moved == NULL) return FALSE;
		}
		else
		{
			g_object_set(deal, "stage", VENTURE_DEAL_STAGE_WON, "closed-at", now, NULL);
			if (!venture_database_save(self->database, deal, actor, error)) return FALSE;
		}
	}
	g_object_set(q, "invoice-id", venture_entity_get_id(invoice), NULL);
	return TRUE;
}

static gboolean
revise(VentureQuoteService *self, VentureEntity *q, VentureEntity *request, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) next = venture_entity_duplicate(q);
	g_autoptr(GPtrArray) lines = NULL;
	g_autofree gchar *number = NULL;
	g_autofree gchar *new_number = NULL;
	gint64 revision = integer(q, "revision") + 1;
	guint i;
	g_object_get(q, "number", &number, NULL);
	new_number = g_strdup_printf("%s-R%" G_GINT64_FORMAT, number, revision);
	venture_entity_set_organization_id(next, venture_entity_get_organization_id(q));
	g_object_set(next, "number", new_number, "revision", revision, "parent-id", venture_entity_get_id(q),
		"status", VENTURE_QUOTE_DRAFT, "issued-at", NULL, "valid-until", NULL,
		"acceptance-token", NULL, "invoice-id", (gint64)0, NULL);
	if (!write_record(self, next, actor, error)) return FALSE;
	lines = find(self, VENTURE_TYPE_QUOTE_LINE, venture_entity_get_organization_id(q), "quote-id", venture_entity_get_id(q), error);
	if (lines == NULL) return FALSE;
	for (i = 0; i < lines->len; i++)
	{
		g_autoptr(VentureEntity) l = venture_entity_duplicate(g_ptr_array_index(lines, i));
		venture_entity_set_organization_id(l, venture_entity_get_organization_id(q));
		g_object_set(l, "quote-id", venture_entity_get_id(next), NULL);
		if (!write_record(self, l, actor, error)) return FALSE;
	}
	g_object_set(request, "result-quote-id", venture_entity_get_id(next), NULL);
	g_object_set(q, "status", VENTURE_QUOTE_SUPERSEDED, NULL);
	return TRUE;
}

gboolean
venture_quote_service_execute(VentureQuoteService *self, VentureEntity *request,
	const gchar *method, const gchar *ip, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) q = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) until = NULL;
	g_autofree gchar *verb = NULL;
	g_autofree gchar *by = NULL;
	g_autofree gchar *reason = NULL;
	gboolean ok = FALSE;
	gint64 org = venture_entity_get_organization_id(request);
	if (self->database == NULL || self->busy || !VENTURE_IS_QUOTE_ACTION(request) || venture_entity_is_persisted(request))
		return refuse(error, VENTURE_ERROR_CONFLICT, "action cannot be replayed or reentered");
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "quote") == G_TYPE_INVALID)
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "quotes module is disabled");
	if (g_strcmp0(method, "manual") != 0 && g_strcmp0(method, "web") != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "unknown acceptance method");
	if (!venture_database_begin(self->database, error)) return FALSE;
	self->busy = TRUE;
	q = get(self, VENTURE_TYPE_QUOTE, org, integer(request, "quote-id"), error);
	if (q == NULL) goto done;
	if (integer(request, "expected-version") != venture_entity_get_version(q))
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "quote changed; stage the action again");
		goto done;
	}
	g_object_get(request, "action", &verb, "accepted-by", &by, "reason", &reason, NULL);
	g_object_get(q, "valid-until", &until, NULL);
	{
		g_autoptr(VentureEntity) snapshot = g_object_new(G_OBJECT_TYPE(q), NULL);
		GError *veto = NULL;
		venture_entity_copy_properties_from(snapshot, q, FALSE);
		g_signal_emit_by_name(self, "before-action", snapshot, verb, &veto);
		if (veto != NULL)
		{
			g_propagate_error(error, veto);
			goto done;
		}
	}
	if (by != NULL) g_strstrip(by);
	if (g_strcmp0(verb, "send") == 0 && state(q) == VENTURE_QUOTE_DRAFT)
	{
		g_autofree gchar *token = secret(error);
		g_autofree gchar *url = NULL;
		g_autoptr(VentureEntity) delivery = VENTURE_ENTITY(venture_quote_delivery_new());
		g_autoptr(GPtrArray) lines = find(self, VENTURE_TYPE_QUOTE_LINE, org, "quote-id", venture_entity_get_id(q), error);
		if (token == NULL || lines == NULL) goto done;
		if (lines->len == 0)
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "a quote needs at least one line");
			goto done;
		}
		if (until == NULL)
		{
			g_autoptr(VentureEntity) organization = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, org, error);
			gint64 days;
			if (organization == NULL) goto done;
			days = integer(organization, "quote-valid-days");
			if (days == 0) days = 30;
			if (days < 1 || days > 3650)
			{
				refuse(error, VENTURE_ERROR_VALIDATION, "quote validity must be 1..3650 days");
				goto done;
			}
			until = g_date_time_add_days(now, (gint)days);
		}
		if (g_date_time_compare(until, now) <= 0)
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "validity must end in the future");
			goto done;
		}
		if (!compute(self, q, error)) goto done;
		g_object_set(q, "status", VENTURE_QUOTE_SENT, "issued-at", now, "valid-until", until,
			"acceptance-token", token, NULL);
		if (!event(self, q, verb, request, method, ip, now, actor, error)) goto done;
		url = g_strconcat("/q/", token, NULL);
		venture_entity_set_organization_id(delivery, org);
		g_object_set(delivery, "quote-id", venture_entity_get_id(q), "channel", "email", "acceptance-url", url, NULL);
		if (!write_record(self, delivery, actor, error)) goto done;
	}
	else if (g_strcmp0(verb, "accept") == 0 && state(q) == VENTURE_QUOTE_SENT)
	{
		if (venture_string_is_empty(by) || strlen(by) > 200 || until == NULL || g_date_time_compare(until, now) <= 0)
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "acceptance requires a typed name and an unexpired quote");
			goto done;
		}
		if (!event(self, q, verb, request, method, ip, now, actor, error) || !handoff(self, q, now, actor, error)) goto done;
		g_object_set(q, "status", VENTURE_QUOTE_ACCEPTED, NULL);
	}
	else if (g_strcmp0(verb, "decline") == 0 && state(q) == VENTURE_QUOTE_SENT)
	{
		if (venture_string_is_empty(reason))
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "decline requires a reason");
			goto done;
		}
		if (!event(self, q, verb, request, method, ip, now, actor, error)) goto done;
		g_object_set(q, "status", VENTURE_QUOTE_DECLINED, NULL);
	}
	else if (g_strcmp0(verb, "revise") == 0 && state(q) != VENTURE_QUOTE_ACCEPTED && state(q) != VENTURE_QUOTE_SUPERSEDED)
	{
		if (!revise(self, q, request, actor, error)) goto done;
	}
	else if (g_strcmp0(verb, "expire") == 0 && state(q) == VENTURE_QUOTE_SENT && until != NULL && g_date_time_compare(until, now) <= 0)
	{
		if (!event(self, q, verb, request, method, ip, now, actor, error)) goto done;
		g_object_set(q, "status", VENTURE_QUOTE_EXPIRED, NULL);
	}
	else
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "action is not allowed in the current status");
		goto done;
	}
	if (!write_record(self, q, actor, error) || !write_record(self, request, actor, error)) goto done;
	ok = TRUE;
done:
	self->busy = FALSE;
	if (ok) return venture_database_commit(self->database, error);
	venture_database_rollback(self->database);
	return FALSE;
}

gboolean
venture_quotes_save_hook(VentureDatabase *db, VentureEntity *r, const VentureActor *actor,
	gboolean *handled, GError **error)
{
	VentureQuoteService *self;
	*handled = FALSE;
	if (!VENTURE_IS_QUOTE(r) && !VENTURE_IS_QUOTE_LINE(r) && !VENTURE_IS_QUOTE_EVENT(r) &&
		!VENTURE_IS_QUOTE_DELIVERY(r) && !VENTURE_IS_QUOTE_ACTION(r) &&
		!VENTURE_IS_PRICE_LIST(r) && !VENTURE_IS_PRICE_LIST_ITEM(r)) return TRUE;
	self = venture_database_get_quote_service(db);
	if (self->writing == r)
	{
		self->writing = NULL;
		return TRUE;
	}
	if (VENTURE_IS_PRICE_LIST(r) || VENTURE_IS_PRICE_LIST_ITEM(r)) return check_refs(self, r, error);
	*handled = TRUE;
	if (VENTURE_IS_QUOTE_ACTION(r)) return venture_quote_service_execute(self, r, "manual", NULL, actor, error);
	if (VENTURE_IS_QUOTE_EVENT(r) || VENTURE_IS_QUOTE_DELIVERY(r))
		return refuse(error, VENTURE_ERROR_VALIDATION, "events and deliveries are immutable service evidence");
	return save_draft(self, r, actor, error);
}

gboolean
venture_quotes_check_removal(VentureDatabase *db, VentureEntity *r, GError **error)
{
	if (VENTURE_IS_QUOTE_EVENT(r) || VENTURE_IS_QUOTE_DELIVERY(r) || VENTURE_IS_QUOTE_ACTION(r))
		return refuse(error, VENTURE_ERROR_VALIDATION, "service evidence cannot be removed");
	if (VENTURE_IS_QUOTE(r) || VENTURE_IS_QUOTE_LINE(r))
	{
		g_autoptr(VentureEntity) stored = venture_database_get(db, G_OBJECT_TYPE(r), venture_entity_get_id(r), error);
		g_autoptr(VentureEntity) q = NULL;
		if (stored == NULL) return refuse(error, VENTURE_ERROR_NOT_FOUND, "record not found");
		if (VENTURE_IS_QUOTE_LINE(r) && integer(r, "quote-id") != integer(stored, "quote-id"))
			return refuse(error, VENTURE_ERROR_VALIDATION, "a removal cannot move a line");
		q = venture_database_get(db, VENTURE_TYPE_QUOTE,
			VENTURE_IS_QUOTE(stored) ? venture_entity_get_id(stored) : integer(stored, "quote-id"), error);
		if (q == NULL) return FALSE;
		if (state(q) != VENTURE_QUOTE_DRAFT)
			return refuse(error, VENTURE_ERROR_VALIDATION, "sent quote history cannot be removed");

	}
	return TRUE;
}

gint
venture_quote_service_sweep(VentureQuoteService *self, gint64 org, GDateTime *now, GError **error)
{
	g_autoptr(GPtrArray) quotes = find(self, VENTURE_TYPE_QUOTE, org, NULL, 0, error);
	guint i;
	gint count = 0;
	if (quotes == NULL) return -1;
	for (i = 0; i < quotes->len; i++)
	{
		VentureEntity *q = g_ptr_array_index(quotes, i);
		g_autoptr(GDateTime) until = NULL;
		g_autoptr(VentureEntity) request = NULL;
		g_object_get(q, "valid-until", &until, NULL);
		if (state(q) != VENTURE_QUOTE_SENT || until == NULL || g_date_time_compare(until, now) > 0) continue;
		request = VENTURE_ENTITY(venture_quote_action_new());
		venture_entity_set_organization_id(request, org);
		g_object_set(request, "quote-id", venture_entity_get_id(q), "action", "expire",
			"expected-version", venture_entity_get_version(q), NULL);
		if (!venture_quote_service_execute(self, request, "manual", NULL, NULL, error)) return -1;
		count++;
	}
	return count;
}

/* Removal and the new draft total share the same transaction. */
gboolean
venture_quotes_remove_hook(VentureDatabase *db, VentureEntity *r, guint operation,
	const VentureActor *actor, gboolean *handled, GError **error)
{
	VentureQuoteService *self;
	g_autoptr(VentureEntity) q = NULL;
	g_autoptr(VentureEntity) snapshot = NULL;
	gboolean ok = FALSE;
	*handled = FALSE;
	if (!VENTURE_IS_QUOTE_LINE(r)) return TRUE;
	self = venture_database_get_quote_service(db);
	if (self->removing == r)
	{
		self->removing = NULL;
		return TRUE;
	}
	*handled = TRUE;
	if (!venture_database_begin(db, error)) return FALSE;
	if (!venture_quotes_check_removal(db, r, error)) goto done;
	q = get(self, VENTURE_TYPE_QUOTE, venture_entity_get_organization_id(r), integer(r, "quote-id"), error);
	if (q == NULL) goto done;
	snapshot = g_object_new(G_OBJECT_TYPE(r), NULL);
	venture_entity_copy_properties_from(snapshot, r, FALSE);
	self->removing = r;
	if (operation == 0) ok = venture_database_delete(db, r, actor, error);
	else if (operation == 1) ok = venture_database_restore(db, r, actor, error);
	else ok = venture_database_purge(db, r, actor, error);
	self->removing = NULL;
	if (ok) ok = compute(self, q, error) && write_record(self, q, actor, error);
done:
	if (ok) return venture_database_commit(db, error);
	venture_database_rollback(db);
	if (snapshot != NULL) venture_entity_copy_properties_from(r, snapshot, FALSE);
	return FALSE;
}
