/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include <gio/gio.h>
/* Jansson and JSON-GLib headers must never share a translation unit. */
GObject *venture_oidc_verifier_new(const gchar *issuer, const gchar *client_id, const gchar *jwks_uri, GError **error);
gchar *venture_oidc_verifier_subject(GObject *verifier, const gchar *issuer, const gchar *client_id,
	const gchar *token, const gchar *nonce, GError **error);
