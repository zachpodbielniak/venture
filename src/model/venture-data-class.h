/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_DATA_CLASS_H
#define VENTURE_DATA_CLASS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * VentureDataClass:
 * @VENTURE_DATA_CLASS_UNKNOWN: no declaration; hosted access is refused
 * @VENTURE_DATA_CLASS_TENANT: workspace business data
 * @VENTURE_DATA_CLASS_PERSONAL: workspace data with additional identity ownership
 * @VENTURE_DATA_CLASS_REFERENCE: shared non-sensitive reference information
 * @VENTURE_DATA_CLASS_PLATFORM: operator machinery, never tenant authority
 * @VENTURE_DATA_CLASS_TENANT_ADMIN: service-managed workspace administration
 *
 * Classification describes authority, not storage. Personal ownership and
 * organization permissions still apply after classification permits access.
 */
typedef enum {
	VENTURE_DATA_CLASS_UNKNOWN,
	VENTURE_DATA_CLASS_TENANT,
	VENTURE_DATA_CLASS_PERSONAL,
	VENTURE_DATA_CLASS_REFERENCE,
	VENTURE_DATA_CLASS_PLATFORM,
	VENTURE_DATA_CLASS_TENANT_ADMIN
} VentureDataClass;
#define VENTURE_TYPE_DATA_CLASS (venture_data_class_get_type())
/**
 * venture_data_class_get_type:
 * Returns: the registered authority-class enumeration type
 */
GType venture_data_class_get_type(void) G_GNUC_CONST;
/**
 * venture_data_class_declare_type:
 * @type: concrete record type
 * @classification: explicit authority declaration
 *
 * Declares a record before exposing it. Plugins must declare every type;
 * deriving from a classified parent does not inherit its authority.
 */
void venture_data_class_declare_type(GType type, VentureDataClass classification);
/**
 * venture_data_class_for_type:
 * @type: concrete record type
 * Returns: its explicit classification, or UNKNOWN
 */
VentureDataClass venture_data_class_for_type(GType type);
/**
 * venture_data_class_declare_resource:
 * @resource: an action, report or other exposed resource
 * @classification: explicit authority declaration
 *
 * Declares an individual resource, independently of its implementation class.
 */
void venture_data_class_declare_resource(GObject *resource, VentureDataClass classification);
/**
 * venture_data_class_for_resource:
 * @resource: a classified action, report or resource
 * Returns: its explicit classification, or UNKNOWN
 */
VentureDataClass venture_data_class_for_resource(GObject *resource);
G_END_DECLS
#endif
