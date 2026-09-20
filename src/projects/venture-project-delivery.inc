/* SPDX-License-Identifier: AGPL-3.0-or-later */

static gboolean
delivery_owned(VentureEntity *record)
{
	return G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_PROJECT_SCOPE) ||
		G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_PROJECT_DELIVERABLE);
}

static gboolean
delivery_check(VentureProjectService *self, VentureEntity *record, gboolean removal, GError **error)
{
	g_autoptr(VentureEntity) previous = NULL;
	gboolean managed;
	if (self->delivery_writing == record && !removal)
	{
		/* Consume before any save signal: a reentrant writer gets no permit. */
		self->delivery_writing = NULL;
		return TRUE;
	}
	if (delivery_owned(record))
		return refuse(error, "scope and delivery evidence require the project actions and are retained");
	if (!G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_CLIENT_PROJECT)) return TRUE;
	managed = number(record, "quote-id") > 0 || number(record, "deal-id") > 0;
	if (venture_entity_is_persisted(record))
	{
		previous = venture_database_get(self->database, VENTURE_TYPE_CLIENT_PROJECT, venture_entity_get_id(record), error);
		if (previous == NULL) return FALSE;
		managed = managed || number(previous, "quote-id") > 0 || number(previous, "deal-id") > 0;
	}
	if (managed) return refuse(error, "sales-linked projects require the delivery actions; their history is retained");
	return TRUE;
}

static gboolean
delivery_save(VentureProjectService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->delivery_writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->delivery_writing = NULL;
	return ok;
}

static const gchar *
delivery_text(GHashTable *params, const gchar *name)
{
	JsonNode *node = g_hash_table_lookup(params, name);
	return node != NULL && !JSON_NODE_HOLDS_NULL(node) ? json_node_get_string(node) : NULL;
}

static gboolean
delivery_bounded(const gchar *value, gsize maximum, GError **error)
{
	if (value == NULL || *value == '\0' || strlen(value) > maximum || !g_utf8_validate(value, -1, NULL))
		return refuse(error, "delivery text must be nonempty, valid UTF-8 and within its documented limit");
	return TRUE;
}

static gchar *
delivery_digest(GHashTable *params)
{
	g_autoptr(GList) keys = g_hash_table_get_keys(params);
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *text = NULL;
	GList *item;
	JsonObject *object = json_object_new();
	json_node_take_object(node, object);
	keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
	for (item = keys; item != NULL; item = item->next)
		json_object_set_member(object, item->data, json_node_copy(g_hash_table_lookup(params, item->data)));
	text = venture_json_to_string(node, FALSE);
	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
}

static VentureEntity *
delivery_find(VentureProjectService *self, GType type, gint64 org,
	const gchar *field, const gchar *key, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	if (!venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_EQ, key, error)) return NULL;
	return venture_database_find_one(self->database, query, error);
}

static gboolean
delivery_same_request(VentureEntity *row, const gchar *digest, GError **error)
{
	g_autofree gchar *stored = NULL;
	g_object_get(row, "request-digest", &stored, NULL);
	if (venture_entity_is_deleted(row) || g_strcmp0(stored, digest))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
			"That delivery identity already exists with different parameters; use its retained record");
		return FALSE;
	}
	return TRUE;
}

static VentureEntity *
delivery_ref(VentureProjectService *self, GType type, gint64 id, gint64 org, GError **error)
{
	g_autoptr(VentureEntity) record = venture_database_get(self->database, type, id, error);
	if (record == NULL) return NULL;
	if (venture_entity_is_deleted(record) || venture_entity_get_organization_id(record) != org)
	{
		refuse(error, "delivery references require live records in the same organization");
		return NULL;
	}
	return g_steal_pointer(&record);
}

static gboolean
delivery_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)entity;
	(void)actor;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "project_scope") == G_TYPE_INVALID)
		return refuse(error, "projects module is disabled");
	return TRUE;
}

gboolean
venture_project_service_record_progress(VentureProjectService *self, VentureEntity *billing,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GError) local_error = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureEntity) scope = NULL;
	g_autoptr(VentureEntity) project = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) allocation = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *currency = NULL;
	gboolean ok = FALSE;
	gint64 org;
	g_return_val_if_fail(VENTURE_IS_PROJECT_SERVICE(self), FALSE);
	/* An absent optional error output must not turn a failed lookup into
	 * a successful "no project" result and lose the financial allocation. */
	if (error == NULL) error = &local_error;
	if (self->database == NULL) return refuse(error, "database is unavailable");
	if (!G_TYPE_CHECK_INSTANCE_TYPE(billing, VENTURE_TYPE_PROGRESS_BILLING) || !venture_entity_is_persisted(billing))
		return refuse(error, "project allocation requires persisted progress billing evidence");
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "project_scope") == G_TYPE_INVALID)
	{
		g_autoptr(VentureEntity) prototype = g_object_new(VENTURE_TYPE_PROJECT_SCOPE, NULL);
		g_autoptr(GHashTable) columns = venture_schema_get_existing_columns(
			venture_database_get_connection(self->database), venture_entity_get_table_name(prototype), error);
		if (columns == NULL) return FALSE;
		if (g_hash_table_size(columns) == 0) return TRUE;
	}
	if (!venture_database_begin(self->database, error)) return FALSE;
	stored = venture_database_get(self->database, VENTURE_TYPE_PROGRESS_BILLING, venture_entity_get_id(billing), error);
	if (stored == NULL) goto out;
	org = venture_entity_get_organization_id(stored);
	key = g_strdup_printf("quote:%" G_GINT64_FORMAT, number(stored, "quote-id"));
	scope = delivery_find(self, VENTURE_TYPE_PROJECT_SCOPE, org, "source-key", key, error);
	if (scope == NULL)
	{
		if (error != NULL && *error != NULL) goto out;
		ok = venture_database_commit(self->database, error);
		goto out;
	}
	if (already_billed(self, org, "progress_billing", venture_entity_get_id(stored), error))
	{
		if (error != NULL && *error != NULL) goto out;
		ok = venture_database_commit(self->database, error);
		goto out;
	}
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "project_scope") == G_TYPE_INVALID)
	{
		refuse(error, "enable projects before invoicing a retained project agreement");
		goto out;
	}
	project = delivery_ref(self, VENTURE_TYPE_CLIENT_PROJECT, number(scope, "project-id"), org, error);
	invoice = delivery_ref(self, VENTURE_TYPE_INVOICE, number(stored, "invoice-id"), org, error);
	if (project == NULL || invoice == NULL) goto out;
	g_object_get(stored, "amount", &amount, NULL);
	g_object_get(project, "currency", &currency, NULL);
	if (amount == NULL || venture_money_is_negative(amount) || g_strcmp0(currency, venture_money_get_currency(amount)))
	{
		refuse(error, "progress allocation must use the project's currency");
		goto out;
	}
	allocation = g_object_new(VENTURE_TYPE_PROJECT_BILLING, "organization-id", org,
		"project-id", venture_entity_get_id(project), "source-type", "progress_billing",
		"source-id", venture_entity_get_id(stored), "invoice-id", venture_entity_get_id(invoice), "amount", amount, NULL);
	if (!write_owned(self, allocation, actor, error)) goto out;
	ok = venture_database_commit(self->database, error);
out:
	if (!ok) venture_database_rollback(self->database);
	return ok;
}

static gboolean
delivery_existing_invoices(VentureProjectService *self, VentureEntity *scope, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureEntity) quote = NULL;
	gint64 org = venture_entity_get_organization_id(scope);
	guint i;
	if (number(scope, "quote-id") == 0) return TRUE;
	quote = delivery_ref(self, VENTURE_TYPE_QUOTE, number(scope, "quote-id"), org, error);
	if (quote == NULL) return FALSE;
	if (number(quote, "invoice-id") > 0)
	{
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureEntity) invoice = delivery_ref(self, VENTURE_TYPE_INVOICE, number(quote, "invoice-id"), org, error);
		g_autoptr(VentureEntity) allocation = NULL;
		if (invoice == NULL) return FALSE;
		g_object_get(scope, "amount", &amount, NULL);
		allocation = g_object_new(VENTURE_TYPE_PROJECT_BILLING, "organization-id", org,
			"project-id", number(scope, "project-id"), "source-type", "project_scope",
			"source-id", venture_entity_get_id(scope), "invoice-id", venture_entity_get_id(invoice), "amount", amount, NULL);
		return write_owned(self, allocation, actor, error);
	}
	query = venture_query_new(VENTURE_TYPE_PROGRESS_BILLING);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "quote-id", VENTURE_FILTER_OP_EQ, number(scope, "quote-id"), error)) return FALSE;
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL) return FALSE;
	for (i = 0; i < rows->len; i++)
		if (!venture_project_service_record_progress(self, g_ptr_array_index(rows, i), actor, error)) return FALSE;
	return TRUE;
}

static VentureEntity *
delivery_handoff(VentureAction *action, VentureEntity *source, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureProjectService *self = venture_action_get_data(action);
	g_autoptr(VentureEntity) project = NULL;
	g_autoptr(VentureEntity) scope = NULL;
	g_autoptr(VentureEntity) customer = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *key = g_strdup_printf("%s:%" G_GINT64_FORMAT,
		venture_entity_get_entity_name(source), venture_entity_get_id(source));
	g_autofree gchar *digest = delivery_digest(params);
	const gchar *name = delivery_text(params, "name");
	const gchar *owner = delivery_text(params, "owner");
	const gchar *description = delivery_text(params, "scope");
	gboolean quote = VENTURE_IS_QUOTE(source);
	gint status;
	gint64 org = venture_entity_get_organization_id(source);
	if (!delivery_bounded(name, 256, error) || !delivery_bounded(owner, 128, error) ||
		!delivery_bounded(description, 65536, error)) return NULL;
	scope = delivery_find(self, VENTURE_TYPE_PROJECT_SCOPE, org, "source-key", key, error);
	if (scope != NULL)
	{
		if (!delivery_same_request(scope, digest, error)) return NULL;
		return delivery_ref(self, VENTURE_TYPE_CLIENT_PROJECT, number(scope, "project-id"), org, error);
	}
	if (error != NULL && *error != NULL) return NULL;
	{
		gint64 deal = quote ? number(source, "deal-id") : venture_entity_get_id(source);
		if (deal > 0)
		{
			g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CLIENT_PROJECT);
			gint64 count;
			venture_query_set_organization(query, org);
			venture_query_set_include_deleted(query, TRUE);
			if (!venture_query_add_filter_int(query, "deal-id", VENTURE_FILTER_OP_EQ, deal, error)) return NULL;
			count = venture_database_count(self->database, query, error);
			if (count < 0) return NULL;
			if (count > 0)
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
					"This deal already has a delivery project; continue its retained workflow");
				return NULL;
			}
		}
	}
	g_object_get(source, quote ? "status" : "stage", &status, quote ? "total" : "value", &amount, NULL);
	if (status != (quote ? VENTURE_QUOTE_ACCEPTED : VENTURE_DEAL_STAGE_WON) || amount == NULL || venture_money_is_negative(amount))
	{
		refuse(error, "handoff requires an accepted quote or won deal with explicit nonnegative value");
		return NULL;
	}
	customer = delivery_ref(self, VENTURE_TYPE_COMPANY, number(source, "company-id"), org, error);
	if (customer == NULL) return NULL;
	project = g_object_new(VENTURE_TYPE_CLIENT_PROJECT, "organization-id", org,
		"name", name, "owner", owner, "delivery-status", VENTURE_PROJECT_DELIVERY_ACTIVE,
		"customer-id", venture_entity_get_id(customer), "venture-id", number(source, "venture-id"),
		"currency", venture_money_get_currency(amount), "budget", amount,
		"billing-kind", quote ? "fixed" : "time", quote ? "quote-id" : "deal-id", venture_entity_get_id(source), NULL);
	if (quote) g_object_set(project, "deal-id", number(source, "deal-id"), NULL);
	if (!delivery_save(self, project, actor, error)) return NULL;
	scope = g_object_new(VENTURE_TYPE_PROJECT_SCOPE, "organization-id", org,
		"project-id", venture_entity_get_id(project), "source-key", key,
		quote ? "quote-id" : "deal-id", venture_entity_get_id(source),
		"scope", description, "owner", owner, "amount", amount,
		"agreed-at", now, "request-digest", digest, NULL);
	if (!delivery_save(self, scope, actor, error)) return NULL;
	if (!delivery_existing_invoices(self, scope, actor, error)) return NULL;
	return g_steal_pointer(&project);
}

static gint64
delivery_id(GHashTable *params, const gchar *name)
{
	JsonNode *node = g_hash_table_lookup(params, name);
	return node != NULL && !JSON_NODE_HOLDS_NULL(node) ? json_node_get_int(node) : 0;
}

static gboolean
delivery_active(VentureEntity *project, GError **error)
{
	gint status;
	g_object_get(project, "delivery-status", &status, NULL);
	if (status != VENTURE_PROJECT_DELIVERY_ACTIVE) return refuse(error, "delivery requires an active project");
	return TRUE;
}

/* A project has one billing model. Priced deliverables and change orders
 * are invoiced against an accepted quote, so they belong only to a project
 * the quote handoff created: a time-and-materials project (deal handoff)
 * bills approved time instead, and a manually created project with a
 * free-text "fixed" kind has no quote for a progress invoice to draw on. */
static gboolean
delivery_fixed_price(VentureEntity *project, GError **error)
{
	g_autofree gchar *kind = NULL;
	g_object_get(project, "billing-kind", &kind, NULL);
	if (number(project, "quote-id") > 0 && !g_strcmp0(kind, "fixed")) return TRUE;
	return refuse(error, project_bills_delivery(project) ?
		"fixed-price billing requires the project a quote handoff created, not a manually labelled billing model" :
		"time-and-materials projects bill approved time; fixed amounts and change orders require a fixed-price quote project");
}

static VentureEntity *
delivery_plan(VentureAction *action, VentureEntity *project, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureProjectService *self = venture_action_get_data(action);
	g_autoptr(VentureEntity) scope = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) deliverable = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) due = NULL;
	g_autofree gchar *currency = NULL;
	g_autofree gchar *owner = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *digest = delivery_digest(params);
	const gchar *input_key = delivery_text(params, "key");
	const gchar *title = delivery_text(params, "title");
	const gchar *amount_text = delivery_text(params, "amount");
	gint64 org = venture_entity_get_organization_id(project);
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "ticket") == G_TYPE_INVALID)
		return refuse(error, "tickets module is disabled"), NULL;
	if (!delivery_bounded(input_key, 128, error) || !delivery_bounded(title, 256, error)) return NULL;
	key = g_strdup_printf("%s:%s", venture_entity_get_uuid(project), input_key);
	deliverable = delivery_find(self, VENTURE_TYPE_PROJECT_DELIVERABLE, org, "request-key", key, error);
	if (deliverable != NULL)
		return delivery_same_request(deliverable, digest, error) ? g_steal_pointer(&deliverable) : NULL;
	if (error != NULL && *error != NULL) return NULL;
	if (!delivery_active(project, error)) return NULL;
	scope = delivery_ref(self, VENTURE_TYPE_PROJECT_SCOPE, delivery_id(params, "scope_id"), org, error);
	if (scope == NULL) return NULL;
	if (number(scope, "project-id") != venture_entity_get_id(project))
		return refuse(error, "agreement belongs to a different project"), NULL;
	due = venture_time_from_string(delivery_text(params, "due"), error);
	if (due == NULL) return NULL;
	g_object_get(project, "currency", &currency, "owner", &owner, NULL);
	if (amount_text != NULL)
	{
		if (!delivery_fixed_price(project, error)) return NULL;
		amount = venture_money_from_string(amount_text, currency, error);
		if (amount == NULL) return NULL;
		if (venture_money_is_negative(amount) || g_strcmp0(venture_money_get_currency(amount), currency))
			return refuse(error, "planned amount must be nonnegative in the project currency"), NULL;
	}
	ticket = g_object_new(VENTURE_TYPE_TICKET, "organization-id", org, "title", title,
		"assignee", owner, "company-id", number(project, "customer-id"),
		"venture-id", number(project, "venture-id"), "due-at", due, NULL);
	if (!venture_database_save(self->database, ticket, actor, error)) return NULL;
	deliverable = g_object_new(VENTURE_TYPE_PROJECT_DELIVERABLE, "organization-id", org,
		"project-id", venture_entity_get_id(project), "scope-id", venture_entity_get_id(scope),
		"ticket-id", venture_entity_get_id(ticket), "request-key", key,
		"request-digest", digest, "amount", amount, NULL);
	if (!delivery_save(self, deliverable, actor, error)) return NULL;
	return g_steal_pointer(&deliverable);
}

static VentureEntity *
delivery_accept(VentureAction *action, VentureEntity *deliverable, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureProjectService *self = venture_action_get_data(action);
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) project = NULL;
	g_autoptr(GDateTime) accepted = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *previous = NULL;
	const gchar *evidence = delivery_text(params, "evidence");
	gint status;
	gint64 org = venture_entity_get_organization_id(deliverable);
	if (!delivery_bounded(evidence, 65536, error)) return NULL;
	g_object_get(deliverable, "accepted-at", &accepted, "acceptance", &previous, NULL);
	if (accepted != NULL)
	{
		if (g_strcmp0(previous, evidence)) return refuse(error, "acceptance evidence is immutable"), NULL;
		return g_object_ref(deliverable);
	}
	project = delivery_ref(self, VENTURE_TYPE_CLIENT_PROJECT, number(deliverable, "project-id"), org, error);
	if (project == NULL || !delivery_active(project, error)) return NULL;
	ticket = delivery_ref(self, VENTURE_TYPE_TICKET, number(deliverable, "ticket-id"), org, error);
	if (ticket == NULL) return NULL;
	g_object_get(ticket, "status", &status, NULL);
	if (status != VENTURE_TICKET_STATUS_DONE) return refuse(error, "finish the linked ticket before accepting its delivery"), NULL;
	g_object_set(deliverable, "accepted-at", now, "accepted-by", actor != NULL ? actor->name : "system",
		"acceptance", evidence, NULL);
	if (!delivery_save(self, deliverable, actor, error)) return NULL;
	return g_object_ref(deliverable);
}

static VentureEntity *
delivery_bill(VentureAction *action, VentureEntity *deliverable, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureProjectService *self = venture_action_get_data(action);
	g_autoptr(VentureEntity) scope = NULL;
	g_autoptr(VentureEntity) project = NULL;
	g_autoptr(VentureEntity) quote = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) accepted = NULL;
	g_autofree gchar *mode = NULL;
	gint64 org = venture_entity_get_organization_id(deliverable);
	(void)params;
	if (number(deliverable, "invoice-id") > 0)
		return delivery_ref(self, VENTURE_TYPE_INVOICE, number(deliverable, "invoice-id"), org, error);
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "progress_billing") == G_TYPE_INVALID)
		return refuse(error, "progress invoicing is disabled"), NULL;
	g_object_get(deliverable, "accepted-at", &accepted, "amount", &amount, NULL);
	if (accepted == NULL || amount == NULL || venture_money_get_amount(amount) <= 0)
		return refuse(error, "billing requires accepted delivery with a positive agreed slice"), NULL;
	project = delivery_ref(self, VENTURE_TYPE_CLIENT_PROJECT, number(deliverable, "project-id"), org, error);
	/* Closing delivery does not erase the right to invoice already accepted work. */
	if (project == NULL) return NULL;
	scope = delivery_ref(self, VENTURE_TYPE_PROJECT_SCOPE, number(deliverable, "scope-id"), org, error);
	if (scope == NULL) return NULL;
	quote = delivery_ref(self, VENTURE_TYPE_QUOTE, number(scope, "quote-id"), org, error);
	if (quote == NULL) return NULL;
	g_object_get(quote, "billing-mode", &mode, NULL);
	if (g_strcmp0(mode, "progress"))
		return refuse(error, "this quote was invoiced in full; use its existing invoice"), NULL;
	invoice = venture_progress_service_invoice(venture_progress_service_get(self->database), VENTURE_QUOTE(quote), 0, amount, actor, error);
	if (invoice == NULL) return NULL;
	g_object_set(deliverable, "invoice-id", venture_entity_get_id(invoice), NULL);
	if (!delivery_save(self, deliverable, actor, error)) return NULL;
	return g_steal_pointer(&invoice);
}

static VentureEntity *
delivery_link_invoice(VentureAction *action, VentureEntity *deliverable, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureProjectService *self = venture_action_get_data(action);
	g_autoptr(VentureEntity) scope = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) billing = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureMoney) expected = NULL;
	g_autoptr(VentureMoney) actual = NULL;
	g_autoptr(GDateTime) accepted = NULL;
	gint64 org = venture_entity_get_organization_id(deliverable);
	gint64 id = delivery_id(params, "invoice_id");
	gint64 count;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "progress_billing") == G_TYPE_INVALID)
		return refuse(error, "quotes module is disabled"), NULL;
	if (id <= 0) return refuse(error, "select a persisted progress invoice"), NULL;
	if (number(deliverable, "invoice-id") > 0)
	{
		if (number(deliverable, "invoice-id") != id)
			return refuse(error, "delivery already has a different invoice"), NULL;
		return delivery_ref(self, VENTURE_TYPE_INVOICE, id, org, error);
	}
	g_object_get(deliverable, "accepted-at", &accepted, "amount", &expected, NULL);
	if (accepted == NULL || expected == NULL || venture_money_get_amount(expected) <= 0)
		return refuse(error, "accept a priced delivery before linking its invoice"), NULL;
	scope = delivery_ref(self, VENTURE_TYPE_PROJECT_SCOPE, number(deliverable, "scope-id"), org, error);
	if (scope == NULL) return NULL;
	invoice = delivery_ref(self, VENTURE_TYPE_INVOICE, id, org, error);
	if (invoice == NULL) return NULL;
	query = venture_query_new(VENTURE_TYPE_PROGRESS_BILLING);
	venture_query_set_organization(query, org);
	if (!venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, id, error) ||
		!venture_query_add_filter_int(query, "quote-id", VENTURE_FILTER_OP_EQ, number(scope, "quote-id"), error)) return NULL;
	billing = venture_database_find_one(self->database, query, error);
	if (billing == NULL)
	{
		if (error == NULL || *error == NULL) refuse(error, "invoice is not progress billing for this agreement");
		return NULL;
	}
	g_object_get(billing, "amount", &actual, NULL);
	if (!venture_money_equal(expected, actual)) return refuse(error, "invoice must exactly match the accepted slice"), NULL;
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_PROJECT_DELIVERABLE);
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	if (!venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, id, error)) return NULL;
	count = venture_database_count(self->database, query, error);
	if (count < 0) return NULL;
	if (count != 0) return refuse(error, "invoice is already allocated to a delivery"), NULL;
	if (!venture_project_service_record_progress(self, billing, actor, error)) return NULL;
	g_object_set(deliverable, "invoice-id", id, NULL);
	if (!delivery_save(self, deliverable, actor, error)) return NULL;
	return g_steal_pointer(&invoice);
}

static VentureEntity *
delivery_manage(VentureAction *action, VentureEntity *project, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureProjectService *self = venture_action_get_data(action);
	const gchar *status = delivery_text(params, "status");
	const gchar *owner = delivery_text(params, "owner");
	const gchar *reason = delivery_text(params, "reason");
	gint old, selected;
	if (!delivery_bounded(owner, 128, error) || !delivery_bounded(reason, 65536, error)) return NULL;
	if (!venture_enum_from_nick(VENTURE_TYPE_PROJECT_DELIVERY_STATUS, status, &selected))
		return refuse(error, "unknown project delivery status"), NULL;
	g_object_get(project, "delivery-status", &old, NULL);
	if ((old == VENTURE_PROJECT_DELIVERY_COMPLETED || old == VENTURE_PROJECT_DELIVERY_CANCELLED) && old != selected)
		return refuse(error, "finished projects are retained; new work requires a new project"), NULL;
	if (selected == VENTURE_PROJECT_DELIVERY_COMPLETED)
	{
		g_autoptr(GPtrArray) rows = project_rows(self->database, VENTURE_TYPE_PROJECT_DELIVERABLE,
			venture_entity_get_organization_id(project), venture_entity_get_id(project), error);
		guint i;
		if (rows == NULL) return NULL;
		if (rows->len == 0) return refuse(error, "complete at least one planned delivery before closing the project"), NULL;
		for (i = 0; i < rows->len; i++)
		{
			g_autoptr(GDateTime) accepted = NULL;
			g_object_get(g_ptr_array_index(rows, i), "accepted-at", &accepted, NULL);
			if (accepted == NULL) return refuse(error, "every planned deliverable must be accepted before completion"), NULL;
		}
	}
	g_object_set(project, "owner", owner, "delivery-status", selected, "delivery-note", reason, NULL);
	if (!delivery_save(self, project, actor, error)) return NULL;
	return g_object_ref(project);
}

static VentureEntity *
delivery_change(VentureAction *action, VentureEntity *project, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureProjectService *self = venture_action_get_data(action);
	g_autoptr(VentureEntity) quote = NULL;
	g_autoptr(VentureEntity) scope = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureMoney) budget = NULL;
	g_autoptr(VentureMoney) total = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *key = g_strdup_printf("quote:%" G_GINT64_FORMAT, delivery_id(params, "quote_id"));
	g_autofree gchar *digest = delivery_digest(params);
	g_autofree gchar *currency = NULL;
	g_autofree gchar *owner = NULL;
	g_autofree gchar *kind = NULL;
	const gchar *description = delivery_text(params, "scope");
	gint64 org = venture_entity_get_organization_id(project);
	gint status;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "quote") == G_TYPE_INVALID)
		return refuse(error, "quotes module is disabled"), NULL;
	if (!delivery_bounded(description, 65536, error)) return NULL;
	scope = delivery_find(self, VENTURE_TYPE_PROJECT_SCOPE, org, "source-key", key, error);
	if (scope != NULL)
	{
		if (number(scope, "project-id") != venture_entity_get_id(project))
			return refuse(error, "this quote already belongs to another project"), NULL;
		return delivery_same_request(scope, digest, error) ? g_steal_pointer(&scope) : NULL;
	}
	if (error != NULL && *error != NULL) return NULL;
	if (!delivery_active(project, error)) return NULL;
	g_object_get(project, "currency", &currency, "owner", &owner, "budget", &budget, "billing-kind", &kind, NULL);
	if (!delivery_fixed_price(project, error)) return NULL;
	quote = delivery_ref(self, VENTURE_TYPE_QUOTE, delivery_id(params, "quote_id"), org, error);
	if (quote == NULL) return NULL;
	g_object_get(quote, "status", &status, "total", &amount, NULL);
	if (status != VENTURE_QUOTE_ACCEPTED || number(quote, "company-id") != number(project, "customer-id") ||
		amount == NULL || venture_money_is_negative(amount) || g_strcmp0(currency, venture_money_get_currency(amount)))
		return refuse(error, "a change requires an accepted quote for the same customer and currency"), NULL;
	if (budget == NULL) return refuse(error, "the existing project budget is unknown"), NULL;
	total = venture_money_add(budget, amount, error);
	if (total == NULL) return NULL;
	scope = g_object_new(VENTURE_TYPE_PROJECT_SCOPE, "organization-id", org,
		"project-id", venture_entity_get_id(project), "source-key", key, "quote-id", venture_entity_get_id(quote),
		"scope", description, "owner", owner, "amount", amount, "agreed-at", now, "request-digest", digest, NULL);
	if (!delivery_save(self, scope, actor, error)) return NULL;
	g_object_set(project, "budget", total, NULL);
	if (!delivery_save(self, project, actor, error)) return NULL;
	if (!delivery_existing_invoices(self, scope, actor, error)) return NULL;
	return g_steal_pointer(&scope);
}

static void
delivery_register(VentureProjectService *self)
{
	const struct {
		const gchar *type;
		const gchar *name;
		const gchar *label;
		VentureActionInvoke invoke;
		const gchar *parameters[5];
		VentureFieldKind kinds[5];
		guint count;
	} actions[] = {
		{ "quote", "handoff", "Start client project", delivery_handoff, { "name", "owner", "scope" }, { VENTURE_FIELD_KIND_STRING, VENTURE_FIELD_KIND_STRING, VENTURE_FIELD_KIND_TEXT }, 3 },
		{ "deal", "handoff", "Start client project", delivery_handoff, { "name", "owner", "scope" }, { VENTURE_FIELD_KIND_STRING, VENTURE_FIELD_KIND_STRING, VENTURE_FIELD_KIND_TEXT }, 3 },
		{ "client_project", "plan_work", "Plan delivery", delivery_plan, { "key", "title", "scope_id", "due", "amount" }, { VENTURE_FIELD_KIND_STRING, VENTURE_FIELD_KIND_STRING, VENTURE_FIELD_KIND_REFERENCE, VENTURE_FIELD_KIND_DATETIME, VENTURE_FIELD_KIND_MONEY }, 5 },
		{ "project_deliverable", "accept", "Accept delivery", delivery_accept, { "evidence" }, { VENTURE_FIELD_KIND_TEXT }, 1 },
		{ "project_deliverable", "bill", "Invoice delivery", delivery_bill, { NULL }, { VENTURE_FIELD_KIND_STRING }, 0 }
		, { "client_project", "manage", "Update delivery ownership and status", delivery_manage, { "owner", "status", "reason" }, { VENTURE_FIELD_KIND_STRING, VENTURE_FIELD_KIND_STRING, VENTURE_FIELD_KIND_TEXT }, 3 }
		, { "client_project", "change_scope", "Add accepted change order", delivery_change, { "quote_id", "scope" }, { VENTURE_FIELD_KIND_REFERENCE, VENTURE_FIELD_KIND_TEXT }, 2 }
		, { "project_deliverable", "link_invoice", "Associate issued progress invoice", delivery_link_invoice, { "invoice_id" }, { VENTURE_FIELD_KIND_REFERENCE }, 1 }
	};
	guint i, j;
	for (i = 0; i < G_N_ELEMENTS(actions); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureAction) action = NULL;
		g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
		for (j = 0; j < actions[i].count; j++)
		{
			VentureFieldSpec *field = venture_field_spec_new(actions[i].parameters[j], NULL, actions[i].kinds[j]);
			field->required = g_strcmp0(field->name, "amount") != 0;
			if (field->kind == VENTURE_FIELD_KIND_REFERENCE)
				field->reference_type = g_strdup(!g_strcmp0(field->name, "scope_id") ? "project_scope" :
					!g_strcmp0(field->name, "quote_id") ? "quote" : "invoice");
			if (field->kind == VENTURE_FIELD_KIND_TEXT) field->max_length = 65536;
			if (field->kind == VENTURE_FIELD_KIND_STRING)
				field->max_length = (!g_strcmp0(field->name, "owner") || !g_strcmp0(field->name, "key")) ? 128 : 256;
			if (!g_strcmp0(field->name, "status"))
			{
				GEnumClass *values = g_type_class_ref(VENTURE_TYPE_PROJECT_DELIVERY_STATUS);
				guint k;
				field->kind = VENTURE_FIELD_KIND_ENUM;
				field->choices = g_new0(gchar *, values->n_values + 1);
				field->choice_labels = g_new0(gchar *, values->n_values + 1);
				for (k = 0; k < values->n_values; k++)
				{
					field->choices[k] = g_strdup(values->values[k].value_nick);
					field->choice_labels[k] = g_strdup(values->values[k].value_nick);
				}
				g_type_class_unref(values);
			}
			g_ptr_array_add(parameters, field);
		}
		action = g_object_new(VENTURE_TYPE_ACTION, "type-name", actions[i].type, "name", actions[i].name,
			"label", actions[i].label, "description", actions[i].label,
			"parameters", parameters, "stageable", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
		if (!venture_action_registry_register(venture_database_get_action_registry(self->database), action,
			delivery_allowed, actions[i].invoke, self, NULL, &error))
			g_error("Delivery action registration: %s", error->message);
	}
}
