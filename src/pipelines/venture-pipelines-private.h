/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PIPELINES_PRIVATE_H
#define VENTURE_PIPELINES_PRIVATE_H
#include "venture.h"
gboolean venture_pipelines_save(VentureDatabase *db, VentureEntity *entity,
	const VentureActor *actor, gboolean *handled, GError **error);
#endif
