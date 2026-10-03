/*
 * venture-forms-records.h - Forms, their fields and their responses
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The forms module's record types. A form is what a site embeds; its
 * fields are ordered questions with a stable key; a response is one
 * person's answers, keyed by those stable keys and choice ids so that a
 * relabelled question still reads its old answers. The rules that span
 * rows are save validators in venture-forms.c, so every writer obeys
 * them.
 */

#ifndef VENTURE_FORMS_RECORDS_H
#define VENTURE_FORMS_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

/**
 * VentureFormState:
 * @VENTURE_FORM_DRAFT: being built; the public address answers not found
 * @VENTURE_FORM_LIVE: accepting responses
 * @VENTURE_FORM_CLOSED: no longer accepting; answers not found like a draft
 *
 * Whether a form takes responses. Draft is the zero value so a form is
 * never live by accident.
 */
typedef enum
{
	VENTURE_FORM_DRAFT = 0,
	VENTURE_FORM_LIVE,
	VENTURE_FORM_CLOSED
} VentureFormState;

/**
 * venture_form_state_get_type:
 *
 * Returns: the #GType of #VentureFormState
 */
GType venture_form_state_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_FORM_STATE (venture_form_state_get_type())

/**
 * VentureFormFieldKind:
 * @VENTURE_FORM_FIELD_SHORT_TEXT: one line of text
 * @VENTURE_FORM_FIELD_LONG_TEXT: a paragraph
 * @VENTURE_FORM_FIELD_EMAIL: an email address
 * @VENTURE_FORM_FIELD_PHONE: a telephone number
 * @VENTURE_FORM_FIELD_URL: a web address
 * @VENTURE_FORM_FIELD_NUMBER: a number, optionally bounded
 * @VENTURE_FORM_FIELD_DATE: a calendar date
 * @VENTURE_FORM_FIELD_SINGLE_CHOICE: exactly one of the choices
 * @VENTURE_FORM_FIELD_MULTIPLE_CHOICE: any of the choices
 * @VENTURE_FORM_FIELD_CHECKBOX: one yes-or-no box
 * @VENTURE_FORM_FIELD_RATING: a whole number on a scale
 * @VENTURE_FORM_FIELD_HIDDEN: a value the page supplies, not the person
 * @VENTURE_FORM_FIELD_FILE: bounded private file attachments
 * @VENTURE_FORM_FIELD_BOOKING: selects a live slot from an existing booking page
 * @VENTURE_FORM_FIELD_PAGE_BREAK: starts a new page, with a heading and introduction
 * @VENTURE_FORM_FIELD_CONSENT: one box that records a permission: the
 *   exact wording shown, the version and the time, and nothing when it is
 *   left unticked
 *
 * What a question asks for, which decides its input and its validation.
 * Short text is the zero value: a field saved without a kind is the
 * least surprising one, and the column was never added to a populated
 * table, so nothing else reads back as it.
 */
typedef enum
{
	VENTURE_FORM_FIELD_SHORT_TEXT = 0,
	VENTURE_FORM_FIELD_LONG_TEXT,
	VENTURE_FORM_FIELD_EMAIL,
	VENTURE_FORM_FIELD_PHONE,
	VENTURE_FORM_FIELD_URL,
	VENTURE_FORM_FIELD_NUMBER,
	VENTURE_FORM_FIELD_DATE,
	VENTURE_FORM_FIELD_SINGLE_CHOICE,
	VENTURE_FORM_FIELD_MULTIPLE_CHOICE,
	VENTURE_FORM_FIELD_CHECKBOX,
	VENTURE_FORM_FIELD_RATING,
	VENTURE_FORM_FIELD_HIDDEN,
	VENTURE_FORM_FIELD_CONSENT,
	VENTURE_FORM_FIELD_PAGE_BREAK,
	VENTURE_FORM_FIELD_BOOKING,
	VENTURE_FORM_FIELD_FILE
} VentureFormFieldKind;

/**
 * venture_form_field_kind_get_type:
 *
 * Returns: the #GType of #VentureFormFieldKind
 */
GType venture_form_field_kind_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_FORM_FIELD_KIND (venture_form_field_kind_get_type())

/**
 * VentureFormRuleAction:
 * @VENTURE_FORM_RULE_SHOW: show a question when its condition matches
 * @VENTURE_FORM_RULE_HIDE: hide a question when its condition matches
 * @VENTURE_FORM_RULE_REQUIRE: require a visible question when matched
 * @VENTURE_FORM_RULE_JUMP: continue at a later page
 * @VENTURE_FORM_RULE_END: finish after the condition's page
 */
typedef enum
{
	VENTURE_FORM_RULE_SHOW = 0,
	VENTURE_FORM_RULE_HIDE,
	VENTURE_FORM_RULE_REQUIRE,
	VENTURE_FORM_RULE_JUMP,
	VENTURE_FORM_RULE_END
} VentureFormRuleAction;

/**
 * venture_form_rule_action_get_type:
 *
 * Returns: the #GType of #VentureFormRuleAction
 */
GType venture_form_rule_action_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_FORM_RULE_ACTION (venture_form_rule_action_get_type())

#define VENTURE_TYPE_FORM_GROUP (venture_form_group_get_type())
VENTURE_DECLARE_ENTITY(VentureFormGroup, venture_form_group, FORM_GROUP)

/**
 * venture_form_group_new:
 *
 * Returns: (transfer full): an unsaved repeating question group
 */

#define VENTURE_TYPE_FORM_RULE (venture_form_rule_get_type())
VENTURE_DECLARE_ENTITY(VentureFormRule, venture_form_rule, FORM_RULE)

/**
 * venture_form_rule_new:
 *
 * Returns: (transfer full): an unsaved conditional rule
 */

#define VENTURE_TYPE_FORM (venture_form_get_type())
VENTURE_DECLARE_ENTITY(VentureForm, venture_form, FORM)

#define VENTURE_TYPE_FORM_FIELD (venture_form_field_get_type())
VENTURE_DECLARE_ENTITY(VentureFormField, venture_form_field, FORM_FIELD)

#define VENTURE_TYPE_FORM_VERSION (venture_form_version_get_type())
VENTURE_DECLARE_ENTITY(VentureFormVersion, venture_form_version, FORM_VERSION)

#define VENTURE_TYPE_FORM_DRAFT_RECORD (venture_form_draft_get_type())
VENTURE_DECLARE_ENTITY(VentureFormDraft, venture_form_draft, FORM_DRAFT_RECORD)

/**
 * venture_form_draft_new:
 *
 * Returns: (transfer full): an unsaved intermediate response, owned by the forms service
 */

#define VENTURE_TYPE_FORM_SUBMISSION (venture_form_submission_get_type())
VENTURE_DECLARE_ENTITY(VentureFormSubmission, venture_form_submission, FORM_SUBMISSION)

/**
 * venture_form_new:
 *
 * Returns: (transfer full): an unsaved form
 */
/**
 * venture_form_field_new:
 *
 * Returns: (transfer full): an unsaved form question
 */
/**
 * venture_form_version_new:
 *
 * Returns: (transfer full): an unsaved form version; only publishing may
 *   save one
 */
/**
 * venture_form_submission_new:
 *
 * Returns: (transfer full): an unsaved form response; only the forms
 *   service may save one
 */

#define VENTURE_TYPE_FORM_PENDING (venture_form_pending_get_type())
VENTURE_DECLARE_ENTITY(VentureFormPending, venture_form_pending, FORM_PENDING)
/**
 * venture_form_pending_new:
 * Returns: (transfer full): unsaved service-owned unconfirmed signup
 */


#define VENTURE_TYPE_FORM_TRANSLATION (venture_form_translation_get_type())
VENTURE_DECLARE_ENTITY(VentureFormTranslation, venture_form_translation, FORM_TRANSLATION)
/**
 * venture_form_translation_new:
 *
 * Creates translated public wording for one form and language.
 *
 * Returns: (transfer full): a new #VentureFormTranslation
 */

#define VENTURE_TYPE_FORM_RESULT_BAND (venture_form_result_band_get_type())
VENTURE_DECLARE_ENTITY(VentureFormResultBand, venture_form_result_band, FORM_RESULT_BAND)
/**
 * venture_form_result_band_new:
 * Returns: (transfer full): an unsaved score range and public result
 */

#define VENTURE_TYPE_FORM_PAYMENT (venture_form_payment_get_type())
VENTURE_DECLARE_ENTITY(VentureFormPayment, venture_form_payment, FORM_PAYMENT)
/**
 * venture_form_payment_new:
 * Returns: (transfer full): an unsaved service-owned payment intake
 */

#define VENTURE_TYPE_FORM_PRICE (venture_form_price_get_type())
VENTURE_DECLARE_ENTITY(VentureFormPrice, venture_form_price, FORM_PRICE)
/**
 * venture_form_price_new:
 * Returns: (transfer full): an unsaved server price declaration
 */
#define VENTURE_TYPE_FORM_UPLOAD (venture_form_upload_get_type())
VENTURE_DECLARE_ENTITY(VentureFormUpload, venture_form_upload, FORM_UPLOAD)
/**
 * venture_form_upload_new:
 * Returns: (transfer full): an unsaved service-owned private upload
 */
G_END_DECLS

#endif /* VENTURE_FORMS_RECORDS_H */
