/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
#include <json-glib/json-glib.h>

static const gchar *const close_kinds[] = {
	"bank_recon", "ar_control", "ap_control", "suspense", "tax",
	"depreciation", "deferrals", "tb_tieout", "subledger_tieout"
};

struct _VentureCloseService
{
	GObject parent_instance;
	GWeakRef database;
	GWeakRef context;
	VentureEntity *writing;
	gboolean busy;
};

G_DEFINE_FINAL_TYPE(VentureCloseService, venture_close_service, G_TYPE_OBJECT)

static void
service_finalize(GObject *object)
{
	VentureCloseService *self = VENTURE_CLOSE_SERVICE(object);
	g_weak_ref_clear(&self->database);
	g_weak_ref_clear(&self->context);
	G_OBJECT_CLASS(venture_close_service_parent_class)->finalize(object);
}

static void
venture_close_service_class_init(VentureCloseServiceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = service_finalize;
}

static void
venture_close_service_init(VentureCloseService *self)
{
	g_weak_ref_init(&self->database, NULL);
	g_weak_ref_init(&self->context, NULL);
}

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureCloseService: %s", message);
	return FALSE;
}

static gboolean
module_on(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"close_workspace") != G_TYPE_INVALID;
}

static gboolean
type_on(const gchar *name)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(), name) != G_TYPE_INVALID;
}

VentureCloseService *
venture_close_service_get(VentureDatabase *database)
{
	VentureCloseService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-close-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_CLOSE_SERVICE, NULL);
		g_weak_ref_set(&self->database, database);
		g_object_set_data_full(G_OBJECT(database), "venture-close-service", self, g_object_unref);
	}
	return self;
}

static VentureDatabase *
service_db(VentureCloseService *self)
{
	return g_weak_ref_get(&self->database);
}

static gboolean
save_internal(VentureCloseService *self, VentureDatabase *db, VentureEntity *entity,
	const VentureActor *actor, GError **error)
{
	VentureEntity *previous = self->writing;
	gboolean ok;
	self->writing = entity;
	ok = venture_database_save(db, entity, actor, error);
	self->writing = previous;
	return ok;
}

static gint64
get_id(gpointer object, const gchar *name)
{
	gint64 id = 0;
	g_object_get(object, name, &id, NULL);
	return id;
}

static GPtrArray *
rows_for(VentureDatabase *db, GType type, const gchar *field, gint64 id, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	if (type == G_TYPE_INVALID)
		return g_ptr_array_new();
	query = venture_query_new(type);
	venture_query_set_limit(query, 0);
	if (org > 0)
		venture_query_set_organization(query, org);
	if (field != NULL && !venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, error))
		return NULL;
	return venture_database_find(db, query, error);
}

static gint64
account_by_code(VentureDatabase *db, gint64 org, const gchar *code)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) account = NULL;
	g_autofree gchar *scoped = NULL;
	if (!type_on("account"))
		return 0;
	query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	account = venture_database_find_one(db, query, NULL);
	if (account == NULL)
	{
		scoped = g_strdup_printf("%" G_GINT64_FORMAT ":%s", org, code);
		g_clear_object(&query);
		query = venture_query_new(VENTURE_TYPE_ACCOUNT);
		venture_query_set_organization(query, org);
		venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, scoped, NULL);
		account = venture_database_find_one(db, query, NULL);
	}
	return account != NULL ? venture_entity_get_id(account) : 0;
}

static gint64
mapped_or_code(VentureDatabase *db, gint64 org, const gchar *role, const gchar *code)
{
	g_autoptr(GError) ignored = NULL;
	gint64 id;

	id = venture_setup_resolve_account(db, org, role, "organization", 0, NULL, &ignored);
	if (ignored != NULL)
		return 0;
	if (id != 0)
		return id;
	return account_by_code(db, org, code);
}

static VentureMoney *
gl_balance(VentureDatabase *db, gint64 org, const gchar *role, const gchar *code,
	const gchar *currency, GDateTime *as_of, GError **error)
{
	VenturePostingService *posting;
	gint64 account;
	if (!type_on("journal"))
		return venture_money_new_zero(currency);
	account = mapped_or_code(db, org, role, code);
	if (account == 0)
		return venture_money_new_zero(currency);
	posting = venture_database_get_posting_service(db);
	if (posting == NULL)
		return venture_money_new_zero(currency);
	return venture_posting_service_account_balance(posting, account, org, currency, as_of, error);
}

static gchar *
control_code_label(VentureDatabase *db, gint64 org, const gchar *role, const gchar *fallback)
{
	gint64 id = mapped_or_code(db, org, role, fallback);
	g_autoptr(VentureEntity) row = NULL;
	gchar *code = NULL;

	if (id == 0)
		return g_strdup(fallback);
	row = venture_database_get(db, VENTURE_TYPE_ACCOUNT, id, NULL);
	if (row == NULL)
		return g_strdup(fallback);
	g_object_get(row, "code", &code, NULL);
	return code != NULL ? code : g_strdup(fallback);
}

static gint64
report_outstanding(VentureContext *context, const gchar *name, VentureDateRange *period,
	JsonObject *options, GError **error)
{
	VentureReport *report;
	g_autoptr(VentureReportResult) result = NULL;
	GPtrArray *metrics;
	guint i;
	report = venture_report_registry_lookup(venture_context_get_report_registry(context), name);
	if (report == NULL)
		return 0;
	result = venture_report_generate(report, context, period, options, error);
	if (result == NULL)
		return G_MININT64;
	metrics = venture_report_result_get_metrics(result);
	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *metric = g_ptr_array_index(metrics, i);
		if (g_strcmp0(venture_metric_get_key(metric), "outstanding") == 0 &&
			venture_metric_get_money(metric) != NULL)
			return venture_money_get_amount(venture_metric_get_money(metric));
	}
	return 0;
}

static gboolean
explained(VentureDatabase *db, gint64 workspace_id, const gchar *kind)
{
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	rows = rows_for(db, VENTURE_TYPE_CLOSE_DISCREPANCY, "workspace-id", workspace_id, 0, NULL);
	if (rows == NULL)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *row_kind = NULL;
		g_autofree gchar *status = NULL;
		g_object_get(g_ptr_array_index(rows, i), "kind", &row_kind, "status", &status, NULL);
		if (g_strcmp0(row_kind, kind) == 0 &&
			(g_strcmp0(status, "explained") == 0 || g_strcmp0(status, "corrected") == 0))
			return TRUE;
	}
	return FALSE;
}

static gboolean
record_check(VentureCloseService *self, VentureDatabase *db, VentureEntity *workspace,
	const gchar *kind, const gchar *notes, const VentureMoney *difference,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) tasks = NULL;
	guint i;
	gint64 workspace_id = venture_entity_get_id(workspace);
	gboolean dirty = difference != NULL && !venture_money_is_zero(difference);
	tasks = rows_for(db, VENTURE_TYPE_CLOSE_TASK, "workspace-id", workspace_id,
		venture_entity_get_organization_id(workspace), error);
	if (tasks == NULL)
		return FALSE;
	for (i = 0; i < tasks->len; i++)
	{
		VentureEntity *task = g_ptr_array_index(tasks, i);
		g_autofree gchar *task_kind = NULL;
		g_object_get(task, "kind", &task_kind, NULL);
		if (g_strcmp0(task_kind, kind) != 0)
			continue;
		g_object_set(task, "notes", notes != NULL ? notes : "", "difference", difference, NULL);
		if (!save_internal(self, db, task, actor, error))
			return FALSE;
	}
	if (dirty && !explained(db, workspace_id, kind))
	{
		g_autoptr(VentureEntity) row = VENTURE_ENTITY(venture_close_discrepancy_new());
		g_object_set(row, "workspace-id", workspace_id, "kind", kind, "status", "open",
			"amount", difference, NULL);
		venture_entity_set_organization_id(row, venture_entity_get_organization_id(workspace));
		if (!save_internal(self, db, row, actor, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
scheduled_in_period(VentureDatabase *db, GType type, VentureEntity *period, GError **error)
{
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(GDateTime) cursor = NULL;
	if (type == G_TYPE_INVALID)
		return FALSE;
	g_object_get(period, "start-at", &start, "end-at", &end, NULL);
	cursor = g_date_time_ref(start);
	while (g_date_time_compare(cursor, end) < 0)
	{
		g_autofree gchar *label = g_date_time_format(cursor, "%Y-%m");
		g_autoptr(VentureQuery) query = venture_query_new(type);
		g_autoptr(GPtrArray) rows = NULL;
		venture_query_set_organization(query, venture_entity_get_organization_id(period));
		if (!venture_query_add_filter_string(query, "period", VENTURE_FILTER_OP_EQ, label, error))
			return TRUE;
		venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_EQ, "scheduled", NULL);
		rows = venture_database_find(db, query, error);
		if (rows == NULL)
			return TRUE;
		if (rows->len > 0)
			return TRUE;
		{
			g_autoptr(GDateTime) next = g_date_time_add_months(cursor, 1);
			g_date_time_unref(cursor);
			cursor = g_steal_pointer(&next);
		}
	}
	return FALSE;
}

static gboolean
run_kind(VentureCloseService *self, VentureDatabase *db, VentureContext *context,
	VentureEntity *workspace, VentureEntity *period, const gchar *kind,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(GDateTime) as_of = NULL;
	g_autoptr(VentureDateRange) range = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(VentureMoney) difference = NULL;
	g_autofree gchar *currency = NULL;
	g_autofree gchar *notes = NULL;
	gint64 org = venture_entity_get_organization_id(workspace);
	g_object_get(workspace, "currency", &currency, NULL);
	g_object_get(period, "start-at", &start, "end-at", &end, NULL);
	as_of = g_date_time_add(end, -1);
	range = venture_date_range_new(start, end);
	json_object_set_int_member(options, "organization_id", org);
	json_object_set_string_member(options, "currency", currency);
	difference = venture_money_new_zero(currency);
	if (g_strcmp0(kind, "bank_recon") == 0 && type_on("bank_transaction"))
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BANK_TRANSACTION);
		g_autoptr(GPtrArray) rows = NULL;
		guint i;
		gint64 amount = 0;
		venture_query_set_organization(query, org);
		venture_query_set_date_range(query, "date", range, NULL);
		venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_EQ, "unmatched", NULL);
		rows = venture_database_find(db, query, error);
		if (rows == NULL)
			return FALSE;
		for (i = 0; i < rows->len; i++)
		{
			g_autoptr(VentureMoney) value = NULL;
			g_object_get(g_ptr_array_index(rows, i), "amount", &value, NULL);
			if (value != NULL)
			{
				gint64 minor = venture_money_get_amount(value);
				/* A close cannot label foreign minor units as book currency. */
				if (g_strcmp0(venture_money_get_currency(value), currency) != 0)
					return refuse(error, VENTURE_ERROR_VALIDATION, "Unmatched bank transactions in another currency remain");
				if (minor == G_MININT64 || __builtin_add_overflow(amount, ABS(minor), &amount))
					return refuse(error, VENTURE_ERROR_VALIDATION, "Unmatched bank total overflows minor units");
			}
		}
		g_clear_pointer(&difference, venture_money_free);
		difference = venture_money_new_for_currency(amount, currency);
		notes = g_strdup_printf("%u unmatched bank transaction(s)", rows->len);
	}
	else if (g_strcmp0(kind, "ar_control") == 0)
	{
		g_autoptr(VentureMoney) gl = gl_balance(db, org, "receivables", "1100", currency, as_of, error);
		g_autofree gchar *code = control_code_label(db, org, "receivables", "1100");
		gint64 outstanding;
		if (gl == NULL)
			return FALSE;
		outstanding = report_outstanding(context, "receivables", range, options, error);
		if (outstanding == G_MININT64)
			return FALSE;
		g_clear_pointer(&difference, venture_money_free);
		difference = venture_money_new_for_currency(ABS(venture_money_get_amount(gl)) - outstanding, currency);
		if (venture_money_get_amount(difference) < 0)
		{
			g_autoptr(VentureMoney) flipped = venture_money_new_for_currency(
				-venture_money_get_amount(difference), currency);
			g_clear_pointer(&difference, venture_money_free);
			difference = g_steal_pointer(&flipped);
		}
		notes = g_strdup_printf("AR control %s vs receivables outstanding", code);
	}
	else if (g_strcmp0(kind, "ap_control") == 0)
	{
		g_autoptr(VentureMoney) gl = gl_balance(db, org, "payables", "2000", currency, as_of, error);
		g_autofree gchar *code = control_code_label(db, org, "payables", "2000");
		gint64 outstanding;
		if (gl == NULL)
			return FALSE;
		outstanding = report_outstanding(context, "payables", range, options, error);
		if (outstanding == G_MININT64)
			return FALSE;
		g_clear_pointer(&difference, venture_money_free);
		difference = venture_money_new_for_currency(ABS(venture_money_get_amount(gl)) - outstanding, currency);
		if (venture_money_get_amount(difference) < 0)
		{
			g_autoptr(VentureMoney) flipped = venture_money_new_for_currency(
				-venture_money_get_amount(difference), currency);
			g_clear_pointer(&difference, venture_money_free);
			difference = g_steal_pointer(&flipped);
		}
		notes = g_strdup_printf("AP control %s vs payables outstanding", code);
	}
	else if (g_strcmp0(kind, "suspense") == 0 && type_on("account") && type_on("journal"))
	{
		g_autoptr(GPtrArray) accounts = rows_for(db, VENTURE_TYPE_ACCOUNT, NULL, 0, org, error);
		gint64 total = 0;
		guint i;
		if (accounts == NULL)
			return FALSE;
		for (i = 0; i < accounts->len; i++)
		{
			g_autofree gchar *code = NULL;
			g_autofree gchar *name = NULL;
			g_autoptr(VentureMoney) gl = NULL;
			g_autofree gchar *lower = NULL;
			g_object_get(g_ptr_array_index(accounts, i), "code", &code, "name", &name, NULL);
			lower = name != NULL ? g_ascii_strdown(name, -1) : NULL;
			if (code == NULL)
				continue;
			if (strstr(code, "9999") == NULL && strstr(code, "suspense") == NULL &&
				(lower == NULL || strstr(lower, "suspense") == NULL))
				continue;
			gl = venture_posting_service_account_balance(venture_database_get_posting_service(db),
				venture_entity_get_id(g_ptr_array_index(accounts, i)), org, currency, as_of, error);
			if (gl == NULL)
				return FALSE;
			total += ABS(venture_money_get_amount(gl));
		}
		g_clear_pointer(&difference, venture_money_free);
		difference = venture_money_new_for_currency(total, currency);
		notes = g_strdup("Suspense accounts must be cleared");
	}
	else if (g_strcmp0(kind, "tax") == 0)
	{
		g_autoptr(VentureMoney) gl = gl_balance(db, org, "tax", "2100", currency, as_of, error);
		g_autofree gchar *code = control_code_label(db, org, "tax", "2100");
		if (gl == NULL)
			return FALSE;
		notes = g_strdup_printf("Tax control %s balance %s",
			code, venture_money_to_string(gl));
	}
	else if (g_strcmp0(kind, "depreciation") == 0)
	{
		if (scheduled_in_period(db, type_on("depreciation_entry") ? VENTURE_TYPE_DEPRECIATION_ENTRY : G_TYPE_INVALID,
			period, error))
		{
			g_clear_pointer(&difference, venture_money_free);
			difference = venture_money_new_for_currency(1, currency);
			notes = g_strdup("Scheduled depreciation remains unposted");
		}
		else
			notes = g_strdup("No unposted depreciation in the period");
	}
	else if (g_strcmp0(kind, "deferrals") == 0)
	{
		if (scheduled_in_period(db, type_on("deferral_entry") ? VENTURE_TYPE_DEFERRAL_ENTRY : G_TYPE_INVALID,
			period, error))
		{
			g_clear_pointer(&difference, venture_money_free);
			difference = venture_money_new_for_currency(1, currency);
			notes = g_strdup("Scheduled deferral releases remain unposted");
		}
		else
			notes = g_strdup("No unposted deferrals in the period");
	}
	else if (g_strcmp0(kind, "tb_tieout") == 0)
	{
		VentureReport *report = venture_report_registry_lookup(
			venture_context_get_report_registry(context), "trial_balance");
		g_autoptr(VentureReportResult) result = NULL;
		g_autoptr(GError) local = NULL;
		if (report != NULL)
			result = venture_report_generate(report, context, range, options, &local);
		if (local != NULL)
		{
			g_clear_pointer(&difference, venture_money_free);
			difference = venture_money_new_for_currency(1, currency);
			notes = g_strdup(local->message);
		}
		else
			notes = g_strdup("Trial balance is in balance");
	}
	else if (g_strcmp0(kind, "subledger_tieout") == 0)
	{
		notes = g_strdup("AR, AP and bank must tie to control accounts");
	}
	if (!record_check(self, db, workspace, kind, notes, difference, actor, error))
		return FALSE;
	if (difference != NULL && !venture_money_is_zero(difference) &&
		!explained(db, venture_entity_get_id(workspace), kind))
		return refuse(error, VENTURE_ERROR_VALIDATION, notes != NULL ? notes : "Subledger difference");
	return TRUE;
}

static gboolean
begin_op(VentureCloseService *self, VentureDatabase *db, GError **error)
{
	if (!module_on())
		return refuse(error, VENTURE_ERROR_CONFIG, "The close module is disabled (modules.close.enabled)");
	if (self->busy)
		return refuse(error, VENTURE_ERROR_CONFLICT, "A close operation is already in progress");
	if (!venture_database_begin(db, error))
		return FALSE;
	self->busy = TRUE;
	return TRUE;
}

static gboolean
finish_op(VentureCloseService *self, VentureDatabase *db, gboolean ok, GError **error)
{
	if (ok)
		ok = venture_database_commit(db, error);
	else
		venture_database_rollback(db);
	self->busy = FALSE;
	return ok;
}

VentureEntity *
venture_close_service_open(VentureCloseService *self, gint64 period_id,
	const gchar *currency, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureEntity) period = NULL;
	g_autoptr(VentureEntity) workspace = NULL;
	g_autoptr(GPtrArray) existing = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *book = NULL;
	const gchar *use_currency = currency;
	guint i;
	g_return_val_if_fail(VENTURE_IS_CLOSE_SERVICE(self), NULL);
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), NULL;
	if (!begin_op(self, db, error))
		return NULL;
	period = venture_database_get(db, VENTURE_TYPE_FISCAL_PERIOD, period_id, error);
	if (period == NULL)
		goto fail;
	existing = rows_for(db, VENTURE_TYPE_CLOSE_WORKSPACE, "fiscal-period-id", period_id,
		venture_entity_get_organization_id(period), error);
	if (existing == NULL)
		goto fail;
	if (existing->len > 0)
	{
		refuse(error, VENTURE_ERROR_ALREADY_EXISTS, "A close workspace already exists for this period");
		goto fail;
	}
	g_object_get(period, "name", &name, NULL);
	workspace = VENTURE_ENTITY(venture_close_workspace_new());
	if (use_currency == NULL || use_currency[0] == '\0')
	{
		g_autoptr(VentureEntity) organization = venture_database_get(db, VENTURE_TYPE_ORGANIZATION,
			venture_entity_get_organization_id(period), NULL);
		if (organization != NULL)
			g_object_get(organization, "default-currency", &book, NULL);
		use_currency = book;
	}
	g_object_set(workspace, "name", name, "fiscal-period-id", period_id, "status", "preparing",
		"currency", use_currency != NULL && use_currency[0] != '\0' ? use_currency : "USD", NULL);
	venture_entity_set_organization_id(workspace, venture_entity_get_organization_id(period));
	if (!save_internal(self, db, workspace, actor, error))
		goto fail;
	for (i = 0; i < G_N_ELEMENTS(close_kinds); i++)
	{
		const gchar *roles[] = { "preparer", "reviewer" };
		guint r;
		for (r = 0; r < 2; r++)
		{
			g_autoptr(VentureEntity) task = VENTURE_ENTITY(venture_close_task_new());
			g_object_set(task, "workspace-id", venture_entity_get_id(workspace),
				"kind", close_kinds[i], "role", roles[r], "status", "open", NULL);
			venture_entity_set_organization_id(task, venture_entity_get_organization_id(workspace));
			if (!save_internal(self, db, task, actor, error))
				goto fail;
		}
	}
	if (!finish_op(self, db, TRUE, error))
		return NULL;
	return g_steal_pointer(&workspace);
fail:
	finish_op(self, db, FALSE, error);
	return NULL;
}

gboolean
venture_close_service_run_checks(VentureCloseService *self, VentureEntity *workspace,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureEntity) period = NULL;
	g_autoptr(GString) failures = g_string_new(NULL);
	g_autofree gchar *currency = NULL;
	gboolean tb = TRUE;
	gboolean sub = TRUE;
	guint i;
	g_return_val_if_fail(VENTURE_IS_CLOSE_SERVICE(self), FALSE);
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed");
	if (!begin_op(self, db, error))
		return FALSE;
	context = g_weak_ref_get(&self->context);
	if (context == NULL)
	{
		refuse(error, VENTURE_ERROR_CONFIG, "Closing needs a reporting context");
		return finish_op(self, db, FALSE, error);
	}
	period = venture_database_get(db, VENTURE_TYPE_FISCAL_PERIOD, get_id(workspace, "fiscal-period-id"), error);
	if (period == NULL)
		return finish_op(self, db, FALSE, error);
	g_object_get(workspace, "currency", &currency, NULL);
	for (i = 0; i < G_N_ELEMENTS(close_kinds); i++)
	{
		g_autoptr(GError) local = NULL;
		if (!run_kind(self, db, context, workspace, period, close_kinds[i], actor, &local))
		{
			if (g_strcmp0(close_kinds[i], "tb_tieout") == 0)
				tb = FALSE;
			if (g_strcmp0(close_kinds[i], "ar_control") == 0 ||
				g_strcmp0(close_kinds[i], "ap_control") == 0 ||
				g_strcmp0(close_kinds[i], "bank_recon") == 0 ||
				g_strcmp0(close_kinds[i], "subledger_tieout") == 0)
				sub = FALSE;
			g_string_append_printf(failures, "%s%s: %s", failures->len ? "; " : "",
				close_kinds[i], local != NULL ? local->message : "failed");
		}
	}
	g_object_set(workspace, "tb-balanced", tb, "subledger-tied", sub, NULL);
	if (!save_internal(self, db, workspace, actor, error))
		return finish_op(self, db, FALSE, error);
	if (!finish_op(self, db, TRUE, error))
		return FALSE;
	if (failures->len > 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, failures->str);
	return TRUE;
}

gboolean
venture_close_service_complete_task(VentureCloseService *self, VentureEntity *task,
	gboolean waive, const gchar *notes, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_return_val_if_fail(VENTURE_IS_CLOSE_SERVICE(self), FALSE);
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed");
	if (!begin_op(self, db, error))
		return FALSE;
	g_object_set(task, "status", waive ? "waived" : "done", NULL);
	if (notes != NULL)
		g_object_set(task, "notes", notes, NULL);
	if (!save_internal(self, db, task, actor, error))
		return finish_op(self, db, FALSE, error);
	return finish_op(self, db, TRUE, error);
}

gboolean
venture_close_service_explain(VentureCloseService *self, VentureEntity *discrepancy,
	const gchar *explanation, const gchar *correction_type, gint64 correction_id,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_return_val_if_fail(VENTURE_IS_CLOSE_SERVICE(self), FALSE);
	if (venture_string_is_empty(explanation))
		return refuse(error, VENTURE_ERROR_VALIDATION, "A discrepancy needs an explanation");
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed");
	if (!begin_op(self, db, error))
		return FALSE;
	g_object_set(discrepancy, "explanation", explanation,
		"status", correction_id > 0 ? "corrected" : "explained",
		"correction-type", correction_type, "correction-id", correction_id, NULL);
	if (!save_internal(self, db, discrepancy, actor, error))
		return finish_op(self, db, FALSE, error);
	return finish_op(self, db, TRUE, error);
}

VentureEntity *
venture_close_service_add_workpaper(VentureCloseService *self, gint64 workspace_id,
	gint64 task_id, const gchar *title, const gchar *body, gint64 document_id,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureEntity) workspace = NULL;
	g_autoptr(VentureEntity) paper = NULL;
	g_return_val_if_fail(VENTURE_IS_CLOSE_SERVICE(self), NULL);
	if (venture_string_is_empty(title))
		return refuse(error, VENTURE_ERROR_VALIDATION, "A workpaper needs a title"), NULL;
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), NULL;
	if (!begin_op(self, db, error))
		return NULL;
	workspace = venture_database_get(db, VENTURE_TYPE_CLOSE_WORKSPACE, workspace_id, error);
	if (workspace == NULL)
		return finish_op(self, db, FALSE, error), NULL;
	paper = VENTURE_ENTITY(venture_close_workpaper_new());
	g_object_set(paper, "workspace-id", workspace_id, "task-id", task_id, "title", title,
		"body", body, "document-id", document_id, NULL);
	venture_entity_set_organization_id(paper, venture_entity_get_organization_id(workspace));
	if (!save_internal(self, db, paper, actor, error))
		return finish_op(self, db, FALSE, error), NULL;
	if (!finish_op(self, db, TRUE, error))
		return NULL;
	return g_steal_pointer(&paper);
}

static gboolean
all_tasks_done(VentureDatabase *db, gint64 workspace_id, const gchar *role, GError **error)
{
	g_autoptr(GPtrArray) tasks = rows_for(db, VENTURE_TYPE_CLOSE_TASK, "workspace-id", workspace_id, 0, error);
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	guint i;
	if (tasks == NULL)
		return FALSE;
	for (i = 0; i < tasks->len; i++)
	{
		g_autofree gchar *task_role = NULL;
		g_autofree gchar *status = NULL;
		g_autofree gchar *kind = NULL;
		g_autofree gchar *key = NULL;
		guint k;
		g_object_get(g_ptr_array_index(tasks, i), "role", &task_role, "status", &status, "kind", &kind, NULL);
		if (role != NULL && g_strcmp0(task_role, role) != 0)
			continue;
		if (g_strcmp0(task_role, "preparer") != 0 && g_strcmp0(task_role, "reviewer") != 0)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Invalid close checklist role");
		for (k = 0; k < G_N_ELEMENTS(close_kinds); k++)
			if (g_strcmp0(close_kinds[k], kind) == 0)
				break;
		if (k == G_N_ELEMENTS(close_kinds))
			return refuse(error, VENTURE_ERROR_VALIDATION, "Invalid close checklist kind");
		key = g_strconcat(task_role, ":", kind, NULL);
		if (!g_hash_table_add(seen, g_steal_pointer(&key)))
			return refuse(error, VENTURE_ERROR_VALIDATION, "Duplicate close checklist task");
		if (g_strcmp0(status, "done") != 0 && g_strcmp0(status, "waived") != 0)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Every checklist task must be done or waived");
	}
	/* Deleting a required task cannot turn an incomplete checklist green. */
	if (g_hash_table_size(seen) != G_N_ELEMENTS(close_kinds) * (role != NULL ? 1 : 2))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Required close checklist tasks are missing");
	return TRUE;
}

static gboolean
has_signoff(VentureDatabase *db, gint64 workspace_id, const gchar *role)
{
	g_autoptr(GPtrArray) rows = rows_for(db, VENTURE_TYPE_CLOSE_SIGNOFF, "workspace-id", workspace_id, 0, NULL);
	guint i;
	if (rows == NULL)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *row_role = NULL;
		g_object_get(g_ptr_array_index(rows, i), "role", &row_role, NULL);
		if (g_strcmp0(row_role, role) == 0)
			return TRUE;
	}
	return FALSE;
}

gboolean
venture_close_service_sign(VentureCloseService *self, VentureEntity *workspace,
	const gchar *role, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureEntity) sign = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(JsonNode) pack = NULL;
	g_autoptr(JsonGenerator) generator = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *hash = NULL;
	const gchar *name;
	g_autofree gchar *status = NULL;
	g_autofree gchar *identity = NULL;
	const VentureAuthPrincipal *principal;
	g_return_val_if_fail(VENTURE_IS_CLOSE_SERVICE(self), FALSE);
	if (g_strcmp0(role, "preparer") != 0 && g_strcmp0(role, "reviewer") != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Signoff role is preparer or reviewer");
	if (actor == NULL || venture_string_is_empty(actor->name))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Signoff requires a named actor");
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed");
	if (!begin_op(self, db, error))
		return FALSE;
	principal = venture_access_policy_get_actor(venture_database_get_access_policy(db));
	identity = principal != NULL && principal->authenticated && principal->user_id > 0 ?
		g_strdup_printf("user:%" G_GINT64_FORMAT, principal->user_id) : g_strdup(actor->name);
	g_object_get(workspace, "status", &status, NULL);
	if (g_strcmp0(status, "completed") == 0 ||
		(g_strcmp0(role, "reviewer") == 0 && g_strcmp0(status, "in_review") != 0))
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "The current close cycle needs a preparer signoff before review");
		return finish_op(self, db, FALSE, error);
	}
	if (!all_tasks_done(db, venture_entity_get_id(workspace),
		g_strcmp0(role, "preparer") == 0 ? "preparer" : NULL, error))
		return finish_op(self, db, FALSE, error);
	if (g_strcmp0(role, "reviewer") == 0 && !has_signoff(db, venture_entity_get_id(workspace), "preparer"))
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "The reviewer signs after the preparer");
		return finish_op(self, db, FALSE, error);
	}
	if (g_strcmp0(role, "reviewer") == 0)
	{
		g_autofree gchar *preparer = NULL;
		g_object_get(workspace, "preparer", &preparer, NULL);
		if (g_strcmp0(preparer, identity) == 0)
		{
			refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "The reviewer must be a different authenticated account from the preparer");
			return finish_op(self, db, FALSE, error);
		}
	}
	now = venture_time_now();
	name = identity;
	sign = VENTURE_ENTITY(venture_close_signoff_new());
	pack = venture_close_service_pack(self, workspace, NULL);
	if (pack != NULL)
	{
		generator = json_generator_new();
		json_generator_set_root(generator, pack);
		text = json_generator_to_data(generator, NULL);
		if (text != NULL)
			hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
	}
	g_object_set(sign, "workspace-id", venture_entity_get_id(workspace), "role", role,
		"actor", name, "signed-at", now, "pack-hash", hash, NULL);
	venture_entity_set_organization_id(sign, venture_entity_get_organization_id(workspace));
	if (!save_internal(self, db, sign, actor, error))
		return finish_op(self, db, FALSE, error);
	if (g_strcmp0(role, "preparer") == 0)
		g_object_set(workspace, "status", "in_review", "preparer", name, "reviewer", NULL, NULL);
	else
		g_object_set(workspace, "status", "signed_off", "reviewer", name, NULL);
	if (!save_internal(self, db, workspace, actor, error))
		return finish_op(self, db, FALSE, error);
	return finish_op(self, db, TRUE, error);
}

static gboolean
no_open_discrepancies(VentureDatabase *db, gint64 workspace_id, GError **error)
{
	g_autoptr(GPtrArray) rows = rows_for(db, VENTURE_TYPE_CLOSE_DISCREPANCY, "workspace-id", workspace_id, 0, error);
	guint i;
	if (rows == NULL)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *status = NULL;
		g_object_get(g_ptr_array_index(rows, i), "status", &status, NULL);
		if (g_strcmp0(status, "open") == 0)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Open discrepancies must be explained or linked to a correction");
	}
	return TRUE;
}

gboolean
venture_close_service_complete(VentureCloseService *self, VentureEntity *workspace,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureEntity) period = NULL;
	gboolean tb = FALSE;
	gboolean sub = FALSE;
	g_autofree gchar *status = NULL;
	g_return_val_if_fail(VENTURE_IS_CLOSE_SERVICE(self), FALSE);
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed");
	/* Cached checks are advisory: accounting may change after the last sweep.
	 * Recompute before taking the completion transaction and closing the period. */
	if (!venture_close_service_run_checks(self, workspace, actor, error))
		return FALSE;
	if (!begin_op(self, db, error))
		return FALSE;
	g_object_get(workspace, "status", &status, NULL);
	if (g_strcmp0(status, "signed_off") != 0)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "The current close cycle must be signed off");
		return finish_op(self, db, FALSE, error);
	}
	if (!all_tasks_done(db, venture_entity_get_id(workspace), NULL, error) ||
		!no_open_discrepancies(db, venture_entity_get_id(workspace), error))
		return finish_op(self, db, FALSE, error);
	if (!has_signoff(db, venture_entity_get_id(workspace), "preparer") ||
		!has_signoff(db, venture_entity_get_id(workspace), "reviewer"))
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "Preparer and reviewer must both sign off");
		return finish_op(self, db, FALSE, error);
	}
	g_object_get(workspace, "tb-balanced", &tb, "subledger-tied", &sub, NULL);
	if (!tb || !sub)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "Trial balance and subledger tie-outs must pass before complete");
		return finish_op(self, db, FALSE, error);
	}
	period = venture_database_get(db, VENTURE_TYPE_FISCAL_PERIOD, get_id(workspace, "fiscal-period-id"), error);
	if (period == NULL)
		return finish_op(self, db, FALSE, error);
	g_object_set(period, "state", VENTURE_PERIOD_CLOSED, NULL);
	if (!venture_database_save(db, period, actor, error))
		return finish_op(self, db, FALSE, error);
	g_object_set(workspace, "status", "completed", NULL);
	if (!save_internal(self, db, workspace, actor, error))
		return finish_op(self, db, FALSE, error);
	return finish_op(self, db, TRUE, error);
}

gboolean
venture_close_service_reopen(VentureCloseService *self, VentureEntity *workspace,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureEntity) period = NULL;
	g_return_val_if_fail(VENTURE_IS_CLOSE_SERVICE(self), FALSE);
	db = service_db(self);
	if (db == NULL)
		return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed");
	if (!begin_op(self, db, error))
		return FALSE;
	period = venture_database_get(db, VENTURE_TYPE_FISCAL_PERIOD, get_id(workspace, "fiscal-period-id"), error);
	if (period == NULL)
		return finish_op(self, db, FALSE, error);
	g_object_set(period, "state", VENTURE_PERIOD_OPEN, NULL);
	if (!venture_database_save(db, period, actor, error))
		return finish_op(self, db, FALSE, error);
	/* Keep historical signatures as evidence, but require both roles anew. */
	g_object_set(workspace, "status", "reopened", "tb-balanced", FALSE, "subledger-tied", FALSE,
		"preparer", NULL, "reviewer", NULL, NULL);
	if (!save_internal(self, db, workspace, actor, error))
		return finish_op(self, db, FALSE, error);
	return finish_op(self, db, TRUE, error);
}

JsonNode *
venture_close_service_pack(VentureCloseService *self, VentureEntity *workspace, GError **error)
{
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureEntity) period = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(VentureDateRange) range = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autofree gchar *currency = NULL;
	const gchar *reports[] = { "trial_balance", "receivables", "payables", "bank_reconciliation",
		"balance_sheet", "income_statement" };
	guint i;
	g_return_val_if_fail(VENTURE_IS_CLOSE_SERVICE(self), NULL);
	db = service_db(self);
	context = g_weak_ref_get(&self->context);
	if (db == NULL || context == NULL)
		return refuse(error, VENTURE_ERROR_CONFIG, "Closing needs a reporting context"), NULL;
	period = venture_database_get(db, VENTURE_TYPE_FISCAL_PERIOD, get_id(workspace, "fiscal-period-id"), error);
	if (period == NULL)
		return NULL;
	g_object_get(period, "start-at", &start, "end-at", &end, NULL);
	g_object_get(workspace, "currency", &currency, NULL);
	range = venture_date_range_new(start, end);
	json_object_set_int_member(options, "organization_id", venture_entity_get_organization_id(workspace));
	json_object_set_string_member(options, "currency", currency);
	json_builder_begin_object(builder);
	for (i = 0; i < G_N_ELEMENTS(reports); i++)
	{
		VentureReport *report = venture_report_registry_lookup(
			venture_context_get_report_registry(context), reports[i]);
		g_autoptr(VentureReportResult) result = NULL;
		g_autoptr(GError) local = NULL;
		json_builder_set_member_name(builder, reports[i]);
		if (report == NULL)
		{
			json_builder_add_null_value(builder);
			continue;
		}
		result = venture_report_generate(report, context, range, options, &local);
		if (result == NULL)
			json_builder_add_string_value(builder, local != NULL ? local->message : "unavailable");
		else
			json_builder_add_value(builder, venture_report_result_to_json(result));
	}
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

gboolean
venture_close_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error)
{
	VentureCloseService *self;
	(void)actor;
	*handled = FALSE;
	if (!VENTURE_IS_CLOSE_WORKSPACE(record) && !VENTURE_IS_CLOSE_TASK(record) &&
		!VENTURE_IS_CLOSE_WORKPAPER(record) && !VENTURE_IS_CLOSE_DISCREPANCY(record) &&
		!VENTURE_IS_CLOSE_SIGNOFF(record))
		return TRUE;
	if (!module_on())
		return refuse(error, VENTURE_ERROR_CONFIG, "The close module is disabled (modules.close.enabled)");
	self = venture_close_service_get(database);
	if (self->writing == record)
		return TRUE;
	/* Flags, signatures and fiscal-period identity describe one checked close
	 * cycle. Generic CRUD must not manufacture or reset that evidence. */
	if (VENTURE_IS_CLOSE_WORKSPACE(record))
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Close workspaces are written only by VentureCloseService");
	if (VENTURE_IS_CLOSE_SIGNOFF(record))
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Signoffs are written only by VentureCloseService");
	if (VENTURE_IS_CLOSE_TASK(record) && venture_entity_is_persisted(record))
	{
		g_autofree gchar *status = NULL;
		g_object_get(record, "status", &status, NULL);
		if (g_strcmp0(status, "done") == 0 || g_strcmp0(status, "waived") == 0)
			return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Close tasks complete through VentureCloseService");
	}
	if (VENTURE_IS_CLOSE_DISCREPANCY(record) && venture_entity_is_persisted(record))
	{
		g_autofree gchar *status = NULL;
		g_object_get(record, "status", &status, NULL);
		if (g_strcmp0(status, "explained") == 0 || g_strcmp0(status, "corrected") == 0)
			return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Discrepancies are explained through VentureCloseService");
	}
	return TRUE;
}

typedef struct { GObject parent_instance; } VentureClosePeriodCheck;
typedef struct { GObjectClass parent_class; } VentureClosePeriodCheckClass;
GType venture_close_period_check_get_type(void);

static const gchar *
close_check_name(VenturePeriodCheck *check)
{
	(void)check;
	return "subledger reconciliation";
}

static gboolean
close_check_run(VenturePeriodCheck *check, VentureDatabase *database, VentureEntity *period, GError **error)
{
	g_autoptr(GPtrArray) workspaces = NULL;
	(void)check;
	if (!module_on())
		return TRUE;
	workspaces = rows_for(database, VENTURE_TYPE_CLOSE_WORKSPACE, "fiscal-period-id",
		venture_entity_get_id(period), venture_entity_get_organization_id(period), error);
	if (workspaces == NULL)
		return FALSE;
	if (workspaces->len > 0)
	{
		g_autofree gchar *status = NULL;
		g_object_get(g_ptr_array_index(workspaces, 0), "status", &status, NULL);
		if (g_strcmp0(status, "signed_off") != 0 && g_strcmp0(status, "completed") != 0)
			return refuse(error, VENTURE_ERROR_VALIDATION,
				"The close workspace must be signed off before the period closes");
	}
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		g_autoptr(GDateTime) start = NULL;
		g_autoptr(GDateTime) end = NULL;
		g_autoptr(VentureDateRange) range = NULL;
		if (!type_on("bank_transaction"))
			return TRUE;
		g_object_get(period, "start-at", &start, "end-at", &end, NULL);
		range = venture_date_range_new(start, end);
		query = venture_query_new(VENTURE_TYPE_BANK_TRANSACTION);
		venture_query_set_organization(query, venture_entity_get_organization_id(period));
		venture_query_set_date_range(query, "date", range, NULL);
		venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_EQ, "unmatched", NULL);
		rows = venture_database_find(database, query, error);
		if (rows == NULL)
			return FALSE;
		if (rows->len > 0)
			return refuse(error, VENTURE_ERROR_VALIDATION, "Unmatched bank transactions remain");
	}
	return TRUE;
}

static void close_check_iface_init(VenturePeriodCheckInterface *iface)
{
	iface->get_name = close_check_name;
	iface->run = close_check_run;
}
G_DEFINE_TYPE_WITH_CODE(VentureClosePeriodCheck, venture_close_period_check, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_PERIOD_CHECK, close_check_iface_init))
static void venture_close_period_check_class_init(VentureClosePeriodCheckClass *klass) { (void)klass; }
static void venture_close_period_check_init(VentureClosePeriodCheck *self) { (void)self; }

static void
on_collect(VenturePeriodChecklist *checklist, GPtrArray *checks, gpointer data)
{
	(void)checklist;
	(void)data;
	g_ptr_array_add(checks, g_object_new(venture_close_period_check_get_type(), NULL));
}

void
venture_close_service_install(VentureContext *context)
{
	VentureCloseService *self;
	VenturePeriodChecklist *checklist;
	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	self = venture_close_service_get(venture_context_get_database(context));
	g_weak_ref_set(&self->context, context);
	checklist = venture_period_service_get_checklist(
		venture_period_service_get(venture_context_get_database(context)));
	if (g_object_get_data(G_OBJECT(checklist), "venture-close-checks") != NULL)
		return;
	g_object_set_data(G_OBJECT(checklist), "venture-close-checks", GINT_TO_POINTER(1));
	g_signal_connect_object(checklist, "collect-checks", G_CALLBACK(on_collect), context, 0);
}

static VentureReportResult *
close_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(VentureReportResult) result = venture_report_result_new("Close workspace", period);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gint64 org;
	guint i;
	if (!module_on())
		return g_steal_pointer(&result);
	org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	if (org == 0)
		org = venture_context_get_default_organization_id(context);
	query = venture_query_new(VENTURE_TYPE_CLOSE_WORKSPACE);
	venture_query_set_organization(query, org);
	rows = venture_database_find(venture_context_get_database(context), query, error);
	if (rows == NULL)
		return NULL;
	venture_report_result_add_column(result, "name", "Period", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "status", "Status", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *name = NULL;
		g_autofree gchar *status = NULL;
		g_object_get(g_ptr_array_index(rows, i), "name", &name, "status", &status, NULL);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "name", name);
		venture_report_result_set_text(result, "status", status);
	}
	return g_steal_pointer(&result);
}

void
venture_close_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT,
		"close_workspace", "Close workspace",
		"Accountant close status, signoffs and subledger tie-outs", close_report)));
}
