/*
 * venture-fiber-private.h - Work on the main thread that can wait without
 * holding the main loop
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef VENTURE_FIBER_PRIVATE_H
#define VENTURE_FIBER_PRIVATE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _VentureFiber VentureFiber;

typedef void (*VentureFiberFunc)(gpointer data);

/*
 * venture_fiber_init:
 *
 * Makes the calling thread the one fibers run on. Call it once, on the
 * main thread, before any fiber is made and before any other thread can
 * ask venture_fiber_current() (the web server does, as it starts). Later
 * calls change nothing.
 */
void
venture_fiber_init(void);

/*
 * venture_fiber_new:
 * @func: the work, run on the fiber's own stack
 * @done: (nullable): run on the waker's stack once @func has returned
 * @swap: (nullable): exchanges main-thread state with the fiber's own copy,
 *   called on the way in and again on the way out of every turn
 * @data: for all three
 *
 * Returns: (nullable): a fiber not yet started, or %NULL when its stack
 *   cannot be mapped (the caller answers "busy" instead of aborting)
 */
VentureFiber *
venture_fiber_new(
	VentureFiberFunc	 func,
	VentureFiberFunc	 done,
	VentureFiberFunc	 swap,
	gpointer		 data
);

void
venture_fiber_wake(VentureFiber *self);

VentureFiber *
venture_fiber_current(void);

void
venture_fiber_yield(void);

/*
 * venture_fiber_count:
 *
 * Returns: how many fibers have been made and not yet finished, started
 *   or not; shutdown waits for this to reach zero
 */
guint
venture_fiber_count(void);

/*
 * One stack at a time for whatever a key names: see venture_fiber_hold()
 * in venture-fiber.c. %NULL is "not held": go on unguarded.
 */
typedef struct _VentureFiberHold VentureFiberHold;

/* Needs VentureDatabase: include <venture.h> first. */
VentureFiberHold *
venture_fiber_hold(const gchar *key, VentureDatabase *database);

void
venture_fiber_release(VentureFiberHold *hold);

/*
 * venture_fiber_hold_waiting:
 * @key: a key venture_fiber_hold() was given
 *
 * Returns: how many fibers are queued for @key behind its holder; 0 when
 *   nobody holds it. For tests, which wait on this rather than on
 *   venture_fiber_count() so a race is set up by what it proves.
 */
guint
venture_fiber_hold_waiting(const gchar *key);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureFiberHold, venture_fiber_release)

G_END_DECLS

#endif /* VENTURE_FIBER_PRIVATE_H */
