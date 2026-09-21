/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

GType
venture_data_class_get_type(void)
{
	static gsize type_id = 0;
	static const GEnumValue values[] = {
		{ VENTURE_DATA_CLASS_UNKNOWN, "VENTURE_DATA_CLASS_UNKNOWN", "unknown" },
		{ VENTURE_DATA_CLASS_TENANT, "VENTURE_DATA_CLASS_TENANT", "tenant" },
		{ VENTURE_DATA_CLASS_PERSONAL, "VENTURE_DATA_CLASS_PERSONAL", "personal" },
		{ VENTURE_DATA_CLASS_REFERENCE, "VENTURE_DATA_CLASS_REFERENCE", "reference" },
		{ VENTURE_DATA_CLASS_PLATFORM, "VENTURE_DATA_CLASS_PLATFORM", "platform" },
		{ VENTURE_DATA_CLASS_TENANT_ADMIN, "VENTURE_DATA_CLASS_TENANT_ADMIN", "tenant_admin" },
		{ 0, NULL, NULL }
	};
	if (g_once_init_enter(&type_id)) {
		GType registered = g_enum_register_static("VentureDataClass", values);
		g_once_init_leave(&type_id, registered);
	}
	return (GType)type_id;
}

static GQuark
classification_key(void)
{
	return g_quark_from_static_string("venture-explicit-data-class");
}

void
venture_data_class_declare_type(GType type, VentureDataClass classification)
{
	g_return_if_fail(g_type_is_a(type, VENTURE_TYPE_ENTITY));
	g_return_if_fail(classification > VENTURE_DATA_CLASS_UNKNOWN &&
	                classification <= VENTURE_DATA_CLASS_TENANT_ADMIN);
	g_type_set_qdata(type, classification_key(), GINT_TO_POINTER(classification));
}

VentureDataClass
venture_data_class_for_type(GType type)
{
	return GPOINTER_TO_INT(g_type_get_qdata(type, classification_key()));
}

void
venture_data_class_declare_resource(GObject *resource, VentureDataClass classification)
{
	g_return_if_fail(G_IS_OBJECT(resource));
	g_return_if_fail(classification > VENTURE_DATA_CLASS_UNKNOWN &&
	                classification <= VENTURE_DATA_CLASS_TENANT_ADMIN);
	g_object_set_qdata(resource, classification_key(), GINT_TO_POINTER(classification));
}

VentureDataClass
venture_data_class_for_resource(GObject *resource)
{
	g_return_val_if_fail(G_IS_OBJECT(resource), VENTURE_DATA_CLASS_UNKNOWN);
	return GPOINTER_TO_INT(g_object_get_qdata(resource, classification_key()));
}
