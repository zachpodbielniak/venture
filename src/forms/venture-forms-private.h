/*
 * venture-forms-private.h - What the forms module's files share
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A question is read two ways: from its form_field record while a form is
 * being edited, and from a published version's frozen definition once it
 * is live. Both become a VentureFormsField, and everything that renders,
 * validates or counts answers takes that -- so a published form cannot be
 * changed by editing its questions, and an old response is always read
 * against the questions it answered.
 */

#ifndef VENTURE_FORMS_PRIVATE_H
#define VENTURE_FORMS_PRIVATE_H

#include "venture.h"

G_BEGIN_DECLS

#define VENTURE_FORMS_KEY_MAX 63

/* Set on a record by the forms service and nowhere else. The validators
 * refuse an unmarked insert of a response or a version, which is what
 * makes the service the only writer of either. */
#define VENTURE_FORMS_ACCEPTING_KEY "venture-forms-accepting"

typedef struct
{
	gchar	*id;
	gchar	*label;
} VentureFormsChoice;

typedef struct
{
	gchar			*key;
	gchar			*label;
	VentureFormFieldKind	 kind;
	gboolean		 required;
	gboolean		 sensitive;
	gint64			 position;
	gchar			*help;
	gchar			*placeholder;
	gchar			*pattern;
	gchar			*default_value;
	gchar			*maps_to;
	gchar			*autocomplete;
	gdouble			 min_value;
	gdouble			 max_value;
	gint64			 min_length;
	gint64			 max_length;
	GPtrArray		*choices;
} VentureFormsField;

/* --- Reading records --- */

gchar *venture_forms_get_string(VentureEntity *entity, const gchar *property);
gint64 venture_forms_get_int(VentureEntity *entity, const gchar *property);
gdouble venture_forms_get_double(VentureEntity *entity, const gchar *property);
gboolean venture_forms_get_bool(VentureEntity *entity, const gchar *property);

/* --- Kinds and choices --- */

const gchar *venture_forms_kind_nick(VentureFormFieldKind kind);
gboolean venture_forms_kind_has_choices(VentureFormFieldKind kind);
gboolean venture_forms_choice_id_valid(const gchar *id);
void venture_forms_choice_free(gpointer data);
GPtrArray *venture_forms_choices_parse(const gchar *text, gboolean assign, GError **error);
const gchar *venture_forms_choice_label(GPtrArray *choices, const gchar *id);
void venture_forms_rating_bounds(const VentureFormsField *field, gint64 *low, gint64 *high);

/* --- Definitions --- */

void venture_forms_field_free(gpointer data);
GPtrArray *venture_forms_definition_from_records(VentureDatabase *database, VentureEntity *form,
	GError **error);
GPtrArray *venture_forms_definition_from_json(const gchar *text, GError **error);
gchar *venture_forms_definition_to_json(GPtrArray *fields);
GPtrArray *venture_forms_definition_for(VentureDatabase *database, VentureEntity *form,
	VentureEntity *version, GError **error);
const VentureFormsField *venture_forms_definition_find(GPtrArray *fields, const gchar *key);

/* --- Versions --- */

VentureEntity *venture_forms_version_by_number(VentureDatabase *database, VentureEntity *form,
	gint64 number, GError **error);

/* --- Tickets --- */

gboolean venture_forms_ticket_parse(VentureEntity *form, const gchar *ticket, gint64 *issued,
	gint64 *version);

G_END_DECLS

#endif /* VENTURE_FORMS_PRIVATE_H */
