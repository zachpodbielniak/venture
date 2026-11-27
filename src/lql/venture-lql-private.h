/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-lql-private.h - What the LQL sources and functions share
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * One VentureLqlCaller is made per environment and shared, by reference,
 * by every source and function in it. It is the caller: who they are,
 * which businesses an unqualified fetch is scoped to, which record types
 * they may read, and the reader the access policy judges every row as.
 */

#ifndef VENTURE_LQL_PRIVATE_H
#define VENTURE_LQL_PRIVATE_H

#include "venture.h"
#include "lql/venture-lql.h"

G_BEGIN_DECLS

/*
 * VentureLqlCaller:
 * @context: the wiring (owned reference)
 * @database: the repository rows are read from (owned reference)
 * @registry: the record types (owned reference)
 * @reader: the access policy's copy of the caller's reader, re-entered
 *   around every read
 * @principal: a copy of the caller
 * @username: (nullable): the caller's username, what me() answers
 * @organizations: (element-type gint64): the selected business and every
 *   business beneath it; empty when nothing is selected
 * @readable: GType -> GType: the record types the caller may read
 * @max_rows: query.max_rows
 * @teams: what my_teams() answers, read when the environment is built
 * @timezone: the operator's calendar (owned reference), captured when the
 *   environment is built
 * @fiscal_year_start_month: the month the fiscal year begins, 1-12
 * @home_thread: the thread that built the environment, the database's
 *   (not a reference): the only one a source may read on
 *
 * Everything a function reads is captured while the environment is built,
 * on the database's thread: a query is evaluated on a worker
 * (docs/lql.org, "Where a query runs"), and no worker may touch the
 * database, the configuration or the context.
 *
 * Reference counted with g_rc_box; venture_lql_caller_ref() and
 * venture_lql_caller_unref() below.
 */
typedef struct
{
	VentureContext		*context;
	VentureDatabase		*database;
	VentureEntityRegistry	*registry;
	VentureAccessSnapshot	*reader;
	VentureAuthPrincipal	*principal;
	gchar			*username;
	GArray			*organizations;
	GHashTable		*readable;
	gint64			 max_rows;
	LqlValue		*teams;
	GTimeZone		*timezone;
	gint			 fiscal_year_start_month;
	GThread			*home_thread;
} VentureLqlCaller;

/* Takes a reference. */
VentureLqlCaller *
venture_lql_caller_ref(VentureLqlCaller *caller);

/* Drops a reference; the last one frees everything the caller holds. */
void
venture_lql_caller_unref(VentureLqlCaller *caller);

/* Whether @caller may read @entity_type (a module that is on, and the
 * gate said yes when the environment was built). */
gboolean
venture_lql_caller_can_read(
	VentureLqlCaller	*caller,
	GType			 entity_type
);

/* A source reading @entity_type, registered as @name, as @caller. */
VentureLqlSource *
venture_lql_source_new(
	VentureLqlCaller	*caller,
	GType			 entity_type,
	const gchar		*name
);

/*
 * Builds @self's schema now, on the database's thread, so a worker that
 * evaluates a query never reaches the entity registry.
 */
void
venture_lql_source_prepare(VentureLqlSource *self);

/* Registers me(), my_teams(), today(), period() and organization(). */
gboolean
venture_lql_register_functions(
	LqlEnvironment		 *environment,
	VentureLqlCaller	 *caller,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_LQL_PRIVATE_H */
