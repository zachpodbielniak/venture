/*
 * test-desk.c - The workdesk: inbox, service levels, macros, worklogs,
 *               sprints, bulk edits, the timeline, budgets and runs
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything here is about the promises that make the desk trustworthy:
 * the right people are told and nobody else; a clock is set once and
 * read honestly; a macro is one transaction; a bulk edit is all or
 * nothing; a budget refuses a run before the money is spent; and the
 * timeline is the record's own story.
 */

#include <venture.h>

#include <glib.h>
#include <string.h>

#include "venture-test-util.h"

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
} Fixture;

static void
file_under_default(
	Fixture		*fixture,
	gpointer	 record
){
	venture_entity_set_organization_id(VENTURE_ENTITY(record),
		venture_context_get_default_organization_id(fixture->context));
}

static gint64
create_user(
	Fixture		*fixture,
	const gchar	*username,
	VentureUserRole	 role
){
	g_autoptr(VentureUser) user = NULL;

	user = venture_user_new();
	g_object_set(user, "username", username, "role", role, "active", TRUE,
	             NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(user), NULL, NULL));

	return venture_entity_get_id(VENTURE_ENTITY(user));
}

static void
actor_for(
	VentureActor	*actor,
	const gchar	*username
){
	actor->kind = VENTURE_ACTOR_KIND_USER;
	actor->name = username;
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

static VentureEntity *
create_ticket(
	Fixture		*fixture,
	const gchar	*title,
	const gchar	*assignee,
	const gchar	*by
){
	VentureTicket *ticket;
	VentureActor actor;

	actor_for(&actor, by);
	ticket = venture_ticket_new();
	g_object_set(ticket, "title", title, "status", VENTURE_TICKET_STATUS_TODO,
	             "assignee", assignee, "kind", VENTURE_TICKET_KIND_EXTERNAL,
	             "priority", VENTURE_PRIORITY_HIGH, NULL);
	file_under_default(fixture, ticket);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(ticket),
	                                    (NULL != by) ? &actor : NULL, NULL));

	return VENTURE_ENTITY(ticket);
}

static void
comment_on(
	Fixture		*fixture,
	VentureEntity	*ticket,
	const gchar	*by,
	const gchar	*body,
	gboolean	 internal
){
	g_autoptr(VentureTicketComment) comment = NULL;
	g_autoptr(GDateTime) now = NULL;
	VentureActor actor;

	actor_for(&actor, by);
	now = venture_time_now();
	comment = venture_ticket_comment_new();
	g_object_set(comment, "ticket-id", venture_entity_get_id(ticket),
	             "body", body, "author", by, "internal", internal,
	             "occurred-at", now, NULL);
	file_under_default(fixture, comment);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(comment), &actor, NULL));
}

static gint64
unread_of_kind(
	Fixture			*fixture,
	gint64			 user_id,
	VentureNotificationKind	 kind
){
	g_autoptr(GPtrArray) rows = NULL;
	gint64 n;
	guint i;

	rows = venture_notify_list(fixture->context, user_id, TRUE, 0, NULL);
	g_assert_nonnull(rows);
	n = 0;

	for (i = 0; i < rows->len; i++)
	{
		VentureNotificationKind row_kind;

		g_object_get(g_ptr_array_index(rows, i), "kind", &row_kind, NULL);

		if (row_kind == kind)
			n++;
	}

	return n;
}

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	(void)user_data;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry,
	                              venture_entity_registry_get_default());
}

/* --- Modules --------------------------------------------------------------- */

/*
 * Each new type is owned by exactly one module, and by the right one:
 * the inbox and saved views belong to everybody, the desk to tickets,
 * the budget to the forge.
 */
static void
test_desk_types_belong_to_modules(void)
{
	g_autoptr(VentureModuleRegistry) registry = NULL;
	const gchar *const *types;

	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);

	types = venture_module_get_entity_names(
		venture_module_registry_lookup(registry, "core"));
	g_assert_true(g_strv_contains(types, "saved_view"));
	g_assert_true(g_strv_contains(types, "watch"));
	g_assert_true(g_strv_contains(types, "notification"));

	types = venture_module_get_entity_names(
		venture_module_registry_lookup(registry, "tickets"));
	g_assert_true(g_strv_contains(types, "sla_policy"));
	g_assert_true(g_strv_contains(types, "macro"));
	g_assert_true(g_strv_contains(types, "worklog"));
	g_assert_true(g_strv_contains(types, "sprint"));

	types = venture_module_get_entity_names(
		venture_module_registry_lookup(registry, "forge"));
	g_assert_true(g_strv_contains(types, "agent_budget"));

	types = venture_module_get_reports(
		venture_module_registry_lookup(registry, "factory"));
	g_assert_true(g_strv_contains(types, "delivery"));
}

/* --- The inbox ------------------------------------------------------------- */

/*
 * @names in a comment, once each, without the punctuation around them,
 * and never the local part of an email address.
 */
static void
test_desk_mentions_are_extracted(void)
{
	g_auto(GStrv) names = NULL;

	names = venture_notify_extract_mentions(
		"@alice can you look? cc @bob-smith, @alice again. "
		"Mail zach@example.org and @carol.");

	g_assert_cmpuint(g_strv_length(names), ==, 3);
	g_assert_cmpstr(names[0], ==, "alice");
	g_assert_cmpstr(names[1], ==, "bob-smith");
	g_assert_cmpstr(names[2], ==, "carol");

	names = (g_strfreev(names), venture_notify_extract_mentions(NULL));
	g_assert_cmpuint(g_strv_length(names), ==, 0);
}

/*
 * The three ways into an inbox: being named, being handed a ticket, and
 * following one. The person who did the thing is never told about it,
 * and one person is told once per event.
 *
 * What breaks if this regresses: an inbox full of your own edits, or a
 * mention that reaches nobody.
 */
static void
test_desk_inbox_is_fed_by_the_audit_trail(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *body = NULL;
	VentureActor actor;
	gint64 alice;
	gint64 bob;
	gint64 carol;
	gint64 ticket_id;

	(void)user_data;

	alice = create_user(fixture, "alice", VENTURE_USER_ROLE_EDITOR);
	bob = create_user(fixture, "bob", VENTURE_USER_ROLE_EDITOR);
	carol = create_user(fixture, "carol", VENTURE_USER_ROLE_VIEWER);

	/* Alice raises a ticket and hands it to Bob: Bob is told, Alice is
	 * not, and both now follow it. */
	ticket = create_ticket(fixture, "Printer on fire", "bob", "alice");
	ticket_id = venture_entity_get_id(ticket);

	g_assert_cmpint(venture_notify_unread_count(fixture->context, bob), ==, 1);
	g_assert_cmpint(venture_notify_unread_count(fixture->context, alice), ==, 0);
	g_assert_true(venture_notify_is_watching(fixture->context, alice, "ticket",
	                                         venture_entity_get_id(ticket)));
	g_assert_true(venture_notify_is_watching(fixture->context, bob, "ticket",
	                                         venture_entity_get_id(ticket)));

	rows = venture_notify_list(fixture->context, bob, TRUE, 0, NULL);
	g_object_get(g_ptr_array_index(rows, 0), "title", &title, NULL);
	g_assert_cmpstr(title, ==, "alice assigned you: Printer on fire");

	/* Bob comments, naming Carol: Carol is told she was mentioned and
	 * starts following; Alice is told as a watcher; Bob hears nothing. */
	comment_on(fixture, ticket, "bob", "Looking now, @carol can you check?",
	           FALSE);

	g_assert_cmpint(unread_of_kind(fixture, carol,
	                               VENTURE_NOTIFICATION_KIND_MENTION), ==, 1);
	g_assert_cmpint(unread_of_kind(fixture, alice,
	                               VENTURE_NOTIFICATION_KIND_WATCHED), ==, 1);
	g_assert_cmpint(venture_notify_unread_count(fixture->context, bob), ==, 1);
	g_assert_true(venture_notify_is_watching(fixture->context, carol, "ticket",
	                                         venture_entity_get_id(ticket)));

	/* Carol changes the status: the two watchers are told what moved.
	 * Re-read first: Bob's visible reply stamped the ticket's first
	 * response, so the copy in hand is a version behind. */
	g_clear_object(&ticket);
	ticket = venture_database_get(fixture->database, VENTURE_TYPE_TICKET,
	                              ticket_id, NULL);
	actor_for(&actor, "carol");
	g_object_set(ticket, "status", VENTURE_TICKET_STATUS_IN_PROGRESS, NULL);
	g_assert_true(venture_database_save(fixture->database, ticket, &actor,
	                                    NULL));

	g_clear_pointer(&rows, g_ptr_array_unref);
	rows = venture_notify_list(fixture->context, alice, TRUE, 0, NULL);
	g_assert_cmpuint(rows->len, ==, 2);
	g_object_get(g_ptr_array_index(rows, 0), "body", &body, NULL);
	g_assert_nonnull(body);
	g_assert_nonnull(strstr(body, "Status"));
	g_assert_nonnull(strstr(body, "in_progress"));
	g_assert_cmpint(unread_of_kind(fixture, carol,
	                               VENTURE_NOTIFICATION_KIND_WATCHED), ==, 0);

	/* Reading is per person: Bob cannot read Alice's, and reading all of
	 * one's own leaves the others' alone. */
	{
		g_autoptr(GError) error = NULL;
		gint64 alices;

		alices = venture_entity_get_id(g_ptr_array_index(rows, 0));

		g_assert_cmpint(venture_notify_mark_read(fixture->context, bob, alices,
		                                         &error), ==, -1);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
		g_clear_error(&error);

		g_assert_cmpint(venture_notify_mark_read(fixture->context, alice, 0,
		                                         &error), ==, 2);
		g_assert_no_error(error);
		g_assert_cmpint(venture_notify_unread_count(fixture->context, alice),
		                ==, 0);
		/* Bob's assignment, and Carol's change to a ticket he watches. */
		g_assert_cmpint(venture_notify_unread_count(fixture->context, bob),
		                ==, 2);
	}

	/* Unwatching stops the flow. */
	g_assert_true(venture_notify_unwatch(fixture->context, alice, "ticket",
	                                     venture_entity_get_id(ticket), NULL));
	g_object_set(ticket, "priority", VENTURE_PRIORITY_LOW, NULL);
	g_assert_true(venture_database_save(fixture->database, ticket, &actor,
	                                    NULL));
	g_assert_cmpint(venture_notify_unread_count(fixture->context, alice), ==, 0);
}

/*
 * A broadcast reaches the roles named and nobody below them, and the
 * inbox is scoped: user 0 gets nothing and refuses nothing.
 */
static void
test_desk_broadcast_respects_roles(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	gint64 owner;
	gint64 editor;

	(void)user_data;

	owner = create_user(fixture, "owner", VENTURE_USER_ROLE_OWNER);
	editor = create_user(fixture, "ed", VENTURE_USER_ROLE_EDITOR);

	g_assert_cmpint(venture_notify_broadcast(fixture->context,
		VENTURE_USER_ROLE_ADMIN, VENTURE_NOTIFICATION_KIND_SYSTEM, "Hello",
		NULL, NULL, 0, NULL, NULL, NULL), ==, 1);
	g_assert_cmpint(venture_notify_unread_count(fixture->context, owner), ==, 1);
	g_assert_cmpint(venture_notify_unread_count(fixture->context, editor), ==, 0);
	g_assert_cmpint(venture_notify_unread_count(fixture->context, 0), ==, 0);
	g_assert_true(venture_notify_send(fixture->context, 0,
		VENTURE_NOTIFICATION_KIND_SYSTEM, "Dropped", NULL, NULL, 0, NULL, NULL,
		NULL));
}

/* --- Service levels -------------------------------------------------------- */

static gint64
create_policy(
	Fixture			*fixture,
	const gchar		*name,
	gboolean		 all_kinds,
	VentureTicketKind	 kind,
	gboolean		 all_priorities,
	VenturePriority		 priority,
	gdouble			 respond,
	gdouble			 resolve
){
	g_autoptr(VentureSlaPolicy) policy = NULL;

	policy = venture_sla_policy_new();
	g_object_set(policy, "name", name, "all-kinds", all_kinds, "kind", kind,
	             "all-priorities", all_priorities, "priority", priority,
	             "first-response-hours", respond, "resolution-hours", resolve,
	             "active", TRUE, NULL);
	file_under_default(fixture, policy);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(policy), NULL, NULL));

	return venture_entity_get_id(VENTURE_ENTITY(policy));
}

/*
 * The most specific policy wins; a ticket gets its clocks on its first
 * save and keeps them; the first visible reply stamps it; a note does
 * not; the sweep marks a breach once and tells the assignee.
 */
static void
test_desk_service_levels(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) policy = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) reread = NULL;
	g_autoptr(GDateTime) respond_by = NULL;
	g_autoptr(GDateTime) resolve_by = NULL;
	g_autoptr(GDateTime) responded = NULL;
	g_autofree gchar *name = NULL;
	VentureSlaStatus status;
	gint64 catch_all;
	gint64 specific;
	gint64 bob;
	gboolean breached = FALSE;

	(void)user_data;

	bob = create_user(fixture, "bob", VENTURE_USER_ROLE_EDITOR);
	catch_all = create_policy(fixture, "Everything", TRUE,
	                          VENTURE_TICKET_KIND_INTERNAL, TRUE,
	                          VENTURE_PRIORITY_NORMAL, 24.0, 72.0);
	specific = create_policy(fixture, "Support high", FALSE,
	                         VENTURE_TICKET_KIND_EXTERNAL, FALSE,
	                         VENTURE_PRIORITY_HIGH, 1.0, 8.0);

	policy = venture_sla_find_policy(fixture->context,
	                                 VENTURE_TICKET_KIND_EXTERNAL,
	                                 VENTURE_PRIORITY_HIGH);
	g_assert_cmpint(venture_entity_get_id(policy), ==, specific);
	g_clear_object(&policy);
	policy = venture_sla_find_policy(fixture->context,
	                                 VENTURE_TICKET_KIND_INTERNAL,
	                                 VENTURE_PRIORITY_LOW);
	g_assert_cmpint(venture_entity_get_id(policy), ==, catch_all);
	g_object_get(policy, "name", &name, NULL);
	g_assert_cmpstr(name, ==, "Everything");

	/* An external high ticket gets the one-hour and eight-hour clocks. */
	ticket = create_ticket(fixture, "Login broken", "bob", "bob");
	g_object_get(ticket, "first-response-due-at", &respond_by,
	             "resolution-due-at", &resolve_by, NULL);
	g_assert_nonnull(respond_by);
	g_assert_nonnull(resolve_by);
	g_assert_cmpint(g_date_time_difference(resolve_by, respond_by)
	                / G_TIME_SPAN_HOUR, ==, 7);

	venture_sla_status(ticket, NULL, &status);
	g_assert_true(status.has_policy);
	g_assert_cmpint(status.first_response, ==, VENTURE_SLA_STATE_OK);
	g_assert_cmpint(status.resolution, ==, VENTURE_SLA_STATE_OK);
	g_assert_cmpint(status.first_response_remaining, >, 3500);

	/* Inside the last fifth it warns; past it, it is breached. */
	{
		g_autoptr(GDateTime) later = NULL;
		g_autoptr(GDateTime) too_late = NULL;

		later = g_date_time_add_minutes(respond_by, -5);
		venture_sla_status(ticket, later, &status);
		g_assert_cmpint(status.first_response, ==, VENTURE_SLA_STATE_WARNING);

		too_late = g_date_time_add_minutes(respond_by, 5);
		venture_sla_status(ticket, too_late, &status);
		g_assert_cmpint(status.first_response, ==, VENTURE_SLA_STATE_BREACHED);
		g_assert_cmpint(status.first_response_remaining, <, 0);
	}

	/* Changing the priority does not move a clock already set. */
	g_object_set(ticket, "priority", VENTURE_PRIORITY_LOW, NULL);
	g_assert_true(venture_database_save(fixture->database, ticket, NULL, NULL));
	reread = venture_database_get(fixture->database, VENTURE_TYPE_TICKET,
	                              venture_entity_get_id(ticket), NULL);
	{
		g_autoptr(GDateTime) still = NULL;

		g_object_get(reread, "first-response-due-at", &still, NULL);
		g_assert_true(g_date_time_equal(still, respond_by));
	}

	/* A note is not a reply; a visible comment is, and it stamps once. */
	comment_on(fixture, ticket, "bob", "Checking the logs", TRUE);
	g_clear_object(&reread);
	reread = venture_database_get(fixture->database, VENTURE_TYPE_TICKET,
	                              venture_entity_get_id(ticket), NULL);
	g_object_get(reread, "first-responded-at", &responded, NULL);
	g_assert_null(responded);

	comment_on(fixture, ticket, "bob", "We are on it", FALSE);
	g_clear_object(&reread);
	reread = venture_database_get(fixture->database, VENTURE_TYPE_TICKET,
	                              venture_entity_get_id(ticket), NULL);
	g_object_get(reread, "first-responded-at", &responded, NULL);
	g_assert_nonnull(responded);
	venture_sla_status(reread, NULL, &status);
	g_assert_true(status.responded);
	g_assert_cmpint(status.first_response, ==, VENTURE_SLA_STATE_OK);

	/* A ticket whose resolution target has passed is marked by the
	 * sweep, once, and the assignee hears about it. */
	{
		g_autoptr(VentureTicket) overdue = NULL;
		g_autoptr(VentureEntity) marked = NULL;
		g_autoptr(GDateTime) yesterday = NULL;
		g_autoptr(GDateTime) now = NULL;

		now = venture_time_now();
		yesterday = g_date_time_add_days(now, -1);
		overdue = venture_ticket_new();
		g_object_set(overdue, "title", "Slow one", "assignee", "bob",
		             "status", VENTURE_TICKET_STATUS_IN_PROGRESS,
		             "resolution-due-at", yesterday, NULL);
		file_under_default(fixture, overdue);
		g_assert_true(venture_database_save(fixture->database,
		                                    VENTURE_ENTITY(overdue), NULL,
		                                    NULL));

		g_assert_cmpint(unread_of_kind(fixture, bob,
		                               VENTURE_NOTIFICATION_KIND_SLA), ==, 0);
		g_assert_cmpint(venture_sla_sweep(fixture->context, 0, NULL), ==, 1);
		g_assert_cmpint(venture_sla_sweep(fixture->context, 0, NULL), ==, 0);

		marked = venture_database_get(fixture->database, VENTURE_TYPE_TICKET,
			venture_entity_get_id(VENTURE_ENTITY(overdue)), NULL);
		g_object_get(marked, "sla-breached", &breached, NULL);
		g_assert_true(breached);
		g_assert_cmpint(unread_of_kind(fixture, bob,
		                               VENTURE_NOTIFICATION_KIND_SLA), ==, 1);
	}

	/* Closing stamps resolved-at. */
	{
		g_autoptr(GDateTime) resolved_at = NULL;

		g_object_set(reread, "status", VENTURE_TICKET_STATUS_DONE, NULL);
		g_assert_true(venture_database_save(fixture->database, reread, NULL,
		                                    NULL));
		g_object_get(reread, "resolved-at", &resolved_at, NULL);
		g_assert_nonnull(resolved_at);
		venture_sla_status(reread, NULL, &status);
		g_assert_true(status.closed);
		g_assert_cmpint(status.resolution, ==, VENTURE_SLA_STATE_OK);
	}
}

/* --- Macros, worklogs, sprints -------------------------------------------- */

/*
 * A macro is a reply plus changes, in one press and one transaction,
 * with {me}, {ticket} and {title} filled in and tags merged rather than
 * replaced. An inactive one is refused.
 */
static void
test_desk_macro_applies_in_one_go(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureMacro) macro = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) found = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) comments = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *tags = NULL;
	g_autofree gchar *assignee = NULL;
	VentureTicketStatus status;
	VentureActor actor;
	gboolean internal = TRUE;

	(void)user_data;

	create_user(fixture, "alice", VENTURE_USER_ROLE_EDITOR);
	ticket = create_ticket(fixture, "Cannot print", NULL, "alice");
	g_object_set(ticket, "tags", "printer, urgent", NULL);
	g_assert_true(venture_database_save(fixture->database, ticket, NULL, NULL));

	macro = venture_macro_new();
	g_object_set(macro,
	             "name", "Ask for logs",
	             "body", "Hi, {me} here on {ticket} ({title}). Please attach "
	                     "the logs.",
	             "internal", FALSE,
	             "apply-status", TRUE, "status", VENTURE_TICKET_STATUS_BLOCKED,
	             "assignee", "{me}",
	             "add-tags", "Urgent, waiting-on-customer",
	             "active", TRUE, NULL);
	file_under_default(fixture, macro);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(macro), NULL, NULL));

	found = venture_desk_find_macro(fixture->context, "Ask for logs");
	g_assert_nonnull(found);

	actor_for(&actor, "alice");
	g_assert_true(venture_desk_apply_macro(fixture->context, ticket, found,
	                                       &actor, &error));
	g_assert_no_error(error);

	g_object_get(ticket, "status", &status, "tags", &tags,
	             "assignee", &assignee, NULL);
	g_assert_cmpint(status, ==, VENTURE_TICKET_STATUS_BLOCKED);
	g_assert_cmpstr(tags, ==, "printer, urgent, waiting-on-customer");
	g_assert_cmpstr(assignee, ==, "alice");

	query = venture_query_new(VENTURE_TYPE_TICKET_COMMENT);
	venture_query_add_filter_int(query, "ticket-id", VENTURE_FILTER_OP_EQ,
	                             venture_entity_get_id(ticket), NULL);
	comments = venture_database_find(fixture->database, query, NULL);
	g_assert_cmpuint(comments->len, ==, 1);
	g_object_get(g_ptr_array_index(comments, 0), "body", &body,
	             "internal", &internal, NULL);
	g_assert_false(internal);
	g_assert_nonnull(strstr(body, "alice here on #"));
	g_assert_nonnull(strstr(body, "(Cannot print)"));

	/* Switched off, it is refused before anything is written. */
	g_object_set(found, "active", FALSE, NULL);
	g_assert_true(venture_database_save(fixture->database, found, NULL, NULL));
	g_assert_false(venture_desk_apply_macro(fixture->context, ticket, found,
	                                        &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_pointer(&comments, g_ptr_array_unref);
	comments = venture_database_find(fixture->database, query, NULL);
	g_assert_cmpuint(comments->len, ==, 1);
}

/*
 * Logged time rolls up onto the ticket, from every door, and follows a
 * worklog being removed. Zero hours is refused.
 */
static void
test_desk_worklogs_roll_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) second = NULL;
	g_autoptr(VentureEntity) reread = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	gdouble logged = 0.0;

	(void)user_data;

	ticket = create_ticket(fixture, "Write the report", "alice", NULL);
	actor_for(&actor, "alice");

	g_assert_null(venture_desk_log_work(fixture->context,
	                                    venture_entity_get_id(ticket), 0.0,
	                                    NULL, &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	first = venture_desk_log_work(fixture->context, venture_entity_get_id(ticket),
	                              1.5, "drafting", &actor, &error);
	g_assert_no_error(error);
	second = venture_desk_log_work(fixture->context,
	                               venture_entity_get_id(ticket), 2.0, NULL,
	                               &actor, &error);
	g_assert_no_error(error);

	reread = venture_database_get(fixture->database, VENTURE_TYPE_TICKET,
	                              venture_entity_get_id(ticket), NULL);
	g_object_get(reread, "logged-hours", &logged, NULL);
	g_assert_cmpfloat(logged, ==, 3.5);

	g_assert_true(venture_database_delete(fixture->database, second, &actor,
	                                      NULL));
	g_clear_object(&reread);
	reread = venture_database_get(fixture->database, VENTURE_TYPE_TICKET,
	                              venture_entity_get_id(ticket), NULL);
	g_object_get(reread, "logged-hours", &logged, NULL);
	g_assert_cmpfloat(logged, ==, 1.5);
}

/*
 * A sprint counts its tickets and their points, done against planned,
 * and the list puts the active sprint first.
 */
static void
test_desk_sprint_burn(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureSprint) sprint = NULL;
	g_autoptr(VentureSprint) old = NULL;
	g_autoptr(GPtrArray) sprints = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GDateTime) starts = NULL;
	g_autoptr(GDateTime) ends = NULL;
	VentureSprintProgress progress;
	JsonObject *object;
	gint64 points[3] = { 3, 5, 8 };
	VentureTicketStatus statuses[3] = {
		VENTURE_TICKET_STATUS_DONE, VENTURE_TICKET_STATUS_IN_PROGRESS,
		VENTURE_TICKET_STATUS_CANCELLED
	};
	guint i;

	(void)user_data;

	now = venture_time_now();
	starts = g_date_time_add_days(now, -3);
	ends = g_date_time_add_days(now, 11);

	old = venture_sprint_new();
	g_object_set(old, "name", "Sprint 1",
	             "status", VENTURE_SPRINT_STATUS_COMPLETED, NULL);
	file_under_default(fixture, old);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(old),
	                                    NULL, NULL));

	sprint = venture_sprint_new();
	g_object_set(sprint, "name", "Sprint 2", "goal", "Ship the desk",
	             "status", VENTURE_SPRINT_STATUS_ACTIVE,
	             "starts-on", starts, "ends-on", ends,
	             "capacity-points", 20, NULL);
	file_under_default(fixture, sprint);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(sprint), NULL, NULL));

	for (i = 0; i < 3; i++)
	{
		g_autoptr(VentureEntity) ticket = NULL;

		ticket = create_ticket(fixture, "Piece", NULL, NULL);
		g_object_set(ticket, "sprint-id",
		             venture_entity_get_id(VENTURE_ENTITY(sprint)),
		             "story-points", points[i], "status", statuses[i], NULL);
		g_assert_true(venture_database_save(fixture->database, ticket, NULL,
		                                    NULL));
	}

	venture_desk_sprint_progress(fixture->context, VENTURE_ENTITY(sprint),
	                             &progress);
	g_assert_cmpint(progress.tickets, ==, 3);
	g_assert_cmpint(progress.done, ==, 2);
	g_assert_cmpint(progress.points, ==, 16);
	g_assert_cmpint(progress.points_done, ==, 11);
	g_assert_cmpint(progress.capacity, ==, 20);
	g_assert_cmpint(progress.days_total, ==, 14);
	g_assert_cmpint(progress.days_left, >=, 10);

	node = venture_desk_sprint_to_json(fixture->context, VENTURE_ENTITY(sprint),
	                                   TRUE);
	object = json_node_get_object(node);
	g_assert_cmpint(venture_json_object_get_int(object, "percent", 0), ==, 68);
	g_assert_cmpuint(json_array_get_length(
		json_object_get_array_member(object, "items")), ==, 3);

	sprints = venture_desk_list_sprints(fixture->context, NULL, 0, NULL);
	g_assert_cmpuint(sprints->len, ==, 2);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(sprints, 0)), ==,
	                venture_entity_get_id(VENTURE_ENTITY(sprint)));
}

/* --- Bulk edits ------------------------------------------------------------ */

/*
 * Forty records changed as one: a value one of them refuses leaves all
 * of them as they were, and each is audited on its own.
 *
 * What breaks if this regresses: half a bulk edit applied, which is
 * worse than none.
 */
static void
test_desk_bulk_is_all_or_nothing(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(JsonObject) changes = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	gint64 ids[3];
	guint i;

	(void)user_data;

	actor_for(&actor, "alice");

	for (i = 0; i < 3; i++)
	{
		g_autoptr(VentureEntity) ticket = NULL;

		ticket = create_ticket(fixture, "Bulk", NULL, NULL);
		ids[i] = venture_entity_get_id(ticket);
	}

	changes = json_object_new();
	json_object_set_string_member(changes, "status", "in_progress");
	json_object_set_string_member(changes, "assignee", "alice");

	g_assert_cmpint(venture_desk_bulk_update(fixture->context,
		VENTURE_TYPE_TICKET, ids, 3, changes, &actor, &error), ==, 3);
	g_assert_no_error(error);

	for (i = 0; i < 3; i++)
	{
		g_autoptr(VentureEntity) ticket = NULL;
		g_autofree gchar *assignee = NULL;
		VentureTicketStatus status;

		ticket = venture_database_get(fixture->database, VENTURE_TYPE_TICKET,
		                              ids[i], NULL);
		g_object_get(ticket, "status", &status, "assignee", &assignee, NULL);
		g_assert_cmpint(status, ==, VENTURE_TICKET_STATUS_IN_PROGRESS);
		g_assert_cmpstr(assignee, ==, "alice");
	}

	/* The third id does not exist: nothing changes, not even the first. */
	json_object_set_string_member(changes, "status", "done");
	ids[2] = 999999;
	g_assert_cmpint(venture_desk_bulk_update(fixture->context,
		VENTURE_TYPE_TICKET, ids, 3, changes, &actor, &error), ==, -1);
	g_assert_nonnull(error);
	g_clear_error(&error);

	{
		g_autoptr(VentureEntity) ticket = NULL;
		VentureTicketStatus status;

		ticket = venture_database_get(fixture->database, VENTURE_TYPE_TICKET,
		                              ids[0], NULL);
		g_object_get(ticket, "status", &status, NULL);
		g_assert_cmpint(status, ==, VENTURE_TICKET_STATUS_IN_PROGRESS);
	}

	/* And a bad value for a real record. */
	json_object_set_string_member(changes, "status", "not-a-status");
	ids[2] = ids[1];
	g_assert_cmpint(venture_desk_bulk_update(fixture->context,
		VENTURE_TYPE_TICKET, ids, 2, changes, &actor, &error), ==, -1);
	g_clear_error(&error);

	g_assert_cmpint(venture_desk_bulk_delete(fixture->context,
		VENTURE_TYPE_TICKET, ids, 2, &actor, &error), ==, 2);
	g_assert_no_error(error);

	{
		g_autoptr(VentureQuery) query = NULL;

		query = venture_query_new(VENTURE_TYPE_TICKET);
		g_assert_cmpint(venture_database_count(fixture->database, query, NULL),
		                ==, 1);
	}
}

/* --- The timeline ---------------------------------------------------------- */

/*
 * A ticket's timeline is its changes, its comments and its time, as one
 * list newest first, each saying who and what.
 */
static void
test_desk_activity_weaves_the_story(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) worklog = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonArray *events;
	VentureActor actor;
	guint changes;
	guint comments;
	guint worklogs;
	guint i;

	(void)user_data;

	create_user(fixture, "alice", VENTURE_USER_ROLE_EDITOR);
	ticket = create_ticket(fixture, "Story", NULL, "alice");
	actor_for(&actor, "alice");
	comment_on(fixture, ticket, "alice", "First look", FALSE);
	worklog = venture_desk_log_work(fixture->context,
	                                venture_entity_get_id(ticket), 0.5, "look",
	                                &actor, NULL);

	/* The reply and the worklog each wrote the ticket behind our back
	 * (first response, logged hours), so the close starts from a fresh
	 * read -- the same thing a form does by reloading. */
	{
		g_autoptr(VentureEntity) fresh = NULL;

		fresh = venture_database_get(fixture->database, VENTURE_TYPE_TICKET,
		                             venture_entity_get_id(ticket), NULL);
		g_object_set(fresh, "status", VENTURE_TICKET_STATUS_DONE, NULL);
		g_assert_true(venture_database_save(fixture->database, fresh, &actor,
		                                    NULL));
	}

	node = venture_desk_activity(fixture->context, "ticket",
	                             venture_entity_get_id(ticket), 0, NULL);
	g_assert_nonnull(node);
	events = json_node_get_array(node);
	changes = comments = worklogs = 0;

	for (i = 0; i < json_array_get_length(events); i++)
	{
		JsonObject *event;
		const gchar *kind;

		event = json_array_get_object_element(events, i);
		kind = venture_json_object_get_string(event, "kind", "");

		/* The roll-ups are the system's; everything else is Alice's. */
		if (0 != g_strcmp0(venture_json_object_get_string(event, "actor_kind",
		                                                  ""), "system"))
			g_assert_cmpstr(venture_json_object_get_string(event, "actor",
			                                               ""), ==, "alice");

		if (0 == g_strcmp0(kind, "change"))
		{
			changes++;

			/* Alice's one update is the close; the system's carry the
			 * roll-ups. */
			if ((0 == g_strcmp0(venture_json_object_get_string(event,
			                                                   "action", ""),
			                    "update")) &&
			    (0 == g_strcmp0(venture_json_object_get_string(event,
			                                                   "actor_kind",
			                                                   ""), "user")))
				g_assert_true(json_object_has_member(
					json_object_get_object_member(event, "changes"),
					"status"));
		}
		else if (0 == g_strcmp0(kind, "comment"))
			comments++;
		else if (0 == g_strcmp0(kind, "worklog"))
			worklogs++;
	}

	/* Create, the logged-hours roll-up and the close. */
	g_assert_cmpuint(changes, >=, 3);
	g_assert_cmpuint(comments, ==, 1);
	g_assert_cmpuint(worklogs, ==, 1);

	/* Newest first. */
	g_assert_cmpstr(venture_json_object_get_string(
		json_array_get_object_element(events, 0), "kind", ""), ==, "change");
}

/* --- Budgets, runs, incidents --------------------------------------------- */

static gint64
create_run(
	Fixture		*fixture,
	gint64		 ticket_id,
	VentureForgeRunState state,
	const gchar	*cost
){
	g_autoptr(VentureForgeRun) run = NULL;
	g_autoptr(VentureMoney) money = NULL;
	g_autoptr(GDateTime) now = NULL;

	now = venture_time_now();
	money = venture_money_from_string(cost, "USD", NULL);
	g_assert_nonnull(money);

	run = venture_forge_run_new();
	g_object_set(run, "ticket-id", ticket_id, "state", state, "cost", money,
	             "started-at", now, "input-tokens", (gint64)1000,
	             "output-tokens", (gint64)500,
	             "pull-request-number",
	             (VENTURE_FORGE_RUN_STATE_SUCCEEDED == state) ? (gint64)7
	                                                          : (gint64)0,
	             NULL);
	file_under_default(fixture, run);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(run),
	                                    NULL, NULL));

	return venture_entity_get_id(VENTURE_ENTITY(run));
}

/*
 * A budget sums the runs' own cost for its window, warns the admins once
 * at the line, and with a hard stop refuses the next run once exhausted.
 *
 * What breaks if this regresses: a run that starts after the money ran
 * out, or an admin told about the same budget on every run.
 */
static void
test_desk_budget_refuses_when_exhausted(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureAgentBudget) budget = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureMoney) limit = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *row;
	gint64 admin;

	(void)user_data;

	admin = create_user(fixture, "admin", VENTURE_USER_ROLE_ADMIN);
	create_user(fixture, "viewer", VENTURE_USER_ROLE_VIEWER);
	ticket = create_ticket(fixture, "Agent work", NULL, NULL);
	limit = venture_money_from_string("1.00", "USD", NULL);

	budget = venture_agent_budget_new();
	g_object_set(budget, "name", "Monthly agents", "limit", limit,
	             "period", VENTURE_BUDGET_PERIOD_MONTHLY,
	             "warn-percent", (gint64)50, "hard-stop", TRUE,
	             "active", TRUE, NULL);
	file_under_default(fixture, budget);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(budget), NULL, NULL));

	/* Nothing spent: allowed, nobody told. */
	g_assert_true(venture_factory_budget_allows_run(fixture->context, 0,
	                                                &error));
	g_assert_cmpint(venture_notify_unread_count(fixture->context, admin), ==, 0);

	/* Sixty cents: over the warning line, told once. */
	create_run(fixture, venture_entity_get_id(ticket),
	           VENTURE_FORGE_RUN_STATE_SUCCEEDED, "0.60");
	g_assert_true(venture_factory_budget_allows_run(fixture->context, 0,
	                                                &error));
	g_assert_true(venture_factory_budget_allows_run(fixture->context, 0,
	                                                &error));
	g_assert_cmpint(unread_of_kind(fixture, admin,
	                               VENTURE_NOTIFICATION_KIND_BUDGET), ==, 1);

	node = venture_factory_budgets_describe(fixture->context, NULL);
	row = json_array_get_object_element(json_node_get_array(node), 0);
	g_assert_cmpint(venture_json_object_get_int(row, "percent", 0), ==, 60);
	g_assert_true(venture_json_object_get_bool(row, "warning", FALSE));
	g_assert_false(venture_json_object_get_bool(row, "exhausted", TRUE));
	g_assert_cmpint(venture_json_object_get_int(row, "runs", 0), ==, 1);

	/* Another sixty: exhausted, told once more, and refused. */
	create_run(fixture, venture_entity_get_id(ticket),
	           VENTURE_FORGE_RUN_STATE_FAILED, "0.60");
	g_assert_false(venture_factory_budget_allows_run(fixture->context, 0,
	                                                 &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_nonnull(strstr(error->message, "Monthly agents"));
	g_clear_error(&error);
	g_assert_false(venture_factory_budget_allows_run(fixture->context, 0,
	                                                 &error));
	g_clear_error(&error);
	g_assert_cmpint(unread_of_kind(fixture, admin,
	                               VENTURE_NOTIFICATION_KIND_BUDGET), ==, 2);

	/* Without the hard stop it warns and lets it through. The check
	 * stamped the budget's warned-at and exhausted-at, so start from a
	 * fresh read. */
	{
		g_autoptr(VentureEntity) fresh = NULL;

		fresh = venture_database_get(fixture->database,
		                             VENTURE_TYPE_AGENT_BUDGET,
		                             venture_entity_get_id(VENTURE_ENTITY(budget)),
		                             NULL);
		g_object_set(fresh, "hard-stop", FALSE, NULL);
		g_assert_true(venture_database_save(fixture->database, fresh, NULL,
		                                    NULL));
	}
	g_assert_true(venture_factory_budget_allows_run(fixture->context, 0,
	                                                &error));
	g_assert_no_error(error);

	/* Mission control totals the same runs. */
	g_clear_pointer(&node, json_node_unref);
	node = venture_factory_runs_describe(fixture->context, NULL, 0, NULL, 0,
	                                     &error);
	g_assert_no_error(error);
	row = json_object_get_object_member(json_node_get_object(node), "totals");
	g_assert_cmpint(venture_json_object_get_int(row, "runs", 0), ==, 2);
	g_assert_cmpint(venture_json_object_get_int(row, "succeeded", 0), ==, 1);
	g_assert_cmpint(venture_json_object_get_int(row, "failed", 0), ==, 1);
	g_assert_cmpint(venture_json_object_get_int(row, "pull_requests", 0), ==, 1);
	g_assert_cmpint(venture_json_object_get_int(row, "input_tokens", 0), ==,
	                2000);
	g_assert_cmpstr(venture_json_object_get_string(row, "cost_display", ""),
	                ==, "$1.20");
	g_assert_cmpstr(venture_json_object_get_string(row,
		"cost_per_success_display", ""), ==, "$0.60");

	g_clear_pointer(&node, json_node_unref);
	node = venture_factory_runs_describe(fixture->context, NULL, 0, "failed", 0,
	                                     &error);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
		json_node_get_object(node), "runs")), ==, 1);
}

/*
 * An incident's fix is a bug, prioritised from the severity, linked both
 * ways, and made once.
 */
static void
test_desk_incident_opens_its_fix(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureIncident) incident = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureEntity) again = NULL;
	g_autoptr(GPtrArray) links = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor actor;
	VenturePriority priority;
	VentureIssueType issue_type;
	gint64 ticket_id = 0;

	(void)user_data;

	actor_for(&actor, "alice");
	incident = venture_incident_new();
	g_object_set(incident, "title", "Checkout down",
	             "severity", VENTURE_INCIDENT_SEVERITY_SEV1,
	             "summary", "500s on every order", NULL);
	file_under_default(fixture, incident);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(incident), NULL, NULL));

	ticket = venture_factory_open_fix_ticket(fixture->context,
	                                         VENTURE_ENTITY(incident), &actor,
	                                         &error);
	g_assert_no_error(error);
	g_assert_nonnull(ticket);
	g_object_get(ticket, "priority", &priority, "issue-type", &issue_type, NULL);
	g_assert_cmpint(priority, ==, VENTURE_PRIORITY_URGENT);
	g_assert_cmpint(issue_type, ==, VENTURE_ISSUE_TYPE_BUG);
	g_object_get(incident, "ticket-id", &ticket_id, NULL);
	g_assert_cmpint(ticket_id, ==, venture_entity_get_id(ticket));

	links = venture_record_link_find_for(fixture->database, "ticket",
	                                     venture_entity_get_id(ticket), NULL);
	g_assert_cmpuint(links->len, ==, 1);

	again = venture_factory_open_fix_ticket(fixture->context,
	                                        VENTURE_ENTITY(incident), &actor,
	                                        &error);
	g_assert_null(again);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
}

/*
 * The delivery report reads the four keys from the factory's records
 * and the agents' cost beside them.
 */
static void
test_desk_delivery_report(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEnvironment) production = NULL;
	g_autoptr(VentureRelease) release = NULL;
	g_autoptr(VentureDeployment) deployment = NULL;
	g_autoptr(VentureIncident) incident = NULL;
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GDateTime) earlier = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	JsonObject *object;
	JsonArray *metrics;
	gboolean saw_frequency = FALSE;
	gboolean saw_cost = FALSE;
	guint i;

	(void)user_data;

	now = venture_time_now();
	earlier = g_date_time_add_hours(now, -4);

	production = venture_environment_new();
	g_object_set(production, "name", "production",
	             "kind", VENTURE_ENVIRONMENT_KIND_PRODUCTION, "active", TRUE,
	             NULL);
	file_under_default(fixture, production);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(production), NULL, NULL));

	release = venture_release_new();
	g_object_set(release, "number", "1.0.0",
	             "status", VENTURE_RELEASE_STATUS_RELEASED,
	             "released-at", earlier, NULL);
	file_under_default(fixture, release);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(release), NULL, NULL));

	ticket = create_ticket(fixture, "Shipped thing", NULL, NULL);
	g_object_set(ticket, "release-id",
	             venture_entity_get_id(VENTURE_ENTITY(release)), NULL);
	g_assert_true(venture_database_save(fixture->database, ticket, NULL, NULL));

	deployment = venture_deployment_new();
	g_object_set(deployment,
	             "release-id", venture_entity_get_id(VENTURE_ENTITY(release)),
	             "environment-id",
	             venture_entity_get_id(VENTURE_ENTITY(production)),
	             "status", VENTURE_DEPLOYMENT_STATUS_SUCCEEDED,
	             "deployed-at", now, NULL);
	file_under_default(fixture, deployment);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(deployment), NULL, NULL));

	incident = venture_incident_new();
	g_object_set(incident, "title", "Blip",
	             "environment-id",
	             venture_entity_get_id(VENTURE_ENTITY(production)),
	             "deployment-id",
	             venture_entity_get_id(VENTURE_ENTITY(deployment)),
	             "started-at", earlier, "resolved-at", now,
	             "status", VENTURE_INCIDENT_STATUS_RESOLVED, NULL);
	file_under_default(fixture, incident);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(incident), NULL, NULL));

	create_run(fixture, venture_entity_get_id(ticket),
	           VENTURE_FORGE_RUN_STATE_SUCCEEDED, "0.25");

	report = venture_report_registry_lookup(
		venture_context_get_report_registry(fixture->context), "delivery");
	g_assert_nonnull(report);
	period = venture_date_range_new_all_time();
	result = venture_report_generate(report, fixture->context, period, NULL,
	                                 &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpuint(venture_report_result_get_row_count(result), ==, 1);

	node = venture_report_result_to_json(result);
	object = json_node_get_object(node);
	metrics = json_object_get_array_member(object, "metrics");

	for (i = 0; i < json_array_get_length(metrics); i++)
	{
		JsonObject *metric;
		const gchar *key;

		metric = json_array_get_object_element(metrics, i);
		key = venture_json_object_get_string(metric, "key", "");

		if (0 == g_strcmp0(key, "deployments"))
			g_assert_cmpint(venture_json_object_get_int(metric, "value", 0),
			                ==, 1);
		else if (0 == g_strcmp0(key, "change_failure_rate"))
			g_assert_cmpfloat(json_object_get_double_member(metric, "value"),
			                  ==, 1.0);
		else if (0 == g_strcmp0(key, "time_to_restore"))
			g_assert_cmpfloat(json_object_get_double_member(metric, "value"),
			                  >=, 3.9);
		else if (0 == g_strcmp0(key, "frequency"))
			saw_frequency = TRUE;
		else if (0 == g_strcmp0(key, "agent_cost"))
			saw_cost = TRUE;
		else if (0 == g_strcmp0(key, "pull_requests"))
			g_assert_cmpint(venture_json_object_get_int(metric, "value", 0),
			                ==, 1);
	}

	g_assert_true(saw_frequency);
	g_assert_true(saw_cost);
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/desk/types-belong-to-modules",
	                test_desk_types_belong_to_modules);
	g_test_add_func("/desk/mentions-are-extracted",
	                test_desk_mentions_are_extracted);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/desk/inbox-is-fed-by-the-audit-trail",
	    test_desk_inbox_is_fed_by_the_audit_trail);
	ADD("/desk/broadcast-respects-roles", test_desk_broadcast_respects_roles);
	ADD("/desk/service-levels", test_desk_service_levels);
	ADD("/desk/macro-applies-in-one-go", test_desk_macro_applies_in_one_go);
	ADD("/desk/worklogs-roll-up", test_desk_worklogs_roll_up);
	ADD("/desk/sprint-burn", test_desk_sprint_burn);
	ADD("/desk/bulk-is-all-or-nothing", test_desk_bulk_is_all_or_nothing);
	ADD("/desk/activity-weaves-the-story", test_desk_activity_weaves_the_story);
	ADD("/desk/budget-refuses-when-exhausted",
	    test_desk_budget_refuses_when_exhausted);
	ADD("/desk/incident-opens-its-fix", test_desk_incident_opens_its_fix);
	ADD("/desk/delivery-report", test_desk_delivery_report);

#undef ADD

	return g_test_run();
}
