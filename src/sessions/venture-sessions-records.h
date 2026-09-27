/*
 * venture-sessions-records.h - Sessions and what they yielded
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The sessions module's two record types. A session is a time-boxed
 * stretch of effort whose outcome is measured; a yield is one thing it
 * produced -- units of a product, or an amount of money. Both are field
 * tables and nothing else: the rules that span rows live in
 * venture-sessions.c as save validators, so every writer obeys them.
 */

#ifndef VENTURE_SESSIONS_RECORDS_H
#define VENTURE_SESSIONS_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

#define VENTURE_TYPE_SESSION (venture_session_get_type())
VENTURE_DECLARE_ENTITY(VentureSession, venture_session, SESSION)

#define VENTURE_TYPE_SESSION_YIELD (venture_session_yield_get_type())
VENTURE_DECLARE_ENTITY(VentureSessionYield, venture_session_yield, SESSION_YIELD)

G_END_DECLS

#endif /* VENTURE_SESSIONS_RECORDS_H */
