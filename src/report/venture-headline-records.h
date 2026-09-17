/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_HEADLINE_RECORDS_H
#define VENTURE_HEADLINE_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_HEADLINE_SETTING (venture_headline_setting_get_type())
VENTURE_DECLARE_ENTITY(VentureHeadlineSetting, venture_headline_setting, HEADLINE_SETTING)
G_END_DECLS
#endif
