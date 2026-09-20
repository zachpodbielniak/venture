/* SPDX-License-Identifier: AGPL-3.0-or-later */

static gboolean
project_type(VentureEntity *record)
{
	return G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_CLIENT_PROJECT) ||
		G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_PROJECT_RATE) ||
		G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_PROJECT_TIME) ||
		G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_PROJECT_COST) ||
		G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_PROJECT_BILLING);
}

static gboolean
project_reference(VentureDatabase *db, VentureEntity *record, VentureEntity *previous,
	const gchar *field, GType type, GError **error)
{
	g_autoptr(VentureEntity) target = NULL;
	gint64 id = number(record, field);
	if (id == 0) return TRUE;
	target = venture_database_get(db, type, id, error);
	if (target == NULL) return FALSE;
	if (venture_entity_get_organization_id(target) != venture_entity_get_organization_id(record))
		return refuse(error, "project references must belong to the same organization");
	if (venture_entity_is_deleted(target) && (previous == NULL || number(previous, field) != id))
		return refuse(error, "project references cannot select deleted records");
	return TRUE;
}

static gboolean
project_money(VentureEntity *record, const gchar *field, const gchar *currency, GError **error)
{
	g_autoptr(VentureMoney) amount = NULL;
	g_object_get(record, field, &amount, NULL);
	if (amount == NULL) return TRUE;
	if (venture_money_is_negative(amount) || g_strcmp0(venture_money_get_currency(amount), currency))
		return refuse(error, "project money must be nonnegative and use the project currency");
	return TRUE;
}

/* A legacy optional currency may be filled once, but never used to
 * relabel retained amounts. Include deleted rows in this compatibility check. */
static gboolean
project_currency_evidence(VentureDatabase *database, VentureEntity *project, const gchar *currency, GError **error)
{
	const struct {
		GType type;
		const gchar *first;
		const gchar *second;
	} sources[] = {
		{ VENTURE_TYPE_PROJECT_RATE, "billing-rate", "cost-rate" },
		{ VENTURE_TYPE_PROJECT_TIME, "amount", "actual-cost" },
		{ VENTURE_TYPE_PROJECT_COST, "amount", NULL },
		{ VENTURE_TYPE_PROJECT_BILLING, "amount", NULL }
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(sources); i++)
	{
		g_autoptr(VentureQuery) query = venture_query_new(sources[i].type);
		g_autoptr(GPtrArray) rows = NULL;
		guint j;
		venture_query_set_organization(query, venture_entity_get_organization_id(project));
		venture_query_set_limit(query, 0);
		venture_query_set_include_deleted(query, TRUE);
		if (!venture_query_add_filter_int(query, "project-id", VENTURE_FILTER_OP_EQ,
			venture_entity_get_id(project), error)) return FALSE;
		rows = venture_database_find(database, query, error);
		if (rows == NULL) return FALSE;
		for (j = 0; j < rows->len; j++)
		{
			VentureEntity *row = g_ptr_array_index(rows, j);
			if (!project_money(row, sources[i].first, currency, error) ||
				(sources[i].second != NULL && !project_money(row, sources[i].second, currency, error))) return FALSE;
		}
	}
	return TRUE;
}

static gboolean
project_validate(VentureDatabase *database, VentureEntity *record, VentureEntity *previous,
	gpointer user_data, GError **error)
{
	VentureProjectService *self = user_data;
	g_autoptr(VentureEntity) project = NULL;
	g_autofree gchar *currency = NULL;
	gboolean is_time, is_cost, is_rate;
	if (!project_type(record)) return TRUE;
	if (G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_PROJECT_BILLING)) return TRUE;
	is_time = G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_PROJECT_TIME);
	is_cost = G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_PROJECT_COST);
	is_rate = G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_PROJECT_RATE);
	if (previous != NULL && venture_entity_is_deleted(record) != venture_entity_is_deleted(previous))
		return refuse(error, "project evidence deletion requires the checked lifecycle operation");
	if (previous != NULL && venture_entity_get_organization_id(record) != venture_entity_get_organization_id(previous))
		return refuse(error, "a saved project record cannot change organization");
	if (previous != NULL && (is_time || is_cost))
	{
		gboolean billed = already_billed(self, venture_entity_get_organization_id(previous),
			venture_entity_get_entity_name(previous), venture_entity_get_id(previous), error);
		if (error != NULL && *error != NULL) return FALSE;
		if ((is_time && flag(previous, "approved")) || billed)
			return refuse(error, "approved or billed project source evidence is immutable");
	}
	if (is_time || is_cost || is_rate)
	{
		if (!project_reference(database, record, previous, "project-id", VENTURE_TYPE_CLIENT_PROJECT, error)) return FALSE;
		project = venture_database_get(database, VENTURE_TYPE_CLIENT_PROJECT, number(record, "project-id"), error);
		if (project == NULL) return FALSE;
	}
	else
	{
		project = g_object_ref(record);
		if (!project_reference(database, record, previous, "customer-id", VENTURE_TYPE_COMPANY, error) ||
			!project_reference(database, record, previous, "venture-id", VENTURE_TYPE_VENTURE, error)) return FALSE;
		if (previous != NULL)
		{
			g_autofree gchar *old_currency = NULL;
			g_autofree gchar *new_currency = NULL;
			g_object_get(previous, "currency", &old_currency, NULL);
			g_object_get(record, "currency", &new_currency, NULL);
			if (g_strcmp0(old_currency, new_currency))
			{
				if (old_currency != NULL && old_currency[0] != '\0')
					return refuse(error, "a saved project's currency is immutable");
				if (!project_currency_evidence(database, record, new_currency, error)) return FALSE;
			}
		}
	}
	g_object_get(project, "currency", &currency, NULL);
	if (currency == NULL || strlen(currency) != 3 ||
		!g_ascii_isupper(currency[0]) || !g_ascii_isupper(currency[1]) || !g_ascii_isupper(currency[2]))
		return refuse(error, "project currency must be an uppercase three-letter currency code");
	if (is_rate)
		return project_money(record, "billing-rate", currency, error) && project_money(record, "cost-rate", currency, error);
	if (is_cost)
		return project_reference(database, record, previous, "expense-id", VENTURE_TYPE_EXPENSE, error) &&
			project_money(record, "amount", currency, error);
	if (is_time)
	{
		g_autoptr(VentureEntity) rate = NULL;
		g_autoptr(VentureMoney) billing = NULL;
		g_autoptr(VentureMoney) cost = NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureMoney) actual = NULL;
		g_autoptr(GDateTime) occurred = NULL;
		gint64 minutes = number(record, "minutes");
		if (!project_reference(database, record, previous, "rate-id", VENTURE_TYPE_PROJECT_RATE, error) ||
			!project_reference(database, record, previous, "worklog-id", VENTURE_TYPE_WORKLOG, error)) return FALSE;
		if (minutes < 0) return refuse(error, "project minutes cannot be negative");
		if (number(record, "rate-id") > 0)
		{
			rate = venture_database_get(database, VENTURE_TYPE_PROJECT_RATE, number(record, "rate-id"), error);
			if (rate == NULL) return FALSE;
			if (number(rate, "project-id") != number(record, "project-id"))
				return refuse(error, "the rate must belong to this project");
		}
		g_object_get(record, "actual-cost", &actual, NULL);
		if (self->approving != record)
		{
			if (flag(record, "approved") || actual != NULL)
				return refuse(error, "time approval and cost evidence require the approval service");
			return project_money(record, "amount", currency, error);
		}
		g_object_get(record, "occurred-at", &occurred, NULL);
		if (rate == NULL || minutes <= 0 || occurred == NULL)
			return refuse(error, "approval requires a rate, positive minutes and an occurrence time");
		if (venture_entity_is_deleted(rate) || venture_entity_is_deleted(project))
			return refuse(error, "approval requires a live project and rate");
		g_object_get(rate, "billing-rate", &billing, "cost-rate", &cost, NULL);
		if (billing == NULL || cost == NULL)
			return refuse(error, "approval requires explicit billing and cost rates, including zero cost");
		if (!project_money(rate, "billing-rate", currency, error) || !project_money(rate, "cost-rate", currency, error)) return FALSE;
		amount = venture_money_multiply_rational(billing, minutes, 60, error);
		if (amount == NULL) return FALSE;
		g_clear_pointer(&actual, venture_money_free);
		actual = venture_money_multiply_rational(cost, minutes, 60, error);
		if (actual == NULL) return FALSE;
		g_object_set(record, "amount", amount, "actual-cost", actual, NULL);
		return TRUE;
	}
	return project_money(record, "budget", currency, error) && project_money(record, "retainer", currency, error);
}

gboolean
venture_projects_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error)
{
	g_autoptr(VentureEntity) previous = NULL;
	VentureProjectService *self;
	gboolean billed;
	if (!delivery_check(venture_project_service_get(database), record, removal, error)) return FALSE;
	if (!removal || !project_type(record)) return TRUE;
	if (G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_PROJECT_BILLING))
		return refuse(error, "project billing evidence is retained");
	if (G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_CLIENT_PROJECT) ||
		G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_PROJECT_RATE))
	{
		GType children[] = { VENTURE_TYPE_PROJECT_TIME, VENTURE_TYPE_PROJECT_COST, VENTURE_TYPE_PROJECT_BILLING };
		gboolean rate = G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_PROJECT_RATE);
		guint i;
		for (i = 0; i < (rate ? 1 : G_N_ELEMENTS(children)); i++)
		{
			g_autoptr(VentureQuery) query = venture_query_new(children[i]);
			gint64 count;
			venture_query_set_include_deleted(query, TRUE);
			if (!venture_query_add_filter_int(query, rate ? "rate-id" : "project-id",
				VENTURE_FILTER_OP_EQ, venture_entity_get_id(record), error)) return FALSE;
			count = venture_database_count(database, query, error);
			if (error != NULL && *error != NULL) return FALSE;
			if (count > 0) return refuse(error, "projects and rates referenced by source evidence are retained");
		}
		return TRUE;
	}
	if (!G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_PROJECT_TIME) &&
		!G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_PROJECT_COST)) return TRUE;
	if (venture_entity_is_persisted(record))
	{
		previous = venture_database_get(database, G_OBJECT_TYPE(record), venture_entity_get_id(record), error);
		if (previous == NULL) return FALSE;
	}
	self = venture_project_service_get(database);
	billed = already_billed(self, venture_entity_get_organization_id(previous != NULL ? previous : record),
		venture_entity_get_entity_name(record), venture_entity_get_id(record), error);
	if (error != NULL && *error != NULL) return FALSE;
	if ((G_TYPE_CHECK_INSTANCE_TYPE(record, VENTURE_TYPE_PROJECT_TIME) &&
		(flag(record, "approved") || (previous != NULL && flag(previous, "approved")))) || billed)
		return refuse(error, "approved or billed project source evidence is retained");
	return TRUE;
}

static GPtrArray *
project_rows(VentureDatabase *db, GType type, gint64 org, gint64 project, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "project-id", VENTURE_FILTER_OP_EQ, project, error)) return NULL;
	return venture_database_find(db, query, error);
}

static gboolean
project_add(VentureMoney **total, const VentureMoney *amount, GError **error)
{
	VentureMoney *next;
	if (amount == NULL) return TRUE;
	next = venture_money_add(*total, amount, error);
	if (next == NULL) return FALSE;
	g_clear_pointer(total, venture_money_free);
	*total = next;
	return TRUE;
}

static gboolean
project_in_period(VentureEntity *record, const gchar *field, VentureDateRange *period)
{
	g_autoptr(GDateTime) date = NULL;
	if (period == NULL) return TRUE;
	g_object_get(record, field, &date, NULL);
	return date != NULL && venture_date_range_contains(period, date);
}

static void
project_source(GString *sources, VentureEntity *record)
{
	if (sources->len > 0) g_string_append(sources, ", ");
	g_string_append_printf(sources, "%s:%" G_GINT64_FORMAT,
		venture_entity_get_entity_name(record), venture_entity_get_id(record));
}

static VentureReportResult *
project_margin_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CLIENT_PROJECT);
	g_autoptr(GPtrArray) projects = NULL;
	g_autoptr(VentureReportResult) result = venture_report_result_new("Project profitability", period);
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	const gchar *money_columns[] = { "budget", "billed", "unbilled", "known_cost", "actual_cost", "profit" };
	const gchar *money_labels[] = { "Project budget", "Billed revenue", "Approved unbilled", "Known actual cost", "Actual cost", "Management profit" };
	guint i;
	if (org == 0) org = venture_context_get_default_organization_id(context);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	projects = venture_database_find(db, query, error);
	if (projects == NULL) return NULL;
	venture_report_result_add_column(result, "name", "Project", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "project_id", "Project ID", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < G_N_ELEMENTS(money_columns); i++)
		venture_report_result_add_column(result, money_columns[i], money_labels[i], VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "unknown_cost", "Unknown cost rows", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "undated", "Undated excluded rows", VENTURE_REPORT_COLUMN_NUMBER);
	venture_report_result_add_column(result, "sources", "Source evidence", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < projects->len; i++)
	{
		VentureEntity *project = g_ptr_array_index(projects, i);
		gint64 project_id = venture_entity_get_id(project);
		g_autoptr(GPtrArray) billings = project_rows(db, VENTURE_TYPE_PROJECT_BILLING, org, project_id, error);
		g_autoptr(GPtrArray) times = NULL;
		g_autoptr(GPtrArray) costs = NULL;
		g_autoptr(GPtrArray) deliveries = NULL;
		g_autoptr(GHashTable) billed_sources = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		g_autoptr(GString) sources = g_string_new(NULL);
		g_autoptr(VentureMoney) billed = NULL;
		g_autoptr(VentureMoney) unbilled = NULL;
		g_autoptr(VentureMoney) actual = NULL;
		g_autoptr(VentureMoney) profit = NULL;
		g_autoptr(VentureMoney) budget = NULL;
		g_autofree gchar *currency = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *id_text = g_strdup_printf("%" G_GINT64_FORMAT, project_id);
		guint j, kind, unknown = 0, undated = 0;
		if (billings == NULL) return NULL;
		times = project_rows(db, VENTURE_TYPE_PROJECT_TIME, org, project_id, error);
		if (times == NULL) return NULL;
		costs = project_rows(db, VENTURE_TYPE_PROJECT_COST, org, project_id, error);
		if (costs == NULL) return NULL;
		deliveries = project_rows(db, VENTURE_TYPE_PROJECT_DELIVERABLE, org, project_id, error);
		if (deliveries == NULL) return NULL;
		g_object_get(project, "currency", &currency, "name", &name, "budget", &budget, NULL);
		if (currency == NULL || currency[0] == '\0')
		{
			refuse(error, "project profitability requires an explicit project currency");
			return NULL;
		}
		billed = venture_money_new_zero(currency);
		unbilled = venture_money_new_zero(currency);
		actual = venture_money_new_zero(currency);
		for (j = 0; j < billings->len; j++)
		{
			VentureEntity *row = g_ptr_array_index(billings, j);
			g_autoptr(VentureEntity) invoice = venture_database_get(db, VENTURE_TYPE_INVOICE, number(row, "invoice-id"), error);
			g_autoptr(VentureMoney) amount = NULL;
			g_autofree gchar *type = NULL;
			g_autoptr(GDateTime) issued = NULL;
			if (invoice == NULL) return NULL;
			if (venture_entity_get_organization_id(invoice) != org)
			{
				refuse(error, "project billing references another organization");
				return NULL;
			}
			g_object_get(row, "amount", &amount, "source-type", &type, NULL);
			g_object_get(invoice, "issued-at", &issued, NULL);
			/* A later invoice cannot erase work that was unbilled at this cutoff. */
			if (period == NULL || (issued != NULL && (venture_date_range_get_end(period) == NULL ||
				g_date_time_compare(issued, venture_date_range_get_end(period)) < 0)))
				g_hash_table_add(billed_sources, g_strdup_printf("%s:%" G_GINT64_FORMAT, type, number(row, "source-id")));
			if (!project_in_period(invoice, "issued-at", period))
			{
				if (issued == NULL) undated++;
				continue;
			}
			if (!project_add(&billed, amount, error)) return NULL;
			project_source(sources, row);
		}
		for (kind = 0; kind < 2; kind++)
		{
			GPtrArray *rows = kind == 0 ? times : costs;
			for (j = 0; j < rows->len; j++)
			{
				VentureEntity *row = g_ptr_array_index(rows, j);
				g_autoptr(VentureMoney) amount = NULL;
				g_autoptr(VentureMoney) cost = NULL;
				g_autofree gchar *key = NULL;
				if (kind == 0 && !flag(row, "approved")) continue;
				if (!project_in_period(row, "occurred-at", period))
				{
					g_autoptr(GDateTime) occurred = NULL;
					g_object_get(row, "occurred-at", &occurred, NULL);
					if (occurred == NULL) undated++;
					continue;
				}
				g_object_get(row, "amount", &amount, kind == 0 ? "actual-cost" : "amount", &cost, NULL);
				if (cost == NULL) unknown++;
				else if (!project_add(&actual, cost, error)) return NULL;
				key = g_strdup_printf("%s:%" G_GINT64_FORMAT, venture_entity_get_entity_name(row), venture_entity_get_id(row));
				if (number(project, "quote-id") == 0 && (kind == 0 || flag(row, "billable")) && !g_hash_table_contains(billed_sources, key) &&
					!project_add(&unbilled, amount, error)) return NULL;
				project_source(sources, row);
			}
		}
		for (j = 0; j < deliveries->len; j++)
		{
			VentureEntity *row = g_ptr_array_index(deliveries, j);
			g_autoptr(GDateTime) accepted = NULL;
			g_autoptr(VentureMoney) amount = NULL;
			g_autofree gchar *scope_key = g_strdup_printf("project_scope:%" G_GINT64_FORMAT, number(row, "scope-id"));
			gboolean invoiced = g_hash_table_contains(billed_sources, scope_key);
			g_object_get(row, "accepted-at", &accepted, "amount", &amount, NULL);
			if (accepted == NULL || !project_in_period(row, "accepted-at", period)) continue;
			if (number(row, "invoice-id") > 0)
			{
				g_autoptr(VentureEntity) invoice = venture_database_get(db, VENTURE_TYPE_INVOICE, number(row, "invoice-id"), error);
				g_autoptr(GDateTime) issued = NULL;
				if (invoice == NULL) return NULL;
				g_object_get(invoice, "issued-at", &issued, NULL);
				invoiced = period == NULL || (issued != NULL && (venture_date_range_get_end(period) == NULL ||
					g_date_time_compare(issued, venture_date_range_get_end(period)) < 0));
			}
			/* Only a quote-created project invoices its accepted slices; a
			 * time-and-materials project's unbilled work is its approved time. */
			if (number(project, "quote-id") > 0 && !invoiced && !project_add(&unbilled, amount, error)) return NULL;
			project_source(sources, row);
		}
		if (unknown == 0)
		{
			profit = venture_money_subtract(billed, actual, error);
			if (profit == NULL) return NULL;
		}
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "name", name);
		venture_report_result_set_text(result, "project_id", id_text);
		venture_report_result_set_money(result, "budget", budget);
		venture_report_result_set_money(result, "billed", billed);
		venture_report_result_set_money(result, "unbilled", unbilled);
		venture_report_result_set_money(result, "known_cost", actual);
		venture_report_result_set_money(result, "actual_cost", unknown == 0 ? actual : NULL);
		venture_report_result_set_money(result, "profit", profit);
		venture_report_result_set_number(result, "unknown_cost", unknown);
		venture_report_result_set_number(result, "undated", undated);
		venture_report_result_set_text(result, "sources", sources->str);
	}
	return g_steal_pointer(&result);
}

static gboolean
project_action_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)actor;
	if (G_TYPE_CHECK_INSTANCE_TYPE(entity, VENTURE_TYPE_PROJECT_TIME) && flag(entity, "approved"))
		return refuse(error, "time is already approved");
	return TRUE;
}

static VentureEntity *
project_approve_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	(void)params;
	if (!venture_project_service_approve_time(venture_action_get_data(action), entity, actor, error)) return NULL;
	return g_object_ref(entity);
}

static VentureEntity *
project_bill_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	JsonNode *node = g_hash_table_lookup(params, "date");
	g_autoptr(GDateTime) date = venture_time_from_string(json_node_get_string(node), error);
	if (date == NULL) return NULL;
	return venture_project_service_bill(venture_action_get_data(action), venture_entity_get_id(entity), date, actor, error);
}

void
venture_projects_actions_register(VentureDatabase *database)
{
	VentureProjectService *self = venture_project_service_get(database);
	const struct {
		const gchar *type;
		const gchar *name;
		const gchar *label;
		const gchar *description;
		VentureActionInvoke invoke;
		gboolean bill;
	} actions[] = {
		{ "project_time", "approve", "Approve time", "Freeze billable amount and actual labour cost", project_approve_invoke, FALSE },
		{ "client_project", "bill", "Invoice approved work", "Issue an invoice for approved unbilled time and billable costs", project_bill_invoke, TRUE }
	};
	guint i;
	delivery_register(self);
	for (i = 0; i < G_N_ELEMENTS(actions); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
		g_autoptr(VentureAction) action = NULL;
		if (actions[i].bill)
		{
			VentureFieldSpec *date = venture_field_spec_new("date", "Invoice date", VENTURE_FIELD_KIND_DATETIME);
			date->required = TRUE;
			g_ptr_array_add(parameters, date);
		}
		action = g_object_new(VENTURE_TYPE_ACTION, "type-name", actions[i].type, "name", actions[i].name,
			"label", actions[i].label, "description", actions[i].description, "parameters", parameters,
			"stageable", TRUE, "service-transaction", actions[i].bill, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
		if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
			project_action_allowed, actions[i].invoke, self, NULL, &error))
			g_error("Project action registration: %s", error->message);
	}
}
