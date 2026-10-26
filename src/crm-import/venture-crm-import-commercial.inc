/* SPDX-License-Identifier: AGPL-3.0-or-later */

static VentureEntity *
commercial_record(Run *run, Table *table, gchar **row, GError **error)
{
	gint64 id = resolve_ref(run, table->object, cell(table, row, "id"), error);
	if (id <= 0) return NULL;
	return venture_database_get(run->self->database, table->module->type(), id, error);
}

static gboolean
commercial_created(Run *run, Table *table, gchar **row)
{
	VentureEntity *tracking = run_row(run, table->object, cell(table, row, "id"));
	gboolean created = FALSE;
	if (tracking != NULL) g_object_get(tracking, "created", &created, NULL);
	return created;
}

static gboolean
commercial_date(Table *table, gchar **row, const gchar *field, GDateTime **date, GError **error)
{
	if (!parse_time(cell(table, row, field), date, table->file, cell(table, row, "id"), field, error)) return FALSE;
	if (*date == NULL) return refuse(error, "%s %s needs its original %s", table->file, cell(table, row, "id"), field);
	return TRUE;
}

static gboolean
commercial_issue(Run *run, Table *table, gchar **row, GError **error)
{
	const gchar *status = native_mapped_value(run, table, "status", cell(table, row, "status"));
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GDateTime) issued = NULL;
	g_autoptr(GDateTime) changed = NULL;
	if (blank(status) || !g_ascii_strcasecmp(status, "draft") || !commercial_created(run, table, row)) return TRUE;
	record = commercial_record(run, table, row, error);
	if (record == NULL || !commercial_date(table, row, "issued_at", &issued, error)) return FALSE;
	if (!venture_settlement_service_transition(venture_settlement_service_get(run->self->database),
		VENTURE_INVOICE(record), "sent", issued, run->actor, error)) return FALSE;
	if (!g_strcmp0(status, "paid") || !g_strcmp0(status, "partially_paid") || !g_strcmp0(status, "sent")) return TRUE;
	if (!commercial_date(table, row, "status_at", &changed, error)) return FALSE;
	return venture_settlement_service_transition(venture_settlement_service_get(run->self->database),
		VENTURE_INVOICE(record), status, changed, run->actor, error);
}

static gboolean
commercial_quote(Run *run, Table *table, gchar **row, GError **error)
{
	const gchar *status = native_mapped_value(run, table, "status", cell(table, row, "status"));
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureQuote) proposed = venture_quote_new();
	g_autoptr(GDateTime) issued = NULL, changed = NULL;
	gint state;
	gint64 invoice;
	if (blank(status) || !g_ascii_strcasecmp(status, "draft") || !commercial_created(run, table, row)) return TRUE;
	record = commercial_record(run, table, row, error);
	if (record == NULL || !commercial_date(table, row, "issued_at", &issued, error) ||
		!commercial_date(table, row, "status_at", &changed, error) ||
		!venture_entity_set_field_from_string(VENTURE_ENTITY(proposed), "status", status, error)) return FALSE;
	g_object_get(proposed, "status", &state, NULL);
	invoice = resolve_ref(run, "invoice", cell(table, row, "invoice_id"), error);
	if (invoice < 0) return FALSE;
	if (invoice > 0) g_object_set(record, "invoice-id", invoice, NULL);
	return venture_quote_service_import_history(venture_database_get_quote_service(run->self->database),
		VENTURE_QUOTE(record), state, issued, changed, cell(table, row, "accepted_by"), run->actor, error);
}

static gboolean
commercial_order(Run *run, Table *table, gchar **row, GPtrArray *tables, GError **error)
{
	const gchar *status = native_mapped_value(run, table, "status", cell(table, row, "status"));
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(GDateTime) shipped = NULL;
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	gint64 invoice;
	guint t, r;
	if (blank(status) || !g_ascii_strcasecmp(status, "draft") || !commercial_created(run, table, row)) return TRUE;
	record = commercial_record(run, table, row, error);
	if (record == NULL || !parse_time(cell(table, row, "shipped_at"), &shipped, table->file,
		cell(table, row, "id"), "shipped_at", error)) return FALSE;
	invoice = resolve_ref(run, "invoice", cell(table, row, "invoice_id"), error);
	if (invoice < 0) return FALSE;
	if (invoice > 0) g_object_set(record, "invoice-id", invoice, NULL);
	for (t = 0; t < tables->len; t++) {
		Table *lt = g_ptr_array_index(tables, t);
		if (g_strcmp0(lt->object, "sales_order_line")) continue;
		for (r = 1; r < lt->rows->len; r++) {
			gchar **lr = g_ptr_array_index(lt->rows, r);
			g_autoptr(VentureEntity) line = NULL;
			const gchar *fields[] = { "allocated-qty", "fulfilled-qty", "invoiced-qty" };
			guint f;
			gint64 quantity = 0;
			if (g_strcmp0(cell(lt, lr, "sales_order_id"), cell(table, row, "id"))) continue;
			line = commercial_record(run, lt, lr, error);
			if (line == NULL) return FALSE;
			g_object_get(line, "quantity", &quantity, NULL);
			for (f = 0; f < G_N_ELEMENTS(fields); f++) {
				const gchar *value = native_cell(lt, lr, fields[f]);
				if (!blank(value)) {
					if (!venture_entity_set_field_from_string(line, fields[f], value, error)) return FALSE;
				} else if ((f == 1 && !g_strcmp0(status, "fulfilled")) || (f == 2 && !g_strcmp0(status, "invoiced")))
					g_object_set(line, fields[f], quantity, NULL);
			}
			g_ptr_array_add(lines, g_steal_pointer(&line));
		}
	}
	return venture_sales_order_service_import_history(venture_sales_order_service_get(run->self->database),
		VENTURE_SALES_ORDER(record), status, shipped, lines, run->actor, error);
}

static gboolean
import_commercial_history(Run *run, GPtrArray *tables, GError **error)
{
	guint t, r;
	for (t = 0; t < tables->len; t++) {
		Table *table = g_ptr_array_index(tables, t);
		if (table->module == NULL || g_strcmp0(table->module->object, "invoice")) continue;
		for (r = 1; r < table->rows->len; r++)
			if (!commercial_issue(run, table, g_ptr_array_index(table->rows, r), error)) return FALSE;
	}
	for (t = 0; t < tables->len; t++) {
		Table *table = g_ptr_array_index(tables, t);
		if (table->module != NULL && !g_strcmp0(table->module->object, "payment") && !import_native_records(run, table, error)) return FALSE;
	}
	for (t = 0; t < tables->len; t++) {
		Table *table = g_ptr_array_index(tables, t);
		for (r = 1; r < table->rows->len; r++) {
			gchar **row = g_ptr_array_index(table->rows, r);
			if (table->module != NULL && !g_strcmp0(table->module->object, "quote") && !commercial_quote(run, table, row, error)) return FALSE;
			if (table->module != NULL && !g_strcmp0(table->module->object, "sales_order") && !commercial_order(run, table, row, tables, error)) return FALSE;
			if (table->module != NULL && !g_strcmp0(table->module->object, "invoice")) {
				const gchar *status = native_mapped_value(run, table, "status", cell(table, row, "status"));
				g_autoptr(VentureEntity) record = NULL;
				g_autoptr(VentureInvoice) expected = venture_invoice_new();
				gint actual, wanted;
				if (blank(status)) continue;
				record = commercial_record(run, table, row, error);
				if (record == NULL || !venture_entity_set_field_from_string(VENTURE_ENTITY(expected), "status", status, error)) return FALSE;
				g_object_get(record, "status", &actual, NULL); g_object_get(expected, "status", &wanted, NULL);
				if (actual != wanted) return refuse(error, "invoice %s: payments do not reconcile to source status %s", cell(table, row, "id"), status);
			}
		}
	}
	return TRUE;
}
