/* SPDX-License-Identifier: AGPL-3.0-or-later */

static gchar *
retained_key(VentureEntity *record)
{
	return g_strdup_printf("%s:%" G_GINT64_FORMAT, venture_entity_get_entity_name(record), venture_entity_get_id(record));
}

/* A posted document retains its parties and catalog references so past
 * reports remain reconstructable. Attachments and other imported live data
 * still retire normally. Traversal uses the same reference metadata as CRUD. */
static gboolean
retain_history(VentureCrmImportService *self, VentureEntity *record, GHashTable *retained, GError **error)
{
	g_autofree gchar *key = retained_key(record);
	g_autofree GParamSpec **properties = NULL;
	VentureEntityClass *klass = VENTURE_ENTITY_GET_CLASS(record);
	guint count, i;
	if (g_hash_table_contains(retained, key)) return TRUE;
	g_hash_table_add(retained, g_steal_pointer(&key));
	properties = venture_entity_class_list_persistent_properties(klass, &count);
	for (i = 0; i < count; i++) {
		const gchar *target = venture_entity_class_get_reference(klass, properties[i]->name);
		g_autoptr(VentureEntity) parent = NULL;
		gint64 id = 0;
		GType type;
		if (target == NULL || G_PARAM_SPEC_VALUE_TYPE(properties[i]) != G_TYPE_INT64) continue;
		g_object_get(record, properties[i]->name, &id, NULL);
		if (id == 0) continue;
		type = record_gtype(target);
		if (type == G_TYPE_INVALID) continue;
		parent = venture_database_get(self->database, type, id, error);
		if (error != NULL && *error != NULL) return FALSE;
		if (parent != NULL && !retain_history(self, parent, retained, error)) return FALSE;
	}
	return TRUE;
}

static gboolean
batch_contains_record(GPtrArray *rows, const gchar *type, gint64 id)
{
	guint i;
	for (i = 0; i < rows->len; i++) {
		g_autofree gchar *record_type = NULL;
		gint64 record_id;
		g_object_get(g_ptr_array_index(rows, i), "record-type", &record_type, "record-id", &record_id, NULL);
		if (record_id == id && !g_strcmp0(type, record_type)) return TRUE;
	}
	return FALSE;
}

static GPtrArray *
rollback_related(VentureCrmImportService *self, GType type, gint64 org, const gchar *field, gint64 id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, org); venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, error)) return NULL;
	return venture_database_find(self->database, query, error);
}

static gboolean
rollback_receipt(VentureCrmImportService *self, VentureEntity *payment, GPtrArray *rows,
	GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) allocations = NULL, credits = NULL;
	gint64 org = venture_entity_get_organization_id(payment), customer;
	guint i, j;
	g_object_get(payment, "customer-id", &customer, NULL);
	allocations = rollback_related(self, VENTURE_TYPE_PAYMENT_ALLOCATION, org, "payment-id", venture_entity_get_id(payment), error);
	if (allocations == NULL) return FALSE;
	for (i = 0; i < allocations->len; i++) {
		VentureEntity *allocation = g_ptr_array_index(allocations, i);
		g_autoptr(GPtrArray) refunds = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureRefund) refund = venture_refund_new();
		gint64 invoice;
		g_object_get(allocation, "invoice-id", &invoice, "amount", &amount, NULL);
		if (!batch_contains_record(rows, "invoice", invoice))
			return refuse(error, "an imported payment was applied outside this batch; reconcile it before rollback");
		refunds = rollback_related(self, VENTURE_TYPE_REFUND, org, "allocation-id", venture_entity_get_id(allocation), error);
		if (refunds == NULL) return FALSE;
		for (j = 0; j < refunds->len; j++) {
			g_autoptr(VentureMoney) refunded = NULL;
			VentureMoney *remaining;
			g_object_get(g_ptr_array_index(refunds, j), "amount", &refunded, NULL);
			remaining = venture_money_subtract(amount, refunded, error);
			if (remaining == NULL) return FALSE;
			venture_money_free(amount); amount = remaining;
		}
		if (venture_money_is_zero(amount)) continue;
		venture_entity_set_organization_id(VENTURE_ENTITY(refund), org);
		g_object_set(refund, "customer-id", customer, "allocation-id", venture_entity_get_id(allocation),
			"date", now, "amount", amount, "reference", "CRM import rollback", NULL);
		if (!venture_database_save(self->database, VENTURE_ENTITY(refund), actor, error)) return FALSE;
	}
	credits = rollback_related(self, VENTURE_TYPE_CUSTOMER_CREDIT, org, "payment-id", venture_entity_get_id(payment), error);
	if (credits == NULL) return FALSE;
	for (i = 0; i < credits->len; i++) {
		VentureEntity *credit = g_ptr_array_index(credits, i);
		g_autoptr(VentureMoney) remaining = NULL;
		g_autoptr(VentureRefund) refund = venture_refund_new();
		g_object_get(credit, "remaining", &remaining, NULL);
		if (remaining == NULL || venture_money_is_zero(remaining)) continue;
		venture_entity_set_organization_id(VENTURE_ENTITY(refund), org);
		g_object_set(refund, "customer-id", customer, "credit-id", venture_entity_get_id(credit),
			"date", now, "amount", remaining, "reference", "CRM import rollback", NULL);
		if (!venture_database_save(self->database, VENTURE_ENTITY(refund), actor, error)) return FALSE;
	}
	return TRUE;
}

static gboolean
rollback_commercial(VentureCrmImportService *self, GPtrArray *rows, GHashTable *retained,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	guint i, pass;
	/* Reverse only receipts this batch created, then void its issued invoices.
	 * Original events, cash projections and journals remain immutable. */
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < rows->len; i++) {
			g_autofree gchar *type = NULL;
			g_autoptr(VentureEntity) record = NULL;
			gint64 id;
			gboolean created, historical = FALSE;
			g_object_get(g_ptr_array_index(rows, i), "record-type", &type, "record-id", &id, "created", &created, NULL);
			if (!created || id <= 0 || type == NULL) continue;
			if (pass == 0 && g_strcmp0(type, "payment")) continue;
			if (pass == 1 && g_strcmp0(type, "invoice") && g_strcmp0(type, "quote") && g_strcmp0(type, "sales_order")) continue;
			record = venture_database_get(self->database, record_gtype(type), id, error);
			if (record == NULL) return FALSE;
			if (pass == 0) {
				if (!rollback_receipt(self, record, rows, now, actor, error)) return FALSE;
				historical = TRUE;
			} else if (VENTURE_IS_INVOICE(record)) {
				gint status;
				g_object_get(record, "status", &status, NULL);
				historical = status != VENTURE_INVOICE_STATUS_DRAFT;
				if (historical && status != VENTURE_INVOICE_STATUS_VOID &&
					!venture_settlement_service_transition(venture_settlement_service_get(self->database),
						VENTURE_INVOICE(record), "void", now, actor, error)) return FALSE;
			} else if (VENTURE_IS_QUOTE(record)) {
				gint status; g_object_get(record, "status", &status, NULL); historical = status != VENTURE_QUOTE_DRAFT;
			} else {
				g_autofree gchar *status = string_of(record, "status"); historical = g_strcmp0(status, "draft") != 0;
			}
			if (historical) {
				g_autoptr(GPtrArray) lines = NULL;
				const gchar *field = VENTURE_IS_INVOICE(record) ? "invoice-id" : (VENTURE_IS_QUOTE(record) ? "quote-id" : "sales-order-id");
				GType line_type = VENTURE_IS_INVOICE(record) ? VENTURE_TYPE_INVOICE_LINE : (VENTURE_IS_QUOTE(record) ? VENTURE_TYPE_QUOTE_LINE : VENTURE_TYPE_SALES_ORDER_LINE);
				guint l;
				if (!retain_history(self, record, retained, error)) return FALSE;
				if (pass == 0) continue;
				lines = rollback_related(self, line_type, venture_entity_get_organization_id(record), field, id, error);
				if (lines == NULL) return FALSE;
				for (l = 0; l < lines->len; l++)
					if (!retain_history(self, g_ptr_array_index(lines, l), retained, error)) return FALSE;
			}
		}
	}
	return TRUE;
}
