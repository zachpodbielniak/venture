/*
 * venture-forms-records.c - Forms, their fields and their responses
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

GType
venture_form_state_get_type(void)
{
	static gsize type_id = 0;

	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_FORM_DRAFT, "VENTURE_FORM_DRAFT", "draft" },
			{ VENTURE_FORM_LIVE, "VENTURE_FORM_LIVE", "live" },
			{ VENTURE_FORM_CLOSED, "VENTURE_FORM_CLOSED", "closed" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureFormState", values);
		g_once_init_leave(&type_id, id);
	}

	return type_id;
}

/* The nicks are the public kind names: they appear in the schema and, with
 * underscores as dashes, in the vf-field--<kind> class. Both are contract,
 * so a nick is never renamed. */
GType
venture_form_field_kind_get_type(void)
{
	static gsize type_id = 0;

	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_FORM_FIELD_SHORT_TEXT, "VENTURE_FORM_FIELD_SHORT_TEXT", "short_text" },
			{ VENTURE_FORM_FIELD_LONG_TEXT, "VENTURE_FORM_FIELD_LONG_TEXT", "long_text" },
			{ VENTURE_FORM_FIELD_EMAIL, "VENTURE_FORM_FIELD_EMAIL", "email" },
			{ VENTURE_FORM_FIELD_PHONE, "VENTURE_FORM_FIELD_PHONE", "phone" },
			{ VENTURE_FORM_FIELD_URL, "VENTURE_FORM_FIELD_URL", "url" },
			{ VENTURE_FORM_FIELD_NUMBER, "VENTURE_FORM_FIELD_NUMBER", "number" },
			{ VENTURE_FORM_FIELD_DATE, "VENTURE_FORM_FIELD_DATE", "date" },
			{ VENTURE_FORM_FIELD_SINGLE_CHOICE, "VENTURE_FORM_FIELD_SINGLE_CHOICE", "single_choice" },
			{ VENTURE_FORM_FIELD_MULTIPLE_CHOICE, "VENTURE_FORM_FIELD_MULTIPLE_CHOICE", "multiple_choice" },
			{ VENTURE_FORM_FIELD_CHECKBOX, "VENTURE_FORM_FIELD_CHECKBOX", "checkbox" },
			{ VENTURE_FORM_FIELD_RATING, "VENTURE_FORM_FIELD_RATING", "rating" },
			{ VENTURE_FORM_FIELD_HIDDEN, "VENTURE_FORM_FIELD_HIDDEN", "hidden" },
			{ VENTURE_FORM_FIELD_CONSENT, "VENTURE_FORM_FIELD_CONSENT", "consent" },
			{ VENTURE_FORM_FIELD_PAGE_BREAK, "VENTURE_FORM_FIELD_PAGE_BREAK", "page_break" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureFormFieldKind", values);
		g_once_init_leave(&type_id, id);
	}

	return type_id;
}

/* ==========================================================================
 * Forms
 *
 * What a site embeds. `name` is what the team calls it and never leaves
 * VENTURE; `title` and `description` are what a visitor reads. Everything
 * after `redirect-url` is behaviour a visitor never sees: who may embed it,
 * how fast it may be filled, what a response turns into. The renderer and
 * the schema read the public half by name, so a field added here is
 * private until somebody writes it out on purpose.
 *
 * `public-token` is a capability: the only thing a stranger holds. It is
 * generated on the first save and unique across every organization,
 * because the public door finds a form by token alone. `ticket-key` signs
 * the fill-time tickets the renderer issues; it is sensitive, so it never
 * reaches a response, a form, a log or the AI.
 * ========================================================================== */

static const VentureFieldDecl venture_form_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What your team calls it; visitors never see this"),
	VENTURE_FIELD_REF("venture-id", "Venture", "Optional: the venture it collects for",
	                  "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("state", "State",
	                   "Draft while you build it, live to take responses, closed to stop",
	                   venture_form_state_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("title", "Title", "The heading visitors see; leave empty for none",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("description", "Introduction", "Shown above the questions",
	              VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("submit-label", "Button", "The submit button's words; Send when empty",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("success-message", "Thank-you message",
	              "Shown after a response is received",
	              VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("redirect-url", "Redirect to",
	              "Optional: an https:// page to send people to instead of the message",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("draft-minutes", "Draft lifetime", "Minutes before an unfinished form expires; 0 uses 60",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("opens-at", "Opens at",
	              "Optional: start taking responses at this time",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("closes-at", "Closes at",
	              "Optional: stop taking responses at this time",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("unique-email-field", "One response per email",
	              "Optional: the key of a required email question; blank allows repeat responses",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("response-limit", "Response limit",
	              "Optional: stop after this many responses; 0 for no limit",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("published-version-id", "Published version",
	                  "What the public sees; set by Publish, or pick an earlier version to go back to it",
	                  "form_version", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("published-number", "Version", "The published version's number",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("slug", "Slug", "A short name for links and the CLI; made from the name",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("public-token", "Public token",
	              "The address the form is reached at; clear it to issue a new one",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_UNIQUE |
	              VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD("allowed-origins", "Allowed sites",
	              "Optional: the only sites that may embed it, one https://host per line; "
	              "empty allows any",
	              VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD("hourly-limit", "Responses per hour",
	              "Refuse more than this many an hour; 0 uses 600",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD("min-fill-seconds", "Minimum fill time",
	              "Seconds a person needs at least; faster is treated as a robot. 0 uses 3",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD("create-lead", "Create a lead",
	              "Turn each response into a lead, using the questions' Maps to",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("lead-source", "Lead source", "The source the leads are given; the title when empty",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("campaign-id", "Lead campaign", "Optional: the campaign the leads belong to",
	                  "campaign", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("on-duplicate", "On a known lead",
	              "merge (the default), create or reject, as lead capture does",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("confirmation-field", "Confirm to",
	              "Optional: the key of an email question whose answer gets a confirmation",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("confirmation-subject", "Confirmation subject", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("confirmation-message", "Confirmation message", NULL,
	              VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("privacy-url", "Privacy notice",
	              "Optional: an https:// page explaining what you do with answers; linked on the form",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("retention-days", "Keep responses for",
	              "Days a response is kept before the retention sweep removes it; 0 keeps them",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("retention-action", "After that",
	              "anonymise (the default: keep the counts, drop the answers) or purge (delete)",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("result", "Result", "What a form action just did; never stored",
	              VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_TRANSIENT | VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD("ticket-key", "Ticket key", "Signs fill-time tickets; never shown",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_SENSITIVE | VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD_TEXT("notes", "Notes", "Internal; visitors never see this")
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureForm, venture_form, venture_form_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Form", NULL);)

/* ==========================================================================
 * Form fields
 *
 * One question. `key` is its identity: answers are stored under it, the
 * schema names it, the input is posted as it. It is therefore checked for
 * shape, unique within its form and fixed once saved -- relabel a question
 * freely, but a new key is a new question.
 *
 * `choices` is one choice per line, "id | Label". A line without an id gets
 * one made from its label on save and keeps it; that is what lets a label
 * be corrected without orphaning the answers that chose it.
 * ========================================================================== */

static const VentureFieldDecl venture_form_field_fields[] = {
	VENTURE_FIELD_REF("group-id", "Repeat group", "Optional: this question repeats with its group",
	                  "form_group", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("form-id", "Form", "The form this question belongs to",
	                  "form", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("position", "Order", "Where it comes; lowest first",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("label", "Question", "What the visitor is asked"),
	VENTURE_FIELD("key", "Key",
	              "Its stable name in answers and the schema: lowercase letters, digits, _. "
	              "Fixed once saved",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED |
	              VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_ENUM("kind", "Kind", "What it asks for",
	                   venture_form_field_kind_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("required", "Required", "Refuse a response without it",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("help", "Help", "Optional: a line under the question",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("placeholder", "Placeholder", "Optional: sample text inside an empty box",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("choices", "Choices",
	              "For choice questions: one per line, optionally \"id | Label\"",
	              VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("min-value", "Minimum", "Numbers and ratings: the lowest allowed",
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("max-value", "Maximum", "Numbers and ratings: the highest allowed; 0 for none",
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("min-length", "Minimum length", "Text: the fewest characters; 0 for none",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("max-length", "Maximum length", "Text: the most characters; 0 for the default",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("pattern", "Pattern", "Text: a regular expression the whole answer must match",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("default-value", "Default", "Optional: the value it starts with",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("maps-to", "Maps to",
	              "With Create a lead: name, email, phone, company_name, website or notes",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("marketing-consent", "Marketing permission",
	              "Consent questions only: record checked permission for the captured lead",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("sensitive", "Sensitive",
	              "Kept out of pages, the AI, webhooks, notifications, search and the audit log",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("autocomplete", "Autofill",
	              "Optional: what the browser may fill in, e.g. given-name, postal-code; "
	              "email, phone and web address questions set their own",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};

/* One key per form, deleted questions included: a removed question's
 * answers stay filed under its key, so the key is retired, never reused
 * for a different question. */
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureFormField, venture_form_field, venture_form_field_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Form question", NULL);
	venture_entity_class_set_field_unique_scope(VENTURE_ENTITY_CLASS(klass), "key", "form-id", NULL);)

/* ==========================================================================
 * Form versions
 *
 * What a form asked when it was published, frozen. `definition` is the
 * questions as JSON text -- keys, labels, kinds, choices and limits --
 * which the public door renders and validates against, and which every
 * response answered on it is read against forever after. Publishing makes
 * one; nothing edits one; a response pointing at one keeps it.
 * ========================================================================== */

static const VentureFieldDecl venture_form_version_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_REF("form-id", "Form", "The form this is a version of", "form",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("number", "Number", "1 for the first publish, then counting up",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("published-at", "Published", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("published-by", "Published by", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("definition", "Questions", "The questions as published",
	              VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_TECHNICAL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureFormVersion, venture_form_version, venture_form_version_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Form version", NULL);)

/* ==========================================================================
 * Form responses
 *
 * One person's answers. Labelled "Form response" because the attribution
 * module already owns "Form submission" for its site captures, and two
 * pages called the same thing listing different rows is worse than a
 * less obvious word.
 *
 * `answers` is a JSON object keyed by field key; a choice is its choice
 * id, a multiple choice an array of them, a number a number and a
 * checkbox a boolean. `summary` is the same answers with labels, for
 * search and for people. Both, and the form, are fixed once written: a
 * response is evidence of what somebody sent, and only the forms service
 * creates one. What staff may change is whether it was reviewed and
 * their own notes on it.
 * ========================================================================== */

static const VentureFieldDecl venture_form_submission_fields[] = {
	VENTURE_FIELD_NAME("name", "From", "Who it is from, as they gave it"),
	VENTURE_FIELD_REF("form-id", "Form", "The form it answers", "form",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("version-id", "Form version", "The version of the questions it answered",
	                  "form_version", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("version-number", "Version", NULL,
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("submitted-at", "Received", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("summary", "Answers", "The answers as a person reads them",
	              VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("answers", "Answer data", "The answers by question key, as stored",
	              VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD("not-shown", "Questions not shown", "Private branch accounting for aggregate reports",
	              VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_SENSITIVE | VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD("sensitive-answers", "Sensitive answers",
	              "Answers to sensitive questions, kept apart so they never leave the record",
	              VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_SENSITIVE | VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD("anonymised-at", "Anonymised", "When the retention sweep removed its answers",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("lead-id", "Lead", "The lead this response became", "lead",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("mapping-note", "Follow-up note",
	              "Why a lead or confirmation was not made, when one was asked for",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("origin", "Sent from", "The site it was sent from, when the browser said",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD("reviewed", "Reviewed", "Someone has read it",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("notes", "Notes", "Your team's notes on it")
};

/* Everything a stranger typed, and what staff wrote about it, stays off
 * the shared audit log: an erased response must leave nothing behind but
 * the fact that it existed. */
static const gchar *const venture_form_submission_private[] = {
	"name", "summary", "answers", "sensitive-answers", "notes", "origin", "mapping-note", NULL
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureFormSubmission, venture_form_submission,
	venture_form_submission_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Form response", NULL);
	venture_entity_class_set_audit_private(VENTURE_ENTITY_CLASS(klass), venture_form_submission_private);)

/* Intermediate answers are all sensitive, including otherwise ordinary fields:
 * a person has not yet submitted them. Generation makes sensitive-only edits
 * visible to the generic writer's empty-diff check and invalidates stale tokens. */
static const VentureFieldDecl venture_form_draft_fields[] = {
	VENTURE_FIELD_NAME("name", "Draft", "Intermediate form response"),
	VENTURE_FIELD_REF("form-id", "Form", NULL, "form", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("version-id", "Form version", NULL, "form_version", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("page", "Page", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("finishing", "Finishing", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD("generation", "Generation", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD("expires-at", "Expires", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("answers", "Unsubmitted answers", NULL, VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("ticket", "Fill-time ticket", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureFormDraft, venture_form_draft, venture_form_draft_fields,
	venture_entity_class_set_working_copy(VENTURE_ENTITY_CLASS(klass));
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Form draft", NULL);)

GType
venture_form_rule_action_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_FORM_RULE_SHOW, "VENTURE_FORM_RULE_SHOW", "show" },
			{ VENTURE_FORM_RULE_HIDE, "VENTURE_FORM_RULE_HIDE", "hide" },
			{ VENTURE_FORM_RULE_REQUIRE, "VENTURE_FORM_RULE_REQUIRE", "require" },
			{ VENTURE_FORM_RULE_JUMP, "VENTURE_FORM_RULE_JUMP", "jump" },
			{ VENTURE_FORM_RULE_END, "VENTURE_FORM_RULE_END", "end" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureFormRuleAction", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

static const VentureFieldDecl venture_form_rule_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "A name for this rule"),
	VENTURE_FIELD_REF("form-id", "Form", "The form this rule belongs to", "form", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("position", "Order", "First matching navigation rule wins", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("action", "Action", "What happens when the conditions match", venture_form_rule_action_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("target-key", "Target", "Question key, or page-break key for a jump; empty for end", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("conditions", "Conditions", "Array of field, operator and value objects; no expressions or scripts", VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("any-condition", "Any condition", "Match any condition instead of requiring all", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureFormRule, venture_form_rule, venture_form_rule_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Form rule", NULL);)

static const VentureFieldDecl venture_form_group_fields[] = {
	VENTURE_FIELD_NAME("label", "Group", "What each repeated row is called"),
	VENTURE_FIELD_REF("form-id", "Form", "The form containing these questions", "form", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("key", "Key", "Stable group name in answers and indexed input names",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("min-rows", "Minimum rows", "Fewest rows accepted; 0 permits no rows", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("max-rows", "Maximum rows", "Most rows accepted; 0 uses 10, at most 50", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureFormGroup, venture_form_group, venture_form_group_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Form repeat group", NULL);
	venture_entity_class_set_field_unique_scope(VENTURE_ENTITY_CLASS(klass), "key", "form-id", NULL);)
