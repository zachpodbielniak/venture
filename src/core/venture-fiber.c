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
 * Fibers run on the thread venture_fiber_init() named, and only there.
 * That thread is fixed once, before any other thread can ask: the Stripe
 * transport asks venture_fiber_current() from whichever thread calls it.
 *
 * Built with AddressSanitizer (gcc defines __SANITIZE_ADDRESS__ under
 * -fsanitize=address), every switch is announced to it, so it tracks the
 * stack actually in use instead of reporting the fiber's as overflow.
 */

#include "venture.h"
#include "core/venture-fiber-private.h"

#include <errno.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__SANITIZE_ADDRESS__)
#define VENTURE_FIBER_ASAN (1)
#include <sanitizer/common_interface_defs.h>
#endif

/* The main thread's own default; VENTURE's handlers were written for it. */
#define VENTURE_FIBER_STACK_SIZE (8u * 1024u * 1024u)

/* Below the stack, never mapped readable: an overflow faults here instead
 * of writing into whatever mapping lies beneath. One page is too little:
 * a frame with a large local array steps clean over it. */
#define VENTURE_FIBER_GUARD_SIZE (64u * 1024u)

struct _VentureFiber
{
	ucontext_t		 context;
	ucontext_t		 caller;
	guchar			*mapping;	/* guard, then stack */
	gsize			 mapping_size;
	VentureFiberFunc	 func;
	VentureFiberFunc	 done_func;
	VentureFiberFunc	 swap;
	gpointer		 data;
	VentureFiber		*outer;		/* running when this one was woken */
	gint			 depth;		/* g_main_depth() it was woken at */
	gboolean		 started;
	gboolean		 done;
#ifdef VENTURE_FIBER_ASAN
	void			*fake_stack;	/* the switched-away stack's */
	const void		*caller_bottom;	/* the waker's stack, for the way back */
	gsize			 caller_size;
#endif
};

static GThread		*fiber_thread;		/* atomic: read from any thread */
static VentureFiber	*fiber_current;
static VentureFiber	*fiber_starting;
static guint		 fiber_live;		/* main thread only */

/* --- Sanitizer bookkeeping ----------------------------------------------- */

/* Each switch is told to ASan before it happens (start) and confirmed by the
 * stack it lands on (finish). Without them, ASan sees a stack pointer far
 * outside the thread's stack and reports the first fiber frame it checks. */

#ifdef VENTURE_FIBER_ASAN
static void
fiber_asan_enter(VentureFiber *self, void **save)
{
	__sanitizer_start_switch_fiber(save, self->mapping + VENTURE_FIBER_GUARD_SIZE, VENTURE_FIBER_STACK_SIZE);
}

static void
fiber_asan_entered(VentureFiber *self)
{
	__sanitizer_finish_switch_fiber(self->fake_stack, &self->caller_bottom, &self->caller_size);
}

static void
fiber_asan_leave(VentureFiber *self, gboolean finished)
{
	/* A finished fiber's stack is about to be unmapped: no fake stack. */
	__sanitizer_start_switch_fiber(finished ? NULL : &self->fake_stack, self->caller_bottom, self->caller_size);
}

static void
fiber_asan_left(void *save)
{
	__sanitizer_finish_switch_fiber(save, NULL, NULL);
}
#else
#define fiber_asan_enter(self, save) ((void)(self), (void)(save))
#define fiber_asan_entered(self) ((void)(self))
#define fiber_asan_leave(self, finished) ((void)(self), (void)(finished))
#define fiber_asan_left(save) ((void)(save))
#endif

/* --- Life cycle ------------------------------------------------------------ */

static GThread *
fiber_owner(void)
{
	return (GThread *)g_atomic_pointer_get(&fiber_thread);
}

void
venture_fiber_init(void)
{
	/* The first caller wins; a second server in the same process (the
	 * tests start several) finds it already set. */
	(void)g_atomic_pointer_compare_and_exchange(&fiber_thread, NULL, g_thread_self());
}

guint
venture_fiber_count(void)
{
	return fiber_live;
}

static void
fiber_entry(void)
{
	VentureFiber *self = fiber_starting;

	fiber_starting = NULL;
	fiber_asan_entered(self);
	self->func(self->data);
	self->done = TRUE;
	fiber_asan_leave(self, TRUE);
	swapcontext(&self->context, &self->caller);
	g_assert_not_reached();
}

static void
fiber_free(VentureFiber *self)
{
	munmap(self->mapping, self->mapping_size);
	g_free(self);
	fiber_live--;
}

VentureFiber *
venture_fiber_new(
	VentureFiberFunc	 func,
	VentureFiberFunc	 done,
	VentureFiberFunc	 swap,
	gpointer		 data
){
	VentureFiber *self;

	g_return_val_if_fail(NULL != func, NULL);
	g_return_val_if_fail(NULL != fiber_owner(), NULL);
	g_return_val_if_fail(g_thread_self() == fiber_owner(), NULL);

	self = g_new0(VentureFiber, 1);
	self->mapping_size = VENTURE_FIBER_GUARD_SIZE + VENTURE_FIBER_STACK_SIZE;
	self->mapping = mmap(NULL, self->mapping_size, PROT_READ | PROT_WRITE,
	                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
	if (MAP_FAILED == self->mapping)
	{
		/* Out of address space or over a limit: this request is refused,
		 * the server goes on. */
		g_warning("Cannot map a fiber stack: %s", g_strerror(errno));
		g_free(self);
		return NULL;
	}
	/* Stacks grow down on every architecture VENTURE builds for (x86_64,
	 * aarch64), so the guard is the mapping's lowest part. A stack that
	 * cannot be guarded is not used: an overflow would land unseen. */
	if (0 != mprotect(self->mapping, VENTURE_FIBER_GUARD_SIZE, PROT_NONE))
	{
		g_warning("Cannot guard a fiber stack: %s", g_strerror(errno));
		munmap(self->mapping, self->mapping_size);
		g_free(self);
		return NULL;
	}
	self->func = func;
	self->done_func = done;
	self->swap = swap;
	self->data = data;
	fiber_live++;
	return self;
}

/*
 * Runs @self until it yields or finishes. A finished fiber's done function
 * runs here, on the waker's stack, and the fiber is freed.
 */
void
venture_fiber_wake(VentureFiber *self)
{
	void *save = NULL;

	g_return_if_fail(NULL != self);
	g_return_if_fail(g_thread_self() == fiber_owner());
	g_return_if_fail(!self->done && self != fiber_current);

	self->outer = fiber_current;
	self->depth = g_main_depth();
	fiber_current = self;
	if (NULL != self->swap)
		self->swap(self->data);
	if (!self->started)
	{
		getcontext(&self->context);
		self->context.uc_stack.ss_sp = self->mapping + VENTURE_FIBER_GUARD_SIZE;
		self->context.uc_stack.ss_size = VENTURE_FIBER_STACK_SIZE;
		self->context.uc_link = NULL;
		makecontext(&self->context, fiber_entry, 0);
		fiber_starting = self;
		self->started = TRUE;
	}
	fiber_asan_enter(self, &save);
	swapcontext(&self->caller, &self->context);
	fiber_asan_left(save);
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
 * The fiber running on this thread, or %NULL on a plain stack or on any
 * other thread.
 *
 * A main loop the fiber runs itself (a nested wait on its own stack) is a
 * plain stack too: what it dispatches did not start the fiber, and a yield
 * from there would leave that loop's dispatch suspended on the fiber's
 * stack while the outer loop dispatches on.
 */
VentureFiber *
venture_fiber_current(void)
{
	if (g_thread_self() != fiber_owner() || NULL == fiber_current)
		return NULL;
	return (g_main_depth() == fiber_current->depth) ? fiber_current : NULL;
}

/* Gives the thread back to whoever woke the running fiber. */
void
venture_fiber_yield(void)
{
	VentureFiber *self = venture_fiber_current();

	g_return_if_fail(NULL != self);
	fiber_asan_leave(self, FALSE);
	swapcontext(&self->context, &self->caller);
	fiber_asan_entered(self);
}

/*
 * Holds: one stack at a time for whatever a key names.
 *
 * Two fibers may each be part-way through the same piece of work -- two
 * Checkouts for one customer, say -- because each yields while it waits
 * on Stripe and the other runs meanwhile. Before fibers the second could
 * not start until the first had finished. A hold puts that back for the
 * work that needs it: whoever holds a key goes on, and a fiber that asks
 * for it meanwhile yields until the holder lets go, then holds it in turn,
 * in the order they asked.
 *
 * A plain stack takes a free key as a fiber does, and while it holds one
 * nothing else runs: it blocks rather than yields. It is never made to
 * wait for a key a yielded fiber holds -- waiting would mean a nested main
 * loop, which VENTURE does not run -- and goes on unguarded, as it would
 * have without holds. So does anything off the fibers' thread.
 *
 * The stack that holds a key may take it again; each take is released.
 */
typedef struct
{
	VentureFiber		*fiber;
	VentureDatabase		*database;	/* resumed outside its transaction */
	gulong			 finished;	/* waiting for that transaction's end */
} HoldWaiter;	/* on the waiting fiber's stack, mapped until it is woken */

struct _VentureFiberHold
{
	gchar			*key;
	VentureFiber		*owner;		/* %NULL: a plain stack */
	gint			 depth;		/* a plain owner's g_main_depth() */
	guint			 count;
	GQueue			 waiters;	/* HoldWaiter, first asked first */
};

static GHashTable	*fiber_holds;

/* Whether the calling stack is the one holding @hold. */
static gboolean
hold_is_mine(const VentureFiberHold *hold)
{
	VentureFiber *self = venture_fiber_current();

	if (NULL != hold->owner)
		return hold->owner == self;
	return NULL == self && NULL == fiber_current && hold->depth == g_main_depth();
}

static gboolean hold_wake(gpointer data);

/* Resumes @waiter from the main loop, not from the caller's stack. At the
 * default priority, as requests are, so a busy loop cannot starve it. */
static void
hold_schedule(HoldWaiter *waiter)
{
	GSource *source = g_idle_source_new();

	g_source_set_priority(source, G_PRIORITY_DEFAULT);
	g_source_set_callback(source, hold_wake, waiter, NULL);
	g_source_set_static_name(source, "venture-fiber-hold");
	g_source_attach(source, g_main_context_default());
	g_source_unref(source);
}

/* The database's transaction-finished: asked again once it has ended. */
static void
hold_transaction_finished(VentureDatabase *database, gboolean committed, gpointer data)
{
	HoldWaiter *waiter = data;

	(void)committed;
	g_signal_handler_disconnect(database, waiter->finished);
	waiter->finished = 0;
	hold_schedule(waiter);
}

static gboolean
hold_wake(gpointer data)
{
	HoldWaiter *waiter = data;

	/* A nested loop inside a transaction may be dispatching this: wait
	 * for the transaction to end, which the database says. */
	if (NULL != waiter->database && venture_database_has_transaction(waiter->database))
	{
		waiter->finished = g_signal_connect(waiter->database, "transaction-finished",
			G_CALLBACK(hold_transaction_finished), waiter);
		return G_SOURCE_REMOVE;
	}
	venture_fiber_wake(waiter->fiber);
	return G_SOURCE_REMOVE;
}

/*
 * Takes @key for the calling stack. A fiber waits for it as above; once
 * handed it, it is resumed from the main loop, and only while no
 * transaction is open on @database (if given): one that is open when it
 * is due is waited out, and the waiter resumed when the database says
 * it has finished. Returns the hold to give venture_fiber_release(), or
 * %NULL where the caller cannot have it (and so goes on unguarded).
 * Never call it with a transaction open: a fiber that waits here yields,
 * and whatever runs meanwhile would write inside it.
 */
VentureFiberHold *
venture_fiber_hold(const gchar *key, VentureDatabase *database)
{
	VentureFiberHold *hold;
	VentureFiber *self;

	g_return_val_if_fail(NULL != key, NULL);
	if (NULL == fiber_owner() || g_thread_self() != fiber_owner())
		return NULL;
	if (NULL == fiber_holds)
		fiber_holds = g_hash_table_new(g_str_hash, g_str_equal);

	self = venture_fiber_current();
	hold = g_hash_table_lookup(fiber_holds, key);
	if (NULL != hold && hold_is_mine(hold))
	{
		hold->count++;
		return hold;
	}
	if (NULL != hold)
	{
		HoldWaiter waiter;

		/* A plain stack, or a fiber's nested frame: no waiting. */
		if (NULL == self)
			return NULL;
		/* venture_fiber_release() hands it over, then wakes this. */
		waiter.fiber = self;
		waiter.database = database;
		waiter.finished = 0;
		g_queue_push_tail(&hold->waiters, &waiter);
		venture_fiber_yield();
		g_warn_if_fail(hold->owner == self);
		return hold;
	}

	hold = g_new0(VentureFiberHold, 1);
	hold->key = g_strdup(key);
	hold->owner = self;
	hold->depth = g_main_depth();
	hold->count = 1;
	g_queue_init(&hold->waiters);
	g_hash_table_insert(fiber_holds, hold->key, hold);
	return hold;
}

/* Lets @hold go: to the first fiber waiting for it, if one is. */
void
venture_fiber_release(VentureFiberHold *hold)
{
	HoldWaiter *next;

	if (NULL == hold)
		return;
	g_return_if_fail(hold->count > 0);
	if (--hold->count > 0)
		return;

	next = g_queue_pop_head(&hold->waiters);
	if (NULL != next)
	{
		/* Woken from the main loop, not from here: the releaser may be
		 * deep in its own stack, and the next holder starts its turn. */
		hold->owner = next->fiber;
		hold->count = 1;
		hold_schedule(next);
		return;
	}
	g_hash_table_remove(fiber_holds, hold->key);
	g_free(hold->key);
	g_free(hold);
}
