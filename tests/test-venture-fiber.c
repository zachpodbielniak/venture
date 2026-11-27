/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "core/venture-fiber-private.h"

typedef struct
{
	GString		*log;
	VentureFiber	*seen;		/* venture_fiber_current() inside the nested dispatch */
	gboolean	 nested_ran;
	gboolean	 done;
} Trace;

static void
trace_two_turns(gpointer data)
{
	Trace *trace = data;

	g_string_append(trace->log, "a");
	venture_fiber_yield();
	g_string_append(trace->log, "c");
}

static void
trace_done(gpointer data)
{
	((Trace *)data)->done = TRUE;
}

/* A fiber runs until it yields, the waker goes on, and the next wake
 * resumes it where it left off; its done function runs once it returns. */
static void
test_yield_and_wake(void)
{
	Trace trace = { g_string_new(NULL), NULL, FALSE, FALSE };
	VentureFiber *fiber = venture_fiber_new(trace_two_turns, trace_done, NULL, &trace);

	venture_fiber_wake(fiber);
	g_string_append(trace.log, "b");
	g_assert_false(trace.done);
	g_assert_null(venture_fiber_current());
	venture_fiber_wake(fiber);
	g_assert_true(trace.done);
	g_assert_cmpstr(trace.log->str, ==, "abc");
	g_string_free(trace.log, TRUE);
}

static gboolean
nested_source(gpointer data)
{
	Trace *trace = data;

	trace->seen = venture_fiber_current();
	trace->nested_ran = TRUE;
	return G_SOURCE_REMOVE;
}

static void
run_nested_loop(gpointer data)
{
	Trace *trace = data;

	g_idle_add(nested_source, trace);
	while (!trace->nested_ran)
		g_main_context_iteration(NULL, TRUE);
}

static gboolean
start_fiber(gpointer data)
{
	venture_fiber_wake(venture_fiber_new(run_nested_loop, trace_done, NULL, data));
	return G_SOURCE_REMOVE;
}

/*
 * Work a fiber's own nested main loop dispatches is not that fiber: it
 * did not start it, and yielding from it would leave the loop's dispatch
 * suspended on the fiber's stack while the outer loop dispatches on.
 */
static void
test_nested_dispatch_is_not_the_fiber(void)
{
	Trace trace = { NULL, NULL, FALSE, FALSE };

	g_idle_add(start_fiber, &trace);
	while (!trace.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_true(trace.nested_ran);
	g_assert_null(trace.seen);
}

/* A fiber is counted from its making to its end, which is what a stop
 * waits on; one never started is still counted. */
static void
test_count(void)
{
	Trace trace = { g_string_new(NULL), NULL, FALSE, FALSE };
	guint before = venture_fiber_count();
	VentureFiber *fiber = venture_fiber_new(trace_two_turns, trace_done, NULL, &trace);

	g_assert_nonnull(fiber);
	g_assert_cmpuint(venture_fiber_count(), ==, before + 1);
	venture_fiber_wake(fiber);
	g_assert_cmpuint(venture_fiber_count(), ==, before + 1);
	venture_fiber_wake(fiber);
	g_assert_cmpuint(venture_fiber_count(), ==, before);
	g_string_free(trace.log, TRUE);
}

/* Not the thread fibers belong to: no fiber is current there, whatever
 * runs on the main thread. */
static gpointer
current_elsewhere(gpointer data)
{
	(void)data;
	return venture_fiber_current();
}

static void
ask_elsewhere(gpointer data)
{
	Trace *trace = data;
	GThread *thread = g_thread_new("elsewhere", current_elsewhere, NULL);

	trace->seen = g_thread_join(thread);
	g_assert_nonnull(venture_fiber_current());
}

static void
test_current_on_another_thread(void)
{
	Trace trace = { NULL, (VentureFiber *)&trace, FALSE, FALSE };

	venture_fiber_wake(venture_fiber_new(ask_elsewhere, trace_done, NULL, &trace));
	g_assert_true(trace.done);
	g_assert_null(trace.seen);
}

/* A deep frame on the fiber's stack: well within its eight megabytes, and
 * past what a one-page guard would have caught as overflow. */
static void
use_deep_stack(gpointer data)
{
	volatile guchar frame[256 * 1024];
	gsize i;

	for (i = 0; i < sizeof frame; i += 4096)
		frame[i] = 0xa5;
	*(gboolean *)data = (0xa5 == frame[sizeof frame - 4096]);
}

static void
test_deep_stack(void)
{
	gboolean ok = FALSE;

	venture_fiber_wake(venture_fiber_new(use_deep_stack, NULL, NULL, &ok));
	g_assert_true(ok);
}

/* Holds: a log of who held "key" when, and a yield to stand in for a
 * wait on Stripe while holding it. */
typedef struct
{
	GString		*log;
	VentureFiber	*parked;	/* the holder, yielded */
	gchar		 name;
	gint		 done;
} Holder;

static void
holder_run(gpointer data)
{
	Holder *holder = data;
	g_autoptr(VentureFiberHold) hold = venture_fiber_hold("key", NULL);
	g_autoptr(VentureFiberHold) again = venture_fiber_hold("key", NULL);

	g_assert_nonnull(hold);
	g_assert_true(again == hold);	/* the holder may take it again */
	g_string_append_c(holder->log, holder->name);
	holder->parked = venture_fiber_current();
	venture_fiber_yield();
	holder->parked = NULL;
	g_string_append_c(holder->log, g_ascii_toupper(holder->name));
}

static void
holder_done(gpointer data)
{
	((Holder *)data)->done++;
}

/* Wakes whichever holder is parked, from the main loop, until all are done. */
static void
holders_finish(Holder *holders, guint n)
{
	guint i, done;

	for (;;)
	{
		for (done = 0, i = 0; i < n; i++)
		{
			done += holders[i].done;
			if (NULL != holders[i].parked)
				venture_fiber_wake(holders[i].parked);
		}
		if (done == n)
			break;
		g_main_context_iteration(NULL, FALSE);
	}
}

/*
 * Fibers that ask for a key another holds wait for it, and hold it in
 * the order they asked: each one's turn starts after the last one's has
 * ended, though each yields part-way. Without the hold this is "abcABC".
 */
static void
test_hold_one_at_a_time(void)
{
	Holder holders[3];
	guint i;
	g_autoptr(GString) log = g_string_new(NULL);

	for (i = 0; i < G_N_ELEMENTS(holders); i++)
	{
		holders[i].log = log;
		holders[i].parked = NULL;
		holders[i].name = (gchar)('a' + i);
		holders[i].done = 0;
	}
	for (i = 0; i < G_N_ELEMENTS(holders); i++)
		venture_fiber_wake(venture_fiber_new(holder_run, holder_done, NULL, &holders[i]));
	holders_finish(holders, G_N_ELEMENTS(holders));
	g_assert_cmpstr(log->str, ==, "aAbBcC");
}

typedef struct
{
	VentureDatabase	*database;
	gboolean	 held;
} TransactionHolder;

static void
transaction_holder_run(gpointer data)
{
	TransactionHolder *holder = data;
	g_autoptr(VentureFiberHold) hold = venture_fiber_hold("key", holder->database);

	g_assert_nonnull(hold);
	g_assert_false(venture_database_has_transaction(holder->database));
	holder->held = TRUE;
}

static void
count_done(gpointer data)
{
	(void)data;
}

/* Whether a timer is attached to the default context: what a waiter
 * that polls for the transaction's end would leave behind. */
static gboolean
default_context_has_timer(void)
{
	GMainContext *context = g_main_context_default();
	GPollFD fds[32];
	gint priority = 0, timeout = -1;

	g_assert_true(g_main_context_acquire(context));
	g_main_context_prepare(context, &priority);
	g_main_context_query(context, priority, &timeout, fds, G_N_ELEMENTS(fds));
	g_main_context_check(context, priority, fds, 0);
	g_main_context_release(context);
	return timeout >= 0;
}

/* A fiber handed the key while a transaction is open resumes once the
 * transaction ends, woken by its end: nothing polls meanwhile. */
static void
test_hold_waits_for_transaction_end(void)
{
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(GError) error = NULL;
	TransactionHolder holder = { NULL, FALSE };
	VentureFiberHold *hold;
	guint before = venture_fiber_count();
	guint refs;

	g_assert_nonnull(database);
	holder.database = database;
	/* Held by this plain stack, at depth 0, so the fiber queues. */
	hold = venture_fiber_hold("key", NULL);
	g_assert_nonnull(hold);
	g_assert_cmpuint(venture_fiber_hold_waiting("key"), ==, 0);
	refs = (guint)g_atomic_int_get(&G_OBJECT(database)->ref_count);
	venture_fiber_wake(venture_fiber_new(transaction_holder_run, count_done, NULL, &holder));
	/* Queued, and keeping the database it is to be resumed outside of
	 * until it has been: a stop that dropped the last other reference
	 * meanwhile would otherwise leave the wake reading a freed object. */
	g_assert_cmpuint(venture_fiber_hold_waiting("key"), ==, 1);
	g_assert_cmpuint((guint)g_atomic_int_get(&G_OBJECT(database)->ref_count), ==, refs + 1);
	g_assert_true(venture_database_begin(database, &error));
	g_assert_no_error(error);
	venture_fiber_release(hold);
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_assert_false(holder.held);
	g_assert_false(default_context_has_timer());

	g_assert_true(venture_database_commit(database, &error));
	g_assert_no_error(error);
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_assert_true(holder.held);
	g_assert_cmpuint(venture_fiber_count(), ==, before);
	g_assert_false(default_context_has_timer());
	g_assert_cmpuint(venture_fiber_hold_waiting("key"), ==, 0);
	g_assert_cmpuint((guint)g_atomic_int_get(&G_OBJECT(database)->ref_count), ==, refs);
}

/* An inner rollback ends nothing: the outer transaction is still open,
 * and the fiber waits on until that ends too, then resumes at once. */
static void
test_hold_waits_for_outermost_end(void)
{
	g_autoptr(VentureDatabase) database = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(GError) error = NULL;
	TransactionHolder holder = { NULL, FALSE };
	VentureFiberHold *hold;
	guint before = venture_fiber_count();

	g_assert_nonnull(database);
	holder.database = database;
	hold = venture_fiber_hold("key", NULL);
	g_assert_nonnull(hold);
	venture_fiber_wake(venture_fiber_new(transaction_holder_run, count_done, NULL, &holder));
	g_assert_true(venture_database_begin(database, &error));
	g_assert_true(venture_database_begin(database, &error));
	g_assert_no_error(error);
	venture_fiber_release(hold);
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_assert_false(holder.held);

	venture_database_rollback(database);	/* the inner level */
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_assert_false(holder.held);
	g_assert_false(default_context_has_timer());

	venture_database_rollback(database);	/* the outermost */
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_assert_true(holder.held);
	g_assert_cmpuint(venture_fiber_count(), ==, before);
	g_assert_false(default_context_has_timer());
}

/* A plain stack never waits for a key a yielded fiber holds: it gets no
 * hold and goes on, as it would have without holds. */
static void
test_hold_plain_does_not_wait(void)
{
	g_autoptr(GString) log = g_string_new(NULL);
	Holder holder = { NULL, NULL, 'a', 0 };

	holder.log = log;
	venture_fiber_wake(venture_fiber_new(holder_run, holder_done, NULL, &holder));
	g_assert_nonnull(holder.parked);
	g_assert_null(venture_fiber_hold("key", NULL));
	holders_finish(&holder, 1);
	g_assert_cmpstr(log->str, ==, "aA");
	/* Let go: free again for anybody. */
	{
		VentureFiberHold *hold = venture_fiber_hold("key", NULL);
		g_assert_nonnull(hold);
		venture_fiber_release(hold);
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_fiber_init();
	g_test_add_func("/fiber/yield-and-wake", test_yield_and_wake);
	g_test_add_func("/fiber/nested-dispatch-is-not-the-fiber", test_nested_dispatch_is_not_the_fiber);
	g_test_add_func("/fiber/count", test_count);
	g_test_add_func("/fiber/current-on-another-thread", test_current_on_another_thread);
	g_test_add_func("/fiber/deep-stack", test_deep_stack);
	g_test_add_func("/fiber/hold/one-at-a-time", test_hold_one_at_a_time);
	g_test_add_func("/fiber/hold/waits-for-transaction-end", test_hold_waits_for_transaction_end);
	g_test_add_func("/fiber/hold/waits-for-outermost-end", test_hold_waits_for_outermost_end);
	g_test_add_func("/fiber/hold/plain-does-not-wait", test_hold_plain_does_not_wait);
	return g_test_run();
}
