/*
 * venture-serializable.c - Objects that can round-trip through JSON and YAML
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <yaml-glib.h>

G_DEFINE_INTERFACE(VentureSerializable, venture_serializable, G_TYPE_OBJECT)

static void
venture_serializable_default_init(VentureSerializableInterface *iface)
{
	/* No default implementations: an implementor must provide to_json and
	 * from_json, since there is nothing sensible to do generically at this
	 * level. #VentureEntity provides the property-walking implementation
	 * that almost every type actually uses. */
	iface->to_json = NULL;
	iface->from_json = NULL;
	iface->get_sensitive_fields = NULL;
}

JsonNode *
venture_serializable_to_json(
	VentureSerializable	*self,
	gboolean		 include_sensitive
){
	VentureSerializableInterface *iface;

	g_return_val_if_fail(VENTURE_IS_SERIALIZABLE(self), NULL);

	iface = VENTURE_SERIALIZABLE_GET_IFACE(self);
	g_return_val_if_fail(NULL != iface->to_json, NULL);

	return iface->to_json(self, include_sensitive);
}

gboolean
venture_serializable_from_json(
	VentureSerializable	 *self,
	JsonNode		 *node,
	GError			**error
){
	VentureSerializableInterface *iface;

	g_return_val_if_fail(VENTURE_IS_SERIALIZABLE(self), FALSE);
	g_return_val_if_fail(NULL != node, FALSE);

	iface = VENTURE_SERIALIZABLE_GET_IFACE(self);
	g_return_val_if_fail(NULL != iface->from_json, FALSE);

	return iface->from_json(self, node, error);
}

gchar *
venture_serializable_to_json_string(
	VentureSerializable	*self,
	gboolean		 include_sensitive,
	gboolean		 pretty
){
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(JsonGenerator) generator = NULL;

	g_return_val_if_fail(VENTURE_IS_SERIALIZABLE(self), NULL);

	node = venture_serializable_to_json(self, include_sensitive);

	if (NULL == node)
		return NULL;

	generator = json_generator_new();
	json_generator_set_root(generator, node);
	json_generator_set_pretty(generator, pretty);
	json_generator_set_indent(generator, 2);

	return json_generator_to_data(generator, NULL);
}

gchar *
venture_serializable_to_yaml(
	VentureSerializable	*self,
	gboolean		 include_sensitive
){
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(YamlDocument) document = NULL;
	g_autoptr(YamlGenerator) generator = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *yaml = NULL;

	g_return_val_if_fail(VENTURE_IS_SERIALIZABLE(self), NULL);

	node = venture_serializable_to_json(self, include_sensitive);

	if (NULL == node)
		return NULL;

	/* Going via JSON rather than serialising to YAML directly keeps a
	 * single source of truth for the shape of a record: whatever to_json()
	 * decides is the representation, the YAML export matches exactly.
	 * yaml-glib converts the two losslessly for the value types used here. */
	document = yaml_document_from_json_node(node);

	if (NULL == document)
	{
		g_warning("venture_serializable_to_yaml: could not convert the "
		          "JSON representation of a %s to YAML",
		          G_OBJECT_TYPE_NAME(self));
		return NULL;
	}

	generator = yaml_generator_new();
	yaml_generator_set_document(generator, document);
	yaml_generator_set_indent(generator, 2);

	yaml = yaml_generator_to_data(generator, NULL, &local_error);

	if (NULL == yaml)
	{
		g_warning("venture_serializable_to_yaml: %s",
		          (NULL != local_error) ? local_error->message : "failed");
		return NULL;
	}

	return g_steal_pointer(&yaml);
}

const gchar * const *
venture_serializable_get_sensitive_fields(VentureSerializable *self)
{
	VentureSerializableInterface *iface;

	g_return_val_if_fail(VENTURE_IS_SERIALIZABLE(self), NULL);

	iface = VENTURE_SERIALIZABLE_GET_IFACE(self);

	if (NULL == iface->get_sensitive_fields)
		return NULL;

	return iface->get_sensitive_fields(self);
}

gboolean
venture_serializable_is_sensitive_field(
	VentureSerializable	*self,
	const gchar		*name
){
	const gchar * const *fields;

	g_return_val_if_fail(VENTURE_IS_SERIALIZABLE(self), FALSE);
	g_return_val_if_fail(NULL != name, FALSE);

	fields = venture_serializable_get_sensitive_fields(self);

	if (NULL == fields)
		return FALSE;

	return g_strv_contains(fields, name);
}
