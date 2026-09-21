/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture-oidc-verifier-private.h"
#include <oidc-glib.h>
#include <openssl/crypto.h>
#include <string.h>
typedef struct { OidcPrincipal *principal; GError *error; gboolean done; } VerifyResult;
static gboolean verify_deadline(gpointer data) { g_cancellable_cancel(data); return G_SOURCE_REMOVE; }
static void verify_complete(GObject *source, GAsyncResult *result, gpointer data)
{
	VerifyResult *out = data;
	out->principal = oidc_verifier_verify_finish(OIDC_VERIFIER(source), result, &out->error);
	out->done = TRUE;
}
/* The library worker owns JWT/HTTP data only. Its completion context is
 * private, so waiting cannot run Venture's database or automation callbacks. */
static OidcPrincipal *verify_bounded(GObject *verifier, const gchar *issuer, const gchar *token, GError **error)
{
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	g_autoptr(GSource) deadline = g_timeout_source_new(5000);
	VerifyResult out;
	out.principal = NULL; out.error = NULL; out.done = FALSE;
	g_main_context_push_thread_default(context);
	g_source_set_callback(deadline, verify_deadline, cancel, NULL); g_source_attach(deadline, context);
	oidc_verifier_verify_async(OIDC_VERIFIER(verifier), issuer, token, cancel, verify_complete, &out);
	while (!out.done) g_main_context_iteration(context, TRUE);
	g_source_destroy(deadline); g_main_context_pop_thread_default(context);
	if (out.error) g_propagate_error(error, out.error);
	return out.principal;
}
GObject *venture_oidc_verifier_new(const gchar *issuer, const gchar *client_id, const gchar *jwks_uri, GError **error)
{
	json_t *settings = json_pack("{s:[{s:s,s:s,s:s,s:i,s:i}]}", "issuers", "issuer", issuer,
		"audience", client_id, "jwks_uri", jwks_uri, "clock_skew", 30, "timeout", 10);
	g_autofree gchar *serialized = json_dumps(settings, JSON_COMPACT);
	OidcVerifier *verifier = oidc_verifier_new(serialized, NULL, error);
	json_decref(settings);
	return verifier != NULL ? G_OBJECT(verifier) : NULL;
}
gchar *venture_oidc_verifier_subject(GObject *verifier, const gchar *issuer, const gchar *client_id,
	const gchar *token, const gchar *nonce, GError **error)
{
	OidcPrincipal *principal = verify_bounded(verifier, issuer, token, error);
	json_t *claims, *aud, *azp_value;
	const gchar *received_nonce, *azp, *subject;
	gchar *result = NULL;
	gboolean valid;
	if (principal == NULL) return NULL;
	claims = oidc_principal_dup_claims(principal);
	received_nonce = json_string_value(json_object_get(claims, "nonce"));
	aud = json_object_get(claims, "aud"); azp_value = json_object_get(claims, "azp");
	azp = json_string_value(azp_value); subject = oidc_principal_get_subject(principal);
	valid = received_nonce != NULL && nonce != NULL && strlen(received_nonce) == strlen(nonce) &&
		CRYPTO_memcmp(received_nonce, nonce, strlen(nonce)) == 0 && subject != NULL && strlen(subject) <= 255;
	/* The resource-server verifier checks audience membership; an ID token
	 * additionally binds its authorized party when present or multi-audience. */
	if (azp_value != NULL || (json_is_array(aud) && json_array_size(aud) > 1))
		valid = valid && azp != NULL && g_strcmp0(azp, client_id) == 0;
	if (valid) result = g_strdup(subject);
	else g_set_error_literal(error, OIDC_ERROR, OIDC_ERROR_CLAIMS, "OIDC nonce or authorized party is invalid");
	json_decref(claims); oidc_principal_unref(principal);
	return result;
}
