/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

struct _VentureProjectService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
};
G_DEFINE_FINAL_TYPE(VentureProjectService, venture_project_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureProjectService: %s", message);
	return FALSE;
}

static void
venture_project_service_class_init(VentureProjectServiceClass *klass)
{
	(void)klass;
}

static void
venture_project_service_init(VentureProjectService *self)
{
	(void)self;
}

VentureProjectService *
venture_project_service_get(VentureDatabase *database)
{
	VentureProjectService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-project-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_PROJECT_SERVICE, NULL);
		self->database = database;
		g_object_set_data_full(G_OBJECT(database), "venture-project-service", self, g_object_unref);
	}
	return self;
}

static gboolean
write_owned(VentureProjectService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
	return ok;
}

static gint64
number(VentureEntity *record, const gchar *field)
{
	gint64 value = 0;
	g_object_get(record, field, &value, NULL);
	return value;
}

static gboolean
flag(VentureEntity *record, const gchar *field)
{
	gboolean value = FALSE;
	g_object_get(record, field, &value, NULL);
	return value;
}

gboolean
venture_project_service_save(VentureProjectService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	return write_owned(self, record, actor, error);
}

gboolean
venture_project_service_approve_time(VentureProjectService *self, VentureEntity *time, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) rate = NULL;
	g_autoptr(VentureMoney) billing = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	gint64 minutes, rate_id;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "client_project") == G_TYPE_INVALID)
		return refuse(error, "projects module is disabled");
	g_object_get(time, "minutes", &minutes, "rate-id", &rate_id, NULL);
	if (minutes <= 0)
		return refuse(error, "approved time needs positive minutes");
	if (rate_id > 0)
	{
		rate = venture_database_get(self->database, VENTURE_TYPE_PROJECT_RATE, rate_id, error);
		if (rate == NULL) return FALSE;
		g_object_get(rate, "billing-rate", &billing, NULL);
		if (billing == NULL)
			return refuse(error, "the rate has no billing amount");
		amount = venture_money_multiply_rational(billing, minutes, 60, error);
		if (amount == NULL) return FALSE;
		g_object_set(time, "amount", amount, NULL);
	}
	g_object_set(time, "approved", TRUE, NULL);
	return write_owned(self, time, actor, error);
}

static gboolean
already_billed(VentureProjectService *self, gint64 org, const gchar *type, gint64 id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PROJECT_BILLING);
	venture_query_set_organization(query, org);
	if (!venture_query_add_filter_string(query, "source-type", VENTURE_FILTER_OP_EQ, type, error) ||
		!venture_query_add_filter_int(query, "source-id", VENTURE_FILTER_OP_EQ, id, error))
		return TRUE;
	return venture_database_count(self->database, query, error) != 0;
}

static VentureEntity *
venture_project_service_bill_impl(VentureProjectService *self, gint64 project_id, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) project = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GPtrArray) times = NULL;
	g_autoptr(GPtrArray) costs = NULL;
	g_autoptr(VentureQuery) tq = NULL;
	g_autoptr(VentureQuery) cq = NULL;
	g_autofree gchar *currency = NULL;
	gint64 org, customer, billed = 0;
	guint i;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "client_project") == G_TYPE_INVALID)
	{
		refuse(error, "projects module is disabled");
		return NULL;
	}
	if (!venture_database_begin(self->database, error)) return NULL;
	project = venture_database_get(self->database, VENTURE_TYPE_CLIENT_PROJECT, project_id, error);
	if (project == NULL) goto fail;
	org = venture_entity_get_organization_id(project);
	g_object_get(project, "customer-id", &customer, "currency", &currency, NULL);
	tq = venture_query_new(VENTURE_TYPE_PROJECT_TIME);
	venture_query_set_organization(tq, org);
	venture_query_set_limit(tq, 0);
	if (!venture_query_add_filter_int(tq, "project-id", VENTURE_FILTER_OP_EQ, project_id, error)) goto fail;
	times = venture_database_find(self->database, tq, error);
	if (times == NULL) goto fail;
	cq = venture_query_new(VENTURE_TYPE_PROJECT_COST);
	venture_query_set_organization(cq, org);
	venture_query_set_limit(cq, 0);
	if (!venture_query_add_filter_int(cq, "project-id", VENTURE_FILTER_OP_EQ, project_id, error)) goto fail;
	costs = venture_database_find(self->database, cq, error);
	if (costs == NULL) goto fail;
	for (i = 0; i < times->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(times, i);
		g_autoptr(VentureMoney) amount = NULL;
		if (!flag(row, "approved") || already_billed(self, org, "project_time", venture_entity_get_id(row), error))
		{
			if (error != NULL && *error != NULL) goto fail;
			continue;
		}
		g_object_get(row, "amount", &amount, NULL);
		if (amount != NULL && !venture_money_is_zero(amount))
			billed++;
	}
	for (i = 0; i < costs->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(costs, i);
		g_autoptr(VentureMoney) amount = NULL;
		if (!flag(row, "billable") || already_billed(self, org, "project_cost", venture_entity_get_id(row), error))
		{
			if (error != NULL && *error != NULL) goto fail;
			continue;
		}
		g_object_get(row, "amount", &amount, NULL);
		if (amount != NULL && !venture_money_is_zero(amount))
			billed++;
	}
	if (billed == 0)
	{
		refuse(error, "no approved unbilled time or costs");
		goto fail;
	}
	billed = 0;
	invoice = g_object_new(VENTURE_TYPE_INVOICE, NULL);
	venture_entity_set_organization_id(invoice, org);
	{
		g_autofree gchar *stamp = date != NULL ? g_date_time_format(date, "%Y%m%d%H%M%S") : g_strdup("now");
		g_autofree gchar *number_text = g_strdup_printf("PRJ-%s-%s", venture_entity_get_uuid(invoice), stamp);
		g_object_set(invoice, "number", number_text,
			"company-id", customer, "venture-id", number(project, "venture-id"),
			"issued-at", date, "due-at", date, NULL);
	}
	if (!venture_database_save(self->database, invoice, actor, error)) goto fail;
	g_clear_object(&tq);
	g_clear_pointer(&times, g_ptr_array_unref);
	tq = venture_query_new(VENTURE_TYPE_PROJECT_TIME);
	venture_query_set_organization(tq, org);
	venture_query_set_limit(tq, 0);
	if (!venture_query_add_filter_int(tq, "project-id", VENTURE_FILTER_OP_EQ, project_id, error)) goto fail;
	times = venture_database_find(self->database, tq, error);
	if (times == NULL) goto fail;
	for (i = 0; i < times->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(times, i);
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureEntity) line = NULL;
		g_autoptr(VentureEntity) allocation = NULL;
		if (!flag(row, "approved") || already_billed(self, org, "project_time", venture_entity_get_id(row), error))
		{
			if (error != NULL && *error != NULL) goto fail;
			continue;
		}
		g_object_get(row, "amount", &amount, NULL);
		if (amount == NULL || venture_money_is_zero(amount)) continue;
		line = g_object_new(VENTURE_TYPE_INVOICE_LINE, NULL);
		venture_entity_set_organization_id(line, org);
		g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "description", "Approved time",
			"quantity", 1.0, "unit-price", amount, NULL);
		if (!venture_database_save(self->database, line, actor, error)) goto fail;
		allocation = g_object_new(VENTURE_TYPE_PROJECT_BILLING, NULL);
		venture_entity_set_organization_id(allocation, org);
		g_object_set(allocation, "project-id", project_id, "source-type", "project_time",
			"source-id", venture_entity_get_id(row), "invoice-id", venture_entity_get_id(invoice),
			"invoice-line-id", venture_entity_get_id(line), "amount", amount, NULL);
		if (!write_owned(self, allocation, actor, error)) goto fail;
		billed++;
	}
	g_clear_object(&cq);
	g_clear_pointer(&costs, g_ptr_array_unref);
	cq = venture_query_new(VENTURE_TYPE_PROJECT_COST);
	venture_query_set_organization(cq, org);
	venture_query_set_limit(cq, 0);
	if (!venture_query_add_filter_int(cq, "project-id", VENTURE_FILTER_OP_EQ, project_id, error)) goto fail;
	costs = venture_database_find(self->database, cq, error);
	if (costs == NULL) goto fail;
	for (i = 0; i < costs->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(costs, i);
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(VentureEntity) line = NULL;
		g_autoptr(VentureEntity) allocation = NULL;
		if (!flag(row, "billable") || already_billed(self, org, "project_cost", venture_entity_get_id(row), error))
		{
			if (error != NULL && *error != NULL) goto fail;
			continue;
		}
		g_object_get(row, "amount", &amount, NULL);
		if (amount == NULL || venture_money_is_zero(amount)) continue;
		line = g_object_new(VENTURE_TYPE_INVOICE_LINE, NULL);
		venture_entity_set_organization_id(line, org);
		g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "description", "Billable cost",
			"quantity", 1.0, "unit-price", amount, NULL);
		if (!venture_database_save(self->database, line, actor, error)) goto fail;
		allocation = g_object_new(VENTURE_TYPE_PROJECT_BILLING, NULL);
		venture_entity_set_organization_id(allocation, org);
		g_object_set(allocation, "project-id", project_id, "source-type", "project_cost",
			"source-id", venture_entity_get_id(row), "invoice-id", venture_entity_get_id(invoice),
			"invoice-line-id", venture_entity_get_id(line), "amount", amount, NULL);
		if (!write_owned(self, allocation, actor, error)) goto fail;
		billed++;
	}
	if (billed == 0)
	{
		refuse(error, "no approved unbilled time or costs");
		goto fail;
	}
	g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL);
	if (!venture_database_save(self->database, invoice, actor, error)) goto fail;
	if (!venture_database_commit(self->database, error)) goto fail;
	return g_steal_pointer(&invoice);
fail:
	venture_database_rollback(self->database);
	return NULL;
}

gboolean
venture_projects_save_hook(VentureDatabase *database, VentureEntity *record, const VentureActor *actor, gboolean *handled, GError **error)
{
	const gchar *name = venture_entity_get_entity_name(record);
	VentureProjectService *self;
	*handled = FALSE;
	if (g_strcmp0(name, "client_project") && g_strcmp0(name, "project_rate") && g_strcmp0(name, "project_time") &&
		g_strcmp0(name, "project_cost") && g_strcmp0(name, "project_billing"))
		return TRUE;
	self = venture_project_service_get(database);
	if (self->writing == record)
	{
		self->writing = NULL;
		return TRUE;
	}
	if (!g_strcmp0(name, "project_billing"))
		return refuse(error, "billing allocations are written only when invoicing approved work");
	(void)actor;
	return TRUE;
}

static VentureReportResult *
project_margin_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CLIENT_PROJECT);
	g_autoptr(GPtrArray) projects = NULL;
	g_autoptr(VentureReportResult) result = venture_report_result_new("Project margin", period);
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	guint i;
	(void)error;
	if (org == 0) org = venture_context_get_default_organization_id(context);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	projects = venture_database_find(db, query, error);
	if (projects == NULL) return NULL;
	venture_report_result_add_column(result, "name", "Project", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "billed", "Billed", VENTURE_REPORT_COLUMN_MONEY);
	for (i = 0; i < projects->len; i++)
	{
		VentureEntity *project = g_ptr_array_index(projects, i);
		g_autoptr(VentureQuery) bq = venture_query_new(VENTURE_TYPE_PROJECT_BILLING);
		g_autoptr(GPtrArray) rows = NULL;
		g_autoptr(VentureMoney) billed = NULL;
		g_autofree gchar *name = NULL;
		guint j;
		venture_query_set_organization(bq, org);
		if (!venture_query_add_filter_int(bq, "project-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(project), error))
			return NULL;
		rows = venture_database_find(db, bq, error);
		if (rows == NULL) return NULL;
		for (j = 0; j < rows->len; j++)
		{
			g_autoptr(VentureMoney) amount = NULL;
			g_object_get(g_ptr_array_index(rows, j), "amount", &amount, NULL);
			if (amount != NULL)
			{
				VentureMoney *next;
				if (billed == NULL)
					billed = venture_money_new_zero(venture_money_get_currency(amount));
				next = venture_money_add(billed, amount, error);
				if (next == NULL) return NULL;
				venture_money_free(billed);
				billed = next;
			}
		}
		if (billed == NULL)
		{
			g_autoptr(VentureEntity) organization = venture_database_get(db, VENTURE_TYPE_ORGANIZATION, org, NULL);
			g_autofree gchar *currency = NULL;
			if (organization != NULL)
				g_object_get(organization, "default-currency", &currency, NULL);
			billed = venture_money_new_zero(currency != NULL && currency[0] != '\0' ? currency : "XXX");
		}
		g_object_get(project, "name", &name, NULL);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "name", name);
		venture_report_result_set_money(result, "billed", billed);
	}
	return g_steal_pointer(&result);
}

void
venture_projects_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"project_margin", "Project margin", "Billed time and costs by project.", project_margin_report)));
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
VentureEntity *
venture_project_service_bill(VentureProjectService *self, gint64 project_id, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	g_autoptr(VentureEntity) subject = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autofree gchar *date_text = NULL;
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return NULL;
	}
	subject = venture_database_get(db, VENTURE_TYPE_CLIENT_PROJECT, project_id, error);
	if (subject == NULL)
		return NULL;
	date_text = date != NULL ? g_date_time_format_iso8601(date) : NULL;
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "project_id", g_variant_new_int64((gint64)project_id));
	g_variant_builder_add(&arguments, "{sv}", "date", g_variant_new_maybe(G_VARIANT_TYPE_STRING, date_text != NULL ? g_variant_new_string(date_text) : NULL));
	operation = venture_accounting_operation_begin(db, "project-bill", subject, NULL,
		g_variant_builder_end(&arguments), venture_entity_get_organization_id(subject), actor, error);
	if (operation == NULL)
		return NULL;
	result = venture_project_service_bill_impl(self, project_id, date, actor, error);
	if (result == NULL)
		return NULL;
	if (!venture_accounting_operation_finish(operation, error))
		return NULL;
	return g_steal_pointer(&result);
}
