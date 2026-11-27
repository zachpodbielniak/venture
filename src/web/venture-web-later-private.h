/*
 * venture-web-later-private.h - Routes answered from a fiber of the main
 * loop (venture-web-later.inc)
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef VENTURE_WEB_LATER_PRIVATE_H
#define VENTURE_WEB_LATER_PRIVATE_H

#include <venture.h>

G_BEGIN_DECLS

/*
 * Answers @request with @callback from a fiber of the main loop, after
 * this dispatch has returned. Returns the placeholder the route answers
 * with. Call it from a route, inside the request pipeline; if the
 * pipeline answers with something else instead, @callback never runs.
 */
HtmxResponse *
venture_web_answer_later(VentureWebServer *self, HtmxRequest *request,
	GHashTable *params, HtmxRouteCallback callback);

G_END_DECLS

#endif /* VENTURE_WEB_LATER_PRIVATE_H */
