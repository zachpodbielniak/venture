/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

gboolean
venture_lead_ai_score_enabled(VentureContext *context, gint64 organization_id)
{
	(void)context; (void)organization_id;
	return FALSE;
}

VentureEntity *
venture_lead_ai_score_propose(VentureContext *context, VentureEntity *lead,
	const VentureActor *actor, GError **error)
{
	(void)context; (void)lead; (void)actor;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "not implemented");
	return NULL;
}

VentureEntity *
venture_lead_ai_score_pending(VentureDatabase *database, VentureEntity *lead)
{
	(void)database; (void)lead;
	return NULL;
}

gboolean
venture_lead_ai_score_decide(VentureContext *context, VentureEntity *proposal,
	gboolean accept, const VentureActor *actor, GError **error)
{
	(void)context; (void)proposal; (void)accept; (void)actor;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "not implemented");
	return FALSE;
}

gboolean
venture_lead_ai_score_check_write(VentureEntity *entity, GError **error)
{
	(void)entity; (void)error;
	return TRUE;
}
