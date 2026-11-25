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

G_END_DECLS

#endif /* VENTURE_FIBER_PRIVATE_H */
