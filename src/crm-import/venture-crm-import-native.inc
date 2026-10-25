/* SPDX-License-Identifier: AGPL-3.0-or-later */

static const gchar *
native_source_object(const gchar *property)
{
	if (g_str_equal(property, "customer-id")) return "company";
	if (g_str_equal(property, "company-id")) return "company";
	if (g_str_equal(property, "contact-id")) return "contact";
	if (g_str_equal(property, "deal-id")) return "deal";
	if (g_str_equal(property, "product-id")) return "product";
	if (g_str_equal(property, "price-list-id")) return "price_book";
	if (g_str_equal(property, "quote-id")) return "quote";
	if (g_str_equal(property, "sales-order-id")) return "sales_order";
	if (g_str_equal(property, "invoice-id")) return "invoice";
	return NULL;
}

static const gchar *
native_cell(Table *table, gchar **row, const gchar *property)
{
	g_autofree gchar *canonical = canonical_name(property);
	const gchar *value = cell(table, row, canonical);
	if (!blank(value))
		return value;
	if (g_str_equal(property, "price-list-id")) return cell(table, row, "price_book_id");
	if (g_str_equal(property, "title")) return cell(table, row, "subject");
	if (g_str_equal(property, "description")) return cell(table, row, "body");
	if (g_str_equal(property, "assignee")) return cell(table, row, "owner");
	if (g_str_equal(property, "notes")) return cell(table, row, "description");
	return value;
}

static const gchar *
native_mapped_value(Run *run, Table *table, const gchar *property, const gchar *value)
{
	JsonObject *all = !blank(value) ? obj_obj(run->manifest, "value_maps") : NULL;
	JsonObject *module = all != NULL ? obj_obj(all, table->file) : NULL;
	g_autofree gchar *canonical = canonical_name(property);
	JsonObject *field = module != NULL ? obj_obj(module, canonical) : NULL;
	const gchar *mapped = field != NULL ? obj_str(field, value) : NULL;
	return mapped != NULL ? mapped : value;
}

static gboolean
native_fill(Run *run, Table *table, gchar **row, const ImportModule *module,
	VentureEntity *record, GError **error)
{
	g_auto(GStrv) fields = g_strsplit(module->fields, ",", -1);
	guint i;
	venture_entity_set_organization_id(record, run->org);
	if (VENTURE_IS_SALES_ORDER(record)) g_object_set(record, "status", "draft", NULL);
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(record), "venture-id") != NULL)
		g_object_set(record, "venture-id", venture_json_object_get_int(run->manifest, "venture_id", 0), NULL);
	for (i = 0; fields[i] != NULL; i++)
	{
		const gchar *property = fields[i];
		const gchar *value = native_cell(table, row, property);
		const gchar *reference = native_source_object(property);
		GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(record), property);
		if (blank(value))
			continue;
		/* Headers and lines are composed before their dated history is applied. */
		if (g_str_equal(property, "status") && (VENTURE_IS_QUOTE(record) || VENTURE_IS_SALES_ORDER(record)))
			continue;
		if (spec == NULL)
			return refuse(error, "%s: destination has no field %s", table->file, property);
		if (reference != NULL)
		{
			gint64 id = resolve_ref(run, reference, value, error);
			if (id < 0) return FALSE;
			if (id == 0)
				return refuse(error, "%s %s: unresolved %s %s", table->file, cell(table, row, "id"), reference, value);
			g_object_set(record, property, id, NULL);
		}
		else if (G_PARAM_SPEC_VALUE_TYPE(spec) == VENTURE_TYPE_MONEY)
		{
			const gchar *currency = cell(table, row, "currency");
			g_autoptr(VentureMoney) amount = NULL;
			if (blank(currency)) currency = obj_str(run->manifest, "currency");
			if (blank(currency))
				return refuse(error, "%s %s: %s needs a currency", table->file, cell(table, row, "id"), property);
			amount = venture_money_from_string(value, currency, error);
			if (amount == NULL) return FALSE;
			g_object_set(record, property, amount, NULL);
		}
		else
		{
			if (g_str_equal(property, "owner") || g_str_equal(property, "assignee"))
				value = owner_of(run, value);
			else
				value = native_mapped_value(run, table, property, value);
			if (!venture_entity_set_field_from_string(record, property, value, error))
				return FALSE;
		}
	}
	if (VENTURE_IS_LEAD(record) && blank(cell(table, row, "name")))
	{
		const gchar *first = cell(table, row, "first_name");
		const gchar *last = cell(table, row, "last_name");
		g_autofree gchar *name = g_strstrip(g_strdup_printf("%s %s", first != NULL ? first : "", last != NULL ? last : ""));
		g_object_set(record, "name", name, "source", run->source, NULL);
	}
	if (g_str_equal(module->object, "vendor"))
		g_object_set(record, "kind", VENTURE_COMPANY_KIND_SUPPLIER, "active", TRUE, NULL);
	if (VENTURE_IS_TICKET(record))
		g_object_set(record, "kind", VENTURE_TICKET_KIND_EXTERNAL, NULL);
	if (VENTURE_IS_PAYMENT(record)) {
		gint64 invoice_id = 0, customer_id = 0;
		g_autofree gchar *external = NULL;
		g_object_get(record, "invoice-id", &invoice_id, "customer-id", &customer_id, "external-id", &external, NULL);
		if (invoice_id > 0 && customer_id == 0) {
			g_autoptr(VentureEntity) invoice = venture_database_get(run->self->database, VENTURE_TYPE_INVOICE, invoice_id, error);
			if (invoice == NULL) return FALSE;
			g_object_get(invoice, "company-id", &customer_id, NULL);
			g_object_set(record, "customer-id", customer_id, NULL);
		}
		if (blank(external)) {
			g_autofree gchar *key = g_strdup_printf("crm:%s:%s", run->source, cell(table, row, "id"));
			g_object_set(record, "external-id", key, NULL);
		}
	}
	if (VENTURE_IS_INVOICE(record))
	{
		g_object_set(record, "dunning-disabled", TRUE, "late-fee-exempt", TRUE, NULL);
	}
	return TRUE;
}

/* Exported users are an identity mapping, never permission to create a
 * login or transfer an account from another organization. */
static gboolean
import_owner_map(Run *run, Table *table, GError **error)
{
	JsonObject *map = obj_obj(run->manifest, "owner_map");
	guint r;
	for (r = 1; r < table->rows->len; r++)
	{
		gchar **row = g_ptr_array_index(table->rows, r);
		const gchar *id = cell(table, row, "id");
		const gchar *mapped = map != NULL && !blank(id) ? obj_str(map, id) : NULL;
		VentureEntity *tracking = blank(id) ? NULL : run_row(run, table->object, id);
		g_autofree gchar *username = g_strdup(mapped);
		const gchar *name = cell(table, row, "name");
		const gchar *email = cell(table, row, "email");
		if (tracking == NULL) continue;
		if (blank(username))
			return refuse(error, "user %s needs an explicit owner_map destination", id);
		if (!blank(name)) json_object_set_string_member(map, name, username);
		if (!blank(email)) json_object_set_string_member(map, email, username);
		if (row_pending(tracking) &&
			!finish_row(run, tracking, "matched", "Owner mapping only; no login created", NULL, 0, FALSE, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
import_stage_history(Run *run, Table *table, gchar **row, VentureEntity *tracking, GError **error)
{
	g_autoptr(VentureDealStageEntry) entry = venture_deal_stage_entry_new();
	g_autoptr(GDateTime) when = NULL;
	StageTarget target;
	gint64 deal;
	const gchar *id = cell(table, row, "id");
	const gchar *stage = cell(table, row, "stage");
	const gchar *from = cell(table, row, "from_stage");
	if (blank(stage)) return refuse(error, "stage history %s needs a stage", id);
	deal = resolve_ref(run, "deal", cell(table, row, "deal_id"), error);
	if (deal < 0) return FALSE;
	if (deal == 0) return refuse(error, "stage history %s has an unresolved deal", id);
	if (!resolve_stage(run->self, run->org, run->manifest, stage, &target, error) ||
		!parse_time(cell(table, row, "occurred_at"), &when, "stage_history", id, "timestamp", error))
		return FALSE;
	venture_entity_set_organization_id(VENTURE_ENTITY(entry), run->org);
	g_object_set(entry, "deal-id", deal, "to-stage", target.stage_id, "entered-at", when,
		"by", owner_of(run, cell(table, row, "owner")), "note", cell(table, row, "note"), NULL);
	if (!blank(from))
	{
		if (!resolve_stage(run->self, run->org, run->manifest, from, &target, error)) return FALSE;
		g_object_set(entry, "from-stage", target.stage_id, NULL);
	}
	if (!apply_custom_fields(run, table, row, VENTURE_ENTITY(entry), error) ||
		!venture_deal_service_import_history(venture_database_get_deal_service(run->self->database), entry, run->actor, error))
		return FALSE;
	return finish_row(run, tracking, "imported", NULL, "deal_stage_entry", venture_entity_get_id(VENTURE_ENTITY(entry)), TRUE, error);
}

/* A rolled-back draft can still reserve its unique document number.
 * Restore only evidence created for this exact vendor/object/source id;
 * never match a different customer's document by number. */
static VentureEntity *
restore_native_record(Run *run, Table *table, const gchar *id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CRM_IMPORT_ROW);
	g_autoptr(VentureEntity) previous = NULL;
	g_autoptr(VentureEntity) record = NULL;
	gint64 record_id = 0;
	g_autofree gchar *record_type = NULL;
	venture_query_set_organization(query, run->org);
	if (!venture_query_add_filter_string(query, "source", VENTURE_FILTER_OP_EQ, run->source, error) ||
		!venture_query_add_filter_string(query, "source-object", VENTURE_FILTER_OP_EQ, table->object, error) ||
		!venture_query_add_filter_string(query, "source-id", VENTURE_FILTER_OP_EQ, id, error) ||
		!venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, "rolled_back", error) ||
		!venture_query_add_filter_int(query, "created", VENTURE_FILTER_OP_EQ, 1, error) ||
		!venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, error))
		return NULL;
	previous = venture_database_find_one(run->self->database, query, error);
	if (previous == NULL) return NULL;
	g_object_get(previous, "record-id", &record_id, "record-type", &record_type, NULL);
	if (record_type == NULL || venture_entity_registry_lookup(venture_entity_registry_get_default(), record_type) != table->module->type())
	{
		refuse(error, "%s %s: destination type differs from the rolled-back import", table->file, id);
		return NULL;
	}
	record = venture_database_get(run->self->database, table->module->type(), record_id, error);
	if (record == NULL) return NULL;
	if (venture_entity_get_organization_id(record) != run->org)
	{
		refuse(error, "%s %s: rolled-back record belongs to another organization", table->file, id);
		return NULL;
	}
	if (!venture_entity_is_deleted(record))
	{
		refuse(error, "%s %s: rolled-back record was restored outside this import", table->file, id);
		return NULL;
	}
	if (VENTURE_IS_SALES_ORDER(record) || VENTURE_IS_SALES_ORDER_LINE(record))
	{
		if (!venture_sales_order_service_set_draft_deleted(venture_sales_order_service_get(run->self->database),
			record, FALSE, run->actor, error)) return NULL;
		return venture_database_get(run->self->database, table->module->type(), record_id, error);
	}
	if (!venture_database_restore(run->self->database, record, run->actor, error)) return NULL;
	return g_steal_pointer(&record);
}

/* The accounting importer may have created the same document already.
 * Matching is organization-scoped; conflicting customer identities refuse
 * the batch rather than silently joining unrelated business records. */
static VentureEntity *
match_commercial_record(Run *run, Table *table, gchar **row, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) existing = NULL;
	const gchar *number = cell(table, row, "number");
	GType type = table->module->type();
	if (type == VENTURE_TYPE_PAYMENT) {
		g_autoptr(VentureEntity) proposed = g_object_new(type, NULL);
		g_autofree gchar *external = NULL;
		gint64 invoice_id = 0;
		gboolean opening_match = FALSE;
		if (!native_fill(run, table, row, table->module, proposed, error)) return NULL;
		g_object_get(proposed, "external-id", &external, "invoice-id", &invoice_id, NULL);
		query = venture_query_new(type);
		venture_query_set_organization(query, run->org);
		if (!venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_EQ, external, error)) return NULL;
		existing = venture_database_find_one(run->self->database, query, error);
		if (existing == NULL && invoice_id > 0) {
			VentureEntity *parent_row = run_row(run, "invoice", cell(table, row, "invoice_id"));
			g_autofree gchar *parent_status = parent_row != NULL ? string_of(parent_row, "status") : NULL;
			if (!g_strcmp0(parent_status, "matched")) {
				g_autoptr(VentureEntity) invoice = venture_database_get(run->self->database, VENTURE_TYPE_INVOICE, invoice_id, error);
				g_autoptr(GDateTime) opening = NULL, wanted_date = NULL;
				g_autoptr(VentureMoney) wanted_amount = NULL;
				g_autoptr(GPtrArray) payments = NULL;
				g_autoptr(VentureQuery) pq = venture_query_new(VENTURE_TYPE_PAYMENT);
				guint p;
				if (invoice == NULL) return NULL;
				g_object_get(invoice, "opening-at", &opening, NULL);
				g_object_get(proposed, "date", &wanted_date, "amount", &wanted_amount, NULL);
				venture_query_set_organization(pq, run->org); venture_query_set_limit(pq, 0);
				if (!venture_query_add_filter_int(pq, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, error)) return NULL;
				payments = venture_database_find(run->self->database, pq, error);
				if (payments == NULL) return NULL;
				for (p = 0; p < payments->len; p++) {
					VentureEntity *candidate = g_ptr_array_index(payments, p);
					g_autoptr(GDateTime) date = NULL;
					g_autoptr(VentureMoney) amount = NULL;
					g_autofree gchar *method = NULL;
					GHashTableIter iter;
					gpointer tracked;
					gboolean from_this_batch = FALSE, used = FALSE;
					g_object_get(candidate, "date", &date, "amount", &amount, "method", &method, NULL);
					if (date == NULL || wanted_date == NULL || amount == NULL || wanted_amount == NULL ||
						venture_money_get_amount(amount) != venture_money_get_amount(wanted_amount) ||
						g_strcmp0(venture_money_get_currency(amount), venture_money_get_currency(wanted_amount))) continue;
					if (g_date_time_compare(date, wanted_date) && !(opening != NULL && !g_strcmp0(method, "opening") &&
						g_date_time_compare(wanted_date, opening) < 0)) continue;
					g_hash_table_iter_init(&iter, run->rows);
					while (g_hash_table_iter_next(&iter, NULL, &tracked)) {
						g_autofree gchar *record_type = string_of(tracked, "record-type");
						gint64 record_id; gboolean created;
						g_object_get(tracked, "record-id", &record_id, "created", &created, NULL);
						if (!g_strcmp0(record_type, "payment") && record_id == venture_entity_get_id(candidate)) {
							from_this_batch = created; used = !created; break;
						}
					}
					if (from_this_batch) continue;
					if (used || existing != NULL) {
						refuse(error, "payment %s needs an explicit unique accounting receipt identity", external); return NULL;
					}
					g_set_object(&existing, candidate);
					opening_match = opening != NULL && !g_strcmp0(method, "opening");
				}
				if (existing == NULL) {
					g_autoptr(VentureMoney) balance = venture_settlement_service_invoice_balance(
						venture_settlement_service_get(run->self->database), invoice_id, NULL, error);
					if (balance == NULL) return NULL;
					if (venture_money_get_amount(balance) <= 0) {
						refuse(error, "payment %s has no matching receipt on an already settled accounting invoice; map its receipt identity", external);
						return NULL;
					}
				}
			}
		}
		if (existing != NULL) {
			g_autoptr(VentureMoney) a = NULL, b = NULL;
			g_autoptr(GDateTime) da = NULL, db = NULL;
			gint64 ca, cb;
			g_object_get(existing, "amount", &a, "date", &da, "customer-id", &ca, NULL);
			g_object_get(proposed, "amount", &b, "date", &db, "customer-id", &cb, NULL);
			if (a == NULL || b == NULL || da == NULL || db == NULL || ca != cb || (!opening_match && g_date_time_compare(da, db)) ||
				venture_money_get_amount(a) != venture_money_get_amount(b) ||
				g_strcmp0(venture_money_get_currency(a), venture_money_get_currency(b))) {
				refuse(error, "payment %s conflicts with an existing accounting receipt", external); return NULL;
			}
		}
		return g_steal_pointer(&existing);
	}
	if (type != VENTURE_TYPE_INVOICE && type != VENTURE_TYPE_QUOTE && type != VENTURE_TYPE_SALES_ORDER)
		return NULL;
	if (type == VENTURE_TYPE_INVOICE && venture_entity_registry_lookup(venture_entity_registry_get_default(), "accounting_cutover_row") != G_TYPE_INVALID) {
		g_autoptr(VentureQuery) cq = venture_query_new(VENTURE_TYPE_ACCOUNTING_CUTOVER_ROW);
		g_autoptr(GPtrArray) mappings = NULL;
		const gchar *source_id = cell(table, row, "accounting_source_id");
		guint m;
		if (blank(source_id)) source_id = cell(table, row, "id");
		venture_query_set_organization(cq, run->org); venture_query_set_limit(cq, 0);
		if (!venture_query_add_filter_string(cq, "source-id", VENTURE_FILTER_OP_EQ, source_id, error) ||
			!venture_query_add_filter_string(cq, "record-type", VENTURE_FILTER_OP_EQ, "invoice", error) ||
			!venture_query_add_filter_string(cq, "status", VENTURE_FILTER_OP_EQ, "imported", error)) return NULL;
		mappings = venture_database_find(run->self->database, cq, error);
		if (mappings == NULL) return NULL;
		for (m = 0; m < mappings->len; m++) {
			g_autoptr(VentureEntity) cutover = NULL, candidate = NULL;
			g_autofree gchar *state = NULL;
			gint64 cutover_id, record_id;
			g_object_get(g_ptr_array_index(mappings, m), "cutover-id", &cutover_id, "record-id", &record_id, NULL);
			cutover = venture_database_get(run->self->database, VENTURE_TYPE_ACCOUNTING_CUTOVER, cutover_id, error);
			if (cutover == NULL) return NULL;
			g_object_get(cutover, "state", &state, NULL);
			if (!g_strcmp0(state, "rolled_back")) continue;
			candidate = venture_database_get(run->self->database, type, record_id, error);
			if (candidate == NULL) return NULL;
			if (venture_entity_is_deleted(candidate) || venture_entity_get_organization_id(candidate) != run->org) continue;
			if (existing != NULL && venture_entity_get_id(existing) != record_id) {
				refuse(error, "invoice source %s has ambiguous accounting matches", source_id); return NULL;
			}
			g_set_object(&existing, candidate);
		}
	}
	if (type == VENTURE_TYPE_INVOICE && !blank(cell(table, row, "external_id"))) {
		g_autoptr(VentureQuery) eq = venture_query_new(type);
		g_autoptr(VentureEntity) by_external = NULL;
		venture_query_set_organization(eq, run->org);
		if (!venture_query_add_filter_string(eq, "external-id", VENTURE_FILTER_OP_EQ, cell(table, row, "external_id"), error)) return NULL;
		by_external = venture_database_find_one(run->self->database, eq, error);
		if (error != NULL && *error != NULL) return NULL;
		if (existing != NULL && by_external != NULL && venture_entity_get_id(existing) != venture_entity_get_id(by_external)) {
			refuse(error, "invoice external id conflicts with the accounting source mapping"); return NULL;
		}
		if (existing == NULL && by_external != NULL) g_set_object(&existing, by_external);
	}
	query = venture_query_new(type);
	venture_query_set_organization(query, run->org);
	if (!blank(number)) {
		g_autoptr(VentureEntity) by_number = NULL;
		if (!venture_query_add_filter_string(query, "number", VENTURE_FILTER_OP_EQ, number, error)) return NULL;
		by_number = venture_database_find_one(run->self->database, query, error);
		if (error != NULL && *error != NULL) return NULL;
		if (existing != NULL && by_number != NULL && venture_entity_get_id(existing) != venture_entity_get_id(by_number)) {
			refuse(error, "invoice source id and document number identify different accounting records"); return NULL;
		}
		if (existing == NULL && by_number != NULL) g_set_object(&existing, by_number);
	}
	if (existing != NULL) {
		gint64 customer = resolve_ref(run, "company", cell(table, row, "company_id"), error);
		gint64 previous = 0;
		g_object_get(existing, "company-id", &previous, NULL);
		if (customer < 0) return NULL;
		if (customer != 0 && previous != customer) {
			refuse(error, "%s %s matches a document number owned by a different customer", table->file, number);
			return NULL;
		}
	}
	return g_steal_pointer(&existing);
}

static gboolean
match_commercial_line(Run *run, Table *table, gchar **row, VentureEntity *tracking,
	gboolean *handled, GError **error)
{
	const gchar *parent = NULL, *field = NULL;
	VentureEntity *parent_row;
	g_autofree gchar *status = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) candidates = NULL;
	g_autoptr(VentureEntity) proposed = NULL;
	VentureEntity *matched = NULL;
	guint i;
	gint64 parent_id = 0;
	*handled = FALSE;
	if (g_str_equal(table->module->object, "invoice_line")) { parent = "invoice"; field = "invoice-id"; }
	if (g_str_equal(table->module->object, "quote_line")) { parent = "quote"; field = "quote-id"; }
	if (g_str_equal(table->module->object, "sales_order_line")) { parent = "sales_order"; field = "sales-order-id"; }
	if (parent == NULL) return TRUE;
	parent_row = run_row(run, parent, native_cell(table, row, field));
	if (parent_row == NULL) return TRUE;
	g_object_get(parent_row, "status", &status, "record-id", &parent_id, NULL);
	if (g_strcmp0(status, "matched") != 0) return TRUE;
	*handled = TRUE;
	proposed = g_object_new(table->module->type(), NULL);
	if (!native_fill(run, table, row, table->module, proposed, error)) return FALSE;
	query = venture_query_new(table->module->type());
	venture_query_set_organization(query, run->org);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, parent_id, error)) return FALSE;
	candidates = venture_database_find(run->self->database, query, error);
	if (candidates == NULL) return FALSE;
	for (i = 0; i < candidates->len; i++) {
		VentureEntity *candidate = g_ptr_array_index(candidates, i);
		g_autofree gchar *a = NULL, *b = NULL;
		g_autoptr(VentureMoney) pa = NULL, pb = NULL;
		gdouble qa = 0, qb = 0;
		gint64 ia = 0, ib = 0, da = 0, db = 0, ta = 0, tb = 0;
		g_object_get(candidate, "description", &a, "unit-price", &pa, NULL);
		g_object_get(proposed, "description", &b, "unit-price", &pb, NULL);
		if (VENTURE_IS_INVOICE_LINE(candidate)) {
			g_object_get(candidate, "quantity", &qa, NULL); g_object_get(proposed, "quantity", &qb, NULL);
		} else {
			g_object_get(candidate, "quantity", &ia, NULL); g_object_get(proposed, "quantity", &ib, NULL);
			qa = ia; qb = ib;
		}
		if (!VENTURE_IS_SALES_ORDER_LINE(candidate)) {
			g_object_get(candidate, "discount-percent", &da, "tax-percent", &ta, NULL);
			g_object_get(proposed, "discount-percent", &db, "tax-percent", &tb, NULL);
		}
		if (g_strcmp0(a, b) == 0 && qa == qb && da == db && ta == tb && pa != NULL && pb != NULL &&
			venture_money_get_amount(pa) == venture_money_get_amount(pb) &&
			g_strcmp0(venture_money_get_currency(pa), venture_money_get_currency(pb)) == 0) {
			if (matched != NULL) return refuse(error, "%s %s has ambiguous matching accounting lines", table->file, cell(table, row, "id"));
			matched = candidate;
		}
	}
	if (matched == NULL) return refuse(error, "%s %s differs from its matched accounting document", table->file, cell(table, row, "id"));
	return finish_row(run, tracking, "matched", "Existing accounting document line", venture_entity_get_entity_name(matched),
		venture_entity_get_id(matched), FALSE, error);
}

static gboolean
import_native_records(Run *run, Table *table, GError **error)
{
	const ImportModule *module = table->module;
	guint r;
	if (module == NULL)
		return refuse(error, "unknown import module %s", table->file);
	if (g_str_equal(module->object, "user"))
		return import_owner_map(run, table, error);
	for (r = 1; r < table->rows->len; r++)
	{
		gchar **row = g_ptr_array_index(table->rows, r);
		const gchar *id = cell(table, row, "id");
		VentureEntity *tracking = blank(id) ? NULL : run_row(run, table->object, id);
		g_autoptr(VentureEntity) record = NULL;
		g_autofree gchar *type = NULL;
		gint64 earlier;
		gboolean handled;
		if (tracking == NULL || !row_pending(tracking)) continue;
		earlier = imported_record(run, table->object, id, &type, error);
		if (earlier < 0) return FALSE;
		if (earlier > 0)
		{
			if (!finish_row(run, tracking, "matched", NULL, type, earlier, FALSE, error))
				return FALSE;
			continue;
		}
		if (g_str_equal(module->object, "stage_history"))
		{
			if (!import_stage_history(run, table, row, tracking, error)) return FALSE;
			continue;
		}
		if (!match_commercial_line(run, table, row, tracking, &handled, error)) return FALSE;
		if (handled) continue;
		record = match_commercial_record(run, table, row, error);
		if (error != NULL && *error != NULL) return FALSE;
		if (record != NULL) {
			if (!finish_row(run, tracking, "matched", VENTURE_IS_PAYMENT(record) ? "Existing accounting receipt matched; its ledger date is retained, original CRM date remains in the manifest" : "Existing accounting record matched by source id or document number",
				venture_entity_get_entity_name(record), venture_entity_get_id(record), FALSE, error)) return FALSE;
			continue;
		}
		record = restore_native_record(run, table, id, error);
		if (error != NULL && *error != NULL) return FALSE;
		if (record == NULL) record = g_object_new(module->type(), NULL);
		if (!native_fill(run, table, row, module, record, error) ||
			!apply_custom_fields(run, table, row, record, error) ||
			!venture_database_save(run->self->database, record, run->actor, error) ||
			!finish_row(run, tracking, "imported", NULL, venture_entity_get_entity_name(record),
				venture_entity_get_id(record), TRUE, error))
			return FALSE;
	}
	return TRUE;
}
