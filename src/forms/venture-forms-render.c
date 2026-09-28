/*
 * venture-forms-render.c - The one renderer, the schema and the embed codes
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The builder's preview, the snippet a site pastes, the fragment the
 * script loader fetches and the hosted page are all venture_forms_render().
 * A second renderer would be a second contract, and the vf-* class names
 * are a contract other people's stylesheets are written against: see the
 * table in docs/forms.org, pinned by /forms/class-contract.
 */

#include "venture-forms-private.h"

#include <string.h>

/* ==========================================================================
 * The renderer
 * ========================================================================== */

/* The minimal opt-in stylesheet for the hosted page: layout only, no
 * colours or fonts, so a page that embeds the iframe still decides how it
 * looks everywhere the frame does not. */
static const gchar forms_basic_css[] =
	".vf-page{max-width:40rem;margin:2rem auto;padding:0 1rem}"
	".vf-field{margin:0 0 1rem;border:0;padding:0}"
	".vf-label{display:block;font-weight:600;margin-bottom:.25rem}"
	".vf-input{box-sizing:border-box;max-width:100%}"
	"input.vf-input:not([type=radio]):not([type=checkbox]),textarea.vf-input{width:100%}"
	".vf-choice{display:block}"
	".vf-help,.vf-error{margin:.25rem 0}"
	".vf-error[hidden]{display:none}";

static void
forms_escape(GString *html, const gchar *text)
{
	venture_html_escape_append(html, text != NULL ? text : "");
}

/* Paragraphs from plain text: blank lines separate them, single newlines
 * are line breaks. Escaped; no markup of the author's passes through. */
static void
forms_paragraphs(GString *html, const gchar *text)
{
	g_auto(GStrv) blocks = NULL;
	guint i;

	if (venture_string_is_empty(text))
		return;
	blocks = g_strsplit(text, "\n\n", -1);
	for (i = 0; NULL != blocks[i]; i++)
	{
		g_auto(GStrv) lines = NULL;
		g_autofree gchar *block = g_strstrip(g_strdup(blocks[i]));
		guint j;

		if ('\0' == block[0])
			continue;
		lines = g_strsplit(block, "\n", -1);
		g_string_append(html, "<p>");
		for (j = 0; NULL != lines[j]; j++)
		{
			if (j > 0)
				g_string_append(html, "<br>");
			forms_escape(html, lines[j]);
		}
		g_string_append(html, "</p>");
	}
}

/* The refill value for a single-valued question. */
static const gchar *
forms_value(const VentureFormsRender *options, const gchar *key)
{
	JsonNode *node;

	if (NULL == options->values || !json_object_has_member(options->values, key))
		return NULL;
	node = json_object_get_member(options->values, key);
	if (JSON_NODE_HOLDS_VALUE(node) && G_TYPE_STRING == json_node_get_value_type(node))
		return json_node_get_string(node);
	if (JSON_NODE_HOLDS_ARRAY(node) && json_array_get_length(json_node_get_array(node)) > 0)
		return json_array_get_string_element(json_node_get_array(node), 0);
	return NULL;
}

static gboolean
forms_value_contains(const VentureFormsRender *options, const gchar *key, const gchar *wanted)
{
	JsonNode *node;

	if (NULL == options->values || !json_object_has_member(options->values, key))
		return FALSE;
	node = json_object_get_member(options->values, key);
	if (JSON_NODE_HOLDS_VALUE(node) && G_TYPE_STRING == json_node_get_value_type(node))
		return 0 == g_strcmp0(json_node_get_string(node), wanted);
	if (JSON_NODE_HOLDS_ARRAY(node))
	{
		JsonArray *array = json_node_get_array(node);
		guint i;

		for (i = 0; i < json_array_get_length(array); i++)
			if (0 == g_strcmp0(json_array_get_string_element(array, i), wanted))
				return TRUE;
	}
	return FALSE;
}

static const gchar *
forms_error(const VentureFormsRender *options, const gchar *key)
{
	if (NULL == options->errors || !json_object_has_member(options->errors, key))
		return NULL;
	return json_object_get_string_member(options->errors, key);
}

/* The attributes every input of a question shares: its describedby list
 * and, when refused, aria-invalid. */
static void
forms_described(GString *html, const gchar *id, gboolean has_help, gboolean invalid)
{
	g_string_append(html, " aria-describedby=\"");
	if (has_help)
		g_string_append_printf(html, "%s-help ", id);
	g_string_append_printf(html, "%s-error\"", id);
	if (invalid)
		g_string_append(html, " aria-invalid=\"true\"");
}

static void
forms_render_label_text(GString *html, const gchar *label, gboolean required)
{
	forms_escape(html, label);
	if (required)
		g_string_append(html, "<span class=\"vf-required\" aria-hidden=\"true\">*</span>");
}

static void
forms_render_help_and_error(GString *html, const gchar *id, const gchar *help, const gchar *message)
{
	if (!venture_string_is_empty(help))
	{
		g_string_append_printf(html, "<p class=\"vf-help\" id=\"%s-help\">", id);
		forms_escape(html, help);
		g_string_append(html, "</p>");
	}
	g_string_append_printf(html, "<p class=\"vf-error\" id=\"%s-error\"%s>", id,
	                       NULL != message ? "" : " hidden");
	forms_escape(html, message);
	g_string_append(html, "</p>");
}

static void
forms_render_field(GString *html, const gchar *prefix, const VentureFormsField *field,
	const VentureFormsRender *options)
{
	const gchar *key = field->key;
	const gchar *label = field->label;
	const gchar *help = field->help;
	const gchar *placeholder = field->placeholder;
	const gchar *pattern = field->pattern;
	const gchar *fallback = field->default_value;
	g_autofree gchar *id = g_strdup_printf("%s-%s", prefix, key);
	g_autofree gchar *kind_class = NULL;
	VentureFormFieldKind kind = field->kind;
	const gchar *nick = venture_forms_kind_nick(kind);
	const gchar *message = forms_error(options, key);
	const gchar *value = forms_value(options, key);
	gboolean required = field->required;
	gboolean has_help = !venture_string_is_empty(help);
	gint64 min_length = field->min_length;
	gint64 max_length = field->max_length;
	gdouble min = field->min_value;
	gdouble max = field->max_value;

	if (NULL == value && NULL == options->values)
		value = fallback;

	kind_class = g_strdup(nick);
	g_strdelimit(kind_class, "_", '-');

	if (VENTURE_FORM_FIELD_HIDDEN == kind)
	{
		g_string_append_printf(html, "<input type=\"hidden\" name=\"%s\" data-vf-field=\"%s\" "
		                       "data-vf-kind=\"hidden\" value=\"", key, key);
		forms_escape(html, value);
		g_string_append(html, "\">");
		return;
	}

	if (venture_forms_kind_has_choices(kind) || VENTURE_FORM_FIELD_RATING == kind)
	{
		g_autoptr(GPtrArray) scale = NULL;
		GPtrArray *choices = field->choices;
		const gchar *type = (VENTURE_FORM_FIELD_MULTIPLE_CHOICE == kind) ? "checkbox" : "radio";
		guint i;

		/* A rating's choices are its scale, made here, never stored. */
		if (VENTURE_FORM_FIELD_RATING == kind)
		{
			gint64 low, high, step;

			venture_forms_rating_bounds(field, &low, &high);
			scale = g_ptr_array_new_with_free_func(venture_forms_choice_free);
			choices = scale;
			for (step = low; step <= high && step - low <= 100; step++)
			{
				VentureFormsChoice *choice = g_new0(VentureFormsChoice, 1);

				choice->id = g_strdup_printf("%" G_GINT64_FORMAT, step);
				choice->label = g_strdup(choice->id);
				g_ptr_array_add(choices, choice);
			}
		}

		g_string_append_printf(html, "<fieldset class=\"vf-field vf-field--%s%s\" data-vf-field=\"%s\" "
		                       "data-vf-kind=\"%s\" id=\"%s\"", kind_class,
		                       NULL != message ? " vf-field--invalid" : "", key, nick, id);
		/* A radio group is one answer and can say it is required; a
		 * box group is several and cannot, so the legend's marker and
		 * the note at the top say it instead. */
		if (VENTURE_FORM_FIELD_MULTIPLE_CHOICE == kind)
			g_string_append(html, " role=\"group\"");
		else
			g_string_append_printf(html, " role=\"radiogroup\"%s", required ? " aria-required=\"true\"" : "");
		forms_described(html, id, has_help, NULL != message);
		g_string_append(html, "><legend class=\"vf-label\">");
		forms_render_label_text(html, label, required);
		g_string_append(html, "</legend><div class=\"vf-choices\">");
		for (i = 0; i < choices->len; i++)
		{
			VentureFormsChoice *choice = g_ptr_array_index(choices, i);

			/* Each box has an id, so the error summary can link to the
			 * group's first one. Choice ids are [a-z0-9_-]. */
			g_string_append_printf(html, "<label class=\"vf-choice\"><input class=\"vf-input\" "
			                       "id=\"%s--%s\" type=\"%s\" name=\"%s\" value=\"", id, choice->id, type, key);
			forms_escape(html, choice->id);
			g_string_append_c(html, '"');
			/* A required radio group is satisfied by any one box; a
			 * required checkbox group cannot say "at least one" in
			 * HTML, so the server alone judges it. */
			if (required && VENTURE_FORM_FIELD_MULTIPLE_CHOICE != kind)
				g_string_append(html, " required");
			if (forms_value_contains(options, key, choice->id))
				g_string_append(html, " checked");
			g_string_append(html, "> <span>");
			forms_escape(html, choice->label);
			g_string_append(html, "</span></label>");
		}
		g_string_append(html, "</div>");
		forms_render_help_and_error(html, id, help, message);
		g_string_append(html, "</fieldset>");
		return;
	}

	g_string_append_printf(html, "<div class=\"vf-field vf-field--%s%s\" data-vf-field=\"%s\" "
	                       "data-vf-kind=\"%s\">", kind_class,
	                       NULL != message ? " vf-field--invalid" : "", key, nick);

	if (VENTURE_FORM_FIELD_CHECKBOX == kind)
	{
		g_string_append_printf(html, "<label class=\"vf-label\" for=\"%s\"><input class=\"vf-input\" "
		                       "type=\"checkbox\" id=\"%s\" name=\"%s\" value=\"on\"", id, id, key);
		if (required)
			g_string_append(html, " required aria-required=\"true\"");
		if (NULL != value && (0 == g_strcmp0(value, "on") || 0 == g_strcmp0(value, "true")))
			g_string_append(html, " checked");
		forms_described(html, id, has_help, NULL != message);
		g_string_append(html, "> ");
		forms_render_label_text(html, label, required);
		g_string_append(html, "</label>");
		forms_render_help_and_error(html, id, help, message);
		g_string_append(html, "</div>");
		return;
	}

	g_string_append_printf(html, "<label class=\"vf-label\" for=\"%s\">", id);
	forms_render_label_text(html, label, required);
	g_string_append(html, "</label>");

	if (VENTURE_FORM_FIELD_LONG_TEXT == kind)
	{
		g_string_append_printf(html, "<textarea class=\"vf-input\" id=\"%s\" name=\"%s\" rows=\"5\"", id, key);
		if (!venture_string_is_empty(field->autocomplete))
			g_string_append_printf(html, " autocomplete=\"%s\"", field->autocomplete);
	}
	else
	{
		const gchar *type = "text", *complete = NULL;

		switch (kind)
		{
		case VENTURE_FORM_FIELD_EMAIL: type = "email"; complete = "email"; break;
		case VENTURE_FORM_FIELD_PHONE: type = "tel"; complete = "tel"; break;
		case VENTURE_FORM_FIELD_URL: type = "url"; complete = "url"; break;
		case VENTURE_FORM_FIELD_NUMBER: type = "number"; break;
		case VENTURE_FORM_FIELD_DATE: type = "date"; break;
		case VENTURE_FORM_FIELD_SHORT_TEXT:
		case VENTURE_FORM_FIELD_LONG_TEXT:
		case VENTURE_FORM_FIELD_SINGLE_CHOICE:
		case VENTURE_FORM_FIELD_MULTIPLE_CHOICE:
		case VENTURE_FORM_FIELD_CHECKBOX:
		case VENTURE_FORM_FIELD_RATING:
		case VENTURE_FORM_FIELD_HIDDEN:
		default:
			break;
		}
		g_string_append_printf(html, "<input class=\"vf-input\" id=\"%s\" name=\"%s\" type=\"%s\"", id, key, type);
		/* The question's own token, else what its kind or its lead
		 * mapping says it holds, so a browser can fill it in. */
		if (!venture_string_is_empty(field->autocomplete))
			complete = field->autocomplete;
		else if (NULL == complete && 0 == g_strcmp0(field->maps_to, "name"))
			complete = "name";
		else if (NULL == complete && 0 == g_strcmp0(field->maps_to, "company_name"))
			complete = "organization";
		if (NULL != complete)
			g_string_append_printf(html, " autocomplete=\"%s\"", complete);
		if (VENTURE_FORM_FIELD_NUMBER == kind)
		{
			gchar number[G_ASCII_DTOSTR_BUF_SIZE];

			g_string_append(html, " step=\"any\"");
			if (min != 0 || max != 0)
				g_string_append_printf(html, " min=\"%s\"", g_ascii_dtostr(number, sizeof number, min));
			if (max != 0)
				g_string_append_printf(html, " max=\"%s\"", g_ascii_dtostr(number, sizeof number, max));
		}
		if (!venture_string_is_empty(pattern))
		{
			g_string_append(html, " pattern=\"");
			forms_escape(html, pattern);
			g_string_append_c(html, '"');
		}
		if (NULL != value)
		{
			g_string_append(html, " value=\"");
			forms_escape(html, value);
			g_string_append_c(html, '"');
		}
	}
	if (required)
		g_string_append(html, " required aria-required=\"true\"");
	if (min_length > 0)
		g_string_append_printf(html, " minlength=\"%" G_GINT64_FORMAT "\"", min_length);
	if (max_length > 0)
		g_string_append_printf(html, " maxlength=\"%" G_GINT64_FORMAT "\"", max_length);
	if (!venture_string_is_empty(placeholder))
	{
		g_string_append(html, " placeholder=\"");
		forms_escape(html, placeholder);
		g_string_append_c(html, '"');
	}
	forms_described(html, id, has_help, NULL != message);
	g_string_append_c(html, '>');
	if (VENTURE_FORM_FIELD_LONG_TEXT == kind)
	{
		forms_escape(html, value);
		g_string_append(html, "</textarea>");
	}
	forms_render_help_and_error(html, id, help, message);
	g_string_append(html, "</div>");
}

/* Where the error summary's link for @field goes: the input, or the first
 * box of a group, which is what takes focus when the link is followed. */
static gchar *
forms_error_target(const gchar *prefix, const VentureFormsField *field)
{
	if (VENTURE_FORM_FIELD_RATING == field->kind)
	{
		gint64 low, high;

		venture_forms_rating_bounds(field, &low, &high);
		return g_strdup_printf("%s-%s--%" G_GINT64_FORMAT, prefix, field->key, low);
	}
	if (venture_forms_kind_has_choices(field->kind) && field->choices->len > 0)
		return g_strdup_printf("%s-%s--%s", prefix, field->key,
			((VentureFormsChoice *)g_ptr_array_index(field->choices, 0))->id);
	return g_strdup_printf("%s-%s", prefix, field->key);
}

/*
 * The error summary: at the top, role=alert, a count and one link per
 * refused answer to its question, in the order they are asked. It is
 * focusable (tabindex -1) so the loader can move focus to it after a
 * failed submit, and on the hosted page it takes focus itself -- a person
 * who cannot see the red must be told first what went wrong and then be
 * able to walk to each place. With no errors it is present and hidden, so
 * the loader has somewhere to put them.
 */
static void
forms_render_summary(GString *html, const gchar *prefix, GPtrArray *fields,
	const VentureFormsRender *options, gboolean hosted)
{
	guint count = 0, i;
	const gchar *form_message = forms_error(options, "_form");

	if (NULL != options->errors)
		count = json_object_get_size(options->errors);
	g_string_append_printf(html, "<div class=\"vf-errors\" id=\"%s-errors\" role=\"alert\" tabindex=\"-1\"%s%s>",
	                       prefix, 0 == count ? " hidden" : "", (count > 0 && hosted) ? " autofocus" : "");
	if (0 == count)
	{
		g_string_append(html, "</div>");
		return;
	}
	if (NULL != form_message && 1 == count)
	{
		g_string_append(html, "<p class=\"vf-errors-title\">");
		forms_escape(html, form_message);
		g_string_append(html, "</p></div>");
		return;
	}
	g_string_append_printf(html, "<p class=\"vf-errors-title\">There %s a problem with %u answer%s.</p>"
	                       "<ul class=\"vf-error-list\">", count == 1 ? "is" : "are", count, count == 1 ? "" : "s");
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);
		const gchar *message = forms_error(options, field->key);
		g_autofree gchar *target = NULL;

		if (NULL == message)
			continue;
		target = forms_error_target(prefix, field);
		g_string_append_printf(html, "<li><a href=\"#%s\">", target);
		forms_escape(html, field->label);
		g_string_append(html, ": ");
		forms_escape(html, message);
		g_string_append(html, "</a></li>");
	}
	/* Names that are no question here have nowhere to link to. */
	{
		JsonObjectIter iter;
		const gchar *key;
		JsonNode *node;

		json_object_iter_init(&iter, options->errors);
		while (json_object_iter_next(&iter, &key, &node))
		{
			if (NULL != venture_forms_definition_find(fields, key))
				continue;
			g_string_append(html, "<li>");
			if (0 != g_strcmp0(key, "_form"))
			{
				forms_escape(html, key);
				g_string_append(html, ": ");
			}
			forms_escape(html, json_node_get_string(node));
			g_string_append(html, "</li>");
		}
	}
	g_string_append(html, "</ul></div>");
}

static void
forms_document_open(GString *html, VentureEntity *form, gboolean basic)
{
	g_autofree gchar *title = venture_forms_get_string(form, "title");

	g_string_append(html, "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
	                "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
	                "<meta name=\"robots\" content=\"noindex\"><title>");
	forms_escape(html, venture_string_is_empty(title) ? "Form" : title);
	g_string_append(html, "</title>");
	if (basic)
		g_string_append_printf(html, "<style>%s</style>", forms_basic_css);
	g_string_append(html, "</head><body><main class=\"vf-page\">");
}

static void
forms_document_close(GString *html)
{
	g_string_append(html, "</main></body></html>");
}

gchar *
venture_forms_render(VentureDatabase *database, VentureEntity *form,
	const VentureFormsRender *options, GError **error)
{
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *token = NULL, *title = NULL, *description = NULL, *submit = NULL;
	g_autofree gchar *prefix = NULL, *action = NULL;
	gboolean hosted, preview;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_FORM(form), NULL);
	g_return_val_if_fail(options != NULL, NULL);

	/* A version renders what it froze; no version is the draft, which
	 * only the builder's preview shows. */
	fields = venture_forms_definition_for(database, form, options->version, error);
	if (NULL == fields)
		return NULL;

	token = venture_forms_get_string(form, "public-token");
	title = venture_forms_get_string(form, "title");
	description = venture_forms_get_string(form, "description");
	submit = venture_forms_get_string(form, "submit-label");
	prefix = g_strdup_printf("vf-%s", token != NULL ? token : "form");
	action = (NULL != options->action) ? g_strdup(options->action) :
	         g_strdup_printf("/pub/form/%s", token != NULL ? token : "");
	hosted = (VENTURE_FORMS_RENDER_HOSTED == options->mode) ||
	         (VENTURE_FORMS_RENDER_HOSTED_BASIC == options->mode);
	preview = (VENTURE_FORMS_RENDER_PREVIEW == options->mode);

	html = g_string_new(NULL);
	if (hosted)
		forms_document_open(html, form, VENTURE_FORMS_RENDER_HOSTED_BASIC == options->mode);

	g_string_append_printf(html, "<form class=\"vf-form\" id=\"%s\" data-vf-form=\"%s\" "
	                       "method=\"post\" accept-charset=\"utf-8\"", prefix, token != NULL ? token : "");
	if (!preview)
	{
		g_string_append(html, " action=\"");
		forms_escape(html, action);
		g_string_append_c(html, '"');
	}
	g_string_append_c(html, '>');

	if (!venture_string_is_empty(title))
	{
		g_string_append_printf(html, "<%s class=\"vf-title\">", hosted ? "h1" : "h2");
		forms_escape(html, title);
		g_string_append_printf(html, "</%s>", hosted ? "h1" : "h2");
	}
	if (!venture_string_is_empty(description))
	{
		g_string_append(html, "<div class=\"vf-description\">");
		forms_paragraphs(html, description);
		g_string_append(html, "</div>");
	}

	forms_render_summary(html, prefix, fields, options, hosted);

	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);

		if (field->required && VENTURE_FORM_FIELD_HIDDEN != field->kind)
		{
			/* The asterisk beside each question is hidden from screen
			 * readers, which hear aria-required instead; this sentence
			 * is what explains it to everyone else. */
			g_string_append(html, "<p class=\"vf-required-note\">Questions marked * are required.</p>");
			break;
		}
	}

	for (i = 0; i < fields->len; i++)
		forms_render_field(html, prefix, g_ptr_array_index(fields, i), options);

	/* Hidden with the attribute, which every browser honours with no
	 * stylesheet at all; a page that overrides [hidden] must hide .vf-hp. */
	g_string_append_printf(html, "<div class=\"vf-hp\" hidden aria-hidden=\"true\">"
	                       "<label for=\"%s-hp\">Leave this empty</label>"
	                       "<input id=\"%s-hp\" type=\"text\" name=\"" VENTURE_FORMS_HONEYPOT "\" "
	                       "tabindex=\"-1\" autocomplete=\"off\" value=\"\"></div>", prefix, prefix);
	if (NULL != options->ticket)
	{
		g_string_append(html, "<input type=\"hidden\" name=\"" VENTURE_FORMS_TICKET "\" value=\"");
		forms_escape(html, options->ticket);
		g_string_append(html, "\">");
	}

	g_string_append(html, "<div class=\"vf-actions\"><button class=\"vf-submit\" type=\"submit\"");
	if (preview)
		g_string_append(html, " disabled");
	g_string_append_c(html, '>');
	forms_escape(html, venture_string_is_empty(submit) ? "Send" : submit);
	g_string_append(html, "</button></div></form>");

	if (hosted)
		forms_document_close(html);

	return g_string_free(g_steal_pointer(&html), FALSE);
}

gchar *
venture_forms_render_success(VentureEntity *form, gboolean hosted)
{
	g_autofree gchar *message = NULL;
	GString *html;

	g_return_val_if_fail(VENTURE_IS_FORM(form), NULL);

	message = venture_forms_get_string(form, "success-message");
	html = g_string_new(NULL);
	if (hosted)
		forms_document_open(html, form, FALSE);
	g_string_append(html, "<div class=\"vf-success\" role=\"status\">");
	forms_paragraphs(html, venture_string_is_empty(message) ?
		"Thank you. Your response has been received." : message);
	g_string_append(html, "</div>");
	if (hosted)
		forms_document_close(html);
	return g_string_free(html, FALSE);
}

/* ==========================================================================
 * The schema
 * ========================================================================== */

JsonNode *
venture_forms_schema(VentureDatabase *database, VentureEntity *form,
	const gchar *action, GDateTime *now, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(VentureEntity) version = NULL;
	g_autofree gchar *token = NULL, *title = NULL, *description = NULL, *submit = NULL, *ticket = NULL;
	g_autofree gchar *success = NULL;
	guint i, j;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_FORM(form), NULL);

	/* The schema is public, so it describes the published version only. */
	version = venture_forms_published_version(database, form, error);
	if (NULL == version)
	{
		if (NULL != error && NULL == *error)
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
		return NULL;
	}
	fields = venture_forms_definition_for(database, form, version, error);
	if (NULL == fields)
		return NULL;

	token = venture_forms_get_string(form, "public-token");
	title = venture_forms_get_string(form, "title");
	description = venture_forms_get_string(form, "description");
	submit = venture_forms_get_string(form, "submit-label");
	success = venture_forms_get_string(form, "success-message");
	ticket = venture_forms_ticket_new_for_version(form, venture_forms_get_int(version, "number"), now);

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "schema");
	json_builder_add_int_value(builder, 1);
	json_builder_set_member_name(builder, "form");
	json_builder_add_string_value(builder, token != NULL ? token : "");
	json_builder_set_member_name(builder, "form_version");
	json_builder_add_int_value(builder, venture_forms_get_int(version, "number"));
	json_builder_set_member_name(builder, "title");
	json_builder_add_string_value(builder, title != NULL ? title : "");
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder, description != NULL ? description : "");
	json_builder_set_member_name(builder, "submit_label");
	json_builder_add_string_value(builder, venture_string_is_empty(submit) ? "Send" : submit);
	json_builder_set_member_name(builder, "success_message");
	json_builder_add_string_value(builder, venture_string_is_empty(success) ?
		"Thank you. Your response has been received." : success);
	json_builder_set_member_name(builder, "action");
	json_builder_add_string_value(builder, action != NULL ? action : "");
	json_builder_set_member_name(builder, "honeypot");
	json_builder_add_string_value(builder, VENTURE_FORMS_HONEYPOT);
	json_builder_set_member_name(builder, "ticket_field");
	json_builder_add_string_value(builder, VENTURE_FORMS_TICKET);
	json_builder_set_member_name(builder, "ticket");
	json_builder_add_string_value(builder, ticket);
	json_builder_set_member_name(builder, "fields");
	json_builder_begin_array(builder);
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "key");
		json_builder_add_string_value(builder, field->key);
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder, field->label != NULL ? field->label : "");
		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder, venture_forms_kind_nick(field->kind));
		json_builder_set_member_name(builder, "required");
		json_builder_add_boolean_value(builder, field->required);
		json_builder_set_member_name(builder, "help");
		json_builder_add_string_value(builder, field->help != NULL ? field->help : "");
		json_builder_set_member_name(builder, "placeholder");
		json_builder_add_string_value(builder, field->placeholder != NULL ? field->placeholder : "");
		json_builder_set_member_name(builder, "default");
		json_builder_add_string_value(builder, field->default_value != NULL ? field->default_value : "");
		if (venture_forms_kind_has_choices(field->kind))
		{
			json_builder_set_member_name(builder, "choices");
			json_builder_begin_array(builder);
			for (j = 0; j < field->choices->len; j++)
			{
				VentureFormsChoice *choice = g_ptr_array_index(field->choices, j);

				json_builder_begin_object(builder);
				json_builder_set_member_name(builder, "id");
				json_builder_add_string_value(builder, choice->id);
				json_builder_set_member_name(builder, "label");
				json_builder_add_string_value(builder, choice->label);
				json_builder_end_object(builder);
			}
			json_builder_end_array(builder);
		}
		if (VENTURE_FORM_FIELD_RATING == field->kind)
		{
			gint64 low, high;

			venture_forms_rating_bounds(field, &low, &high);
			json_builder_set_member_name(builder, "min");
			json_builder_add_int_value(builder, low);
			json_builder_set_member_name(builder, "max");
			json_builder_add_int_value(builder, high);
		}
		else if (VENTURE_FORM_FIELD_NUMBER == field->kind && (field->min_value != 0 || field->max_value != 0))
		{
			json_builder_set_member_name(builder, "min");
			json_builder_add_double_value(builder, field->min_value);
			if (field->max_value != 0)
			{
				json_builder_set_member_name(builder, "max");
				json_builder_add_double_value(builder, field->max_value);
			}
		}
		if (field->min_length > 0)
		{
			json_builder_set_member_name(builder, "min_length");
			json_builder_add_int_value(builder, field->min_length);
		}
		if (field->max_length > 0)
		{
			json_builder_set_member_name(builder, "max_length");
			json_builder_add_int_value(builder, field->max_length);
		}
		if (!venture_string_is_empty(field->pattern))
		{
			json_builder_set_member_name(builder, "pattern");
			json_builder_add_string_value(builder, field->pattern);
		}
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

/* ==========================================================================
 * Embed codes
 * ========================================================================== */

gchar *
venture_forms_embed_code(VentureDatabase *database, VentureEntity *form,
	const gchar *base_url, VentureFormsEmbed kind, GError **error)
{
	g_autofree gchar *token = NULL, *public_path = NULL, *base = NULL, *title = NULL;
	GString *code;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_FORM(form), NULL);

	base = g_strdup(base_url != NULL ? base_url : "");
	while (g_str_has_suffix(base, "/"))
		base[strlen(base) - 1] = '\0';
	token = venture_forms_get_string(form, "public-token");
	title = venture_forms_get_string(form, "title");
	public_path = g_strdup_printf("%s/pub/form/%s", base, token != NULL ? token : "");
	code = g_string_new(NULL);

	switch (kind)
	{
	case VENTURE_FORMS_EMBED_HTML:
		{
			VentureFormsRender options;
			g_autoptr(GDateTime) now = venture_time_now();
			g_autoptr(VentureEntity) version = venture_forms_published_version(database, form, NULL);
			g_autofree gchar *ticket = NULL;
			g_autofree gchar *html = NULL;

			/* A snippet copies the published form; before the first
			 * publish there is nothing public to copy, so it shows the
			 * draft, which the door will refuse until it is published. */
			ticket = venture_forms_ticket_new_for_version(form,
				venture_forms_get_int(version, "number"), now);
			options.mode = VENTURE_FORMS_RENDER_SNIPPET;
			options.action = public_path;
			options.ticket = ticket;
			options.values = NULL;
			options.errors = NULL;
			options.version = version;
			html = venture_forms_render(database, form, &options, error);
			if (NULL == html)
			{
				g_string_free(code, TRUE);
				return NULL;
			}
			g_string_append(code, html);
		}
		break;
	case VENTURE_FORMS_EMBED_SCRIPT:
		g_string_append(code, "<div data-venture-form=\"");
		forms_escape(code, public_path);
		g_string_append(code, "/fragment\"></div>\n<script src=\"");
		forms_escape(code, base);
		g_string_append(code, "/pub/forms.js\" defer></script>");
		break;
	case VENTURE_FORMS_EMBED_IFRAME:
		g_string_append(code, "<iframe src=\"");
		forms_escape(code, public_path);
		g_string_append(code, "\" title=\"");
		forms_escape(code, venture_string_is_empty(title) ? "Form" : title);
		g_string_append(code, "\" width=\"100%\" height=\"640\" loading=\"lazy\"></iframe>");
		break;
	case VENTURE_FORMS_EMBED_SCHEMA:
	default:
		g_string_append(code, public_path);
		g_string_append(code, "/schema");
		break;
	}
	return g_string_free(code, FALSE);
}

/* ==========================================================================
 * A response, read against the version it answered
 * ========================================================================== */

/* One stored answer as a person reads it, in @field's terms. */
static gchar *
forms_answer_text(const VentureFormsField *field, JsonNode *node)
{
	if (JSON_NODE_HOLDS_ARRAY(node))
	{
		JsonArray *array = json_node_get_array(node);
		GString *text = g_string_new(NULL);
		guint i;

		for (i = 0; i < json_array_get_length(array); i++)
		{
			const gchar *id = json_array_get_string_element(array, i);
			const gchar *label = field != NULL ? venture_forms_choice_label(field->choices, id) : NULL;

			if (i > 0)
				g_string_append(text, ", ");
			g_string_append(text, label != NULL ? label : id);
		}
		return g_string_free(text, FALSE);
	}
	if (!JSON_NODE_HOLDS_VALUE(node))
		return json_to_string(node, FALSE);
	switch (json_node_get_value_type(node))
	{
	case G_TYPE_BOOLEAN:
		return g_strdup(json_node_get_boolean(node) ? "Yes" : "No");
	case G_TYPE_INT64:
		return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));
	case G_TYPE_DOUBLE:
		{
			gchar number[G_ASCII_DTOSTR_BUF_SIZE];

			return g_strdup(g_ascii_dtostr(number, sizeof number, json_node_get_double(node)));
		}
	case G_TYPE_STRING:
		{
			const gchar *text = json_node_get_string(node);
			const gchar *label = (field != NULL && venture_forms_kind_has_choices(field->kind)) ?
				venture_forms_choice_label(field->choices, text) : NULL;

			return g_strdup(label != NULL ? label : text);
		}
	default:
		return json_to_string(node, FALSE);
	}
}

gchar *
venture_forms_render_answers(VentureDatabase *database, VentureEntity *submission, GError **error)
{
	g_autoptr(VentureEntity) form = NULL;
	g_autoptr(VentureEntity) version = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(JsonNode) answers = NULL;
	g_autofree gchar *text = NULL;
	GString *html;
	JsonObject *object;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_FORM_SUBMISSION(submission), NULL);

	form = venture_database_get(database, VENTURE_TYPE_FORM, venture_forms_get_int(submission, "form-id"), error);
	if (NULL == form)
		return NULL;
	version = venture_database_get(database, VENTURE_TYPE_FORM_VERSION,
		venture_forms_get_int(submission, "version-id"), NULL);
	/* The version the person answered, never today's questions; a response
	 * with no version predates versions and reads against the draft. */
	fields = venture_forms_definition_for(database, form, version, error);
	if (NULL == fields)
		return NULL;
	text = venture_forms_get_string(submission, "answers");
	answers = venture_string_is_empty(text) ? NULL : json_from_string(text, NULL);
	if (NULL == answers || !JSON_NODE_HOLDS_OBJECT(answers))
		return g_strdup("<p class=\"muted\">No answers.</p>");
	object = json_node_get_object(answers);

	html = g_string_new("<dl class=\"form-answers\">");
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);
		g_autofree gchar *shown = NULL;

		if (!json_object_has_member(object, field->key))
			continue;
		shown = forms_answer_text(field, json_object_get_member(object, field->key));
		g_string_append(html, "<dt>");
		forms_escape(html, field->label);
		g_string_append(html, "</dt><dd>");
		forms_paragraphs(html, shown);
		g_string_append(html, "</dd>");
	}
	g_string_append(html, "</dl>");
	if (NULL != version)
		g_string_append_printf(html, "<p class=\"field-help\">Answered on version %" G_GINT64_FORMAT
		                       " of the form.</p>", venture_forms_get_int(version, "number"));
	return g_string_free(html, FALSE);
}
