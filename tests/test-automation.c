/*
 * test-automation.c - The automation engine's dispatch contract
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * These tests exist because of where podomation's dispatch can put a
 * handler rather than because of what a handler computes. The engine picks
 * between a direct call and an async wrapper based on a timeout it holds in
 * its own configuration, and the async wrapper runs a synchronous handler on
 * a worker thread while the caller blocks in a nested main loop. VENTURE's
 * pod module writes to the database and reads a cascade guard, so which of
 * those two paths it lands on is not a detail -- it decides whether the
 * whole automation subsystem is thread-safe.
 */

#include <venture.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <string.h>

#include "venture-test-util.h"

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
	gchar		*state_dir;
} Fixture;

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	(void)user_data;

	fixture->state_dir = g_dir_make_tmp("venture-automation-XXXXXX", &error);
	g_assert_no_error(error);

	fixture->config = venture_config_new();
	g_object_set(fixture->config, "state-dir", fixture->state_dir, NULL);
	g_object_set(fixture->config, "automation-enabled", TRUE, NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config, fixture->database);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	/*
	 * g_file_delete() on a directory fails exactly the way g_rmdir()
	 * does when it is not empty, and its GError was discarded -- so the
	 * pod state the engine writes under here kept the directory alive
	 * and nothing said so.
	 */
	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}
}

typedef struct
{
	GThread	*handler_thread;
	guint	 saves;
} ThreadWitness;

static void
on_entity_saved(
	VentureDatabase	*database,
	VentureEntity	*entity,
	gboolean	 created,
	gpointer	 user_data
){
	ThreadWitness *witness = user_data;

	(void)database;
	(void)created;

	/* Only the record the rule creates, not the one that triggered it. */
	if (!VENTURE_IS_IDEA(entity))
		return;

	witness->handler_thread = g_thread_self();
	witness->saves++;
}

/*
 * A pod handler must run on the thread that dispatched it.
 *
 * What breaks if this regresses: VentureAutomation calls pod_engine_new()
 * and podomation's default handler_timeout_seconds is 30. Any non-zero
 * timeout selects the async dispatch path, which fires handle_event_async
 * and blocks the caller in g_main_loop_run() on the default context.
 * VenturePodModule implements only the synchronous handle_event, so
 * podomation's default wrapper runs it via g_task_run_in_thread -- and that
 * handler calls venture_database_save() and reads the `dispatching` cascade
 * guard, a plain guint. The symptoms are not a crash: they are automations
 * that silently stop firing for some records because a non-atomic counter
 * was mutated from two threads, and HTTP requests dispatched re-entrantly
 * from inside the nested loop.
 *
 * The fix is one line in venture_automation_build_engine() setting the
 * timeout to zero. This test is what stops it being tidied away again.
 */
static void
test_automation_handlers_run_on_the_calling_thread(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(VentureTicket) ticket = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *pods_path = NULL;
	ThreadWitness witness = { NULL, 0 };
	static const gchar *const rules =
		"pod watcher = venture->new();\n"
		"watcher->on_created => venture->create(\"idea\","
		" \"title=raised by automation\");\n";

	(void)user_data;

	pods_path = g_build_filename(fixture->state_dir, "automations.pod", NULL);
	g_assert_true(g_file_set_contents(pods_path, rules, -1, &error));
	g_assert_no_error(error);

	automation = venture_automation_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_nonnull(automation);

	g_assert_true(venture_automation_start(automation, &error));
	g_assert_no_error(error);

	g_signal_connect(fixture->database, "entity-saved",
	                 G_CALLBACK(on_entity_saved), &witness);

	ticket = venture_ticket_new();
	g_object_set(ticket, "title", "trigger", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(ticket), NULL, &error));
	g_assert_no_error(error);

	/* The rule ran at all... */
	g_assert_cmpuint(witness.saves, ==, 1);

	/* ...and it ran here, not on a worker thread podomation borrowed. */
	g_assert_true(witness.handler_thread == g_thread_self());

	venture_automation_stop(automation);
}

/*
 * A record written by an automation must not trigger the automation again.
 *
 * What breaks if this regresses: a rule bound to on_created that creates a
 * record is an unbounded loop with a disk-full at the end of it. The guard
 * is a depth counter in venture_automation_emit_entity_event(), and it is
 * only sound while handlers stay on one thread -- which is what the test
 * above pins. This one pins the behaviour that depends on it.
 */
static void
test_automation_does_not_cascade(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(VentureTicket) ticket = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *pods_path = NULL;
	static const gchar *const rules =
		"pod watcher = venture->new();\n"
		"watcher->on_created => venture->create(\"idea\","
		" \"title=raised by automation\");\n";

	(void)user_data;

	pods_path = g_build_filename(fixture->state_dir, "automations.pod", NULL);
	g_assert_true(g_file_set_contents(pods_path, rules, -1, &error));
	g_assert_no_error(error);

	automation = venture_automation_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_automation_start(automation, &error));
	g_assert_no_error(error);

	ticket = venture_ticket_new();
	g_object_set(ticket, "title", "trigger", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(ticket), NULL, &error));
	g_assert_no_error(error);

	/* Exactly one, not one per generation. */
	query = venture_query_new(VENTURE_TYPE_IDEA);
	g_assert_cmpint(venture_database_count(fixture->database, query, NULL),
	                ==, 1);

	venture_automation_stop(automation);
}

/* Sets one field from its text form, the way a form or the CLI would. */
static void
dunning_field(
	VentureEntity	*row,
	const gchar	*name,
	const gchar	*value
){
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_entity_set_field_from_string(row, name, value, &error));
	g_assert_no_error(error);
}

static gint64
dunning_save(
	Fixture		*fixture,
	VentureEntity	*row
){
	g_autoptr(GError) error = NULL;

	g_assert_true(venture_database_save(fixture->database, row, NULL, &error));
	g_assert_no_error(error);

	return venture_entity_get_id(row);
}

/*
 * venture->dunning_sweep() runs the overdue-reminder sweep on a schedule.
 *
 * What breaks if this regresses: docs/dunning.org told operators to schedule
 * venture->act("dunning_policy", "0", "sweep"), and the venture module has
 * no act handler, so that rule failed on every tick and automation never
 * sent a reminder. The handler takes as_of, organization and limit, like
 * the recurring sweeps, and a second tick on the same day sends nothing.
 */
static void
test_automation_dunning_sweep(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(VentureLogMailer) mailer = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(VentureEntity) template = NULL;
	g_autoptr(VentureEntity) policy = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureQuery) events = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GDateTime) issued = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *org_text = NULL;
	g_autofree gchar *steps = NULL;
	const gchar *arguments[4];
	const gchar *malformed[4];
	gint64 org;
	gint64 count;

	(void)user_data;

	org = venture_context_get_default_organization_id(fixture->context);
	org_text = g_strdup_printf("%" G_GINT64_FORMAT, org);
	mailer = venture_log_mailer_new();
	g_object_set(venture_database_get_mail_outbox(fixture->database),
	             "mailer", mailer, NULL);

	company = VENTURE_ENTITY(venture_company_new());
	g_object_set(company, "organization-id", org, "name", "Overdue Ltd",
	             "email", "accounts@example.test", NULL);
	dunning_save(fixture, company);

	template = VENTURE_ENTITY(venture_mail_template_new());
	g_object_set(template, "organization-id", org, "name", "Reminder",
	             "subject", "Invoice {number}", "text-body", "{open_balance}",
	             NULL);
	dunning_save(fixture, template);

	policy = VENTURE_ENTITY(venture_dunning_policy_new());
	steps = g_strdup_printf("[{\"offset\":-3,\"template_id\":%" G_GINT64_FORMAT "}]",
	                        venture_entity_get_id(template));
	g_object_set(policy, "organization-id", org, "name", "Standard",
	             "steps", steps, "is-default", TRUE, NULL);
	dunning_save(fixture, policy);

	invoice = VENTURE_ENTITY(venture_invoice_new());
	g_object_set(invoice, "organization-id", org, "number", "INV-AUTO",
	             "company-id", venture_entity_get_id(company), NULL);
	dunning_field(invoice, "issued-at", "2026-01-01");
	dunning_field(invoice, "due-at", "2026-01-10");
	dunning_save(fixture, invoice);

	line = VENTURE_ENTITY(venture_invoice_line_new());
	g_object_set(line, "organization-id", org,
	             "invoice-id", venture_entity_get_id(invoice),
	             "description", "Work", "quantity", 1.0, NULL);
	dunning_field(line, "unit-price", "40 USD");
	dunning_save(fixture, line);

	issued = venture_time_from_string("2026-01-01", NULL);
	g_assert_true(venture_settlement_service_transition(
		venture_settlement_service_get(fixture->database),
		VENTURE_INVOICE(invoice), "sent", issued, NULL, &error));
	g_assert_no_error(error);

	automation = venture_automation_new(fixture->context, &error);
	g_assert_no_error(error);

	arguments[0] = "2026-01-07";
	arguments[1] = org_text;
	arguments[2] = "10";
	arguments[3] = NULL;

	g_assert_true(venture_automation_invoke(automation, "dunning_sweep",
	                                        arguments, &result, &error));
	g_assert_no_error(error);
	g_assert_true(g_variant_lookup(result, "count", "x", &count));
	g_assert_cmpint(count, ==, 1);

	events = venture_query_new(VENTURE_TYPE_DUNNING_EVENT);
	venture_query_set_organization(events, org);
	g_assert_cmpint(venture_database_count(fixture->database, events, NULL),
	                ==, 1);

	/* The same day again is idempotent. */
	g_clear_pointer(&result, g_variant_unref);
	g_assert_true(venture_automation_invoke(automation, "dunning_sweep",
	                                        arguments, &result, &error));
	g_assert_no_error(error);
	g_assert_true(g_variant_lookup(result, "count", "x", &count));
	g_assert_cmpint(count, ==, 0);

	/* A limit outside 1..1000 is refused, not clamped into something else. */
	malformed[0] = "";
	malformed[1] = org_text;
	malformed[2] = "0";
	malformed[3] = NULL;
	g_assert_false(venture_automation_invoke(automation, "dunning_sweep",
	                                         malformed, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION);
}

/*
 * venture->mail_sync("1", "N") sweeps an organization's inbound accounts.
 *
 * What breaks if this regresses: docs/mail.org tells operators to schedule
 * inbound sync with this rule. Without the handler podomation logs "venture
 * has no handler" every run and no mailbox is ever read; with a handler that
 * treats one failing account as a failed rule, a single bad password makes
 * the whole schedule look broken. A malformed budget must refuse rather than
 * silently sweep with no bound.
 */
static void
test_automation_mail_sync_handler(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *org = NULL;
	gint accounts = -1, messages = -1, errors = -1;
	const gchar *args[] = { NULL, "50", NULL };
	const gchar *bad[] = { NULL, "0", NULL };

	(void)user_data;

	org = g_strdup_printf("%" G_GINT64_FORMAT,
		venture_context_get_default_organization_id(fixture->context));
	args[0] = org;
	bad[0] = org;

	/* An account whose secret is not in the environment: the sweep
	 * reaches it, records the failure on it and still succeeds. */
	g_unsetenv("VENTURE_IMAP_AUTOMATION_MISSING");
	account = g_object_new(VENTURE_TYPE_MAIL_ACCOUNT,
	                       "organization-id",
	                       venture_context_get_default_organization_id(fixture->context),
	                       "address", "ops@example.test",
	                       "imap-host", "imap.example.test",
	                       "secret-env", "VENTURE_IMAP_AUTOMATION_MISSING",
	                       "active", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database, account, NULL,
	                                    &error));
	g_assert_no_error(error);

	automation = venture_automation_new(fixture->context, &error);
	g_assert_no_error(error);

	g_assert_true(venture_automation_invoke(automation, "mail_sync", args,
	                                        &result, &error));
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_true(g_variant_lookup(result, "accounts", "i", &accounts));
	g_assert_true(g_variant_lookup(result, "messages", "i", &messages));
	g_assert_true(g_variant_lookup(result, "errors", "i", &errors));
	g_assert_cmpint(accounts, ==, 1);
	g_assert_cmpint(messages, ==, 0);
	g_assert_cmpint(errors, ==, 1);

	g_clear_pointer(&result, g_variant_unref);
	g_assert_false(venture_automation_invoke(automation, "mail_sync", bad,
	                                         &result, NULL));
}

/*
 * Every rule file shipped in data/examples/ parses.
 *
 * These are documentation people copy into a live rules file, and the
 * Automations page validates with this same parser -- so a shipped example
 * that does not parse is a bug report waiting to be filed by whoever
 * followed it. The directory is walked rather than listed, so a new example
 * is covered the moment it is added.
 */
static void
test_automation_shipped_examples_parse(void)
{
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *name;
	guint checked;

	dir = g_dir_open(VENTURE_TEST_EXAMPLES, 0, &error);

	g_assert_no_error(error);
	g_assert_nonnull(dir);

	checked = 0;

	while (NULL != (name = g_dir_read_name(dir)))
	{
		g_autofree gchar *path = NULL;
		g_autofree gchar *source = NULL;
		g_autofree gchar *message = NULL;

		if (!g_str_has_suffix(name, ".pod"))
			continue;

		path = g_build_filename(VENTURE_TEST_EXAMPLES, name, NULL);

		g_assert_true(g_file_get_contents(path, &source, NULL, &error));
		g_assert_no_error(error);

		if (!venture_automation_validate_dsl(source, &message))
		{
			g_error("%s does not parse: %s", name,
			        (NULL != message) ? message : "no reason given");
		}

		checked++;
	}

	/* A walk that found nothing passes silently, and would keep passing
	 * if the directory moved. */
	g_assert_cmpuint(checked, >, 0);
}

/* ==========================================================================
 * The handler and event registry
 * ========================================================================== */

typedef struct
{
	guint		 calls;
	gchar		*last_argument;
	gboolean	 emit_from_inside;
} Witness;

static void
witness_clear(Witness *witness)
{
	g_clear_pointer(&witness->last_argument, g_free);
}

/* A plugin's handler: notes what it was called with, answers like any. */
static gboolean
witness_handler(
	VentureContext	 *context,
	const gchar	 *name,
	GVariant	 *params,
	GVariant	**result,
	gpointer	  user_data,
	GError		**error
){
	Witness *witness = user_data;

	(void)name;
	(void)error;

	witness->calls++;
	g_free(witness->last_argument);
	witness->last_argument = g_strdup(venture_automation_argument(params, 0));

	/* A handler that raises an event: the cascade guard must swallow it. */
	if (witness->emit_from_inside)
	{
		g_autoptr(GError) local_error = NULL;

		g_assert_true(venture_automation_emit(context, "feed_finished",
			g_variant_new_parsed("{'source': <'from-inside'>}"),
			&local_error));
		g_assert_no_error(local_error);
	}

	if (NULL != result)
		*result = venture_automation_result_new(7, "seven", NULL);

	return TRUE;
}

/* Writes @rules as the pods file and starts an engine over them. */
static VentureAutomation *
start_rules(
	Fixture		*fixture,
	const gchar	*rules
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *pods_path = NULL;
	VentureAutomation *automation;

	pods_path = g_build_filename(fixture->state_dir, "automations.pod", NULL);
	g_assert_true(g_file_set_contents(pods_path, rules, -1, &error));
	g_assert_no_error(error);

	automation = venture_automation_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_nonnull(automation);

	/* As main() does, so venture_automation_emit() can find it. */
	venture_context_set_automation(fixture->context, automation);

	g_assert_true(venture_automation_start(automation, &error));
	g_assert_no_error(error);

	return automation;
}

static void
stop_rules(
	Fixture			*fixture,
	VentureAutomation	*automation
){
	venture_automation_stop(automation);
	venture_context_set_automation(fixture->context, NULL);
}

static void
save_ticket(Fixture *fixture)
{
	g_autoptr(VentureTicket) ticket = NULL;
	g_autoptr(GError) error = NULL;

	ticket = venture_ticket_new();
	g_object_set(ticket, "title", "trigger", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(ticket), NULL, &error));
	g_assert_no_error(error);
}

/*
 * A plugin's handler is a step a rule can call, exactly like a built-in.
 *
 * What breaks if this regresses: the handler list was a fixed array and an
 * if-chain inside the venture module, so a plugin could add a record type,
 * a report and a page but never a step an automation could call -- every
 * rule naming one failed with "venture has no handler". The registry lives
 * on the context because plugins load before the engine exists, so this
 * registers first and builds the engine after, as main() does.
 */
static void
test_automation_plugin_handler_runs_from_a_pod(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureAutomationHandlerRegistry *registry;
	Witness witness = { 0, NULL, FALSE };
	const gchar *const arguments[] = { "by hand", NULL };
	gint64 count;

	(void)user_data;

	registry = venture_context_get_automation_handlers(fixture->context);

	/* Built-ins come first and keep their names. */
	g_assert_cmpstr(venture_automation_handler_registry_get_names(registry)[0],
	                ==, "query");
	g_assert_false(venture_automation_handler_registry_add(registry, "count",
		NULL, witness_handler, &witness, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_clear_error(&error);

	/* A name a rule could not spell is refused at the door. */
	g_assert_false(venture_automation_handler_registry_add(registry,
		"Shout-Loud", NULL, witness_handler, &witness, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	g_assert_true(venture_automation_handler_registry_add(registry, "tally",
		"Counts calls", witness_handler, &witness, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(g_strv_contains(
		venture_automation_handler_registry_get_names(registry), "tally"));

	automation = start_rules(fixture,
		"pod watcher = venture->new();\n"
		"watcher->on_created => venture->tally(\"{event->type}\");\n");

	save_ticket(fixture);

	/* The rule reached the plugin's handler, with the event's data. */
	g_assert_cmpuint(witness.calls, ==, 1);
	g_assert_cmpstr(witness.last_argument, ==, "ticket");

	/* And a direct invocation reaches the same handler, with its result. */
	g_assert_true(venture_automation_invoke(automation, "tally", arguments,
	                                        &result, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(witness.calls, ==, 2);
	g_assert_cmpstr(witness.last_argument, ==, "by hand");
	g_assert_true(g_variant_lookup(result, "count", "x", &count));
	g_assert_cmpint(count, ==, 7);

	stop_rules(fixture, automation);
	witness_clear(&witness);
}

/*
 * A name nobody registered is refused, naming what does exist -- a
 * plugin's handler among them.
 *
 * What breaks if this regresses: an unknown name reaching a handler table
 * as a silent no-op is a rule that does nothing forever and logs nothing.
 */
static void
test_automation_unknown_handler_refused(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GError) error = NULL;
	Witness witness = { 0, NULL, FALSE };

	(void)user_data;

	g_assert_true(venture_automation_handler_registry_add(
		venture_context_get_automation_handlers(fixture->context), "tally",
		NULL, witness_handler, &witness, NULL, &error));
	g_assert_no_error(error);

	automation = venture_automation_new(fixture->context, &error);
	g_assert_no_error(error);

	g_assert_false(venture_automation_invoke(automation, "tallly", NULL, NULL,
	                                         &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION);
	g_assert_nonnull(strstr(error->message, "tallly"));
	g_assert_nonnull(strstr(error->message, "tally"));
	g_assert_nonnull(strstr(error->message, "dunning_sweep"));
	g_assert_cmpuint(witness.calls, ==, 0);
}

/*
 * A plugin's own event reaches a pod bound to it, with its data.
 *
 * What breaks if this regresses: the supported events were a fixed list
 * of the three record changes, so "when my feed finishes, do X" had no
 * way to be said. The refusals matter as much: a misspelt event must be
 * found the first time it is emitted, not never, and a plugin must not be
 * able to forge a record change every rule trusts.
 */
static void
test_automation_custom_event_reaches_a_pod(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GError) error = NULL;
	VentureAutomationHandlerRegistry *registry;
	Witness witness = { 0, NULL, FALSE };

	(void)user_data;

	registry = venture_context_get_automation_handlers(fixture->context);

	g_assert_true(venture_automation_handler_registry_add_event(registry,
		"feed_finished", "A feed run finished", &error));
	g_assert_no_error(error);
	g_assert_false(venture_automation_handler_registry_add_event(registry,
		"feed_finished", NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_clear_error(&error);
	g_assert_true(g_strv_contains(
		venture_automation_handler_registry_get_events(registry),
		"feed_finished"));

	g_assert_true(venture_automation_handler_registry_add(registry, "tally",
		NULL, witness_handler, &witness, NULL, &error));
	g_assert_no_error(error);

	/* Before the engine exists, an emit is delivered to nobody -- and a
	 * misspelt one is still refused. */
	g_assert_true(venture_automation_emit(fixture->context, "feed_finished",
	                                      NULL, &error));
	g_assert_no_error(error);
	g_assert_false(venture_automation_emit(fixture->context, "feed_finshed",
	                                       NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION);
	g_clear_error(&error);

	automation = start_rules(fixture,
		"pod watcher = venture->new();\n"
		"watcher->feed_finished => venture->tally(\"{event->source}\");\n");

	g_assert_true(venture_automation_emit(fixture->context, "feed_finished",
		g_variant_new_parsed("{'source': <'auction-house'>}"), &error));
	g_assert_no_error(error);
	g_assert_cmpuint(witness.calls, ==, 1);
	g_assert_cmpstr(witness.last_argument, ==, "auction-house");

	/* Only the database raises a record change. */
	g_assert_false(venture_automation_emit(fixture->context, "on_created",
	                                       NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_AUTOMATION);
	g_clear_error(&error);

	/* The data is a dictionary, as the record events' is. */
	g_assert_false(venture_automation_emit(fixture->context, "feed_finished",
		g_variant_new_string("not a dictionary"), &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	g_assert_cmpuint(witness.calls, ==, 1);

	stop_rules(fixture, automation);
	witness_clear(&witness);
}

/*
 * An event raised from inside a handler does not trigger automations.
 *
 * What breaks if this regresses: the cascade guard that stops a rule's
 * own write from raising on_created again would not hold for custom
 * events, and a handler that emits the event its own pod is bound to is
 * an unbounded loop.
 */
static void
test_automation_custom_event_does_not_cascade(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GError) error = NULL;
	VentureAutomationHandlerRegistry *registry;
	Witness witness = { 0, NULL, TRUE };

	(void)user_data;

	registry = venture_context_get_automation_handlers(fixture->context);

	g_assert_true(venture_automation_handler_registry_add_event(registry,
		"feed_finished", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_automation_handler_registry_add(registry, "tally",
		NULL, witness_handler, &witness, NULL, &error));
	g_assert_no_error(error);

	automation = start_rules(fixture,
		"pod watcher = venture->new();\n"
		"watcher->feed_finished => venture->tally(\"{event->source}\");\n");

	g_assert_true(venture_automation_emit(fixture->context, "feed_finished",
		g_variant_new_parsed("{'source': <'outside'>}"), &error));
	g_assert_no_error(error);

	/* Once for the outside emit; the one the handler raised went nowhere. */
	g_assert_cmpuint(witness.calls, ==, 1);
	g_assert_cmpstr(witness.last_argument, ==, "outside");

	stop_rules(fixture, automation);
	witness_clear(&witness);
}

/*
 * Installs the handler script as plugin @name, with a manifest providing
 * @provides. Each attempt gets its own name: the manager loads a path
 * once, failed or not.
 */
static gchar *
write_handler_plugin(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*provides
){
	g_autofree gchar *source = NULL;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *directory = NULL;
	g_autofree gchar *script = NULL;
	g_autofree gchar *manifest = NULL;
	gchar *path;
	gsize length;

	g_autofree gchar *manifest_name = NULL;

	directory = g_build_filename(fixture->state_dir, "plugins", name, NULL);
	g_assert_cmpint(g_mkdir_with_parents(directory, 0755), ==, 0);

	source = g_build_filename(VENTURE_TEST_FIXTURES, "exec",
	                          "automation-handler.sh", NULL);
	g_assert_true(g_file_get_contents(source, &contents, &length, NULL));

	script = g_build_filename(directory, "tally.sh", NULL);
	g_assert_true(g_file_set_contents(script, contents, (gssize)length, NULL));
	g_assert_cmpint(g_chmod(script, 0755), ==, 0);

	manifest = g_strdup_printf("name: %s\n"
	                           "runtime: exec\n"
	                           "protocol: 1\n"
	                           "entry: tally.sh\n"
	                           "exec:\n"
	                           "  timeout: 20\n"
	                           "provides:\n"
	                           "%s", name, provides);
	manifest_name = g_strdup_printf("%s.plugin.yaml", name);

	path = g_build_filename(directory, manifest_name, NULL);
	g_assert_true(g_file_set_contents(path, manifest, -1, NULL));

	return path;
}

static const gchar *
result_string(
	GVariant	*result,
	const gchar	*key
){
	const gchar *value = NULL;

	g_assert_true(g_variant_lookup(result, key, "&s", &value));

	return value;
}

/*
 * An exec plugin's manifest can provide a handler, run as its program.
 *
 * What breaks if this regresses: the arguments must arrive as
 * `params.args` with the declared command, the program's `result` must
 * become count and summary, a program with no `result` is counted by its
 * messages, and plugins.allow_exec -- off by default -- must stop the run
 * at the next call, not only at load: switching it off on a running
 * install is how an operator stops a program without a restart.
 */
static void
test_automation_exec_handler(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;
	g_autoptr(VentureAutomation) automation = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GVariant) fallback = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *manifest = NULL;
	const gchar *const arguments[] = { "alpha", "beta", NULL };
	const gchar *const fallback_arguments[] = { "fallback", NULL };
	const gchar *detail;
	gint64 count;

	(void)user_data;

	g_object_set(fixture->config, "plugins-allow-exec", TRUE, NULL);

	manager = venture_plugin_manager_new(fixture->context);
	venture_context_set_plugin_manager(fixture->context, manager);

	manifest = write_handler_plugin(fixture, "tally",
		"  - kind: automation_handler\n"
		"    name: tally_up\n"
		"    command: tally\n"
		"    description: Counts what it was given\n");

	g_assert_true(venture_plugin_manager_load_file(manager, manifest, &error));
	g_assert_no_error(error);
	g_assert_true(venture_automation_handler_registry_has(
		venture_context_get_automation_handlers(fixture->context),
		"tally_up"));
	g_assert_cmpstr(venture_automation_handler_registry_get_description(
		venture_context_get_automation_handlers(fixture->context), "tally_up"),
		==, "Counts what it was given");

	automation = venture_automation_new(fixture->context, &error);
	g_assert_no_error(error);

	g_assert_true(venture_automation_invoke(automation, "tally_up", arguments,
	                                        &result, &error));
	g_assert_no_error(error);

	g_assert_true(g_variant_lookup(result, "count", "x", &count));
	g_assert_cmpint(count, ==, 2);
	g_assert_cmpstr(result_string(result, "summary"), ==, "two of them");

	/* The request the program read, handed back as its log line. */
	detail = result_string(result, "detail");
	g_assert_nonnull(strstr(detail, "\"command\":\"tally\""));
	g_assert_nonnull(strstr(detail, "\"args\":[\"alpha\",\"beta\"]"));

	/* No result message: two cursors are a count of two. */
	g_assert_true(venture_automation_invoke(automation, "tally_up",
	                                        fallback_arguments, &fallback,
	                                        &error));
	g_assert_no_error(error);
	g_assert_true(g_variant_lookup(fallback, "count", "x", &count));
	g_assert_cmpint(count, ==, 2);
	g_assert_cmpstr(result_string(fallback, "summary"), ==, "");

	/* Switched off on a running install: the next run is refused. */
	g_object_set(fixture->config, "plugins-allow-exec", FALSE, NULL);
	g_assert_false(venture_automation_invoke(automation, "tally_up", arguments,
	                                         NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);

	venture_context_set_plugin_manager(fixture->context, NULL);
}

/*
 * With exec off the plugin does not load and its handler does not exist;
 * an entry with a misspelt key fails the load rather than running the
 * wrong command; and a handler may not take a built-in's name.
 */
static void
test_automation_exec_handler_refusals(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VenturePluginManager) manager = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *manifest = NULL;
	VentureAutomationHandlerRegistry *registry;

	(void)user_data;

	registry = venture_context_get_automation_handlers(fixture->context);
	manager = venture_plugin_manager_new(fixture->context);
	venture_context_set_plugin_manager(fixture->context, manager);

	/* plugins.allow_exec is off by default. */
	manifest = write_handler_plugin(fixture, "tally-off",
		"  - kind: automation_handler\n"
		"    name: tally_up\n");
	g_assert_false(venture_plugin_manager_load_file(manager, manifest, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_clear_error(&error);
	g_assert_false(venture_automation_handler_registry_has(registry,
	                                                       "tally_up"));

	g_object_set(fixture->config, "plugins-allow-exec", TRUE, NULL);

	g_clear_pointer(&manifest, g_free);
	manifest = write_handler_plugin(fixture, "tally-typo",
		"  - kind: automation_handler\n"
		"    name: tally_up\n"
		"    commnd: tally\n");
	g_assert_false(venture_plugin_manager_load_file(manager, manifest, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_assert_nonnull(strstr(error->message, "commnd"));
	g_clear_error(&error);
	g_assert_false(venture_automation_handler_registry_has(registry,
	                                                       "tally_up"));

	g_clear_pointer(&manifest, g_free);
	manifest = write_handler_plugin(fixture, "tally-clash",
		"  - kind: automation_handler\n"
		"    name: report\n");
	g_assert_false(venture_plugin_manager_load_file(manager, manifest, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN);
	g_assert_nonnull(strstr(error->message, "already registered"));
	g_clear_error(&error);

	venture_context_set_plugin_manager(fixture->context, NULL);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add("/automation/handlers-run-on-the-calling-thread", Fixture, NULL,
	           fixture_set_up,
	           test_automation_handlers_run_on_the_calling_thread,
	           fixture_tear_down);
	g_test_add("/automation/does-not-cascade", Fixture, NULL,
	           fixture_set_up, test_automation_does_not_cascade,
	           fixture_tear_down);

	g_test_add("/automation/dunning-sweep", Fixture, NULL,
	           fixture_set_up, test_automation_dunning_sweep,
	           fixture_tear_down);
	g_test_add("/automation/mail-sync-handler", Fixture, NULL,
	           fixture_set_up, test_automation_mail_sync_handler,
	           fixture_tear_down);

	g_test_add("/automation/plugin-handler-runs-from-a-pod", Fixture, NULL,
	           fixture_set_up, test_automation_plugin_handler_runs_from_a_pod,
	           fixture_tear_down);
	g_test_add("/automation/unknown-handler-refused", Fixture, NULL,
	           fixture_set_up, test_automation_unknown_handler_refused,
	           fixture_tear_down);
	g_test_add("/automation/custom-event-reaches-a-pod", Fixture, NULL,
	           fixture_set_up, test_automation_custom_event_reaches_a_pod,
	           fixture_tear_down);
	g_test_add("/automation/custom-event-does-not-cascade", Fixture, NULL,
	           fixture_set_up, test_automation_custom_event_does_not_cascade,
	           fixture_tear_down);
	g_test_add("/automation/exec-handler", Fixture, NULL,
	           fixture_set_up, test_automation_exec_handler,
	           fixture_tear_down);
	g_test_add("/automation/exec-handler-refusals", Fixture, NULL,
	           fixture_set_up, test_automation_exec_handler_refusals,
	           fixture_tear_down);

	g_test_add_func("/automation/shipped-examples-parse",
	                test_automation_shipped_examples_parse);

	return g_test_run();
}
