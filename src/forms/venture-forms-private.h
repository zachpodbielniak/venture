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

typedef enum
{
	VENTURE_FORMS_GROUP_NONE = 0,
	VENTURE_FORMS_GROUP_START,
	VENTURE_FORMS_GROUP_ROW_START,
	VENTURE_FORMS_GROUP_ROW_END,
	VENTURE_FORMS_GROUP_END
} VentureFormsGroupBoundary;

typedef struct
{
	gchar			*key;
	gchar			*label;
	VentureFormFieldKind	 kind;
	gboolean		 marketing_consent;
	gboolean		 allow_prefill;
	gchar			*contact_field;
	gboolean		 required;
	gboolean		 sensitive;
	gint64			 position;
	gchar			*help;
	gchar			*placeholder;
	gchar			*pattern;
	gchar			*default_value;
	gchar			*maps_to;
	gchar			*autocomplete;
	gint64			 group_id;
	gchar			*group_key;
	gchar			*group_label;
	gchar			*base_key;
	guint			 group_min;
	guint			 group_max;
	guint			 row_index;
	guint			 row_count;
	guint			 group_boundary;
	gdouble			 min_value;
	gdouble			 max_value;
	gint64			 min_length;
	gint64			 max_length;
	GPtrArray		*choices;
	/* Each question owns a reference to its version's immutable rule array. */
	JsonArray		*rules;
	gint64			 file_max_bytes;
	guint			 file_max_count;
	gchar			*file_types;
	gint64			 booking_page_id;
	gchar			*booking_name_field;
	gchar			*booking_email_field;
	JsonNode		*booking_slots;
	gchar			*scoring;
	JsonObject		*quiz;
	JsonObject		*payment;
	JsonObject		*catalog;
	gchar			*language;
} VentureFormsField;

#define VENTURE_FORMS_PENDING_WRITE "venture-forms-pending-write"
#define VENTURE_FORMS_CONFIRMING "venture-forms-confirming"

#define VENTURE_FORMS_PERSONAL "_vf_personal"
#define VENTURE_FORMS_PREFILL "_vf_prefill"
#define VENTURE_FORMS_PERSONAL_WRITE "venture-forms-personal-write"

#define VENTURE_FORMS_DRAFT_TOKEN "_vf_draft"
#define VENTURE_FORMS_MOVE "_vf_move"
#define VENTURE_FORMS_RESUME_EMAIL "_vf_resume_email"
#define VENTURE_FORMS_DRAFT_WRITE "venture-forms-draft-write"

typedef struct {
	VentureEntity *version;
	JsonObject *values;
	JsonObject *errors;
	gchar *token;
	gchar *ticket;
	gchar *resume_url;
	gboolean changed;
	guint page;
	guint pages;
	gboolean complete;
	gboolean awaiting_confirmation;
	VentureEntity *submission;
	gint64 contact_id;
} VentureFormsStep;

void venture_forms_step_free(VentureFormsStep *step);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureFormsStep, venture_forms_step_free)
guint venture_forms_page_count(GPtrArray *fields);
GPtrArray *venture_forms_page_fields(GPtrArray *fields, guint page);
VentureFormsStep *venture_forms_step(VentureDatabase *database, VentureEntity *form,
	GHashTable *answers, const gchar *origin, GDateTime *now, GError **error);
gchar *venture_forms_render_step(VentureDatabase *database, VentureEntity *form,
	const VentureFormsRender *options, const VentureFormsStep *step, GError **error);

VentureFormsStep *venture_forms_resume(VentureDatabase *database, VentureEntity *form,
	const gchar *capability, GDateTime *now, gboolean consume, GError **error);

gboolean venture_forms_public_origin_valid(const gchar *origin, GError **error);
gboolean venture_forms_personal_bind(VentureDatabase *database, VentureEntity *form,
	VentureEntity *submission, GDateTime *now, JsonObject *refused, GError **error);
VentureEntity *venture_forms_personal_contact(VentureDatabase *database, VentureEntity *form,
	const gchar *token, GDateTime *now, GError **error);
JsonObject *venture_forms_prefill_values(VentureDatabase *database, VentureEntity *form,
	VentureEntity *version, GHashTable *query, const gchar *personal, GDateTime *now, GError **error);
gchar *venture_forms_prefill_pack(VentureEntity *form, VentureEntity *version, JsonObject *values);
JsonObject *venture_forms_prefill_unpack(VentureEntity *form, VentureEntity *version, const gchar *seed, GError **error);

JsonObject *venture_forms_flatten_json_answers(JsonObject *object, GError **error);
gboolean venture_forms_has_groups(GPtrArray *fields);
JsonArray *venture_forms_group_render_rules(GPtrArray *fields);
JsonObject *venture_forms_groups_overlay(GPtrArray *fields, guint page, JsonObject *previous, JsonObject *posted);
void venture_forms_groups_clear_page(GPtrArray *fields, guint page, JsonObject *values);
gboolean venture_forms_group_move(GPtrArray *fields, const gchar *move, JsonObject *values, JsonObject *errors);
void venture_forms_groups_fold(GPtrArray *fields, JsonObject *answers, GHashTable *not_shown, gboolean secret);
GPtrArray *venture_forms_expand_groups(GPtrArray *fields, JsonObject *values, gboolean rendering, JsonObject *errors);
void venture_forms_groups_validate(GPtrArray *fields, GHashTable *not_shown, JsonObject *errors);
gboolean venture_forms_groups_check(GPtrArray *fields, GError **error);
gboolean venture_forms_key_valid(const gchar *key);
gboolean venture_forms_validate_group(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error);
gboolean venture_forms_groups_load(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GError **error);

gboolean venture_forms_rules_restore(GPtrArray *fields, JsonNode *node, GError **error);
gboolean venture_forms_rules_load(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GError **error);
gboolean venture_forms_validate_rule(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error);
gboolean venture_forms_condition_matches(JsonObject *condition, JsonObject *values);
gboolean venture_forms_rule_matches(JsonObject *rule, JsonObject *values);
gboolean venture_forms_field_active(const VentureFormsField *field, JsonObject *values, gboolean *required);
guint venture_forms_next_page(GPtrArray *fields, guint page, JsonObject *values);
void venture_forms_rules_filter(GPtrArray *fields, JsonObject *values, GHashTable *not_shown);

gboolean venture_forms_quiz_field_valid(const VentureFormsField *field, GError **error);
gboolean venture_forms_quiz_validate_band(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error);
gboolean venture_forms_quiz_load(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GError **error);
gboolean venture_forms_quiz_restore(GPtrArray *fields, JsonObject *quiz, GError **error);
JsonObject *venture_forms_quiz(GPtrArray *fields);
gboolean venture_forms_quiz_apply(GPtrArray *fields, JsonObject *answers, VentureEntity *response, GError **error);
gchar *venture_forms_quiz_message(GPtrArray *fields, JsonObject *values, VentureEntity *response, const gchar *success);
void venture_forms_quiz_report(VentureReportResult *result, GPtrArray *responses);

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

/* --- Answer piping: fixed placeholders over validated public answers --- */
gboolean venture_forms_groups_load_metadata(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GError **error);
VentureFormsField *venture_forms_field_from_record(VentureEntity *row);
gboolean venture_forms_validate_piping(VentureDatabase *database, VentureEntity *entity, GError **error);
gboolean venture_forms_piping_definition(GPtrArray *fields, const gchar *success, GError **error);
JsonObject *venture_forms_pipe_values(GPtrArray *fields, JsonObject *raw);
gchar *venture_forms_pipe_text(const gchar *text, GPtrArray *fields,
	const VentureFormsField *target, JsonObject *values);
JsonObject *venture_forms_pipe_stored_values(GPtrArray *fields, JsonObject *answers);
void venture_forms_piping_apply(GPtrArray *fields, JsonObject *values);
JsonObject *venture_forms_pipe_context(GPtrArray *selected, GPtrArray *fields, JsonObject *values);
gchar *venture_forms_pipe_answer(const VentureFormsField *field, JsonNode *answer);
void venture_forms_pipe_counts(GPtrArray *fields, JsonObject *raw, JsonObject *values);
gchar *venture_forms_success_message(VentureDatabase *database, VentureEntity *form,
	VentureEntity *response);
gchar *venture_forms_render_success_text(VentureEntity *form, const gchar *message, gboolean hosted, const gchar *language);

JsonNode *venture_forms_schema_language(VentureDatabase *database, VentureEntity *form,
	const gchar *action, GDateTime *now, const gchar *language, GError **error);
JsonObject *venture_forms_answers_state(GHashTable *answers);
/* --- Versioned public wording --- */
#define VENTURE_FORMS_LANGUAGE "_vf_lang"
GPtrArray *venture_forms_record_definition(VentureDatabase *database, VentureEntity *form, VentureEntity *record, GError **error);
gboolean venture_forms_pipe_validate_text(const gchar *text, GPtrArray *fields, const VentureFormsField *target, GError **error);
gboolean venture_forms_translations_check(GPtrArray *fields, GError **error);
gboolean venture_forms_language_valid(const gchar *language);
gboolean venture_forms_translations_load(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GError **error);
gboolean venture_forms_validate_translation(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error);
void venture_forms_catalog_restore(GPtrArray *fields, JsonObject *catalog);
JsonObject *venture_forms_catalog(GPtrArray *fields);
gchar *venture_forms_language_choose(GPtrArray *fields, const gchar *explicit_language, const gchar *accept);
void venture_forms_localize(GPtrArray *fields, const gchar *language);
const gchar *venture_forms_language(GPtrArray *fields);
const gchar *venture_forms_text(GPtrArray *fields, const gchar *key, const gchar *fallback);
const gchar *venture_forms_field_text(const VentureFormsField *field, const gchar *key, const gchar *fallback);
gchar *venture_forms_language_from_values(VentureEntity *form, VentureEntity *version, JsonObject *values);
void venture_forms_translation_errors(GPtrArray *fields, JsonObject *errors);
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

gboolean venture_forms_booking_definition(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GError **error);
gboolean venture_forms_booking_slots(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GDateTime *now, GError **error);
gboolean venture_forms_booking_prepare(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, JsonObject *answers,
	GDateTime *now, VentureEntity **hold, JsonObject *errors, GError **error);
gboolean venture_forms_booking_finish(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, JsonObject *answers,
	VentureEntity *hold, VentureEntity *response, GDateTime *now, JsonObject *errors, GError **error);
JsonObject *venture_forms_payment(GPtrArray *fields);
gboolean venture_forms_price_load(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GError **error);
gboolean venture_forms_price_restore(GPtrArray *fields, JsonObject *payment, GError **error);
gboolean venture_forms_price_validate(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error);
VentureMoney *venture_forms_price_total(GPtrArray *fields, JsonObject *answers, JsonArray **invoice_lines, GError **error);
/* --- Private attachment lifecycle --- */
typedef struct {
	gchar *key;
	gchar *filename;
	GBytes *bytes;
} VentureFormsUploadPart;
void venture_forms_upload_part_free(gpointer data);
gboolean venture_forms_upload_field_validate(VentureEntity *entity, GError **error);
gboolean venture_forms_upload_definition(GPtrArray *fields, GError **error);
gboolean venture_forms_upload_check_write(VentureEntity *entity, GError **error);
void venture_forms_upload_normalize(GPtrArray *fields, GHashTable *answers);
void venture_forms_upload_check(VentureDatabase *database, VentureEntity *form, GDateTime *now,
	const VentureFormsField *field, GPtrArray *values, JsonObject *answers, JsonObject *errors, GString *summary);
gboolean venture_forms_receive_uploads(VentureDatabase *database, VentureEntity *form, VentureEntity *version,
	GPtrArray *fields, GHashTable *answers, GPtrArray *parts, const gchar *client, GDateTime *now, GError **error);
gsize venture_forms_upload_body_limit(VentureDatabase *database, const gchar *token);
gboolean venture_forms_upload_claim(VentureDatabase *database, VentureEntity *form, VentureEntity *response,
	GPtrArray *fields, GDateTime *now, GPtrArray **claimed, GError **error);
gboolean venture_forms_upload_bind_response(VentureDatabase *database, VentureEntity *response, GPtrArray *claimed, GError **error);
gboolean venture_forms_upload_bind_source(VentureDatabase *database, VentureEntity *form, VentureEntity *source,
	JsonObject *values, GDateTime *expires, GError **error);
gboolean venture_forms_upload_purge_source(VentureDatabase *database, VentureEntity *source, GError **error);
gint64 venture_forms_upload_sweep(VentureDatabase *database, gint64 organization, guint limit, GDateTime *now, GError **error);
GBytes *venture_forms_upload_read(VentureDatabase *database, VentureEntity *upload, GError **error);

G_END_DECLS


#define VENTURE_FORMS_PAYMENT_NONCE "_vf_payment"
#define VENTURE_FORMS_PAYMENT_WRITE "venture-forms-payment-write"
#define VENTURE_FORMS_PAYMENT_SETTLING "venture-forms-payment-settling"
#define VENTURE_FORMS_PAYMENT_REDIRECT "venture-forms-payment-redirect"
gchar *venture_forms_payment_nonce(VentureEntity *form);
gboolean venture_forms_payment_nonce_valid(VentureEntity *form, const gchar *nonce);

gboolean venture_forms_upload_export(VentureDatabase *database, gint64 form_id, JsonNode *answers, GError **error);

#endif /* VENTURE_FORMS_PRIVATE_H */
