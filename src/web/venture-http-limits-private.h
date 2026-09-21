/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_HTTP_LIMITS_PRIVATE_H
#define VENTURE_HTTP_LIMITS_PRIVATE_H
#include <venture.h>
gboolean venture_http_limits_validate(VentureConfig *config, GError **error);
void venture_http_limits_install(SoupServer *server, VentureConfig *config);
#endif
