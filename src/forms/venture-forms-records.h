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
	VENTURE_FORM_FIELD_HIDDEN
} VentureFormFieldKind;

/**
 * venture_form_field_kind_get_type:
 *
 * Returns: the #GType of #VentureFormFieldKind
 */
GType venture_form_field_kind_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_FORM_FIELD_KIND (venture_form_field_kind_get_type())

#define VENTURE_TYPE_FORM (venture_form_get_type())
VENTURE_DECLARE_ENTITY(VentureForm, venture_form, FORM)

#define VENTURE_TYPE_FORM_FIELD (venture_form_field_get_type())
VENTURE_DECLARE_ENTITY(VentureFormField, venture_form_field, FORM_FIELD)

#define VENTURE_TYPE_FORM_VERSION (venture_form_version_get_type())
VENTURE_DECLARE_ENTITY(VentureFormVersion, venture_form_version, FORM_VERSION)

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

G_END_DECLS

#endif /* VENTURE_FORMS_RECORDS_H */
