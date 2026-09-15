/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CUSTOM_FIELDS_RECORDS_H
#define VENTURE_CUSTOM_FIELDS_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_ACCOUNTING_CUSTOM_FIELD (venture_accounting_custom_field_get_type())
VENTURE_DECLARE_ENTITY(VentureAccountingCustomField, venture_accounting_custom_field, ACCOUNTING_CUSTOM_FIELD)
#define VENTURE_TYPE_ACCOUNTING_LAYOUT (venture_accounting_layout_get_type())
VENTURE_DECLARE_ENTITY(VentureAccountingLayout, venture_accounting_layout, ACCOUNTING_LAYOUT)
#define VENTURE_TYPE_CUSTOM_FIELD_VALUE (venture_custom_field_value_get_type())
VENTURE_DECLARE_ENTITY(VentureCustomFieldValue, venture_custom_field_value, CUSTOM_FIELD_VALUE)
G_END_DECLS
#endif
