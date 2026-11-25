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

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/fiber/yield-and-wake", test_yield_and_wake);
	g_test_add_func("/fiber/nested-dispatch-is-not-the-fiber", test_nested_dispatch_is_not_the_fiber);
	return g_test_run();
}
