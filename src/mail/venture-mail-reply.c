/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

VentureEntity *venture_mail_reply_source(VentureDatabase *database, VentureEntity *interaction)
{
	(void)database; (void)interaction;
	return NULL;
}
VentureMailMessage *venture_mail_reply_enqueue(VentureContext *context, VentureEntity *interaction,
	const gchar *body, const VentureActor *actor, GError **error)
{
	(void)context; (void)interaction; (void)body; (void)actor;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "not implemented");
	return NULL;
}
