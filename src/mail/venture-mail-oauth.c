/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

gboolean venture_mail_oauth_configure_client(VentureDatabase *database, VentureConfig *config,
	VentureEntity *provider, const gchar *client_id, const gchar *client_secret,
	const VentureActor *actor, GError **error)
{
	(void)database; (void)config; (void)provider; (void)client_id; (void)client_secret; (void)actor;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "not implemented");
	return FALSE;
}
gboolean venture_mail_oauth_client_configured(VentureDatabase *database, VentureEntity *provider)
{
	(void)database; (void)provider;
	return FALSE;
}
gchar *venture_mail_oauth_begin(VentureDatabase *database, VentureConfig *config,
	VentureEntity *account, const gchar *redirect_uri, gint64 user_id, GError **error)
{
	(void)database; (void)config; (void)account; (void)redirect_uri; (void)user_id;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "not implemented");
	return NULL;
}
gint64 venture_mail_oauth_complete(VentureDatabase *database, VentureConfig *config,
	const gchar *state, const gchar *code, gint64 user_id, const VentureActor *actor, GError **error)
{
	(void)database; (void)config; (void)state; (void)code; (void)user_id; (void)actor;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "not implemented");
	return 0;
}
gboolean venture_mail_oauth_refresh(VentureDatabase *database, VentureConfig *config,
	VentureEntity *account, gboolean force, GError **error)
{
	(void)database; (void)config; (void)account; (void)force;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "not implemented");
	return FALSE;
}
gchar *venture_mail_oauth_pkce_challenge(const gchar *verifier)
{
	(void)verifier;
	return g_strdup("");
}
gchar *venture_mail_oauth_xoauth2(const gchar *username, const gchar *access_token)
{
	(void)username; (void)access_token;
	return NULL;
}
