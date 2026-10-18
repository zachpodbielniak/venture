/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#define STR(n, l, h) VENTURE_FIELD(n, l, h, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
#define INT(n, l, h) VENTURE_FIELD(n, l, h, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)

/* Writes need an owner or administrator of the organization (read by
 * venture_access_policy_can()): a brand, an image every message links to,
 * a shared section and a design's history change every email the
 * organization sends, so they are settings, not data entry. */
#define MANAGER_WRITE \
	g_type_set_qdata(G_TYPE_FROM_CLASS(klass), \
		g_quark_from_static_string("venture-access-manager-write"), GINT_TO_POINTER(1));

static const VentureFieldDecl brand_fields[] = {
	VENTURE_FIELD_NAME("name", "Business name", "As it appears in the header and footer of every email"),
	VENTURE_FIELD_REF("logo-image-id", "Logo", "An uploaded hosted image", "hosted_image", VENTURE_COLUMN_FLAG_NONE),
	STR("primary-color", "Primary colour", "#rrggbb: headings and buttons"),
	STR("accent-color", "Accent colour", "#rrggbb: links"),
	STR("text-color", "Text colour", "#rrggbb"),
	STR("background-color", "Background colour", "#rrggbb: around the message"),
	STR("dark-primary-color", "Dark-mode primary colour", "#rrggbb; empty derives one"),
	STR("dark-text-color", "Dark-mode text colour", "#rrggbb; empty derives one"),
	STR("dark-background-color", "Dark-mode background colour", "#rrggbb; empty derives one"),
	STR("font", "Font", "sans, serif, rounded, system or mono: web-safe faces with a fallback"),
	STR("website-url", "Website", "Your own web address"),
	VENTURE_FIELD_TEXT("footer-address", "Postal address", "Shown in the footer"),
	/* Whether the designer has asked for a logo: "" not yet, then
	 * asked, dismissed or answered. It is asked once per organization. */
	VENTURE_FIELD("logo-ask", "Logo request", "Whether the designer asked for a logo: asked, dismissed or answered",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_TECHNICAL)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureEmailBrand, venture_email_brand, brand_fields,
	MANAGER_WRITE
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Email brand", NULL);)

static const VentureFieldDecl image_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	STR("alt-text", "Alt text", "What the image shows, for people who cannot see it"),
	/* Everything below is set from the bytes by the image store and never
	 * changes; the store is the only writer that may make one. */
	VENTURE_FIELD_REF("document-id", "File", "The stored upload", "document", VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD("token", "Address token", "The unguessable part of its public address",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_TECHNICAL | VENTURE_COLUMN_FLAG_INDEXED),
	STR("content-type", "Type", "Read from the bytes"),
	INT("width", "Width", "Pixels"),
	INT("height", "Height", "Pixels"),
	INT("size-bytes", "Size", "Bytes")
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureHostedImage, venture_hosted_image, image_fields,
	MANAGER_WRITE
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Hosted image", NULL);)

static const VentureFieldDecl section_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "A shared header, footer or signature"),
	VENTURE_FIELD("design", "Blocks", "Email blocks as JSON, like a template's design", VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureEmailSection, venture_email_section, section_fields,
	MANAGER_WRITE
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Email section", NULL);)

static const VentureFieldDecl version_fields[] = {
	VENTURE_FIELD_REF("template-id", "Template", NULL, "mail_template", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("number", "Number", "1 for the first design, then one more for each change",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("design", "Design", "The design exactly as it was", VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	STR("source", "Made by", "manual, ai or ai-edit"),
	VENTURE_FIELD_TEXT("note", "Brief or instruction", "What was asked for")
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureEmailDesignVersion, venture_email_design_version, version_fields,
	MANAGER_WRITE
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Email design version", NULL);)
