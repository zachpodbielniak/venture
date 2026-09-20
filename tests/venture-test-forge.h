/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include <venture.h>

/* Explicit fixture injection avoids remote credential verification. Production
 * configuration always goes through the forge adapter's account lookup. */
static inline void venture_test_forge_bind(VentureDatabase *database, VentureEntity *forge,
	const gchar *token, const gchar *secret)
{
	g_autoptr(GBytes) key = g_bytes_new_static("01234567890123456789012345678901", 32);
	g_autoptr(JsonNode) settings = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *provider = NULL, *base = NULL;
	VentureForgeKind kind;
	g_assert_cmpint(venture_entity_get_organization_id(forge), >, 0);
	g_assert_true(venture_integration_service_set_key(venture_integration_service_get(database), key, &error));
	g_assert_no_error(error);
	g_object_get(forge, "base-url", &base, "kind", &kind, NULL);
	json_node_take_object(settings, json_object_new());
	json_object_set_string_member(json_node_get_object(settings), "token", token);
	json_object_set_string_member(json_node_get_object(settings), "webhook_secret", secret);
	json_object_set_string_member(json_node_get_object(settings), "base_url", base);
	json_object_set_int_member(json_node_get_object(settings), "kind", kind);
	provider = g_strdup_printf("forge_%" G_GINT64_FORMAT, venture_entity_get_id(forge));
	binding = venture_integration_service_configure(venture_integration_service_get(database),
		venture_entity_get_organization_id(forge), provider, "venture-bot", "live", settings, 0, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(binding);
}
