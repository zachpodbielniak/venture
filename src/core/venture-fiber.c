/*
 * venture-fiber.c - Work on the main thread that can wait without
 * holding the main loop
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * VENTURE's database, registries and signal handlers belong to the main
 * thread, and its provider clients are synchronous. A request that waits
 * on Stripe therefore has to wait on the main thread, and a nested main
 * loop lets others in only at the price of nesting: the first waiter is
 * answered after the last, however long the last one takes.
 *
 * A fiber is that work on a stack of its own, still on the main thread.
 * When it has to wait it yields, the main loop goes back to what it was
 * doing, and whoever it was waiting on wakes it from the main loop when
 * the answer is in. Waits therefore end in the order their answers come,
 * and nothing runs beside main-thread code: there is still one thread,
 * which runs one stack at a time.
 *
 * What one stack leaves in shared state -- the access policy's actor, the
 * tenant service's request scope -- is the fiber's alone. Its owner gives
 * a swap function, called on the way in and again on the way out, that
 * exchanges the live state with the fiber's own copy, so a fiber that
 * yields leaves the main loop as it found it and finds its own state as
 * it left it.
 *
 * Fibers run on the thread that made the first one, and only there.
 */

#include "venture.h"
#include "core/venture-fiber-private.h"

#include <errno.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>

/* The main thread's own default; VENTURE's handlers were written for it. */
#define VENTURE_FIBER_STACK_SIZE (8u * 1024u * 1024u)

struct _VentureFiber
{
	ucontext_t		 context;
	ucontext_t		 caller;
	guchar			*stack;
	gsize			 size;
	VentureFiberFunc	 func;
	VentureFiberFunc	 done_func;
	VentureFiberFunc	 swap;
	gpointer		 data;
	VentureFiber		*outer;		/* running when this one was woken */
	gint			 depth;		/* g_main_depth() it was woken at */
	gboolean		 started;
	gboolean		 done;
};

static GThread		*fiber_thread;
static VentureFiber	*fiber_current;
static VentureFiber	*fiber_starting;

static void
fiber_entry(void)
{
	VentureFiber *self = fiber_starting;

	fiber_starting = NULL;
	self->func(self->data);
	self->done = TRUE;
	swapcontext(&self->context, &self->caller);
	g_assert_not_reached();
}

static void
fiber_free(VentureFiber *self)
{
	munmap(self->stack, self->size);
	g_free(self);
}

VentureFiber *
venture_fiber_new(
	VentureFiberFunc	 func,
	VentureFiberFunc	 done,
	VentureFiberFunc	 swap,
	gpointer		 data
){
	VentureFiber *self;
	gsize page = (gsize)sysconf(_SC_PAGESIZE);

	g_return_val_if_fail(NULL != func, NULL);
	if (NULL == fiber_thread)
		fiber_thread = g_thread_self();
	g_return_val_if_fail(g_thread_self() == fiber_thread, NULL);

	self = g_new0(VentureFiber, 1);
	self->size = VENTURE_FIBER_STACK_SIZE;
	self->stack = mmap(NULL, self->size, PROT_READ | PROT_WRITE,
	                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
	if (MAP_FAILED == self->stack)
		g_error("Cannot map a fiber stack: %s", g_strerror(errno));
	/* An overflow faults on the guard instead of writing over the heap. */
	if (0 != mprotect(self->stack, page, PROT_NONE))
		g_warning("A fiber stack has no guard page: %s", g_strerror(errno));
	self->func = func;
	self->done_func = done;
	self->swap = swap;
	self->data = data;
	return self;
}

/*
 * Runs @self until it yields or finishes. A finished fiber's done function
 * runs here, on the waker's stack, and the fiber is freed.
 */
void
venture_fiber_wake(VentureFiber *self)
{
	g_return_if_fail(NULL != self);
	g_return_if_fail(g_thread_self() == fiber_thread);
	g_return_if_fail(!self->done && self != fiber_current);

	self->outer = fiber_current;
	self->depth = g_main_depth();
	fiber_current = self;
	if (NULL != self->swap)
		self->swap(self->data);
	if (!self->started)
	{
		getcontext(&self->context);
		self->context.uc_stack.ss_sp = self->stack;
		self->context.uc_stack.ss_size = self->size;
		self->context.uc_link = NULL;
		makecontext(&self->context, fiber_entry, 0);
		fiber_starting = self;
		self->started = TRUE;
	}
	swapcontext(&self->caller, &self->context);
	if (NULL != self->swap)
		self->swap(self->data);
	fiber_current = self->outer;

	if (self->done)
	{
		if (NULL != self->done_func)
			self->done_func(self->data);
		fiber_free(self);
	}
}

/*
 * The fiber running on this thread, or %NULL on a plain stack.
 *
 * A main loop the fiber runs itself (a nested wait on its own stack) is a
 * plain stack too: what it dispatches did not start the fiber, and a yield
 * from there would leave that loop's dispatch suspended on the fiber's
 * stack while the outer loop dispatches on.
 */
VentureFiber *
venture_fiber_current(void)
{
	if (g_thread_self() != fiber_thread || NULL == fiber_current)
		return NULL;
	return (g_main_depth() == fiber_current->depth) ? fiber_current : NULL;
}

/* Gives the thread back to whoever woke the running fiber. */
void
venture_fiber_yield(void)
{
	VentureFiber *self = venture_fiber_current();

	g_return_if_fail(NULL != self);
	swapcontext(&self->context, &self->caller);
}
