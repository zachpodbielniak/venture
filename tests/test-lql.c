/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-lql.c - VENTURE's records as LQL sources (docs/lql.org)
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * What the LQL record source promises, and what breaks if it regresses:
 *
 *   - Pushdown never changes an answer: every filter run right after the
 *     fetch (offered to the database) gives exactly the rows it gives
 *     after a `sort` (where LQL alone evaluates it), and the conjuncts
 *     meant to reach the database do.
 *   - A viewer's `fetch ticket` is GET /api/v1/ticket: the same rows and
 *     the same fields, through the very door the query surfaces will use
 *     (venture_web_server_new_lql_environment()); a type the list API
 *     refuses is no source, and a label never names a record the caller
 *     cannot read.
 *   - No field LQL sees is sensitive; a module that is off has no source;
 *     soft-deleted rows stay out unless asked for.
 *   - Money is {amount, currency, exponent, formatted}, amount an Int.
 *   - NULL is three-valued on both paths.
 *   - query.max_rows refuses, it never truncates; a limit still works.
 *   - me(), my_teams(), organization(), today(), period().
 *   - No file, program, SQL, git or Kubernetes source exists.
 */

#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "lql/venture-lql.h"
#include "venture-test-util.h"
#include "venture-test-member.h"

/* 2026-11-15T12:00:00Z: every query's now(), so periods are known. */
#define TEST_NOW_USEC (G_GINT64_CONSTANT(1794744000) * G_USEC_PER_SEC)

/* --- The fixture -------------------------------------------------------- */

typedef struct
{
	gchar			*directory;
	VentureConfig		*config;
	VentureDatabase		*db;
	VentureContext		*context;
	gint64			 org;
	gint64			 admin_id;
	gint64			 viewer_id;
	gint64			 team_id;
	gint64			 acme_id;
	gint64			 globex_id;
	GArray			*tickets;	/* gint64: the live tickets' ids */
	gint64			 deleted_ticket;
	VentureAuthPrincipal	 admin;
} Fixture;

/* A user, saved as given. */
static gint64
fixture_user(
	Fixture		*f,
	const gchar	*username,
	VentureUserRole	 role
){
	g_autoptr(VentureEntity) user = NULL;
	g_autoptr(GError) error = NULL;

	user = g_object_new(VENTURE_TYPE_USER, "username", username, "role", role,
	                    "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, user, NULL, &error));
	g_assert_no_error(error);

	return venture_entity_get_id(user);
}

/* A membership of the fixture's business. */
static void
fixture_membership(
	Fixture		*f,
	gint64		 user_id,
	gint		 role
){
	g_autoptr(VentureEntity) member = NULL;

	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", f->org,
		"user-id", user_id, "active", TRUE, "role", role, NULL);
	g_assert_true(venture_database_save(f->db, member, NULL, NULL));
}

/* A company of the fixture's business. */
static gint64
fixture_company(
	Fixture		*f,
	const gchar	*name
){
	g_autoptr(VentureEntity) company = NULL;

	company = g_object_new(VENTURE_TYPE_COMPANY, "name", name, "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, company, NULL, NULL));

	return venture_entity_get_id(company);
}

/*
 * Twelve tickets with every kind of value the pushdown table names:
 * statuses and priorities (enums), assignees that are a name, NULL and
 * empty, estimates (doubles), due times that are NULL, whole seconds and
 * half seconds (stored text compares by instant only with care), and two
 * companies. One is on a team the viewer belongs to, one is owned by the
 * viewer; one more is soft-deleted.
 */
static void
fixture_tickets(Fixture *f)
{
	static const VentureTicketStatus statuses[] = {
		VENTURE_TICKET_STATUS_TRIAGE, VENTURE_TICKET_STATUS_TODO,
		VENTURE_TICKET_STATUS_IN_PROGRESS, VENTURE_TICKET_STATUS_DONE,
		VENTURE_TICKET_STATUS_CANCELLED
	};
	static const VenturePriority priorities[] = {
		VENTURE_PRIORITY_LOW, VENTURE_PRIORITY_NORMAL, VENTURE_PRIORITY_HIGH
	};
	g_autoptr(VentureEntity) deleted = NULL;
	guint i;

	for (i = 0; i < 12; i++)
	{
		g_autoptr(VentureEntity) ticket = NULL;
		g_autoptr(GError) error = NULL;
		g_autofree gchar *title = g_strdup_printf("T%02u", i);
		const gchar *assignee = NULL;
		gint64 id;

		switch (i % 4)
		{
		case 0: assignee = "vee"; break;
		case 1: assignee = "ana"; break;
		case 2: assignee = NULL; break;
		default: assignee = ""; break;
		}

		ticket = g_object_new(VENTURE_TYPE_TICKET, "title", title,
			"organization-id", f->org,
			"status", statuses[i % G_N_ELEMENTS(statuses)],
			"priority", priorities[i % G_N_ELEMENTS(priorities)],
			"assignee", assignee,
			"estimate-hours", (gdouble)i * 1.5,
			"company-id", (i % 2 == 0) ? f->acme_id : f->globex_id,
			NULL);

		/* Due on the first of the month plus i days at 09:00; every
		 * third has none, every other odd one is half a second in. */
		if (0 != i % 3)
		{
			g_autoptr(GDateTime) due = g_date_time_new_utc(2026, 11, (gint)i + 1, 9, 0,
				(1 == i % 2) ? 0.5 : 0.0);

			g_object_set(ticket, "due-at", due, NULL);
		}

		/* Ticket 5 is on the viewer's team, ticket 6 is the viewer's own. */
		if (5 == i)
			g_object_set(ticket, "team-id", f->team_id, NULL);
		if (6 == i)
			g_object_set(ticket, "owner-user-id", f->viewer_id, NULL);

		g_assert_true(venture_database_save(f->db, ticket, NULL, &error));
		g_assert_no_error(error);
		id = venture_entity_get_id(ticket);
		g_array_append_val(f->tickets, id);
	}

	deleted = g_object_new(VENTURE_TYPE_TICKET, "title", "Gone", "organization-id", f->org,
		"assignee", "vee", NULL);
	g_assert_true(venture_database_save(f->db, deleted, NULL, NULL));
	f->deleted_ticket = venture_entity_get_id(deleted);
	g_assert_true(venture_database_delete(f->db, deleted, NULL, NULL));
}

static void
fixture_setup(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(VentureEntity) team = NULL;
	g_autoptr(VentureEntity) membership = NULL;
	const gchar *disabled_module = data;

	f->directory = g_dir_make_tmp("venture-lql-XXXXXX", NULL);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->directory, "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET", NULL);

	if (NULL != disabled_module)
		venture_config_set_module_enabled(f->config, disabled_module, FALSE);

	f->db = venture_database_new("sqlite://:memory:", NULL);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), NULL));
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	f->tickets = g_array_new(FALSE, FALSE, sizeof(gint64));

	/* ada administers everything; vee is a viewer of the business; ana is
	 * an ordinary member somebody else's tickets go to. */
	f->admin_id = fixture_user(f, "ada", VENTURE_USER_ROLE_ADMIN);
	f->viewer_id = fixture_user(f, "vee", VENTURE_USER_ROLE_VIEWER);
	fixture_membership(f, f->viewer_id, VENTURE_ORGANIZATION_ROLE_VIEWER);
	venture_test_member(f->db, f->org, "ana");

	team = g_object_new(VENTURE_TYPE_TEAM, "name", "Support", "organization-id", f->org, NULL);
	g_assert_true(venture_database_save(f->db, team, NULL, NULL));
	f->team_id = venture_entity_get_id(team);
	membership = g_object_new(VENTURE_TYPE_TEAM_MEMBERSHIP, "organization-id", f->org,
		"team-id", f->team_id, "user-id", f->viewer_id, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, membership, NULL, NULL));

	f->acme_id = fixture_company(f, "Acme");
	f->globex_id = fixture_company(f, "Globex");
	fixture_tickets(f);

	memset(&f->admin, 0, sizeof f->admin);
	f->admin.user_id = f->admin_id;
	f->admin.role = VENTURE_USER_ROLE_ADMIN;
	f->admin.authenticated = TRUE;
}

static void
fixture_teardown(
	Fixture		*f,
	gconstpointer	 data
){
	(void)data;

	g_clear_pointer(&f->tickets, g_array_unref);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->directory);
	g_clear_pointer(&f->directory, g_free);
}

/* --- Helpers ------------------------------------------------------------- */

static gboolean
allow_every_type(
	GType		  entity_type,
	gpointer	  user_data,
	GError		**error
){
	(void)entity_type;
	(void)user_data;
	(void)error;

	return TRUE;
}

/* An environment for @who scoped to the fixture's business, at TEST_NOW. */
static LqlEnvironment *
environment_for(
	Fixture				*f,
	const VentureAuthPrincipal	*who
){
	g_autoptr(GError) error = NULL;
	LqlEnvironment *environment;

	environment = venture_lql_environment_new(f->context, who, &f->org, 1,
		allow_every_type, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(environment);
	lql_environment_set_now(environment, TEST_NOW_USEC);

	return environment;
}

/* Runs @text; asserts it succeeds. */
static LqlTable *
run(
	LqlEnvironment	*environment,
	const gchar	*text
){
	g_autoptr(GError) error = NULL;
	LqlTable *table;

	table = lql_environment_run_sync(environment, text, NULL, NULL, &error);

	if (NULL != error)
		g_error("%s: %s", text, error->message);

	return table;
}

/* The "id" of every row, in row order, as "1,2,3". */
static gchar *
row_ids(LqlTable *table)
{
	g_autoptr(GString) out = g_string_new(NULL);
	guint i;

	for (i = 0; i < lql_table_get_n_rows(table); i++)
	{
		LqlValue *id = lql_record_get(lql_table_get_row(table, i), "id");

		g_string_append_printf(out, "%s%" G_GINT64_FORMAT, (i > 0) ? "," : "",
		                       lql_value_get_int(id));
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* How the source took each conjunct of @text, as "EHN" (exact, hint, none). */
static gchar *
pushdowns(
	LqlEnvironment	*environment,
	const gchar	*text
){
	g_autoptr(LqlQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) out = g_string_new(NULL);
	LqlFetchRequest *request;
	guint i;

	query = lql_environment_prepare(environment, text, NULL, &error);
	g_assert_no_error(error);
	request = lql_query_get_fetch_request(query);
	g_assert_nonnull(request);

	for (i = 0; i < lql_fetch_request_get_n_predicates(request); i++)
	{
		switch (lql_fetch_request_get_pushdown(request, i))
		{
		case LQL_PUSHDOWN_EXACT: g_string_append_c(out, 'E'); break;
		case LQL_PUSHDOWN_HINT:  g_string_append_c(out, 'H'); break;
		default:                 g_string_append_c(out, 'N'); break;
		}
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* --- No dangerous sources ------------------------------------------------- */

/*
 * Nothing in VENTURE's environment reads a file, runs a program or talks
 * SQL: those words are unknown, there are no source commands at all, and
 * every `fetch` name is a record source.
 *
 * Breaks if somebody adds LqlFileProvider, LqlCommandProvider, the SQL,
 * git or Kubernetes providers -- each reads around the access policy.
 */
static void
test_no_dangerous_sources(
	Fixture		*f,
	gconstpointer	 data
){
	static const gchar * const refused[] = {
		"read \"/etc/passwd\"",
		"sql \"SELECT * FROM users\"",
		"sqlite \"/tmp/x.db\", \"SELECT 1\"",
		"postgres \"postgres://localhost/x\", \"SELECT 1\"",
		"exec [\"cat\", \"/etc/passwd\"]",
		"git_log",
		"k8s \"pods\"",
		"journal",
	};
	g_autoptr(LqlEnvironment) environment = environment_for(f, &f->admin);
	g_auto(GStrv) commands = NULL;
	g_auto(GStrv) sources = NULL;
	guint i;

	(void)data;

	for (i = 0; i < G_N_ELEMENTS(refused); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(LqlTable) table = NULL;

		table = lql_environment_run_sync(environment, refused[i], NULL, NULL, &error);
		g_assert_null(table);
		g_assert_error(error, LQL_ERROR, LQL_ERROR_UNKNOWN_COMMAND);
	}

	commands = lql_environment_list_source_commands(environment);
	g_assert_cmpuint(g_strv_length(commands), ==, 0);

	sources = lql_environment_list_sources(environment);
	g_assert_true(g_strv_contains((const gchar * const *)sources, "ticket"));

	for (i = 0; NULL != sources[i]; i++)
		g_assert_true(VENTURE_IS_LQL_SOURCE(lql_environment_lookup_source(environment, sources[i])));
}

/* --- Pushdown against LQL alone ------------------------------------------- */

typedef struct
{
	const gchar	*filter;
	const gchar	*taken;	/* how each conjunct is taken: E, H or N */
} PushdownCase;

/*
 * Every filter gives the same rows whether it is offered to the database
 * (right after the fetch) or evaluated by LQL alone (after a sort), and
 * each conjunct is taken as the pushdown table says.
 *
 * Breaks if a mapped filter means something else in SQL than in LQL: an
 * empty string, a NULL, a half-second timestamp, a value of another type,
 * a LIKE wildcard, an IN list.
 */
static void
test_pushdown_same_rows(
	Fixture		*f,
	gconstpointer	 data
){
	static const PushdownCase cases[] = {
		{ "status == \"done\"", "E" },
		{ "\"done\" == status", "E" },
		{ "status != \"done\"", "E" },
		{ "status in [\"todo\", \"triage\"]", "E" },
		{ "status not in [\"done\", \"cancelled\"]", "E" },
		{ "priority == \"high\" and status != \"cancelled\"", "EE" },
		{ "assignee == \"vee\"", "E" },
		{ "assignee != \"vee\"", "E" },
		{ "assignee == \"\"", "N" },
		{ "assignee != \"\"", "N" },
		{ "is_null(assignee)", "E" },
		{ "is_not_null(assignee)", "E" },
		{ "assignee in [\"vee\", \"ana\"]", "E" },
		{ "assignee == me()", "E" },
		{ "estimate_hours > 4", "E" },
		{ "estimate_hours <= 3.0", "E" },
		{ "4.5 < estimate_hours", "E" },
		{ "between(estimate_hours, 2, 9)", "E" },
		{ "due_at < timestamp(\"2026-11-06T09:00:00Z\")", "E" },
		{ "due_at >= timestamp(\"2026-11-06T09:00:00Z\")", "E" },
		{ "due_at > timestamp(\"2026-11-06T09:00:00Z\")", "E" },
		{ "due_at <= timestamp(\"2026-11-06T09:00:00.5Z\")", "E" },
		{ "due_at > timestamp(\"2026-11-02T09:00:00Z\")", "E" },
		{ "due_at > timestamp(\"2026-11-03T09:00:00Z\")", "E" },
		{ "due_at >= timestamp(\"2026-11-03T09:00:00Z\")", "E" },
		{ "due_at <= timestamp(\"2026-11-03T09:00:00Z\")", "E" },
		{ "due_at < timestamp(\"2026-11-03T09:00:00Z\")", "E" },
		{ "between(due_at, timestamp(\"2026-11-02T09:00:00Z\"), timestamp(\"2026-11-08T09:00:00.5Z\"))", "E" },
		{ "due_at == timestamp(\"2026-11-03T09:00:00Z\")", "N" },
		{ "is_null(due_at)", "E" },
		{ "due_at >= period(\"this_month\").start and due_at < period(\"this_month\").end", "EE" },
		{ "due_at < today()", "E" },
		{ "due_at > now() - 10d", "E" },
		{ "id > 3", "E" },
		{ "id in [1, 2, 3]", "E" },
		{ "title == 5", "N" },
		{ "estimate_hours == \"3\"", "N" },
		{ "contains(title, \"0\")", "H" },
		{ "starts_with(title, \"T1\")", "H" },
		{ "ends_with(title, \"1\")", "H" },
		{ "like(title, \"T_1\")", "H" },
		{ "like(title, \"t%\")", "H" },
		{ "contains(title, \"t0\", TRUE)", "N" },
		{ "status == \"done\" or priority == \"high\"", "N" },
		{ "not (status == \"done\")", "N" },
		{ "company_label == \"Acme\"", "N" },
		{ "length(title) == 3", "N" },
	};
	g_autoptr(LqlEnvironment) environment = environment_for(f, &f->admin);
	guint i;

	(void)data;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *pushed_text = g_strdup_printf("fetch ticket | filter %s | sort id", cases[i].filter);
		g_autofree gchar *local_text = g_strdup_printf("fetch ticket | sort id | filter %s", cases[i].filter);
		g_autofree gchar *taken = pushdowns(environment, pushed_text);
		g_autoptr(LqlTable) pushed = run(environment, pushed_text);
		g_autoptr(LqlTable) local = run(environment, local_text);
		g_autofree gchar *pushed_ids = row_ids(pushed);
		g_autofree gchar *local_ids = row_ids(local);

		g_test_message("%s: %s -> %s", cases[i].filter, taken, pushed_ids);
		g_assert_cmpstr(taken, ==, cases[i].taken);
		g_assert_cmpstr(pushed_ids, ==, local_ids);
	}

	/* The cases are not vacuous: the "done" filter finds rows, and the
	 * whole-second boundary is where a half-second row sits beside it. */
	{
		g_autoptr(LqlTable) done = run(environment, "fetch ticket | filter status == \"done\"");
		g_autoptr(LqlTable) all = run(environment, "fetch ticket");

		g_assert_cmpuint(lql_table_get_n_rows(done), >, 0);
		g_assert_cmpuint(lql_table_get_n_rows(all), ==, f->tickets->len);
	}
}

/* --- NULL ------------------------------------------------------------------ */

/*
 * A comparison with NULL is NULL on both paths: `due_at < X` and
 * `not (due_at < X)` both leave the undated tickets out, `filter_out`
 * keeps them, and `is_null` finds exactly them.
 */
static void
test_nulls(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(LqlEnvironment) environment = environment_for(f, &f->admin);
	g_autoptr(LqlTable) before = NULL;
	g_autoptr(LqlTable) not_before = NULL;
	g_autoptr(LqlTable) kept = NULL;
	g_autoptr(LqlTable) undated = NULL;
	guint dated = 0;
	guint i;

	(void)data;

	for (i = 0; i < f->tickets->len; i++)
		dated += (0 != i % 3) ? 1 : 0;

	before = run(environment, "fetch ticket | filter due_at < timestamp(\"2026-11-06T00:00:00Z\")");
	not_before = run(environment, "fetch ticket | filter not (due_at < timestamp(\"2026-11-06T00:00:00Z\"))");
	kept = run(environment, "fetch ticket | filter_out due_at < timestamp(\"2026-11-06T00:00:00Z\")");
	undated = run(environment, "fetch ticket | filter is_null(due_at)");

	g_assert_cmpuint(lql_table_get_n_rows(before) + lql_table_get_n_rows(not_before), ==, dated);
	g_assert_cmpuint(lql_table_get_n_rows(kept), ==,
	                 lql_table_get_n_rows(not_before) + lql_table_get_n_rows(undated));
	g_assert_cmpuint(lql_table_get_n_rows(undated), ==, f->tickets->len - dated);

	/* An unassigned ticket's assignee is NULL or "", whichever it holds:
	 * the row says which, the API's way. */
	{
		g_autoptr(LqlTable) nulls = run(environment, "fetch ticket | filter is_null(assignee)");
		g_autoptr(LqlTable) empty = run(environment, "fetch ticket | filter assignee == \"\"");

		g_assert_cmpuint(lql_table_get_n_rows(nulls), ==, 3);
		g_assert_cmpuint(lql_table_get_n_rows(empty), ==, 3);
	}
}

/* --- Money ---------------------------------------------------------------- */

/*
 * A money field is the API's record -- an Int amount in minor units, the
 * currency, the exponent and the formatted text -- never a Double, and
 * its amount and currency filter in the database.
 */
static void
test_money(
	Fixture		*f,
	gconstpointer	 data
){
	static const gchar * const values[] = { "100 USD", "2500 USD", "50 EUR", NULL };
	g_autoptr(LqlEnvironment) environment = NULL;
	g_autoptr(LqlTable) big = NULL;
	g_autoptr(LqlTable) euros = NULL;
	g_autoptr(LqlTable) none = NULL;
	LqlValue *value;
	LqlRecord *money;
	guint i;

	(void)data;

	for (i = 0; i < G_N_ELEMENTS(values); i++)
	{
		g_autoptr(VentureEntity) deal = NULL;
		g_autoptr(GError) error = NULL;
		g_autofree gchar *name = g_strdup_printf("Deal %u", i);

		deal = g_object_new(VENTURE_TYPE_DEAL, "organization-id", f->org, "name", name, NULL);

		if (NULL != values[i])
			g_assert_true(venture_entity_set_field_from_string(deal, "value", values[i], &error));

		g_assert_true(venture_database_save(f->db, deal, NULL, &error));
		g_assert_no_error(error);
	}

	environment = environment_for(f, &f->admin);

	g_assert_cmpstr(pushdowns(environment, "fetch deal | filter value.amount > 5000"), ==, "E");
	g_assert_cmpstr(pushdowns(environment, "fetch deal | filter value.currency == \"EUR\""), ==, "E");
	g_assert_cmpstr(pushdowns(environment, "fetch deal | filter is_null(value)"), ==, "E");

	big = run(environment, "fetch deal | filter value.amount > 5000 | sort name");
	g_assert_cmpuint(lql_table_get_n_rows(big), ==, 2);
	value = lql_record_get(lql_table_get_row(big, 0), "value");
	g_assert_cmpint(lql_value_get_value_type(value), ==, LQL_VALUE_TYPE_RECORD);
	money = lql_value_get_record(value);
	g_assert_cmpint(lql_value_get_value_type(lql_record_get(money, "amount")), ==, LQL_VALUE_TYPE_INT);
	g_assert_cmpint(lql_value_get_int(lql_record_get(money, "amount")), ==, 10000);
	g_assert_cmpstr(lql_value_get_string(lql_record_get(money, "currency")), ==, "USD");
	g_assert_cmpint(lql_value_get_int(lql_record_get(money, "exponent")), ==, 2);
	g_assert_nonnull(strstr(lql_value_get_string(lql_record_get(money, "formatted")), "100"));

	euros = run(environment, "fetch deal | filter value.currency == \"EUR\"");
	g_assert_cmpuint(lql_table_get_n_rows(euros), ==, 1);

	none = run(environment, "fetch deal | filter is_null(value)");
	g_assert_cmpuint(lql_table_get_n_rows(none), ==, 1);

	/* The same rows when LQL alone filters. */
	{
		g_autoptr(LqlTable) local = run(environment, "fetch deal | sort name | filter value.amount > 5000");
		g_autofree gchar *a = row_ids(big);
		g_autofree gchar *b = row_ids(local);

		g_assert_cmpstr(a, ==, b);
	}

	/* A column that is never a Double: the schema says Record. */
	{
		g_autoptr(GPtrArray) columns = lql_source_get_columns(lql_environment_lookup_source(environment, "deal"));

		for (i = 0; i < columns->len; i++)
		{
			LqlColumn *column = g_ptr_array_index(columns, i);

			if (0 == g_strcmp0(lql_column_get_name(column), "value"))
				g_assert_cmpint(lql_column_get_column_type(column), ==, LQL_VALUE_TYPE_RECORD);
		}
	}
}

/* --- Legacy NULL columns (VENTURE #45) ------------------------------------ */

/* Runs @statement, which must succeed; printf-style. */
static void
legacy_sql(
	Fixture		*f,
	const gchar	*format,
	...
) G_GNUC_PRINTF(2, 3);

static void
legacy_sql(
	Fixture		*f,
	const gchar	*format,
	...
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *statement = NULL;
	va_list args;

	va_start(args, format);
	statement = g_strdup_vprintf(format, args);
	va_end(args);

	g_assert_true(venture_database_execute(f->db, statement, NULL, &error));
	g_assert_no_error(error);
}

/* The table @type is stored in. */
static gchar *
legacy_table(GType type)
{
	g_autoptr(VentureEntity) prototype = g_object_new(type, NULL);

	return g_strdup(venture_entity_get_table_name(prototype));
}

/*
 * Each filter of @cases on @type, pushed (right after the fetch) and
 * LQL-only (after a sort), must give the same rows and be taken as the
 * case says. Every case is run before failing, so a regression names all
 * the cases it breaks at once.
 */
static void
legacy_check(
	LqlEnvironment		*environment,
	const gchar		*type,
	const PushdownCase	*cases,
	guint			 n_cases
){
	g_autoptr(GString) failures = g_string_new(NULL);
	guint i;

	for (i = 0; i < n_cases; i++)
	{
		g_autofree gchar *pushed_text = g_strdup_printf("fetch %s | filter %s | sort id",
			type, cases[i].filter);
		g_autofree gchar *local_text = g_strdup_printf("fetch %s | sort id | filter %s",
			type, cases[i].filter);
		g_autofree gchar *taken = pushdowns(environment, pushed_text);
		g_autoptr(LqlTable) pushed = run(environment, pushed_text);
		g_autoptr(LqlTable) local = run(environment, local_text);
		g_autofree gchar *pushed_ids = row_ids(pushed);
		g_autofree gchar *local_ids = row_ids(local);

		g_test_message("%s: %s -> pushed [%s], LQL-only [%s]", cases[i].filter, taken,
		               pushed_ids, local_ids);

		if (0 != g_strcmp0(taken, cases[i].taken))
			g_string_append_printf(failures, "\n  %s: taken %s, expected %s",
			                       cases[i].filter, taken, cases[i].taken);
		if (0 != g_strcmp0(pushed_ids, local_ids))
			g_string_append_printf(failures, "\n  %s: pushed [%s] != LQL-only [%s]",
			                       cases[i].filter, pushed_ids, local_ids);
	}

	g_assert_cmpstr(failures->str, ==, "");
}

/*
 * A column a later migration added is NULL in every older row, but the
 * record reads a value there: priority (an enum) reads "low", its zero
 * member; story_points and company_id (integers) read 0; estimate_hours
 * (a double) 0; sla_breached (a boolean) FALSE. A filter pushed to the
 * database must keep exactly the rows LQL keeps from what the rows read:
 * is_null is never true, `priority == "low"` takes the NULL row too, and
 * `!=` and `not in` take it unless they exclude the value it reads as.
 *
 * A money value: a NULL currency column under an amount reads as the
 * default currency, and a NULL amount is a NULL money whatever its
 * currency column says.
 *
 * Breaks if the source pushes the stored-column filter for these again
 * (the cases that move a NULL row fail at edd58bd7), or if the value a
 * NULL column reads as is taken from anywhere but what loading a row
 * leaves.
 *
 * The list API's ?priority=low is deliberately unchanged: it compares the
 * stored column, so it still leaves the NULL row out.
 */
static void
test_legacy_null_columns(
	Fixture		*f,
	gconstpointer	 data
){
	static const PushdownCase ticket_cases[] = {
		{ "is_null(priority)", "E" },
		{ "is_not_null(priority)", "E" },
		{ "priority == \"low\"", "E" },
		{ "priority != \"low\"", "E" },
		{ "priority == \"high\"", "E" },
		{ "priority != \"high\"", "E" },
		{ "priority in [\"low\", \"high\"]", "E" },
		{ "priority in [\"normal\", \"high\"]", "E" },
		{ "priority not in [\"low\"]", "E" },
		{ "priority not in [\"normal\", \"high\"]", "E" },
		{ "is_null(story_points)", "E" },
		{ "is_not_null(story_points)", "E" },
		{ "story_points == 0", "E" },
		{ "story_points != 0", "E" },
		{ "story_points != 3", "E" },
		{ "story_points in [0, 3]", "E" },
		{ "story_points not in [3]", "E" },
		{ "story_points < 1", "E" },
		{ "story_points >= 1", "E" },
		{ "between(story_points, 0, 2)", "E" },
		{ "is_null(company_id)", "E" },
		{ "company_id == 0", "E" },
		{ "is_null(estimate_hours)", "E" },
		{ "estimate_hours == 0", "E" },
		{ "estimate_hours <= 1.5", "E" },
		{ "estimate_hours > 1", "E" },
		{ "is_null(sla_breached)", "E" },
		{ "sla_breached == FALSE", "E" },
		{ "sla_breached != TRUE", "E" },
		{ "sla_breached == TRUE", "E" },
		{ "is_null(due_at)", "E" },
		{ "created_at >= period(\"this_week\").start", "E" },
		{ "updated_at < now()", "E" },
		{ "priority == \"low\" and is_not_null(story_points)", "EE" },
	};
	g_autofree gchar *tickets = legacy_table(VENTURE_TYPE_TICKET);
	g_autofree gchar *deals = legacy_table(VENTURE_TYPE_DEAL);
	g_autofree gchar *book = g_ascii_strup(venture_money_get_default_currency(), -1);
	g_autofree gchar *book_case = g_strdup_printf("value.currency == \"%s\"", book);
	g_autoptr(LqlEnvironment) environment = NULL;
	gint64 legacy = g_array_index(f->tickets, gint64, 1);
	gint64 deal_ids[4];
	guint i;

	(void)data;

	/* Some tickets have points, and one is breached, so the NULL row is
	 * not the only one either side of each comparison. */
	legacy_sql(f, "UPDATE \"%s\" SET \"story_points\" = 3 WHERE \"id\" IN (%" G_GINT64_FORMAT
	           ", %" G_GINT64_FORMAT ")", tickets,
	           g_array_index(f->tickets, gint64, 2), g_array_index(f->tickets, gint64, 3));
	legacy_sql(f, "UPDATE \"%s\" SET \"sla_breached\" = 1 WHERE \"id\" = %" G_GINT64_FORMAT,
	           tickets, g_array_index(f->tickets, gint64, 4));

	/* Ticket 1 (priority "normal" as written) as a row older than every
	 * one of these columns. */
	legacy_sql(f, "UPDATE \"%s\" SET \"priority\" = NULL, \"story_points\" = NULL, "
	           "\"company_id\" = NULL, \"estimate_hours\" = NULL, \"sla_breached\" = NULL "
	           "WHERE \"id\" = %" G_GINT64_FORMAT, tickets, legacy);

	/* Four deals: USD; EUR; USD whose currency column goes NULL (reads as
	 * the default); EUR whose amount goes NULL (a NULL money). */
	{
		static const gchar * const values[] = { "100 USD", "50 EUR", "70 USD", "20 EUR" };

		for (i = 0; i < G_N_ELEMENTS(values); i++)
		{
			g_autoptr(VentureEntity) deal = NULL;
			g_autoptr(GError) error = NULL;
			g_autofree gchar *name = g_strdup_printf("Legacy %u", i);

			deal = g_object_new(VENTURE_TYPE_DEAL, "organization-id", f->org, "name", name, NULL);
			g_assert_true(venture_entity_set_field_from_string(deal, "value", values[i], &error));
			g_assert_true(venture_database_save(f->db, deal, NULL, &error));
			g_assert_no_error(error);
			deal_ids[i] = venture_entity_get_id(deal);
		}
	}
	legacy_sql(f, "UPDATE \"%s\" SET \"value_currency\" = NULL WHERE \"id\" = %" G_GINT64_FORMAT,
	           deals, deal_ids[2]);
	legacy_sql(f, "UPDATE \"%s\" SET \"value_amount\" = NULL WHERE \"id\" = %" G_GINT64_FORMAT,
	           deals, deal_ids[3]);

	environment = environment_for(f, &f->admin);

	/* The legacy row reads the values the cases rely on. */
	{
		g_autofree gchar *text = g_strdup_printf("fetch ticket | filter id == %" G_GINT64_FORMAT, legacy);
		g_autoptr(LqlTable) row = run(environment, text);
		LqlRecord *record;

		g_assert_cmpuint(lql_table_get_n_rows(row), ==, 1);
		record = lql_table_get_row(row, 0);
		g_assert_cmpstr(lql_value_get_string(lql_record_get(record, "priority")), ==, "low");
		g_assert_cmpint(lql_value_get_int(lql_record_get(record, "story_points")), ==, 0);
		g_assert_cmpint(lql_value_get_int(lql_record_get(record, "company_id")), ==, 0);
		g_assert_false(lql_value_get_boolean(lql_record_get(record, "sla_breached")));
	}

	legacy_check(environment, "ticket", ticket_cases, G_N_ELEMENTS(ticket_cases));

	/* Not vacuous: the NULL row is in `priority == "low"` (four written,
	 * plus it), and is_null finds nothing. */
	{
		g_autoptr(LqlTable) low = run(environment, "fetch ticket | filter priority == \"low\"");
		g_autoptr(LqlTable) none = run(environment, "fetch ticket | filter is_null(priority)");

		g_assert_cmpuint(lql_table_get_n_rows(low), ==, 5);
		g_assert_cmpuint(lql_table_get_n_rows(none), ==, 0);
	}

	/* Money. */
	{
		static const PushdownCase deal_cases[] = {
			{ "value.currency == \"EUR\"", "E" },
			{ "is_null(value)", "E" },
			{ "is_not_null(value)", "E" },
			{ "value.amount > 0", "E" },
		};
		g_autofree gchar *book_text = g_strdup_printf("fetch deal | filter %s", book_case);
		g_autoptr(LqlTable) book_rows = NULL;
		g_autoptr(LqlTable) euros = NULL;
		guint in_book = 1;	/* the deal with no currency column */
		PushdownCase book_check;

		legacy_check(environment, "deal", deal_cases, G_N_ELEMENTS(deal_cases));
		book_check.filter = book_case;
		book_check.taken = "E";
		legacy_check(environment, "deal", &book_check, 1);

		/* The deal with no currency column is in the default currency's
		 * rows; the one with no amount is in no currency's. */
		in_book += (0 == g_strcmp0(book, "USD")) ? 1 : 0;
		in_book += (0 == g_strcmp0(book, "EUR")) ? 1 : 0;
		book_rows = run(environment, book_text);
		g_assert_cmpuint(lql_table_get_n_rows(book_rows), ==, in_book);
		euros = run(environment, "fetch deal | filter value.currency == \"EUR\"");
		g_assert_cmpuint(lql_table_get_n_rows(euros), ==,
		                 1 + ((0 == g_strcmp0(book, "EUR")) ? 1 : 0));
	}

	/* The list API's ?priority=low still compares the stored column
	 * (docs/lql.org): four rows, not the five the records read. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TICKET);
		g_autoptr(GHashTable) params = g_hash_table_new(g_str_hash, g_str_equal);
		g_autoptr(GError) error = NULL;

		g_hash_table_insert(params, (gpointer)"priority", (gpointer)"low");
		g_assert_true(venture_query_apply_query_string(query, params, &error));
		g_assert_no_error(error);
		venture_query_set_organization(query, f->org);
		g_assert_cmpint(venture_database_count(f->db, query, NULL), ==, 4);
	}
}

/* --- Schema --------------------------------------------------------------- */

/*
 * No column of any source is a sensitive property, and the test is not
 * vacuous: some types do have sensitive properties.
 */
static void
test_no_sensitive_fields(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(LqlEnvironment) environment = environment_for(f, &f->admin);
	g_auto(GStrv) sources = lql_environment_list_sources(environment);
	guint sensitive = 0;
	guint i;

	(void)data;

	for (i = 0; NULL != sources[i]; i++)
	{
		LqlSource *source = lql_environment_lookup_source(environment, sources[i]);
		GType entity_type = venture_lql_source_get_entity_type(VENTURE_LQL_SOURCE(source));
		g_autoptr(VentureEntityClass) klass = g_type_class_ref(entity_type);
		g_autoptr(GPtrArray) columns = lql_source_get_columns(source);
		g_autofree GParamSpec **properties = NULL;
		guint n_properties;
		guint j;

		properties = venture_entity_class_list_persistent_properties(klass, &n_properties);

		for (j = 0; j < n_properties; j++)
		{
			g_autofree gchar *column = NULL;
			gboolean listed = FALSE;
			guint k;

			if (0 == (venture_entity_class_get_column_flags(klass, properties[j]->name) &
			          VENTURE_COLUMN_FLAG_SENSITIVE))
				continue;

			sensitive++;
			column = venture_entity_property_to_column(properties[j]->name);

			for (k = 0; k < columns->len; k++)
				listed = listed || (0 == g_strcmp0(lql_column_get_name(g_ptr_array_index(columns, k)), column));

			if (listed)
				g_error("%s.%s is sensitive but is a column", sources[i], column);
		}
	}

	g_assert_cmpuint(sensitive, >, 0);
}

/*
 * Every reference has a <ref>_label holding what it points at by name,
 * and soft-deleted rows appear only when asked for.
 */
static void
test_labels_and_deleted(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(LqlEnvironment) environment = environment_for(f, &f->admin);
	g_autoptr(LqlTable) acme = NULL;
	g_autoptr(LqlTable) live = NULL;
	g_autoptr(LqlTable) all = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *where = NULL;
	g_autofree gchar *with_deleted = NULL;

	(void)data;

	where = g_strdup_printf("fetch ticket | filter company_id == %" G_GINT64_FORMAT
	                        " | fields id, company_id, company_label", f->acme_id);
	acme = run(environment, where);
	g_assert_cmpuint(lql_table_get_n_rows(acme), ==, f->tickets->len / 2);
	g_assert_cmpstr(lql_value_get_string(lql_record_get(lql_table_get_row(acme, 0), "company_label")),
	                ==, "Acme");

	live = run(environment, "fetch ticket | filter title == \"Gone\"");
	g_assert_cmpuint(lql_table_get_n_rows(live), ==, 0);

	with_deleted = g_strdup_printf("fetch ticket, include_deleted: TRUE | filter id == %" G_GINT64_FORMAT,
	                               f->deleted_ticket);
	all = run(environment, with_deleted);
	g_assert_cmpuint(lql_table_get_n_rows(all), ==, 1);
	g_assert_false(lql_value_is_null(lql_record_get(lql_table_get_row(all, 0), "deleted_at")));

	/* Anything else is refused, not ignored. */
	g_assert_null(lql_environment_run_sync(environment, "fetch ticket, deleted: TRUE", NULL, NULL, &error));
	g_assert_error(error, LQL_ERROR, LQL_ERROR_ARGUMENT);
}

/* --- Module gates ---------------------------------------------------------- */

/*
 * A type whose module is off is not a source, whatever the gate says.
 */
static void
test_module_off(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(LqlEnvironment) environment = environment_for(f, &f->admin);
	g_autoptr(GError) error = NULL;

	(void)data;

	g_assert_null(lql_environment_lookup_source(environment, "goal"));
	g_assert_nonnull(lql_environment_lookup_source(environment, "ticket"));
	g_assert_null(lql_environment_run_sync(environment, "fetch goal", NULL, NULL, &error));
	g_assert_error(error, LQL_ERROR, LQL_ERROR_UNKNOWN_COMMAND);
}

/* --- The row bound --------------------------------------------------------- */

/*
 * query.max_rows refuses a fetch that would read more and says which
 * bound; a limit, or a filter the database applies, keeps under it; a
 * filter LQL alone evaluates does not.
 */
static void
test_row_bound(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(LqlEnvironment) environment = NULL;
	g_autoptr(LqlTable) limited = NULL;
	g_autoptr(LqlTable) narrowed = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *narrow = NULL;
	g_autofree gchar *late = NULL;

	(void)data;

	g_object_set(f->config, "query-max-rows", (gint64)5, NULL);
	environment = environment_for(f, &f->admin);

	g_assert_null(lql_environment_run_sync(environment, "fetch ticket", NULL, NULL, &error));
	g_assert_error(error, LQL_ERROR, LQL_ERROR_LIMIT);
	g_assert_nonnull(strstr(error->message, "query.max_rows"));
	g_clear_error(&error);

	limited = run(environment, "fetch ticket | limit 3");
	g_assert_cmpuint(lql_table_get_n_rows(limited), ==, 3);

	narrow = g_strdup_printf("fetch ticket | filter id <= %" G_GINT64_FORMAT,
	                         g_array_index(f->tickets, gint64, 3));
	narrowed = run(environment, narrow);
	g_assert_cmpuint(lql_table_get_n_rows(narrowed), ==, 4);

	late = g_strdup_printf("fetch ticket | sort id | filter id <= %" G_GINT64_FORMAT,
	                       g_array_index(f->tickets, gint64, 3));
	g_assert_null(lql_environment_run_sync(environment, late, NULL, NULL, &error));
	g_assert_error(error, LQL_ERROR, LQL_ERROR_LIMIT);
	g_clear_error(&error);

	/* The bound is the environment's too, and the timeout is set. */
	g_assert_cmpint(lql_environment_get_limit(environment, LQL_LIMIT_MAX_ROWS), ==, 5);
	g_assert_cmpint(lql_environment_get_limit(environment, LQL_LIMIT_TIMEOUT_MS), ==, 5000);
}

/* Neither bound can be switched off or set absurdly. */
static void
test_bound_validation(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_config_validate(config, &error));
	g_assert_no_error(error);

	g_object_set(config, "query-max-rows", (gint64)0, NULL);
	g_assert_false(venture_config_validate(config, &error));
	g_assert_nonnull(strstr(error->message, "query.max_rows"));
	g_clear_error(&error);

	g_object_set(config, "query-max-rows", (gint64)10000, "query-timeout-ms", (gint64)0, NULL);
	g_assert_false(venture_config_validate(config, &error));
	g_assert_nonnull(strstr(error->message, "query.timeout_ms"));
}

/* --- Functions -------------------------------------------------------------- */

/* The single value of `data {} | fields v = EXPR`, as LQL source text. */
static gchar *
evaluate(
	LqlEnvironment	*environment,
	const gchar	*expression
){
	g_autofree gchar *text = g_strdup_printf("data {} | fields v = %s", expression);
	g_autoptr(LqlTable) table = run(environment, text);

	g_assert_cmpuint(lql_table_get_n_rows(table), ==, 1);

	return lql_value_to_literal(lql_record_get(lql_table_get_row(table, 0), "v"));
}

/*
 * me() is the username, my_teams() the caller's active teams,
 * organization() the selected business, today() and period() the
 * business calendar (America/New_York) at the query's now().
 */
static void
test_functions(
	Fixture		*f,
	gconstpointer	 data
){
	VentureAuthPrincipal viewer;
	g_autoptr(LqlEnvironment) admin = environment_for(f, &f->admin);
	g_autoptr(LqlEnvironment) environment = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *me = NULL;
	g_autofree gchar *teams = NULL;
	g_autofree gchar *expected_teams = NULL;
	g_autofree gchar *organization = NULL;
	g_autofree gchar *expected_organization = NULL;
	g_autofree gchar *today = NULL;
	g_autofree gchar *start = NULL;
	g_autofree gchar *end = NULL;
	g_autofree gchar *last = NULL;
	g_autofree gchar *admin_teams = NULL;

	(void)data;

	memset(&viewer, 0, sizeof viewer);
	viewer.user_id = f->viewer_id;
	viewer.role = VENTURE_USER_ROLE_VIEWER;
	viewer.authenticated = TRUE;
	environment = environment_for(f, &viewer);

	me = evaluate(environment, "me()");
	g_assert_cmpstr(me, ==, "\"vee\"");

	teams = evaluate(environment, "my_teams()");
	expected_teams = g_strdup_printf("[%" G_GINT64_FORMAT "]", f->team_id);
	g_assert_cmpstr(teams, ==, expected_teams);
	admin_teams = evaluate(admin, "my_teams()");
	g_assert_cmpstr(admin_teams, ==, "[]");

	organization = evaluate(environment, "organization()");
	expected_organization = g_strdup_printf("%" G_GINT64_FORMAT, f->org);
	g_assert_cmpstr(organization, ==, expected_organization);

	/* 2026-11-15T12:00Z is the 15th in New York too. */
	today = evaluate(environment, "format_timestamp(today())");
	g_assert_cmpstr(today, ==, "\"2026-11-15T00:00:00Z\"");
	start = evaluate(environment, "format_timestamp(period(\"this_month\").start)");
	g_assert_cmpstr(start, ==, "\"2026-11-01T00:00:00Z\"");
	end = evaluate(environment, "format_timestamp(period(\"this_month\").end)");
	g_assert_cmpstr(end, ==, "\"2026-12-01T00:00:00Z\"");
	last = evaluate(environment, "period(\"last_month\").label");
	g_assert_nonnull(last);

	/* An unknown period is the caller's mistake, said as such. */
	g_assert_null(lql_environment_run_sync(environment, "data {} | fields v = period(\"someday\")",
	                                       NULL, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "someday"));
	g_clear_error(&error);

	/* `team_id in my_teams()` and `assignee == me()` reach the database,
	 * and find the viewer's team ticket and their three. */
	{
		g_autofree gchar *taken = pushdowns(environment, "fetch ticket | filter team_id in my_teams()");
		g_autofree gchar *mine_taken = pushdowns(environment, "fetch ticket | filter assignee == me()");
		g_autoptr(LqlTable) team = run(environment, "fetch ticket | filter team_id in my_teams()");
		g_autoptr(LqlTable) mine = run(environment, "fetch ticket | filter assignee == me()");

		g_assert_cmpstr(taken, ==, "E");
		g_assert_cmpstr(mine_taken, ==, "E");
		g_assert_cmpuint(lql_table_get_n_rows(team), ==, 1);
		g_assert_cmpuint(lql_table_get_n_rows(mine), ==, 3);
	}

	/* The plan says what reaches the database, and as whom. */
	{
		g_autoptr(LqlQuery) query = lql_environment_prepare(environment,
			"fetch ticket | filter status == \"done\"", NULL, &error);
		g_autofree gchar *plan = NULL;

		g_assert_no_error(error);
		plan = lql_query_explain(query);
		g_test_message("%s", plan);
		g_assert_nonnull(strstr(plan, "venture ticket"));
		g_assert_nonnull(strstr(plan, "read as the caller"));
	}
}

/*
 * The queries docs/lql.org shows run as written, and the epic's own
 * example ("not done, assigned to me", grouped) pushes both conjuncts.
 */
static void
test_documented_queries(
	Fixture		*f,
	gconstpointer	 data
){
	static const gchar * const queries[] = {
		"fetch ticket | filter status not in [\"done\", \"cancelled\"] and assignee == me() "
			"| summarize n = count(), by: priority",
		"fetch ticket\n"
			"| filter updated_at >= period(\"this_week\").start and updated_at < period(\"this_week\").end\n"
			"| summarize n = count(), by: status",
		"fetch deal | filter value.currency == \"USD\" and value.amount > 10000",
		"fetch ticket | filter team_id in my_teams()",
		"fetch ticket, include_deleted: TRUE | limit 5",
	};
	g_autoptr(LqlEnvironment) environment = environment_for(f, &f->admin);
	g_autofree gchar *taken = NULL;
	guint i;

	(void)data;

	for (i = 0; i < G_N_ELEMENTS(queries); i++)
	{
		g_autoptr(LqlTable) table = run(environment, queries[i]);

		g_assert_nonnull(table);
	}

	taken = pushdowns(environment, queries[0]);
	g_assert_cmpstr(taken, ==, "EE");
}

/* --- A viewer through the web door ------------------------------------------ */

typedef struct
{
	GBytes		*bytes;
	GError		*error;
	gboolean	 done;
} HttpResult;

static void
http_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 data
){
	HttpResult *r = data;

	r->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &r->error);
	r->done = TRUE;
}

/* GET @path with @secret as the bearer; the parsed JSON body. */
static JsonNode *
http_get_json(
	VentureWebServer	*server,
	const gchar		*secret,
	const gchar		*path,
	guint			*out_status
){
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 30, NULL);
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(server), path, NULL);
	g_autofree gchar *bearer = g_strconcat("Bearer ", secret, NULL);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	g_autofree gchar *body = NULL;
	HttpResult result = { NULL, NULL, FALSE };
	gsize length;

	soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", bearer);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &result);

	while (!result.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(result.error);
	*out_status = soup_message_get_status(message);
	body = g_strndup(g_bytes_get_data(result.bytes, &length), g_bytes_get_size(result.bytes));
	g_bytes_unref(result.bytes);

	return venture_json_parse(body, NULL);
}

/*
 * A test route standing in for the query surfaces of later stories: it
 * builds the environment through venture_web_server_new_lql_environment(),
 * runs ?q=, and answers the rows' ids, the columns and the company labels.
 */
static HtmxResponse *
lql_route(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *server = user_data;
	g_autoptr(LqlEnvironment) environment = NULL;
	g_autoptr(LqlTable) table = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *body = NULL;
	HtmxResponse *response;
	guint i;

	(void)params;

	environment = venture_web_server_new_lql_environment(server, request, &error);

	if (NULL != environment)
		table = lql_environment_run_sync(environment, htmx_request_get_query_param(request, "q"),
		                                 NULL, NULL, &error);

	json_builder_begin_object(builder);

	if (NULL == table)
	{
		json_builder_set_member_name(builder, "error");
		json_builder_add_string_value(builder, error->message);
	}
	else
	{
		json_builder_set_member_name(builder, "ids");
		json_builder_begin_array(builder);
		for (i = 0; i < lql_table_get_n_rows(table); i++)
			json_builder_add_int_value(builder,
				lql_value_get_int(lql_record_get(lql_table_get_row(table, i), "id")));
		json_builder_end_array(builder);

		json_builder_set_member_name(builder, "columns");
		json_builder_begin_array(builder);
		for (i = 0; i < lql_table_get_n_columns(table); i++)
			json_builder_add_string_value(builder, lql_column_get_name(lql_table_get_column(table, i)));
		json_builder_end_array(builder);

		json_builder_set_member_name(builder, "labels");
		json_builder_begin_array(builder);
		for (i = 0; i < lql_table_get_n_rows(table); i++)
		{
			LqlValue *label = lql_record_get(lql_table_get_row(table, i), "company_label");

			if (lql_value_is_null(label))
				json_builder_add_null_value(builder);
			else
				json_builder_add_string_value(builder, lql_value_get_string(label));
		}
		json_builder_end_array(builder);
	}

	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	body = json_to_string(root, FALSE);
	response = htmx_response_new_with_content(body);
	htmx_response_set_content_type(response, "application/json; charset=utf-8");
	htmx_response_set_status(response, (NULL == table) ? 400 : 200);

	return response;
}

/* GCompareFunc over gint64. */
static gint
compare_int64(
	gconstpointer	a,
	gconstpointer	b
){
	gint64 x = *(const gint64 *)a;
	gint64 y = *(const gint64 *)b;

	return (x > y) - (x < y);
}

/* The ids of a JSON array of ints, or of the "id" of each object, sorted. */
static gchar *
sorted_ids(JsonArray *array)
{
	g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
	g_autoptr(GString) out = g_string_new(NULL);
	guint i;

	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonNode *element = json_array_get_element(array, i);
		gint64 id = JSON_NODE_HOLDS_OBJECT(element)
			? json_object_get_int_member(json_node_get_object(element), "id")
			: json_node_get_int(element);

		g_array_append_val(ids, id);
	}

	g_array_sort(ids, compare_int64);

	for (i = 0; i < ids->len; i++)
		g_string_append_printf(out, "%s%" G_GINT64_FORMAT, (i > 0) ? "," : "",
		                       g_array_index(ids, gint64, i));

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * A viewer's `fetch ticket` is GET /api/v1/ticket for the same token:
 * the same rows (assigned to them, on their team, theirs), the same
 * fields plus the labels; the same for companies (none); a type the list
 * API refuses a viewer (user) is no source; and a label does not name a
 * company the viewer cannot read.
 */
static void
test_viewer_matches_list_api(
	Fixture		*f,
	gconstpointer	 data
){
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	g_autoptr(JsonNode) listed = NULL;
	g_autoptr(JsonNode) queried = NULL;
	g_autoptr(JsonNode) companies = NULL;
	g_autoptr(JsonNode) queried_companies = NULL;
	g_autoptr(JsonNode) users = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *listed_ids = NULL;
	g_autofree gchar *queried_ids = NULL;
	JsonArray *records;
	JsonArray *columns;
	JsonArray *labels;
	guint status = 0;
	guint i;

	(void)data;

	g_object_set(token, "name", "vee", "user-id", f->viewer_id, "role", VENTURE_USER_ROLE_VIEWER, NULL);
	secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(token), NULL, NULL));

	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	venture_web_server_add_classified_route(server, HTMX_METHOD_GET, "/test/lql",
		VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, lql_route, server);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);

	/* Tickets: the list and the query, row for row. */
	listed = http_get_json(server, secret, "/api/v1/ticket", &status);
	g_assert_cmpuint(status, ==, 200);
	queried = http_get_json(server, secret, "/test/lql?q=fetch%20ticket", &status);
	g_assert_cmpuint(status, ==, 200);

	records = json_object_get_array_member(json_node_get_object(listed), "records");
	listed_ids = sorted_ids(records);
	queried_ids = sorted_ids(json_object_get_array_member(json_node_get_object(queried), "ids"));
	g_test_message("list API: %s; fetch ticket: %s", listed_ids, queried_ids);
	g_assert_cmpstr(listed_ids, ==, queried_ids);

	/* 0, 4 and 8 are assigned to vee, 5 is on vee's team, 6 is vee's. */
	g_assert_cmpuint(json_array_get_length(records), ==, 5);

	/* Every field the API sent is a column, and the only columns it did
	 * not send are the labels and an empty attributes record. */
	columns = json_object_get_array_member(json_node_get_object(queried), "columns");

	for (i = 0; i < json_array_get_length(records); i++)
	{
		JsonObject *record = json_array_get_object_element(records, i);
		g_autoptr(GList) members = json_object_get_members(record);
		GList *iter;

		for (iter = members; NULL != iter; iter = iter->next)
		{
			gboolean found = FALSE;
			guint j;

			if (0 == g_strcmp0(iter->data, "type"))
				continue;

			for (j = 0; j < json_array_get_length(columns); j++)
				found = found || (0 == g_strcmp0(json_array_get_string_element(columns, j), iter->data));

			if (!found)
				g_error("the list API sends %s, which fetch ticket lacks", (const gchar *)iter->data);
		}
	}

	for (i = 0; i < json_array_get_length(columns); i++)
	{
		const gchar *column = json_array_get_string_element(columns, i);

		if (g_str_has_suffix(column, "_label") || (0 == g_strcmp0(column, "attributes")))
			continue;

		if (!json_object_has_member(json_array_get_object_element(records, 0), column))
			g_error("fetch ticket has %s, which the list API does not send", column);
	}

	/* The viewer reads no company, so no ticket names one. */
	labels = json_object_get_array_member(json_node_get_object(queried), "labels");

	for (i = 0; i < json_array_get_length(labels); i++)
		g_assert_true(JSON_NODE_HOLDS_NULL(json_array_get_element(labels, i)));

	/* Companies: none listed, none fetched. */
	companies = http_get_json(server, secret, "/api/v1/company", &status);
	g_assert_cmpuint(status, ==, 200);
	queried_companies = http_get_json(server, secret, "/test/lql?q=fetch%20company", &status);
	g_assert_cmpuint(status, ==, 200);
	{
		g_autofree gchar *a = sorted_ids(json_object_get_array_member(json_node_get_object(companies), "records"));
		g_autofree gchar *b = sorted_ids(json_object_get_array_member(json_node_get_object(queried_companies), "ids"));

		g_assert_cmpstr(a, ==, b);
	}

	/* Users: the list API refuses a viewer, and there is no source. */
	{
		g_autoptr(JsonNode) refused = http_get_json(server, secret, "/api/v1/user", &status);

		g_assert_cmpuint(status, ==, 403);
	}
	users = http_get_json(server, secret, "/test/lql?q=fetch%20user", &status);
	g_assert_cmpuint(status, ==, 400);
	g_assert_nonnull(strstr(json_object_get_string_member(json_node_get_object(users), "error"),
	                        "no source named 'user'"));

	venture_web_server_stop(server);
}

/* --- main -------------------------------------------------------------------- */

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);
	g_setenv("VENTURE_TEST_SESSION_SECRET", "lql-test-secret", TRUE);

#define ADD(path, func, data) \
	g_test_add(path, Fixture, data, fixture_setup, func, fixture_teardown)

	ADD("/lql/environment/no-file-exec-sql-sources", test_no_dangerous_sources, NULL);
	ADD("/lql/pushdown/same-rows-as-lql-alone", test_pushdown_same_rows, NULL);
	ADD("/lql/nulls/three-valued", test_nulls, NULL);
	ADD("/lql/money/record-with-int-amount", test_money, NULL);
	ADD("/lql/pushdown/legacy-null-reads-as-default", test_legacy_null_columns, NULL);
	ADD("/lql/schema/no-sensitive-fields", test_no_sensitive_fields, NULL);
	ADD("/lql/schema/labels-and-deleted", test_labels_and_deleted, NULL);
	ADD("/lql/access/module-off-is-no-source", test_module_off, "goals");
	ADD("/lql/bounds/max-rows-refuses", test_row_bound, NULL);
	ADD("/lql/functions/me-teams-organization-periods", test_functions, NULL);
	ADD("/lql/docs/examples-run", test_documented_queries, NULL);
	ADD("/lql/access/viewer-matches-list-api", test_viewer_matches_list_api, NULL);
	g_test_add_func("/lql/bounds/config-validation", test_bound_validation);

#undef ADD

	return g_test_run();
}
