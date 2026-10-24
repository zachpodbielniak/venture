/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_EMAIL_DESIGN_RECORDS_H
#define VENTURE_EMAIL_DESIGN_RECORDS_H
#include "model/venture-entity.h"
#include "model/venture-entity-macros.h"
G_BEGIN_DECLS
/* An organization's look in email: logo, colours, font. One per organization. */
#define VENTURE_TYPE_EMAIL_BRAND (venture_email_brand_get_type())
VENTURE_DECLARE_ENTITY(VentureEmailBrand, venture_email_brand, EMAIL_BRAND)
/* An image served on a public, unguessable address for mail clients. */
#define VENTURE_TYPE_HOSTED_IMAGE (venture_hosted_image_get_type())
VENTURE_DECLARE_ENTITY(VentureHostedImage, venture_hosted_image, HOSTED_IMAGE)
/* A saved group of blocks other designs reference by id. */
#define VENTURE_TYPE_EMAIL_SECTION (venture_email_section_get_type())
VENTURE_DECLARE_ENTITY(VentureEmailSection, venture_email_section, EMAIL_SECTION)
/* One saved version of a template's design, written only by the design service. */
#define VENTURE_TYPE_EMAIL_DESIGN_VERSION (venture_email_design_version_get_type())
VENTURE_DECLARE_ENTITY(VentureEmailDesignVersion, venture_email_design_version, EMAIL_DESIGN_VERSION)
G_END_DECLS
#endif
