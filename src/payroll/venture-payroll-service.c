/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

struct _VenturePayrollService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *writing;
	gboolean busy;
};
G_DEFINE_FINAL_TYPE(VenturePayrollService, venture_payroll_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VenturePayrollService: %s", message);
	return FALSE;
}

static gboolean
module_on(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"payroll_run") != G_TYPE_INVALID;
}

static void
venture_payroll_service_class_init(VenturePayrollServiceClass *klass)
{
	(void)klass;
}

static void
venture_payroll_service_init(VenturePayrollService *self)
{
	(void)self;
}

VenturePayrollService *
venture_payroll_service_get(VentureDatabase *database)
{
	VenturePayrollService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-payroll-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_PAYROLL_SERVICE, NULL);
		self->database = database;
		g_object_set_data_full(G_OBJECT(database), "venture-payroll-service", self, g_object_unref);
	}
	return self;
}

static gboolean
write_owned(VenturePayrollService *self, VentureEntity *record, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->writing = record;
	ok = venture_database_save(self->database, record, actor, error);
	self->writing = NULL;
	return ok;
}

static gboolean
begin_op(VenturePayrollService *self, GError **error)
{
	if (!module_on())
		return refuse(error, VENTURE_ERROR_CONFIG, "The payroll module is disabled (payroll.enabled)");
	if (self->busy)
		return refuse(error, VENTURE_ERROR_CONFLICT, "A payroll operation is already in progress");
	if (!venture_database_begin(self->database, error))
		return FALSE;
	self->busy = TRUE;
	return TRUE;
}

static gboolean
finish_op(VenturePayrollService *self, gboolean ok, GError **error)
{
	if (ok)
		ok = venture_database_commit(self->database, error);
	else
		venture_database_rollback(self->database);
	self->busy = FALSE;
	return ok;
}

static gint64
account_id(VenturePayrollService *self, const gchar *role, const gchar *code,
	VentureAccountKind kind, gint64 organization_id, GError **error)
{
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *scoped_code = NULL;
	gint64 mapped;
	gboolean active;
	gint actual_kind;

	mapped = venture_setup_resolve_account(self->database, organization_id, role, "organization", 0, NULL, error);
	if (error != NULL && *error != NULL)
		return 0;
	if (mapped != 0)
		account = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, mapped, error);
	else
	{
		query = venture_query_new(VENTURE_TYPE_ACCOUNT);
		venture_query_set_organization(query, organization_id);
		if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, error))
			return 0;
		account = venture_database_find_one(self->database, query, error);
		if (account == NULL && (error == NULL || *error == NULL))
		{
			scoped_code = g_strdup_printf("%" G_GINT64_FORMAT ":%s", organization_id, code);
			g_clear_object(&query);
			query = venture_query_new(VENTURE_TYPE_ACCOUNT);
			venture_query_set_organization(query, organization_id);
			if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, scoped_code, error))
				return 0;
			account = venture_database_find_one(self->database, query, error);
		}
	}
	if (account == NULL && (error == NULL || *error == NULL))
	{
		account = VENTURE_ENTITY(venture_account_new());
		g_object_set(account, "organization-id", organization_id,
			"code", scoped_code != NULL ? scoped_code : code,
			"name", g_str_equal(role, "tax") ? "Tax payable" :
			(g_str_equal(role, "payables") ? "Net pay" :
			(g_str_equal(role, "expense") ? "Wages" : "Cash")),
			"kind", kind, "active", TRUE, NULL);
		if (!venture_database_save(self->database, account, NULL, error))
			return 0;
	}
	if (account == NULL)
	{
		if (error == NULL || *error == NULL)
			refuse(error, VENTURE_ERROR_CONFIG, "Map wages, tax payable and cash accounts");
		return 0;
	}
	g_object_get(account, "active", &active, "kind", &actual_kind, NULL);
	if (!active || actual_kind != (gint)kind || venture_entity_get_organization_id(account) != organization_id)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "The posting account must be active, of the right class, and in the same organization");
		return 0;
	}
	return venture_entity_get_id(account);
}

static gboolean
add_entry(GPtrArray *entries, gint64 org, const gchar *transaction, gint64 account,
	VentureLedgerSide side, const VentureMoney *amount, GDateTime *date, gint64 source_id)
{
	VentureLedgerEntry *entry = venture_ledger_entry_new();
	g_object_set(entry, "transaction-id", transaction, "account-id", account,
		"side", side, "amount", amount, "occurred-at", date,
		"source-type", "payroll_run", "source-id", source_id, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(entry), org);
	g_ptr_array_add(entries, entry);
	return TRUE;
}

static VentureMoney *
money_member(JsonObject *object, const gchar *name, GError **error)
{
	const gchar *text = venture_json_object_get_string(object, name, NULL);
	if (text == NULL)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "Each pay line needs gross, employer_cost, deductions, net and liabilities");
		return NULL;
	}
	return venture_money_from_string(text, NULL, error);
}

static gboolean
line_balances(const VentureMoney *gross, const VentureMoney *employer, const VentureMoney *net,
	const VentureMoney *liabilities, GError **error)
{
	g_autoptr(VentureMoney) cost = venture_money_add(gross, employer, error);
	g_autoptr(VentureMoney) right = NULL;
	if (cost == NULL)
		return FALSE;
	right = venture_money_add(net, liabilities, error);
	if (right == NULL)
		return FALSE;
	if (venture_money_get_amount(cost) != venture_money_get_amount(right))
		return refuse(error, VENTURE_ERROR_VALIDATION, "gross plus employer_cost must equal net plus liabilities");
	return TRUE;
}

static gboolean
post_legs(VenturePayrollService *self, VentureEntity *run, const gchar *suffix,
	gint64 debit, gint64 credit, const VentureMoney *amount, GDateTime *date,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(g_object_unref);
	g_autofree gchar *transaction = g_strdup_printf("payroll:%s:%s", suffix, venture_entity_get_uuid(run));
	gint64 org = venture_entity_get_organization_id(run);
	gint64 id = venture_entity_get_id(run);
	if (amount == NULL || venture_money_is_zero(amount))
		return TRUE;
	add_entry(entries, org, transaction, debit, VENTURE_LEDGER_SIDE_DEBIT, amount, date, id);
	add_entry(entries, org, transaction, credit, VENTURE_LEDGER_SIDE_CREDIT, amount, date, id);
	return venture_posting_service_post_entries(venture_database_get_posting_service(self->database),
		entries, NULL, actor, error);
}

static gboolean
post_import(VenturePayrollService *self, VentureEntity *run, const VentureMoney *cost,
	const VentureMoney *net, const VentureMoney *liabilities, GDateTime *date,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) entries = g_ptr_array_new_with_free_func(g_object_unref);
	g_autofree gchar *transaction = g_strdup_printf("payroll:import:%s", venture_entity_get_uuid(run));
	gint64 org = venture_entity_get_organization_id(run);
	gint64 wages, tax, payable, id;
	wages = account_id(self, "expense", "6100", VENTURE_ACCOUNT_KIND_EXPENSE, org, error);
	if (wages == 0)
		return FALSE;
	tax = account_id(self, "tax", "2200", VENTURE_ACCOUNT_KIND_LIABILITY, org, error);
	if (tax == 0)
		return FALSE;
	payable = account_id(self, "payables", "2000", VENTURE_ACCOUNT_KIND_LIABILITY, org, error);
	if (payable == 0)
		return FALSE;
	id = venture_entity_get_id(run);
	add_entry(entries, org, transaction, wages, VENTURE_LEDGER_SIDE_DEBIT, cost, date, id);
	if (!venture_money_is_zero(liabilities))
		add_entry(entries, org, transaction, tax, VENTURE_LEDGER_SIDE_CREDIT, liabilities, date, id);
	if (!venture_money_is_zero(net))
		add_entry(entries, org, transaction, payable, VENTURE_LEDGER_SIDE_CREDIT, net, date, id);
	return venture_posting_service_post_entries(venture_database_get_posting_service(self->database),
		entries, NULL, actor, error);
}

static gboolean
add_line(VenturePayrollService *self, VentureEntity *run, JsonObject *object,
	const VentureActor *actor, VentureMoney **cost, VentureMoney **net, VentureMoney **liab, GError **error)
{
	g_autoptr(VentureMoney) gross = NULL;
	g_autoptr(VentureMoney) employer = NULL;
	g_autoptr(VentureMoney) deductions = NULL;
	g_autoptr(VentureMoney) line_net = NULL;
	g_autoptr(VentureMoney) liabilities = NULL;
	g_autoptr(VentureMoney) expected_net = NULL;
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureMoney) line_cost = NULL;
	const gchar *employee;
	/* Stop at the first parse failure rather than overwriting an existing GError. */
	if ((gross = money_member(object, "gross", error)) == NULL ||
		(employer = money_member(object, "employer_cost", error)) == NULL ||
		(deductions = money_member(object, "deductions", error)) == NULL ||
		(line_net = money_member(object, "net", error)) == NULL ||
		(liabilities = money_member(object, "liabilities", error)) == NULL)
		return FALSE;
	if (gross->amount < 0 || employer->amount < 0 || deductions->amount < 0 ||
		line_net->amount < 0 || liabilities->amount < 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Pay line amounts must be nonnegative");
	expected_net = venture_money_subtract(gross, deductions, error);
	if (expected_net == NULL)
		return FALSE;
	if (!venture_money_equal(expected_net, line_net))
		return refuse(error, VENTURE_ERROR_VALIDATION, "gross less deductions must equal net");
	if (!line_balances(gross, employer, line_net, liabilities, error))
		return FALSE;
	employee = venture_json_object_get_string(object, "employee", NULL);
	if (employee == NULL || employee[0] == '\0')
		return refuse(error, VENTURE_ERROR_VALIDATION, "Each pay line names an employee");
	line = VENTURE_ENTITY(venture_payroll_line_new());
	venture_entity_set_organization_id(line, venture_entity_get_organization_id(run));
	g_object_set(line, "run-id", venture_entity_get_id(run), "employee", employee,
		"gross", gross, "employer-cost", employer, "deductions", deductions,
		"net", line_net, "liabilities", liabilities, NULL);
	if (!write_owned(self, line, actor, error))
		return FALSE;
	line_cost = venture_money_add(gross, employer, error);
	if (line_cost == NULL)
		return FALSE;
	if (*cost == NULL)
		*cost = g_steal_pointer(&line_cost);
	else
	{
		g_autoptr(VentureMoney) next = venture_money_add(*cost, line_cost, error);
		if (next == NULL)
			return FALSE;
		venture_money_free(*cost);
		*cost = g_steal_pointer(&next);
	}
	if (*net == NULL)
		*net = venture_money_copy(line_net);
	else
	{
		g_autoptr(VentureMoney) next = venture_money_add(*net, line_net, error);
		if (next == NULL)
			return FALSE;
		venture_money_free(*net);
		*net = g_steal_pointer(&next);
	}
	if (*liab == NULL)
		*liab = venture_money_copy(liabilities);
	else
	{
		g_autoptr(VentureMoney) next = venture_money_add(*liab, liabilities, error);
		if (next == NULL)
			return FALSE;
		venture_money_free(*liab);
		*liab = g_steal_pointer(&next);
	}
	return TRUE;
}

static VentureEntity *
import_object(VenturePayrollService *self, gint64 organization_id, JsonObject *payload,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) run = NULL;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(VentureMoney) cost = NULL;
	g_autoptr(VentureMoney) net = NULL;
	g_autoptr(VentureMoney) liab = NULL;
	JsonArray *lines;
	const gchar *key;
	const gchar *currency;
	guint i;

	if (!begin_op(self, error))
		return NULL;
	key = venture_json_object_get_string(payload, "run_key", NULL);
	currency = venture_json_object_get_string(payload, "currency", "USD");
	if (key == NULL || key[0] == '\0')
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "An imported pay run needs a run_key");
		return finish_op(self, FALSE, error), NULL;
	}
	if (venture_json_object_get_string(payload, "period_start", NULL) != NULL)
		start = g_date_time_new_from_iso8601(venture_json_object_get_string(payload, "period_start", NULL), NULL);
	if (venture_json_object_get_string(payload, "period_end", NULL) != NULL)
		end = g_date_time_new_from_iso8601(venture_json_object_get_string(payload, "period_end", NULL), NULL);
	if (start == NULL || end == NULL || g_date_time_compare(start, end) >= 0)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "An imported pay run needs period_start and period_end");
		return finish_op(self, FALSE, error), NULL;
	}
	if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(self->database)),
		self->database, organization_id, start, error))
		return finish_op(self, FALSE, error), NULL;
	{
		JsonNode *node = json_object_get_member(payload, "lines");
		lines = node != NULL && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
	}
	if (lines == NULL || json_array_get_length(lines) == 0)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "Import at least one pay line");
		return finish_op(self, FALSE, error), NULL;
	}
	run = VENTURE_ENTITY(venture_payroll_run_new());
	venture_entity_set_organization_id(run, organization_id);
	g_object_set(run, "run-key", key, "period-start", start, "period-end", end,
		"currency", currency, "status", "imported", NULL);
	if (!write_owned(self, run, actor, error))
		return finish_op(self, FALSE, error), NULL;
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		JsonNode *node = json_array_get_element(lines, i);
		if (!JSON_NODE_HOLDS_OBJECT(node))
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "Each pay line must be a JSON object");
			return finish_op(self, FALSE, error), NULL;
		}
		if (!add_line(self, run, json_node_get_object(node), actor, &cost, &net, &liab, error))
			return finish_op(self, FALSE, error), NULL;
	}
	if (cost == NULL)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "Import at least one pay line");
		return finish_op(self, FALSE, error), NULL;
	}
	if (!post_import(self, run, cost, net, liab, start, actor, error))
		return finish_op(self, FALSE, error), NULL;
	if (!finish_op(self, TRUE, error))
		return NULL;
	return g_steal_pointer(&run);
}

VentureEntity *
venture_payroll_service_import_json(VenturePayrollService *self, gint64 organization_id,
	JsonObject *payload, const VentureActor *actor, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_PAYROLL_SERVICE(self), NULL);
	g_return_val_if_fail(payload != NULL, NULL);
	return import_object(self, organization_id, payload, actor, error);
}

static gchar *
unquote(gchar *field)
{
	gsize len;
	g_strstrip(field);
	len = strlen(field);
	if (len >= 2 && field[0] == '"' && field[len - 1] == '"')
	{
		field[len - 1] = '\0';
		memmove(field, field + 1, len - 1);
	}
	return field;
}

VentureEntity *
venture_payroll_service_import_csv(VenturePayrollService *self, gint64 organization_id,
	const gchar *run_key, const gchar *period_start, const gchar *period_end, const gchar *currency,
	const gchar *csv, const VentureActor *actor, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) node = NULL;
	g_auto(GStrv) rows = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_PAYROLL_SERVICE(self), NULL);
	if (csv == NULL)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "CSV is required");
		return NULL;
	}
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "run_key");
	json_builder_add_string_value(builder, run_key);
	json_builder_set_member_name(builder, "period_start");
	json_builder_add_string_value(builder, period_start);
	json_builder_set_member_name(builder, "period_end");
	json_builder_add_string_value(builder, period_end);
	json_builder_set_member_name(builder, "currency");
	json_builder_add_string_value(builder, currency != NULL ? currency : "USD");
	json_builder_set_member_name(builder, "lines");
	json_builder_begin_array(builder);
	rows = g_strsplit(csv, "\n", 0);
	for (i = 0; rows[i] != NULL; i++)
	{
		g_auto(GStrv) cols = NULL;
		if (rows[i][0] == '\0')
			continue;
		if (i == 0 && strstr(rows[i], "employee") != NULL)
			continue;
		cols = g_strsplit(rows[i], ",", 6);
		if (cols[0] == NULL || cols[1] == NULL || cols[2] == NULL || cols[3] == NULL || cols[4] == NULL || cols[5] == NULL)
		{
			refuse(error, VENTURE_ERROR_VALIDATION, "CSV lines are employee,gross,employer_cost,deductions,net,liabilities");
			return NULL;
		}
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "employee");
		json_builder_add_string_value(builder, unquote(cols[0]));
		json_builder_set_member_name(builder, "gross");
		json_builder_add_string_value(builder, unquote(cols[1]));
		json_builder_set_member_name(builder, "employer_cost");
		json_builder_add_string_value(builder, unquote(cols[2]));
		json_builder_set_member_name(builder, "deductions");
		json_builder_add_string_value(builder, unquote(cols[3]));
		json_builder_set_member_name(builder, "net");
		json_builder_add_string_value(builder, unquote(cols[4]));
		json_builder_set_member_name(builder, "liabilities");
		json_builder_add_string_value(builder, unquote(cols[5]));
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);
	return import_object(self, organization_id, json_node_get_object(node), actor, error);
}

static GPtrArray *
load_lines(VenturePayrollService *self, gint64 run_id, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PAYROLL_LINE);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "run-id", VENTURE_FILTER_OP_EQ, run_id, error))
		return NULL;
	return venture_database_find(self->database, query, error);
}

static gboolean
sum_field(GPtrArray *lines, const gchar *field, VentureMoney **total, GError **error)
{
	guint i;
	for (i = 0; i < lines->len; i++)
	{
		g_autoptr(VentureMoney) amount = NULL;
		g_object_get(g_ptr_array_index(lines, i), field, &amount, NULL);
		if (amount == NULL)
			continue;
		if (*total == NULL)
			*total = g_steal_pointer(&amount);
		else
		{
			g_autoptr(VentureMoney) next = venture_money_add(*total, amount, error);
			if (next == NULL)
				return FALSE;
			venture_money_free(*total);
			*total = g_steal_pointer(&next);
		}
	}
	return TRUE;
}

gboolean
venture_payroll_service_disburse(VenturePayrollService *self, VentureEntity *run,
	const gchar *kind, const VentureActor *actor, GError **error)
{
	g_autofree gchar *status = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureMoney) net = NULL;
	g_autoptr(VentureMoney) liab = NULL;
	g_autoptr(GDateTime) date = NULL;
	gboolean net_done = FALSE;
	gboolean tax_done = FALSE;
	gint64 org, cash, payable, tax;
	g_return_val_if_fail(VENTURE_IS_PAYROLL_SERVICE(self), FALSE);
	if (kind != NULL && g_strcmp0(kind, "all") != 0 && g_strcmp0(kind, "net") != 0 && g_strcmp0(kind, "tax") != 0)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Disburse all, net or tax");
	if (!begin_op(self, error))
		return FALSE;
	g_object_get(run, "status", &status, "net-disbursed", &net_done, "tax-disbursed", &tax_done,
		"period-end", &date, NULL);
	if (g_strcmp0(status, "imported") != 0 && g_strcmp0(status, "disbursed") != 0)
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "Only an imported run can be disbursed");
		return finish_op(self, FALSE, error);
	}
	org = venture_entity_get_organization_id(run);
	if (!venture_period_guard_is_postable(VENTURE_PERIOD_GUARD(venture_database_get_period_guard(self->database)),
		self->database, org, date, error))
		return finish_op(self, FALSE, error);
	lines = load_lines(self, venture_entity_get_id(run), org, error);
	if (lines == NULL || !sum_field(lines, "net", &net, error) || !sum_field(lines, "liabilities", &liab, error))
		return finish_op(self, FALSE, error);
	cash = account_id(self, "cash", "1000", VENTURE_ACCOUNT_KIND_ASSET, org, error);
	payable = account_id(self, "payables", "2000", VENTURE_ACCOUNT_KIND_LIABILITY, org, error);
	tax = account_id(self, "tax", "2200", VENTURE_ACCOUNT_KIND_LIABILITY, org, error);
	if (cash == 0 || payable == 0 || tax == 0)
		return finish_op(self, FALSE, error);
	if ((kind == NULL || g_strcmp0(kind, "all") == 0 || g_strcmp0(kind, "net") == 0) && !net_done)
	{
		if (!post_legs(self, run, "net", payable, cash, net, date, actor, error))
			return finish_op(self, FALSE, error);
		net_done = TRUE;
	}
	if ((kind == NULL || g_strcmp0(kind, "all") == 0 || g_strcmp0(kind, "tax") == 0) && !tax_done)
	{
		if (!post_legs(self, run, "tax", tax, cash, liab, date, actor, error))
			return finish_op(self, FALSE, error);
		tax_done = TRUE;
	}
	g_object_set(run, "status", "disbursed", "net-disbursed", net_done, "tax-disbursed", tax_done, NULL);
	if (!write_owned(self, run, actor, error))
		return finish_op(self, FALSE, error);
	return finish_op(self, TRUE, error);
}

gboolean
venture_payroll_service_reverse(VenturePayrollService *self, VentureEntity *run,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) journals = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *status = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_PAYROLL_SERVICE(self), FALSE);
	if (!begin_op(self, error))
		return FALSE;
	g_object_get(run, "status", &status, "period-end", &when, NULL);
	if (g_strcmp0(status, "reversed") == 0)
	{
		refuse(error, VENTURE_ERROR_CONFLICT, "The run is already reversed");
		return finish_op(self, FALSE, error);
	}
	journals = venture_posting_service_find_source(venture_database_get_posting_service(self->database),
		"payroll_run", venture_entity_get_id(run), venture_entity_get_organization_id(run), error);
	if (journals == NULL)
		return finish_op(self, FALSE, error);
	for (i = 0; i < journals->len; i++)
	{
		VentureEntity *journal = g_ptr_array_index(journals, i);
		gint state = 0;
		gint64 reverses = 0;
		g_object_get(journal, "state", &state, "reverses-id", &reverses, NULL);
		if (state != VENTURE_JOURNAL_POSTED || reverses > 0)
			continue;
		if (venture_posting_service_reverse(venture_database_get_posting_service(self->database),
			venture_entity_get_id(journal), when, "Reverse payroll run", actor, error) == NULL)
			return finish_op(self, FALSE, error);
	}
	g_object_set(run, "status", "reversed", NULL);
	if (!write_owned(self, run, actor, error))
		return finish_op(self, FALSE, error);
	return finish_op(self, TRUE, error);
}

gboolean
venture_payroll_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error)
{
	VenturePayrollService *self;
	(void)actor;
	*handled = FALSE;
	if (!VENTURE_IS_PAYROLL_RUN(record) && !VENTURE_IS_PAYROLL_LINE(record))
		return TRUE;
	if (!module_on())
		return refuse(error, VENTURE_ERROR_CONFIG, "The payroll module is disabled (payroll.enabled)");
	self = venture_payroll_service_get(database);
	if (self->writing == record)
		return TRUE;
	return refuse(error, VENTURE_ERROR_VALIDATION, "Pay runs are written through VenturePayrollService");
}

gboolean
venture_payroll_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error)
{
	(void)database;
	if (!VENTURE_IS_PAYROLL_RUN(record) && !VENTURE_IS_PAYROLL_LINE(record))
		return TRUE;
	if (!removal)
		return TRUE;
	return refuse(error, VENTURE_ERROR_VALIDATION, "Imported pay runs are retained; reverse them instead of deleting");
}

static VentureReportResult *
payroll_reconciliation_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_PAYROLL_RUN);
	g_autoptr(GPtrArray) runs = NULL;
	g_autoptr(VentureReportResult) result = venture_report_result_new("Payroll reconciliation", period);
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	guint i;
	if (org == 0)
		org = venture_context_get_default_organization_id(context);
	if (!module_on())
	{
		refuse(error, VENTURE_ERROR_CONFIG, "The payroll module is disabled (payroll.enabled)");
		return NULL;
	}
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 0);
	runs = venture_database_find(db, query, error);
	if (runs == NULL)
		return NULL;
	venture_report_result_add_column(result, "run_key", "Run", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "liabilities", "Liabilities", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "unpaid", "Unpaid", VENTURE_REPORT_COLUMN_MONEY);
	for (i = 0; i < runs->len; i++)
	{
		VentureEntity *run = g_ptr_array_index(runs, i);
		g_autoptr(GPtrArray) lines = NULL;
		g_autoptr(VentureMoney) liab = NULL;
		g_autoptr(VentureMoney) unpaid = NULL;
		g_autofree gchar *key = NULL;
		g_autofree gchar *status = NULL;
		gboolean tax_done = FALSE;
		gint64 run_id = venture_entity_get_id(run);
		g_object_get(run, "run-key", &key, "status", &status, "tax-disbursed", &tax_done, NULL);
		if (g_strcmp0(status, "reversed") == 0)
			continue;
		{
			g_autoptr(VentureQuery) lq = venture_query_new(VENTURE_TYPE_PAYROLL_LINE);
			venture_query_set_organization(lq, org);
			if (!venture_query_add_filter_int(lq, "run-id", VENTURE_FILTER_OP_EQ, run_id, error))
				return NULL;
			lines = venture_database_find(db, lq, error);
			if (lines == NULL)
				return NULL;
		}
		if (!sum_field(lines, "liabilities", &liab, error))
			return NULL;
		if (liab == NULL)
			continue;
		unpaid = tax_done ? venture_money_new_zero(venture_money_get_currency(liab)) : venture_money_copy(liab);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "run_key", key);
		venture_report_result_set_money(result, "liabilities", liab);
		venture_report_result_set_money(result, "unpaid", unpaid);
	}
	return g_steal_pointer(&result);
}

void
venture_payroll_register_reports(VentureReportRegistry *registry)
{
	venture_report_registry_add(registry, VENTURE_REPORT(venture_func_report_new(
		"payroll_reconciliation", "Payroll reconciliation",
		"Imported tax liabilities versus unpaid remittances.", payroll_reconciliation_report)));
}
