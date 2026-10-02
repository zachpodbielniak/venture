/*
 * venture-forms-personal.c - Scoped personal form links
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "venture-forms-private.h"
#include <string.h>

static gchar *
personal_mac(VentureEntity *form, const gchar *payload)
{
	g_autofree gchar *key = venture_forms_get_string(form, "ticket-key");
	g_autofree gchar *public_token = venture_forms_get_string(form, "public-token");
	g_autofree gchar *message = g_strdup_printf("personal:%s:%s", public_token, payload);
	if (venture_string_is_empty(key)) return NULL;
	return g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)key, strlen(key), message, -1);
}

VentureEntity *
venture_forms_personal_contact(VentureDatabase *database, VentureEntity *form,
	const gchar *token, GDateTime *now, GError **error)
{
	g_auto(GStrv) parts = NULL;
	g_autofree gchar *payload = NULL, *expected = NULL;
	g_autoptr(VentureEntity) contact = NULL;
	gint64 id = 0, expires = 0;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "contact") == G_TYPE_INVALID ||
	    venture_string_is_empty(token) || strlen(token) > 256) goto missing;
	parts = g_strsplit(token, ".", 6);
	if (g_strv_length(parts) != 5 ||
	    !g_ascii_string_to_signed(parts[0], 10, 1, G_MAXINT64, &id, NULL) ||
	    !g_uuid_string_is_valid(parts[1]) || !g_uuid_string_is_valid(parts[2]) ||
	    !g_ascii_string_to_signed(parts[3], 10, 1, G_MAXINT64, &expires, NULL) ||
	    expires <= g_date_time_to_unix(now)) goto missing;
	payload = g_strdup_printf("%s.%s.%s.%s", parts[0], parts[1], parts[2], parts[3]);
	expected = personal_mac(form, payload);
	if (expected == NULL || !venture_constant_time_equal(expected, parts[4])) goto missing;
	contact = venture_database_get(database, VENTURE_TYPE_CONTACT, id, NULL);
	if (contact == NULL || venture_entity_is_deleted(contact) ||
	    venture_entity_get_organization_id(contact) != venture_entity_get_organization_id(form) ||
	    g_strcmp0(venture_entity_get_uuid(contact), parts[1]) != 0) goto missing;
	return g_steal_pointer(&contact);
missing:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
	return NULL;
}

gboolean
venture_forms_public_origin_valid(const gchar *origin, GError **error)
{
	g_autoptr(GUri) uri = NULL;
	const gchar *scheme, *host, *path;

	if (venture_string_is_empty(origin) || strlen(origin) > 2048 || strpbrk(origin, "\r\n\t \"'<>")) goto invalid;
	uri = g_uri_parse(origin, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL || g_uri_get_userinfo(uri) != NULL || g_uri_get_query(uri) != NULL || g_uri_get_fragment(uri) != NULL) goto invalid;
	scheme = g_uri_get_scheme(uri); host = g_uri_get_host(uri); path = g_uri_get_path(uri);
	if (host == NULL || *host == '\0' || (path != NULL && *path != '\0' && strcmp(path, "/") != 0)) goto invalid;
	if (g_strcmp0(scheme, "https") != 0 && !(g_strcmp0(scheme, "http") == 0 &&
	    (g_strcmp0(host, "localhost") == 0 || g_strcmp0(host, "127.0.0.1") == 0 || g_strcmp0(host, "::1") == 0))) goto invalid;
	return TRUE;
invalid:
	venture_set_error_validation(error, "Public origin", "use an HTTPS origin without credentials, path, query or fragment; HTTP is allowed only on loopback");
	return FALSE;
}

/**
 * venture_forms_personal_link:
 * @database: database containing the form and contact
 * @form: published form in the contact's organization
 * @contact: existing non-deleted contact
 * @origin: public HTTPS origin (HTTP is permitted only on loopback)
 * @expires: expiry, after @now and at most 30 days later
 * @now: generation time
 * @error: (out) (optional): invalid scope, address or expiry
 *
 * Creates a bearer capability for this form only. The caller must have
 * permission to read the contact and distribute their mapped form values.
 * Keep the returned URL out of logs, shared audit records and AI context.
 *
 * Returns: (transfer full) (nullable): personal public URL
 */
gchar *
venture_forms_personal_link(VentureDatabase *database, VentureEntity *form,
	VentureEntity *contact, const gchar *origin, GDateTime *expires,
	GDateTime *now, GError **error)
{
	g_autoptr(VentureEntity) saved = NULL;
	g_autofree gchar *nonce = NULL, *payload = NULL, *mac = NULL, *public_token = NULL;
	gint64 seconds;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_FORM(form), NULL);
	g_return_val_if_fail(VENTURE_IS_CONTACT(contact), NULL);
	g_return_val_if_fail(now != NULL && expires != NULL, NULL);
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "form") == G_TYPE_INVALID ||
	    venture_entity_registry_lookup(venture_entity_registry_get_default(), "contact") == G_TYPE_INVALID ||
	    venture_entity_is_deleted(form) || venture_forms_get_int(form, "published-number") <= 0) goto invalid;
	if (!venture_forms_public_origin_valid(origin, error)) return NULL;
	seconds = g_date_time_to_unix(expires);
	if (seconds <= g_date_time_to_unix(now) || seconds - g_date_time_to_unix(now) > 30 * G_TIME_SPAN_DAY / G_TIME_SPAN_SECOND) goto invalid;
	saved = venture_database_get(database, VENTURE_TYPE_CONTACT, venture_entity_get_id(contact), NULL);
	if (saved == NULL || venture_entity_is_deleted(saved) ||
	    venture_entity_get_organization_id(saved) != venture_entity_get_organization_id(form)) goto invalid;
	nonce = g_uuid_string_random();
	payload = g_strdup_printf("%" G_GINT64_FORMAT ".%s.%s.%" G_GINT64_FORMAT,
		venture_entity_get_id(saved), venture_entity_get_uuid(saved), nonce, seconds);
	mac = personal_mac(form, payload);
	public_token = venture_forms_get_string(form, "public-token");
	if (mac == NULL || venture_string_is_empty(public_token)) goto invalid;
	return g_strdup_printf("%.*s/pub/form/%s?personal=%s.%s",
		(gint)(strlen(origin) - (g_str_has_suffix(origin, "/") ? 1 : 0)), origin, public_token, payload, mac);
invalid:
	venture_set_error_validation(error, "Personal link", "use an existing contact in this organization, a public HTTPS origin and an expiry within 30 days");
	return NULL;
}

static gchar *
prefill_mac(VentureEntity *form, VentureEntity *version, const gchar *encoded)
{
	g_autofree gchar *key = venture_forms_get_string(form, "ticket-key");
	g_autofree gchar *public_token = venture_forms_get_string(form, "public-token");
	g_autofree gchar *payload = g_strdup_printf("prefill:%s:%" G_GINT64_FORMAT ":%s",
		public_token, venture_forms_get_int(version, "number"), encoded);
	if (venture_string_is_empty(key)) return NULL;
	return g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)key, strlen(key), payload, -1);
}

/* This seed carries validated initial defaults to pages not rendered yet.
 * It grants no identity: only the independently signed personal link does. */
gchar *
venture_forms_prefill_pack(VentureEntity *form, VentureEntity *version, JsonObject *values)
{
	g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *text = NULL, *encoded = NULL, *mac = NULL;
	json_node_set_object(root, values);
	text = json_to_string(root, FALSE);
	if (strlen(text) > 32768) return NULL;
	encoded = g_base64_encode((const guchar *)text, strlen(text));
	mac = prefill_mac(form, version, encoded);
	return mac != NULL ? g_strdup_printf("%s:%s", encoded, mac) : NULL;
}

JsonObject *
venture_forms_prefill_unpack(VentureEntity *form, VentureEntity *version, const gchar *seed, GError **error)
{
	g_auto(GStrv) parts = NULL;
	g_autofree gchar *expected = NULL, *text = NULL;
	g_autofree guchar *bytes = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonNode *root;
	gsize length;
	if (venture_string_is_empty(seed)) return json_object_new();
	if (strlen(seed) > 45000) goto missing;
	parts = g_strsplit(seed, ":", 3);
	if (g_strv_length(parts) != 2) goto missing;
	expected = prefill_mac(form, version, parts[0]);
	if (expected == NULL || !venture_constant_time_equal(expected, parts[1])) goto missing;
	bytes = g_base64_decode(parts[0], &length);
	if (length > 32768 || memchr(bytes, '\0', length) != NULL) goto missing;
	text = g_strndup((const gchar *)bytes, length);
	if (!json_parser_load_from_data(parser, text, (gssize)length, NULL)) goto missing;
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root) || json_object_get_size(json_node_get_object(root)) > 500) goto missing;
	return json_object_ref(json_node_get_object(root));
missing:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
	return NULL;
}

/* Called under the intake transaction before leads, consent or mail. */
gboolean
venture_forms_personal_bind(VentureDatabase *database, VentureEntity *form,
	VentureEntity *submission, GDateTime *now, JsonObject *refused, GError **error)
{
	const gchar *token = g_object_get_data(G_OBJECT(submission), VENTURE_FORMS_PERSONAL_WRITE);
	g_autoptr(VentureEntity) contact = NULL;
	g_autofree gchar *hash = NULL;
	guint i;
	const gchar *limits[] = { "one-per-contact", "one-per-link" };
	if (token == NULL) return TRUE;
	contact = venture_forms_personal_contact(database, form, token, now, error);
	if (contact == NULL) return FALSE;
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
	for (i = 0; i < G_N_ELEMENTS(limits); i++)
	{
		g_autoptr(VentureQuery) query = NULL;
		gint64 count;
		if (!venture_forms_get_bool(form, limits[i])) continue;
		query = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
		venture_query_set_organization(query, venture_entity_get_organization_id(form));
		venture_query_set_include_deleted(query, TRUE);
		venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
		if (i == 0) venture_query_add_filter_int(query, "contact-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(contact), NULL);
		else venture_query_add_filter_string(query, "personal-hash", VENTURE_FILTER_OP_EQ, hash, NULL);
		count = venture_database_count(database, query, error);
		if (count < 0) return FALSE;
		if (count > 0)
		{
			json_object_set_string_member(refused, "_form", "A response has already been received for this personal link.");
			return FALSE;
		}
	}
	g_object_set(submission, "contact-id", venture_entity_get_id(contact), "personal-hash", hash, NULL);
	return TRUE;
}
