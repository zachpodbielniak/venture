/*
 * venture-forms.c - Forms: validators, the renderer and the public intake
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A form is the one place VENTURE takes writes from strangers with no
 * session at all. Two rules shape this file:
 *
 *  - One renderer. The builder's preview, the snippet a site pastes, the
 *    fragment the script loader fetches and the hosted page are all
 *    venture_forms_render(). A second renderer would be a second contract,
 *    and the vf-* class names are a contract other people's stylesheets are
 *    written against.
 *
 *  - The server is the authority. Every answer is checked here against the
 *    question's kind whatever the browser did; an unknown name is refused,
 *    never dropped, because a dropped field is a silent data loss the
 *    person who filled the form in never hears about.
 */

#include "venture.h"

#include <math.h>
#include <string.h>

#define VENTURE_FORMS_STATE_KEY "venture-forms-installed"

/* Set on a response by venture_forms_submit() and nowhere else: the
 * validator refuses a new response without it, which is what makes the
 * public door the only way one is created. */
#define VENTURE_FORMS_ACCEPTING_KEY "venture-forms-accepting"

#define VENTURE_FORMS_DEFAULT_FILL_SECONDS 3
#define VENTURE_FORMS_KEY_MAX 63

/* ==========================================================================
 * Small readers
 * ========================================================================== */

static gchar *
forms_string(VentureEntity *entity, const gchar *property)
{
	gchar *value = NULL;

	g_object_get(entity, property, &value, NULL);
	return value;
}

static gint64
forms_int(VentureEntity *entity, const gchar *property)
{
	gint64 value = 0;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);
	return value;
}

static gdouble
forms_double(VentureEntity *entity, const gchar *property)
{
	gdouble value = 0;

	g_object_get(entity, property, &value, NULL);
	return value;
}

static gboolean
forms_bool(VentureEntity *entity, const gchar *property)
{
	gboolean value = FALSE;

	g_object_get(entity, property, &value, NULL);
	return value;
}

static VentureFormFieldKind
forms_kind(VentureEntity *field)
{
	VentureFormFieldKind kind = VENTURE_FORM_FIELD_SHORT_TEXT;

	g_object_get(field, "kind", &kind, NULL);
	return kind;
}

/* The kind's public name, "short_text"; the class uses dashes. */
static const gchar *
forms_kind_nick(VentureFormFieldKind kind)
{
	GEnumClass *klass;
	GEnumValue *value;

	klass = g_type_class_peek(VENTURE_TYPE_FORM_FIELD_KIND);
	if (NULL == klass)
		klass = g_type_class_ref(VENTURE_TYPE_FORM_FIELD_KIND);
	value = g_enum_get_value(klass, (gint)kind);
	return (NULL != value) ? value->value_nick : "short_text";
}

static gboolean
forms_kind_has_choices(VentureFormFieldKind kind)
{
	return (VENTURE_FORM_FIELD_SINGLE_CHOICE == kind) ||
	       (VENTURE_FORM_FIELD_MULTIPLE_CHOICE == kind);
}

/* ==========================================================================
 * Choices
 *
 * Stored as lines of "id | Label". Parsed here into pairs; the save
 * validator is what writes ids onto lines that lack one.
 * ========================================================================== */

typedef struct
{
	gchar	*id;
	gchar	*label;
} FormsChoice;

static void
forms_choice_free(gpointer data)
{
	FormsChoice *choice = data;

	g_free(choice->id);
	g_free(choice->label);
	g_free(choice);
}

/* A choice id: what a label becomes when it has no id of its own. */
static gchar *
forms_choice_slug(const gchar *label)
{
	GString *id;
	const gchar *cursor;
	gboolean gap = FALSE;

	id = g_string_new(NULL);
	for (cursor = label; *cursor != '\0'; cursor++)
	{
		if (g_ascii_isalnum(*cursor))
		{
			if (gap && id->len > 0)
				g_string_append_c(id, '_');
			g_string_append_c(id, g_ascii_tolower(*cursor));
			gap = FALSE;
		}
		else
			gap = TRUE;

		if (id->len >= VENTURE_FORMS_KEY_MAX - 4)
			break;
	}
	if (0 == id->len || !g_ascii_isalpha(id->str[0]))
		g_string_prepend(id, "choice_");
	return g_string_free(id, FALSE);
}

static gboolean
forms_choice_id_valid(const gchar *id)
{
	const gchar *cursor;

	if (venture_string_is_empty(id) || strlen(id) > VENTURE_FORMS_KEY_MAX)
		return FALSE;
	for (cursor = id; *cursor != '\0'; cursor++)
		if (!(g_ascii_islower(*cursor) || g_ascii_isdigit(*cursor) ||
		      '_' == *cursor || '-' == *cursor))
			return FALSE;
	return TRUE;
}

/*
 * Parses @text into choices. With @assign, a line with no id gets one made
 * from its label, made unique against the ids before it; without, such a
 * line uses the made id without the uniqueness suffix (only a stored,
 * already-assigned list is parsed that way, so it never arises).
 */
static GPtrArray *
forms_choices_parse(const gchar *text, gboolean assign, GError **error)
{
	g_autoptr(GPtrArray) choices = NULL;
	g_auto(GStrv) lines = NULL;
	guint i;

	choices = g_ptr_array_new_with_free_func(forms_choice_free);
	if (venture_string_is_empty(text))
		return g_steal_pointer(&choices);

	lines = g_strsplit(text, "\n", -1);
	for (i = 0; NULL != lines[i]; i++)
	{
		g_autofree gchar *line = g_strstrip(g_strdup(lines[i]));
		FormsChoice *choice;
		gchar *bar;
		guint j;

		if ('\0' == line[0])
			continue;

		choice = g_new0(FormsChoice, 1);
		bar = strchr(line, '|');
		if (NULL != bar)
		{
			*bar = '\0';
			choice->id = g_strstrip(g_strdup(line));
			choice->label = g_strstrip(g_strdup(bar + 1));
		}
		else
		{
			choice->label = g_strdup(line);
			choice->id = forms_choice_slug(line);
			if (assign)
			{
				g_autofree gchar *base = g_strdup(choice->id);
				guint n = 2;

				for (j = 0; j < choices->len; j++)
				{
					FormsChoice *before = g_ptr_array_index(choices, j);

					if (0 == g_strcmp0(before->id, choice->id))
					{
						g_free(choice->id);
						choice->id = g_strdup_printf("%s_%u", base, n++);
						j = (guint)-1;
					}
				}
			}
		}
		g_ptr_array_add(choices, choice);

		if (!forms_choice_id_valid(choice->id))
		{
			venture_set_error_validation(error, "Choices",
				"\"%s\" is not a usable id: lowercase letters, digits, _ and - only",
				choice->id);
			return NULL;
		}
		if ('\0' == choice->label[0])
		{
			venture_set_error_validation(error, "Choices",
				"the choice \"%s\" has no label", choice->id);
			return NULL;
		}
		for (j = 0; j + 1 < choices->len; j++)
		{
			FormsChoice *before = g_ptr_array_index(choices, j);

			if (0 == g_strcmp0(before->id, choice->id))
			{
				venture_set_error_validation(error, "Choices",
					"the id \"%s\" is used twice", choice->id);
				return NULL;
			}
		}
	}
	return g_steal_pointer(&choices);
}

static GPtrArray *
forms_field_choices(VentureEntity *field)
{
	g_autofree gchar *text = forms_string(field, "choices");
	GPtrArray *choices;

	choices = forms_choices_parse(text, FALSE, NULL);
	return (NULL != choices) ? choices : g_ptr_array_new_with_free_func(forms_choice_free);
}

static const gchar *
forms_choice_label(GPtrArray *choices, const gchar *id)
{
	guint i;

	for (i = 0; i < choices->len; i++)
	{
		FormsChoice *choice = g_ptr_array_index(choices, i);

		if (0 == g_strcmp0(choice->id, id))
			return choice->label;
	}
	return NULL;
}

/* The scale a rating question offers: its bounds, or 1 to 5. */
static void
forms_rating_bounds(VentureEntity *field, gint64 *low, gint64 *high)
{
	gdouble min = forms_double(field, "min-value");
	gdouble max = forms_double(field, "max-value");

	if (0 == min && 0 == max)
	{
		*low = 1;
		*high = 5;
		return;
	}
	*low = (gint64)ceil(min);
	*high = (gint64)floor(max);
}

/* ==========================================================================
 * Save validators
 * ========================================================================== */

/* An origin as a browser sends it: scheme, host and optional port. */
static gboolean
forms_origin_valid(const gchar *origin)
{
	g_autoptr(GUri) uri = NULL;
	const gchar *scheme;

	uri = g_uri_parse(origin, G_URI_FLAGS_NONE, NULL);
	if (NULL == uri)
		return FALSE;
	scheme = g_uri_get_scheme(uri);
	if (0 != g_strcmp0(scheme, "https") && 0 != g_strcmp0(scheme, "http"))
		return FALSE;
	if (venture_string_is_empty(g_uri_get_host(uri)))
		return FALSE;
	if (!venture_string_is_empty(g_uri_get_path(uri)) || NULL != g_uri_get_query(uri) ||
	    NULL != g_uri_get_fragment(uri) || NULL != g_uri_get_userinfo(uri))
		return FALSE;
	return TRUE;
}

/* The allowed origins as a list, split on lines, commas and spaces. */
static GStrv
forms_origins(VentureEntity *form)
{
	g_autofree gchar *text = forms_string(form, "allowed-origins");
	g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
	g_auto(GStrv) parts = NULL;
	guint i;

	if (venture_string_is_empty(text))
		return g_strv_builder_end(builder);
	g_strdelimit(text, ",\t\r ", '\n');
	parts = g_strsplit(text, "\n", -1);
	for (i = 0; NULL != parts[i]; i++)
		if ('\0' != parts[i][0])
			g_strv_builder_add(builder, parts[i]);
	return g_strv_builder_end(builder);
}

static gboolean
forms_redirect_valid(const gchar *url)
{
	g_autoptr(GUri) uri = NULL;
	const gchar *scheme;

	uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	if (NULL == uri)
		return FALSE;
	scheme = g_uri_get_scheme(uri);
	return (0 == g_strcmp0(scheme, "https") || 0 == g_strcmp0(scheme, "http")) &&
	       !venture_string_is_empty(g_uri_get_host(uri));
}

static gboolean
forms_validate_form(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autofree gchar *token = forms_string(entity, "public-token");
	g_autofree gchar *key = forms_string(entity, "ticket-key");
	g_autofree gchar *slug = forms_string(entity, "slug");
	g_autofree gchar *redirect = forms_string(entity, "redirect-url");
	g_autofree gchar *policy = forms_string(entity, "on-duplicate");
	g_autofree gchar *confirm = forms_string(entity, "confirmation-field");
	g_auto(GStrv) origins = NULL;
	guint i;

	(void)previous;
	(void)user_data;

	if (!venture_string_is_empty(redirect) && !forms_redirect_valid(redirect))
	{
		venture_set_error_validation(error, "Redirect to",
			"must be an http:// or https:// address");
		return FALSE;
	}

	origins = forms_origins(entity);
	for (i = 0; NULL != origins[i]; i++)
	{
		if (!forms_origin_valid(origins[i]))
		{
			venture_set_error_validation(error, "Allowed sites",
				"\"%s\" is not an origin: write https://host or https://host:port, "
				"with no path", origins[i]);
			return FALSE;
		}
	}

	if (forms_int(entity, "response-limit") < 0)
	{
		venture_set_error_validation(error, "Response limit", "cannot be negative");
		return FALSE;
	}
	if (forms_int(entity, "hourly-limit") < 0)
	{
		venture_set_error_validation(error, "Responses per hour", "cannot be negative");
		return FALSE;
	}
	if (forms_int(entity, "min-fill-seconds") < 0 || forms_int(entity, "min-fill-seconds") > 3600)
	{
		venture_set_error_validation(error, "Minimum fill time",
			"must be between 0 and 3600 seconds");
		return FALSE;
	}
	if (!venture_string_is_empty(policy) && 0 != g_strcmp0(policy, "merge") &&
	    0 != g_strcmp0(policy, "create") && 0 != g_strcmp0(policy, "reject"))
	{
		venture_set_error_validation(error, "On a known lead",
			"must be merge, create or reject, not \"%s\" (duplicate handling)", policy);
		return FALSE;
	}
	if (!venture_string_is_empty(confirm) && !forms_choice_id_valid(confirm))
	{
		venture_set_error_validation(error, "Confirm to", "must be a question's key");
		return FALSE;
	}

	/* A capability is only as good as its randomness; a blank one is
	 * reissued rather than refused, which is also how it is rotated. */
	if (venture_string_is_empty(token))
	{
		g_free(token);
		token = venture_generate_token(16);
		g_object_set(entity, "public-token", token, NULL);
	}
	else if (strlen(token) > 128 || strchr(token, '/') != NULL || strchr(token, '?') != NULL ||
	         strchr(token, '#') != NULL || strchr(token, '%') != NULL)
	{
		venture_set_error_validation(error, "Public token",
			"must be plain letters and digits; clear it to issue a new one");
		return FALSE;
	}

	/* The door finds a form by token alone, across every organization. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM);
		g_autoptr(GPtrArray) rows = NULL;

		venture_query_set_include_deleted(query, TRUE);
		venture_query_set_limit(query, 2);
		venture_query_add_filter_string(query, "public-token", VENTURE_FILTER_OP_EQ, token, NULL);
		rows = venture_database_find(database, query, error);
		if (NULL == rows)
			return FALSE;
		for (i = 0; i < rows->len; i++)
		{
			if (venture_entity_get_id(g_ptr_array_index(rows, i)) != venture_entity_get_id(entity))
			{
				venture_set_error_validation(error, "Public token",
					"is already in use by another form; clear it to issue a new one");
				return FALSE;
			}
		}
	}

	if (venture_string_is_empty(key))
	{
		g_free(key);
		key = venture_generate_token(32);
		g_object_set(entity, "ticket-key", key, NULL);
	}

	/* The slug index includes deleted rows, so the check does too. A slug
	 * made from the name is numbered until free; a typed one that is taken
	 * is refused, since its author meant that exact word. */
	{
		gboolean made = venture_string_is_empty(slug);
		g_autofree gchar *base = NULL;
		guint n = 2;

		if (made)
		{
			g_autofree gchar *name = forms_string(entity, "name");

			g_free(slug);
			slug = venture_slugify(name);
			if (venture_string_is_empty(slug))
			{
				g_free(slug);
				slug = g_strdup("form");
			}
		}
		base = g_strdup(slug);
		for (;;)
		{
			g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM);
			g_autoptr(GPtrArray) rows = NULL;
			gboolean taken = FALSE;

			venture_query_set_include_deleted(query, TRUE);
			venture_query_set_organization(query, venture_entity_get_organization_id(entity));
			venture_query_set_limit(query, 2);
			venture_query_add_filter_string(query, "slug", VENTURE_FILTER_OP_EQ, slug, NULL);
			rows = venture_database_find(database, query, error);
			if (NULL == rows)
				return FALSE;
			for (i = 0; i < rows->len; i++)
				taken = taken || venture_entity_get_id(g_ptr_array_index(rows, i)) != venture_entity_get_id(entity);
			if (!taken)
				break;
			if (!made || n > 1000)
			{
				venture_set_error_validation(error, "Slug",
					"\"%s\" is already used by another form here", slug);
				return FALSE;
			}
			g_free(slug);
			slug = g_strdup_printf("%s-%u", base, n++);
		}
		if (made)
			g_object_set(entity, "slug", slug, NULL);
	}

	return TRUE;
}

static const gchar *const forms_lead_targets[] = {
	"name", "email", "phone", "company_name", "website", "notes", NULL
};

static gboolean
forms_validate_field(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureEntity) form = NULL;
	g_autofree gchar *key = forms_string(entity, "key");
	g_autofree gchar *pattern = forms_string(entity, "pattern");
	g_autofree gchar *maps = forms_string(entity, "maps-to");
	g_autofree gchar *choices_text = forms_string(entity, "choices");
	VentureFormFieldKind kind = forms_kind(entity);
	gint64 form_id = forms_int(entity, "form-id");
	gdouble min = forms_double(entity, "min-value");
	gdouble max = forms_double(entity, "max-value");
	gint64 min_length = forms_int(entity, "min-length");
	gint64 max_length = forms_int(entity, "max-length");

	(void)user_data;

	/* --- The form it belongs to, which never changes --- */

	if (NULL != previous && forms_int(previous, "form-id") != form_id)
	{
		venture_set_error_validation(error, "Form",
			"cannot be changed; answers already filed under this question would "
			"move to another form");
		return FALSE;
	}
	form = venture_database_get(database, VENTURE_TYPE_FORM, form_id, NULL);
	if (NULL == form || venture_entity_get_organization_id(form) != venture_entity_get_organization_id(entity))
	{
		venture_set_error_validation(error, "Form",
			"#%" G_GINT64_FORMAT " does not exist in this organization", form_id);
		return FALSE;
	}

	/* --- The key: a stable identity --- */

	if (NULL != previous)
	{
		g_autofree gchar *before = forms_string(previous, "key");

		if (0 != g_strcmp0(before, key))
		{
			venture_set_error_validation(error, "Key",
				"cannot be changed once saved; stored answers are filed under "
				"\"%s\". Add a new question instead", before);
			return FALSE;
		}
	}
	else
	{
		const gchar *cursor;
		gboolean ok = !venture_string_is_empty(key) && strlen(key) <= VENTURE_FORMS_KEY_MAX &&
		              g_ascii_islower(key[0]);

		for (cursor = key; ok && *cursor != '\0'; cursor++)
			ok = g_ascii_islower(*cursor) || g_ascii_isdigit(*cursor) || '_' == *cursor;
		if (!ok)
		{
			venture_set_error_validation(error, "Key",
				"must start with a lowercase letter and use only lowercase letters, "
				"digits and _ (at most %d)", VENTURE_FORMS_KEY_MAX);
			return FALSE;
		}
		{
			g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_FIELD);
			g_autoptr(GPtrArray) rows = NULL;

			/* Deleted questions too: the index does, and so must this. */
			venture_query_set_include_deleted(query, TRUE);
			venture_query_set_organization(query, venture_entity_get_organization_id(entity));
			venture_query_set_limit(query, 1);
			venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, form_id, NULL);
			venture_query_add_filter_string(query, "key", VENTURE_FILTER_OP_EQ, key, NULL);
			rows = venture_database_find(database, query, error);
			if (NULL == rows)
				return FALSE;
			if (rows->len > 0)
			{
				venture_set_error_validation(error, "Key",
					"\"%s\" is already a question on this form, or was one: a removed "
					"question's answers keep its key, so choose another", key);
				return FALSE;
			}
		}
	}

	/* --- Settings that have to mean something --- */

	if (max != 0 && max < min)
	{
		venture_set_error_validation(error, "Maximum",
			"%g is below the minimum %g", max, min);
		return FALSE;
	}
	if (min_length < 0 || max_length < 0 || (max_length != 0 && max_length < min_length))
	{
		venture_set_error_validation(error, "Maximum length",
			"must be at least the minimum length, and neither may be negative");
		return FALSE;
	}
	if (VENTURE_FORM_FIELD_RATING == kind)
	{
		gint64 low, high;

		forms_rating_bounds(entity, &low, &high);
		if (high <= low || high - low > 100)
		{
			venture_set_error_validation(error, "Maximum",
				"a rating needs a scale of whole numbers, at most 100 steps; "
				"leave both at 0 for 1 to 5");
			return FALSE;
		}
	}
	if (!venture_string_is_empty(pattern))
	{
		g_autofree gchar *anchored = g_strdup_printf("\\A(?:%s)\\z", pattern);
		g_autoptr(GRegex) regex = NULL;
		g_autoptr(GError) regex_error = NULL;

		regex = g_regex_new(anchored, G_REGEX_OPTIMIZE, 0, &regex_error);
		if (NULL == regex)
		{
			venture_set_error_validation(error, "Pattern",
				"is not a regular expression: %s", regex_error->message);
			return FALSE;
		}
	}
	if (!venture_string_is_empty(maps) && !g_strv_contains(forms_lead_targets, maps))
	{
		venture_set_error_validation(error, "Maps to",
			"must be one of name, email, phone, company_name, website or notes");
		return FALSE;
	}

	/* --- Choices: ids written once, kept through relabelling --- */

	if (forms_kind_has_choices(kind) || !venture_string_is_empty(choices_text))
	{
		g_autoptr(GPtrArray) choices = forms_choices_parse(choices_text, TRUE, error);
		g_autoptr(GString) normal = NULL;
		guint i;

		if (NULL == choices)
			return FALSE;
		if (forms_kind_has_choices(kind) && 0 == choices->len)
		{
			venture_set_error_validation(error, "Choices",
				"a choice question needs at least one choice, one per line");
			return FALSE;
		}
		normal = g_string_new(NULL);
		for (i = 0; i < choices->len; i++)
		{
			FormsChoice *choice = g_ptr_array_index(choices, i);

			if (i > 0)
				g_string_append_c(normal, '\n');
			g_string_append_printf(normal, "%s | %s", choice->id, choice->label);
		}
		if (0 != g_strcmp0(normal->str, choices_text))
			g_object_set(entity, "choices", normal->str, NULL);
	}

	return TRUE;
}

static gboolean
forms_validate_submission(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	(void)database;
	(void)user_data;

	if (NULL == previous)
	{
		if (NULL == g_object_get_data(G_OBJECT(entity), VENTURE_FORMS_ACCEPTING_KEY))
		{
			venture_set_error_validation(error, "Form",
				"responses are created by the form itself; post to its public address");
			return FALSE;
		}
		return TRUE;
	}

	/* What was sent is evidence. The team may mark it reviewed and add
	 * notes; they may not rewrite it. */
	{
		g_autofree gchar *before = forms_string(previous, "answers");
		g_autofree gchar *after = forms_string(entity, "answers");
		g_autofree gchar *summary_before = forms_string(previous, "summary");
		g_autofree gchar *summary_after = forms_string(entity, "summary");
		g_autofree gchar *origin_before = forms_string(previous, "origin");
		g_autofree gchar *origin_after = forms_string(entity, "origin");
		g_autoptr(GDateTime) at_before = NULL;
		g_autoptr(GDateTime) at_after = NULL;

		g_object_get(previous, "submitted-at", &at_before, NULL);
		g_object_get(entity, "submitted-at", &at_after, NULL);
		if (0 != g_strcmp0(before, after) ||
		    forms_int(previous, "form-id") != forms_int(entity, "form-id") ||
		    0 != g_strcmp0(summary_before, summary_after) ||
		    0 != g_strcmp0(origin_before, origin_after) ||
		    ((NULL == at_before) != (NULL == at_after)) ||
		    (NULL != at_before && !g_date_time_equal(at_before, at_after)))
		{
			venture_set_error_validation(error, "Answers",
				"cannot be changed; a response is what was sent. Add notes instead");
			return FALSE;
		}
	}
	return TRUE;
}

void
venture_forms_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The tests build several contexts over one database; validators are
	 * per database, so the second one must add nothing. */
	if (NULL != g_object_get_data(G_OBJECT(database), VENTURE_FORMS_STATE_KEY))
		return;
	g_object_set_data(G_OBJECT(database), VENTURE_FORMS_STATE_KEY, GINT_TO_POINTER(1));

	venture_database_add_save_validator(database, VENTURE_TYPE_FORM,
	                                    forms_validate_form, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_FORM_FIELD,
	                                    forms_validate_field, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_FORM_SUBMISSION,
	                                    forms_validate_submission, NULL, NULL);
}

/* ==========================================================================
 * Finding forms and their questions
 * ========================================================================== */

static gint
forms_field_order(gconstpointer a, gconstpointer b)
{
	VentureEntity *left = *(VentureEntity *const *)a;
	VentureEntity *right = *(VentureEntity *const *)b;
	gint64 lp = forms_int(left, "position"), rp = forms_int(right, "position");

	if (lp != rp)
		return lp < rp ? -1 : 1;
	return venture_entity_get_id(left) < venture_entity_get_id(right) ? -1 :
	       venture_entity_get_id(left) > venture_entity_get_id(right) ? 1 : 0;
}

GPtrArray *
venture_forms_fields(VentureDatabase *database, VentureEntity *form, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	GPtrArray *rows;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_FORM(form), NULL);

	query = venture_query_new(VENTURE_TYPE_FORM_FIELD);
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_set_limit(query, 500);
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ,
	                             venture_entity_get_id(form), NULL);
	rows = venture_database_find(database, query, error);
	if (NULL != rows)
		g_ptr_array_sort(rows, forms_field_order);
	return rows;
}

static gint64
forms_response_count(VentureDatabase *database, VentureEntity *form)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);

	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ,
	                             venture_entity_get_id(form), NULL);
	return venture_database_count(database, query, NULL);
}

/* Whether @form takes responses at @now, judged the same way by the door
 * and, again, under the lock by the save. */
static gboolean
forms_is_open(VentureDatabase *database, VentureEntity *form, GDateTime *now)
{
	g_autoptr(GDateTime) closes = NULL;
	VentureFormState state = VENTURE_FORM_DRAFT;
	gint64 limit = forms_int(form, "response-limit");

	if (venture_entity_is_deleted(form))
		return FALSE;
	g_object_get(form, "state", &state, "closes-at", &closes, NULL);
	if (VENTURE_FORM_LIVE != state)
		return FALSE;
	if (NULL != closes && g_date_time_compare(now, closes) >= 0)
		return FALSE;
	if (limit > 0 && forms_response_count(database, form) >= limit)
		return FALSE;
	return TRUE;
}

VentureEntity *
venture_forms_find_live(VentureDatabase *database, const gchar *token, GDateTime *now, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	VentureEntity *form;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	if (G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(), "form") ||
	    venture_string_is_empty(token) || strlen(token) > 128)
		goto missing;

	query = venture_query_new(VENTURE_TYPE_FORM);
	venture_query_set_limit(query, 2);
	venture_query_add_filter_string(query, "public-token", VENTURE_FILTER_OP_EQ, token, NULL);
	rows = venture_database_find(database, query, error);
	if (NULL == rows)
		return NULL;
	if (1 != rows->len)
		goto missing;
	form = g_ptr_array_index(rows, 0);
	if (!forms_is_open(database, form, now))
		goto missing;
	return g_object_ref(form);

missing:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
	return NULL;
}

gboolean
venture_forms_origin_allowed(VentureEntity *form, const gchar *origin)
{
	g_auto(GStrv) origins = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_FORM(form), FALSE);

	if (venture_string_is_empty(origin))
		return TRUE;
	origins = forms_origins(form);
	if (NULL == origins[0])
		return TRUE;
	for (i = 0; NULL != origins[i]; i++)
		if (0 == g_ascii_strcasecmp(origins[i], origin))
			return TRUE;
	return FALSE;
}

/* ==========================================================================
 * Tickets
 *
 * "<issued unix seconds>.<hex HMAC-SHA256 of token:seconds>", keyed by the
 * form's private ticket key. A robot can post one back, but not one older
 * than the form it fetched, so the minimum fill time is a real delay for
 * anything that fetches the form fresh. A snippet pasted into a static
 * page carries the ticket it was generated with, which is always old; the
 * honeypot is what guards that way in, and the docs say so.
 * ========================================================================== */

static gchar *
forms_ticket_mac(VentureEntity *form, gint64 issued)
{
	g_autofree gchar *key = forms_string(form, "ticket-key");
	g_autofree gchar *token = forms_string(form, "public-token");
	g_autofree gchar *payload = g_strdup_printf("%s:%" G_GINT64_FORMAT, token != NULL ? token : "", issued);

	if (venture_string_is_empty(key))
		return NULL;
	return g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)key, strlen(key), payload, -1);
}

gchar *
venture_forms_ticket_new(VentureEntity *form, GDateTime *issued)
{
	g_autofree gchar *mac = NULL;
	gint64 seconds;

	g_return_val_if_fail(VENTURE_IS_FORM(form), NULL);
	g_return_val_if_fail(issued != NULL, NULL);

	seconds = g_date_time_to_unix(issued);
	mac = forms_ticket_mac(form, seconds);
	return g_strdup_printf("%" G_GINT64_FORMAT ".%s", seconds, mac != NULL ? mac : "");
}

static const gchar *
forms_first(GHashTable *answers, const gchar *name)
{
	GPtrArray *values = g_hash_table_lookup(answers, name);

	return (NULL != values && values->len > 0) ? g_ptr_array_index(values, 0) : NULL;
}

gboolean
venture_forms_screen(VentureEntity *form, GHashTable *answers, GDateTime *now)
{
	const gchar *honeypot, *ticket, *dot;
	g_autofree gchar *expected = NULL;
	gint64 issued, fill, age;

	g_return_val_if_fail(VENTURE_IS_FORM(form), FALSE);
	g_return_val_if_fail(answers != NULL, FALSE);

	honeypot = forms_first(answers, VENTURE_FORMS_HONEYPOT);
	if (!venture_string_is_empty(honeypot))
		return FALSE;

	ticket = forms_first(answers, VENTURE_FORMS_TICKET);
	if (venture_string_is_empty(ticket) || strlen(ticket) > 100)
		return FALSE;
	dot = strchr(ticket, '.');
	if (NULL == dot)
		return FALSE;
	{
		g_autofree gchar *number = g_strndup(ticket, (gsize)(dot - ticket));

		if (!g_ascii_string_to_signed(number, 10, 0, G_MAXINT64, &issued, NULL))
			return FALSE;
	}
	expected = forms_ticket_mac(form, issued);
	if (NULL == expected || !venture_constant_time_equal(expected, dot + 1))
		return FALSE;

	fill = forms_int(form, "min-fill-seconds");
	if (fill <= 0)
		fill = VENTURE_FORMS_DEFAULT_FILL_SECONDS;
	age = g_date_time_to_unix(now) - issued;
	return age >= fill;
}

/* ==========================================================================
 * Answers as posted
 * ========================================================================== */

GHashTable *
venture_forms_answers_new(void)
{
	return g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                             (GDestroyNotify)g_ptr_array_unref);
}

void
venture_forms_answers_add(GHashTable *answers, const gchar *name, const gchar *value)
{
	GPtrArray *values;

	g_return_if_fail(answers != NULL);
	g_return_if_fail(name != NULL);

	values = g_hash_table_lookup(answers, name);
	if (NULL == values)
	{
		values = g_ptr_array_new_with_free_func(g_free);
		g_hash_table_insert(answers, g_strdup(name), values);
	}
	g_ptr_array_add(values, g_strdup(value != NULL ? value : ""));
}

/* One urlencoded component: '+' is a space, then %-decoding. */
static gchar *
forms_unescape(const gchar *start, gsize length)
{
	g_autofree gchar *plus = g_strndup(start, length);
	gchar *decoded;

	g_strdelimit(plus, "+", ' ');
	/* An escaped NUL makes this NULL, which is the refusal we want. */
	decoded = g_uri_unescape_string(plus, NULL);
	if (NULL == decoded)
		return NULL;
	if (!g_utf8_validate(decoded, -1, NULL))
	{
		g_free(decoded);
		return NULL;
	}
	return decoded;
}

GHashTable *
venture_forms_answers_from_urlencoded(const gchar *body, gsize length, GError **error)
{
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	const gchar *cursor = body, *end = body + length;
	guint pairs = 0;

	if (NULL != memchr(body, '\0', length))
		goto malformed;

	while (cursor < end)
	{
		const gchar *amp = memchr(cursor, '&', (gsize)(end - cursor));
		const gchar *stop = (NULL != amp) ? amp : end;
		const gchar *eq = memchr(cursor, '=', (gsize)(stop - cursor));
		g_autofree gchar *name = NULL;
		g_autofree gchar *value = NULL;

		if (stop > cursor)
		{
			if (++pairs > 1000)
				goto malformed;
			name = forms_unescape(cursor, (gsize)((NULL != eq ? eq : stop) - cursor));
			value = (NULL != eq) ? forms_unescape(eq + 1, (gsize)(stop - eq - 1)) : g_strdup("");
			/* A %00 decodes to a string shorter than its escape said; the
			 * unescape refuses it, and so does this. */
			if (NULL == name || NULL == value)
				goto malformed;
			if ('\0' != name[0])
				venture_forms_answers_add(answers, name, value);
		}
		cursor = stop + 1;
	}
	return g_steal_pointer(&answers);

malformed:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
	                    "The form data could not be read");
	return NULL;
}

GHashTable *
venture_forms_answers_from_json(JsonObject *object, GError **error)
{
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	JsonObjectIter iter;
	const gchar *name;
	JsonNode *node;

	g_return_val_if_fail(object != NULL, NULL);

	if (json_object_get_size(object) > 500)
		goto malformed;
	json_object_iter_init(&iter, object);
	while (json_object_iter_next(&iter, &name, &node))
	{
		if (JSON_NODE_HOLDS_NULL(node))
			continue;
		if (JSON_NODE_HOLDS_ARRAY(node))
		{
			JsonArray *array = json_node_get_array(node);
			guint i;

			if (json_array_get_length(array) > 500)
				goto malformed;
			for (i = 0; i < json_array_get_length(array); i++)
			{
				JsonNode *item = json_array_get_element(array, i);

				if (!JSON_NODE_HOLDS_VALUE(item) || G_TYPE_STRING != json_node_get_value_type(item))
					goto shape;
				venture_forms_answers_add(answers, name, json_node_get_string(item));
			}
			continue;
		}
		if (!JSON_NODE_HOLDS_VALUE(node))
			goto shape;
		switch (json_node_get_value_type(node))
		{
		case G_TYPE_STRING:
			venture_forms_answers_add(answers, name, json_node_get_string(node));
			break;
		case G_TYPE_BOOLEAN:
			venture_forms_answers_add(answers, name, json_node_get_boolean(node) ? "true" : "");
			break;
		case G_TYPE_INT64:
			{
				g_autofree gchar *text = g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));

				venture_forms_answers_add(answers, name, text);
			}
			break;
		case G_TYPE_DOUBLE:
			{
				gchar text[G_ASCII_DTOSTR_BUF_SIZE];

				g_ascii_dtostr(text, sizeof text, json_node_get_double(node));
				venture_forms_answers_add(answers, name, text);
			}
			break;
		default:
			goto shape;
		}
	}
	return g_steal_pointer(&answers);

shape:
	venture_set_error_validation(error, name,
		"must be a string, a number, true or false, or a list of strings");
	return NULL;
malformed:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
	                    "The answers could not be read");
	return NULL;
}

JsonObject *
venture_forms_answers_to_json(GHashTable *answers)
{
	JsonObject *object = json_object_new();
	GHashTableIter iter;
	gpointer key, value;

	g_return_val_if_fail(answers != NULL, object);

	g_hash_table_iter_init(&iter, answers);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		GPtrArray *values = value;
		guint i;

		if (g_str_has_prefix(key, "_vf_"))
			continue;
		if (1 == values->len)
		{
			json_object_set_string_member(object, key, g_ptr_array_index(values, 0));
			continue;
		}
		{
			JsonArray *array = json_array_new();

			for (i = 0; i < values->len; i++)
				json_array_add_string_element(array, g_ptr_array_index(values, i));
			json_object_set_array_member(object, key, array);
		}
	}
	return object;
}

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
forms_render_field(GString *html, const gchar *prefix, VentureEntity *field,
	const VentureFormsRender *options)
{
	g_autofree gchar *key = forms_string(field, "key");
	g_autofree gchar *label = forms_string(field, "label");
	g_autofree gchar *help = forms_string(field, "help");
	g_autofree gchar *placeholder = forms_string(field, "placeholder");
	g_autofree gchar *pattern = forms_string(field, "pattern");
	g_autofree gchar *fallback = forms_string(field, "default-value");
	g_autofree gchar *id = g_strdup_printf("%s-%s", prefix, key);
	g_autofree gchar *kind_class = NULL;
	VentureFormFieldKind kind = forms_kind(field);
	const gchar *nick = forms_kind_nick(kind);
	const gchar *message = forms_error(options, key);
	const gchar *value = forms_value(options, key);
	gboolean required = forms_bool(field, "required");
	gboolean has_help = !venture_string_is_empty(help);
	gint64 min_length = forms_int(field, "min-length");
	gint64 max_length = forms_int(field, "max-length");
	gdouble min = forms_double(field, "min-value");
	gdouble max = forms_double(field, "max-value");

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

	if (forms_kind_has_choices(kind) || VENTURE_FORM_FIELD_RATING == kind)
	{
		g_autoptr(GPtrArray) choices = forms_field_choices(field);
		const gchar *type = (VENTURE_FORM_FIELD_MULTIPLE_CHOICE == kind) ? "checkbox" : "radio";
		guint i;

		if (VENTURE_FORM_FIELD_RATING == kind)
		{
			gint64 low, high, step;

			forms_rating_bounds(field, &low, &high);
			g_ptr_array_set_size(choices, 0);
			for (step = low; step <= high && step - low <= 100; step++)
			{
				FormsChoice *choice = g_new0(FormsChoice, 1);

				choice->id = g_strdup_printf("%" G_GINT64_FORMAT, step);
				choice->label = g_strdup(choice->id);
				g_ptr_array_add(choices, choice);
			}
		}

		g_string_append_printf(html, "<fieldset class=\"vf-field vf-field--%s%s\" data-vf-field=\"%s\" "
		                       "data-vf-kind=\"%s\" id=\"%s\"", kind_class,
		                       NULL != message ? " vf-field--invalid" : "", key, nick, id);
		forms_described(html, id, has_help, NULL != message);
		g_string_append(html, "><legend class=\"vf-label\">");
		forms_render_label_text(html, label, required);
		g_string_append(html, "</legend><div class=\"vf-choices\">");
		for (i = 0; i < choices->len; i++)
		{
			FormsChoice *choice = g_ptr_array_index(choices, i);

			g_string_append_printf(html, "<label class=\"vf-choice\"><input class=\"vf-input\" "
			                       "type=\"%s\" name=\"%s\" value=\"", type, key);
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
		g_string_append_printf(html, "<textarea class=\"vf-input\" id=\"%s\" name=\"%s\" rows=\"5\"", id, key);
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

static void
forms_document_open(GString *html, VentureEntity *form, gboolean basic)
{
	g_autofree gchar *title = forms_string(form, "title");

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
	const gchar *form_message;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_FORM(form), NULL);
	g_return_val_if_fail(options != NULL, NULL);

	fields = venture_forms_fields(database, form, error);
	if (NULL == fields)
		return NULL;

	token = forms_string(form, "public-token");
	title = forms_string(form, "title");
	description = forms_string(form, "description");
	submit = forms_string(form, "submit-label");
	prefix = g_strdup_printf("vf-%s", token != NULL ? token : "form");
	action = (NULL != options->action) ? g_strdup(options->action) :
	         g_strdup_printf("/pub/form/%s", token != NULL ? token : "");
	hosted = (VENTURE_FORMS_RENDER_HOSTED == options->mode) ||
	         (VENTURE_FORMS_RENDER_HOSTED_BASIC == options->mode);
	preview = (VENTURE_FORMS_RENDER_PREVIEW == options->mode);
	form_message = forms_error(options, "_form");

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

	g_string_append_printf(html, "<div class=\"vf-errors\" id=\"%s-errors\" role=\"alert\"%s>",
	                       prefix, (NULL != options->errors && json_object_get_size(options->errors) > 0) ? "" : " hidden");
	if (NULL != form_message)
		forms_escape(html, form_message);
	else if (NULL != options->errors && json_object_get_size(options->errors) > 0)
		g_string_append(html, "Please correct the answers marked below.");
	g_string_append(html, "</div>");

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

	message = forms_string(form, "success-message");
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
	g_autofree gchar *token = NULL, *title = NULL, *description = NULL, *submit = NULL, *ticket = NULL;
	g_autofree gchar *success = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_FORM(form), NULL);

	fields = venture_forms_fields(database, form, error);
	if (NULL == fields)
		return NULL;

	token = forms_string(form, "public-token");
	title = forms_string(form, "title");
	description = forms_string(form, "description");
	submit = forms_string(form, "submit-label");
	success = forms_string(form, "success-message");
	ticket = venture_forms_ticket_new(form, now);

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "version");
	json_builder_add_int_value(builder, 1);
	json_builder_set_member_name(builder, "form");
	json_builder_add_string_value(builder, token != NULL ? token : "");
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
		VentureEntity *field = g_ptr_array_index(fields, i);
		g_autofree gchar *key = forms_string(field, "key");
		g_autofree gchar *label = forms_string(field, "label");
		g_autofree gchar *help = forms_string(field, "help");
		g_autofree gchar *placeholder = forms_string(field, "placeholder");
		g_autofree gchar *pattern = forms_string(field, "pattern");
		g_autofree gchar *fallback = forms_string(field, "default-value");
		VentureFormFieldKind kind = forms_kind(field);
		gdouble min = forms_double(field, "min-value"), max = forms_double(field, "max-value");

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "key");
		json_builder_add_string_value(builder, key);
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder, label != NULL ? label : "");
		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder, forms_kind_nick(kind));
		json_builder_set_member_name(builder, "required");
		json_builder_add_boolean_value(builder, forms_bool(field, "required"));
		json_builder_set_member_name(builder, "help");
		json_builder_add_string_value(builder, help != NULL ? help : "");
		json_builder_set_member_name(builder, "placeholder");
		json_builder_add_string_value(builder, placeholder != NULL ? placeholder : "");
		json_builder_set_member_name(builder, "default");
		json_builder_add_string_value(builder, fallback != NULL ? fallback : "");
		if (forms_kind_has_choices(kind))
		{
			g_autoptr(GPtrArray) choices = forms_field_choices(field);
			guint j;

			json_builder_set_member_name(builder, "choices");
			json_builder_begin_array(builder);
			for (j = 0; j < choices->len; j++)
			{
				FormsChoice *choice = g_ptr_array_index(choices, j);

				json_builder_begin_object(builder);
				json_builder_set_member_name(builder, "id");
				json_builder_add_string_value(builder, choice->id);
				json_builder_set_member_name(builder, "label");
				json_builder_add_string_value(builder, choice->label);
				json_builder_end_object(builder);
			}
			json_builder_end_array(builder);
		}
		if (VENTURE_FORM_FIELD_RATING == kind)
		{
			gint64 low, high;

			forms_rating_bounds(field, &low, &high);
			json_builder_set_member_name(builder, "min");
			json_builder_add_int_value(builder, low);
			json_builder_set_member_name(builder, "max");
			json_builder_add_int_value(builder, high);
		}
		else if (VENTURE_FORM_FIELD_NUMBER == kind && (min != 0 || max != 0))
		{
			json_builder_set_member_name(builder, "min");
			json_builder_add_double_value(builder, min);
			if (max != 0)
			{
				json_builder_set_member_name(builder, "max");
				json_builder_add_double_value(builder, max);
			}
		}
		if (forms_int(field, "min-length") > 0)
		{
			json_builder_set_member_name(builder, "min_length");
			json_builder_add_int_value(builder, forms_int(field, "min-length"));
		}
		if (forms_int(field, "max-length") > 0)
		{
			json_builder_set_member_name(builder, "max_length");
			json_builder_add_int_value(builder, forms_int(field, "max-length"));
		}
		if (!venture_string_is_empty(pattern))
		{
			json_builder_set_member_name(builder, "pattern");
			json_builder_add_string_value(builder, pattern);
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
	token = forms_string(form, "public-token");
	title = forms_string(form, "title");
	public_path = g_strdup_printf("%s/pub/form/%s", base, token != NULL ? token : "");
	code = g_string_new(NULL);

	switch (kind)
	{
	case VENTURE_FORMS_EMBED_HTML:
		{
			VentureFormsRender options;
			g_autoptr(GDateTime) now = venture_time_now();
			g_autofree gchar *ticket = venture_forms_ticket_new(form, now);
			g_autofree gchar *html = NULL;

			options.mode = VENTURE_FORMS_RENDER_SNIPPET;
			options.action = public_path;
			options.ticket = ticket;
			options.values = NULL;
			options.errors = NULL;
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
 * Validation
 * ========================================================================== */

static gboolean
forms_single_line(const gchar *text)
{
	return NULL == strpbrk(text, "\r\n");
}

static gboolean
forms_email_valid(const gchar *text)
{
	const gchar *at = strchr(text, '@');
	const gchar *cursor;

	if (NULL == at || at == text || strchr(at + 1, '@') != NULL || strlen(text) > 254)
		return FALSE;
	if ((gsize)(at - text) > 64 || NULL == strchr(at + 1, '.'))
		return FALSE;
	if (g_str_has_suffix(text, ".") || at[1] == '.')
		return FALSE;
	for (cursor = text; *cursor != '\0'; cursor++)
		if (g_ascii_isspace(*cursor) || g_ascii_iscntrl(*cursor) || '<' == *cursor || '>' == *cursor)
			return FALSE;
	return TRUE;
}

static gboolean
forms_phone_valid(const gchar *text)
{
	const gchar *cursor;
	guint digits = 0;

	if (strlen(text) > 40)
		return FALSE;
	for (cursor = text; *cursor != '\0'; cursor++)
	{
		if (g_ascii_isdigit(*cursor))
			digits++;
		else if (NULL == strchr("+()-. ", *cursor))
			return FALSE;
	}
	return digits >= 4 && digits <= 20;
}

static gboolean
forms_url_valid(const gchar *text)
{
	return strlen(text) <= 2048 && forms_redirect_valid(text);
}

static gboolean
forms_date_valid(const gchar *text)
{
	gint year, month, day;
	guint i;

	if (10 != strlen(text) || '-' != text[4] || '-' != text[7])
		return FALSE;
	for (i = 0; i < 10; i++)
		if (4 != i && 7 != i && !g_ascii_isdigit(text[i]))
			return FALSE;
	year = (gint)g_ascii_strtoll(text, NULL, 10);
	month = (gint)g_ascii_strtoll(text + 5, NULL, 10);
	day = (gint)g_ascii_strtoll(text + 8, NULL, 10);
	return year >= 1 && g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year);
}

static gboolean
forms_number_parse(const gchar *text, gdouble *number)
{
	gchar *end = NULL;

	if (strlen(text) > 64)
		return FALSE;
	*number = g_ascii_strtod(text, &end);
	if (end == text || '\0' != *end)
		return FALSE;
	/* strtod reads "nan" and "inf" happily; an answer is a real number. */
	return isfinite(*number);
}

static gboolean
forms_pattern_matches(const gchar *pattern, const gchar *text)
{
	g_autofree gchar *anchored = g_strdup_printf("\\A(?:%s)\\z", pattern);
	g_autoptr(GRegex) regex = g_regex_new(anchored, 0, 0, NULL);

	/* A pattern that no longer compiles was valid when saved; refusing
	 * every answer is safer than accepting any. */
	return NULL != regex && g_regex_match(regex, text, 0, NULL);
}

/* The default ceiling on text by kind, so a question with no maximum set
 * still cannot be used to store a novel. */
static gint64
forms_default_max_length(VentureFormFieldKind kind)
{
	switch (kind)
	{
	case VENTURE_FORM_FIELD_LONG_TEXT: return 10000;
	case VENTURE_FORM_FIELD_EMAIL: return 254;
	case VENTURE_FORM_FIELD_URL: return 2048;
	case VENTURE_FORM_FIELD_PHONE: return 40;
	case VENTURE_FORM_FIELD_SHORT_TEXT:
	case VENTURE_FORM_FIELD_NUMBER:
	case VENTURE_FORM_FIELD_DATE:
	case VENTURE_FORM_FIELD_SINGLE_CHOICE:
	case VENTURE_FORM_FIELD_MULTIPLE_CHOICE:
	case VENTURE_FORM_FIELD_CHECKBOX:
	case VENTURE_FORM_FIELD_RATING:
	case VENTURE_FORM_FIELD_HIDDEN:
	default:
		return 1000;
	}
}

static void
forms_refuse(JsonObject *errors, const gchar *key, const gchar *message)
{
	if (!json_object_has_member(errors, key))
		json_object_set_string_member(errors, key, message);
}

/*
 * Checks one question's values and, when they are good, writes the
 * stored answer into @answers and a line into @summary. The messages are
 * for the person filling the form in, so they say what to do.
 */
static void
forms_check_field(VentureEntity *field, GPtrArray *values, JsonObject *answers,
	JsonObject *errors, GString *summary)
{
	g_autofree gchar *key = forms_string(field, "key");
	g_autofree gchar *label = forms_string(field, "label");
	g_autofree gchar *pattern = forms_string(field, "pattern");
	g_autofree gchar *fallback = forms_string(field, "default-value");
	g_autofree gchar *text = NULL;
	VentureFormFieldKind kind = forms_kind(field);
	gboolean required = forms_bool(field, "required");
	gint64 min_length = forms_int(field, "min-length");
	gint64 max_length = forms_int(field, "max-length");
	gdouble min = forms_double(field, "min-value");
	gdouble max = forms_double(field, "max-value");
	glong length;
	guint i;

	if (max_length <= 0)
		max_length = forms_default_max_length(kind);

	if (VENTURE_FORM_FIELD_MULTIPLE_CHOICE == kind)
	{
		g_autoptr(GPtrArray) choices = forms_field_choices(field);
		JsonArray *picked = json_array_new();
		g_autoptr(GString) labels = g_string_new(NULL);

		for (i = 0; NULL != values && i < values->len; i++)
		{
			const gchar *id = g_ptr_array_index(values, i);
			const gchar *choice = forms_choice_label(choices, id);
			guint j;
			gboolean seen = FALSE;

			if ('\0' == id[0])
				continue;
			if (NULL == choice)
			{
				forms_refuse(errors, key, "Choose from the options given.");
				json_array_unref(picked);
				return;
			}
			for (j = 0; j < json_array_get_length(picked); j++)
				seen = seen || 0 == g_strcmp0(json_array_get_string_element(picked, j), id);
			if (seen)
				continue;
			json_array_add_string_element(picked, id);
			if (labels->len > 0)
				g_string_append(labels, ", ");
			g_string_append(labels, choice);
		}
		if (0 == json_array_get_length(picked))
		{
			json_array_unref(picked);
			if (required)
				forms_refuse(errors, key, "Choose at least one.");
			return;
		}
		json_object_set_array_member(answers, key, picked);
		g_string_append_printf(summary, "%s: %s\n", label, labels->str);
		return;
	}

	if (NULL != values && values->len > 1)
	{
		forms_refuse(errors, key, "Give one answer only.");
		return;
	}

	if (VENTURE_FORM_FIELD_CHECKBOX == kind)
	{
		const gchar *value = (NULL != values) ? g_ptr_array_index(values, 0) : "";
		gboolean ticked;

		if (g_strv_contains((const gchar *const[]) { "on", "true", "1", "yes", NULL }, value))
			ticked = TRUE;
		else if (g_strv_contains((const gchar *const[]) { "", "off", "false", "0", "no", NULL }, value))
			ticked = FALSE;
		else
		{
			forms_refuse(errors, key, "Tick the box or leave it empty.");
			return;
		}
		if (required && !ticked)
		{
			forms_refuse(errors, key, "This box must be ticked.");
			return;
		}
		json_object_set_boolean_member(answers, key, ticked);
		g_string_append_printf(summary, "%s: %s\n", label, ticked ? "Yes" : "No");
		return;
	}

	if (NULL != values)
		text = g_strstrip(g_strdup(g_ptr_array_index(values, 0)));
	if (VENTURE_FORM_FIELD_HIDDEN == kind && venture_string_is_empty(text) && !venture_string_is_empty(fallback))
	{
		g_free(text);
		text = g_strdup(fallback);
	}

	if (venture_string_is_empty(text))
	{
		if (required)
			forms_refuse(errors, key, "This question needs an answer.");
		return;
	}

	length = g_utf8_strlen(text, -1);
	if (VENTURE_FORM_FIELD_LONG_TEXT != kind && !forms_single_line(text))
	{
		forms_refuse(errors, key, "Keep this answer to one line.");
		return;
	}
	if (length > max_length)
	{
		g_autofree gchar *message = g_strdup_printf("Use at most %" G_GINT64_FORMAT " characters.", max_length);

		forms_refuse(errors, key, message);
		return;
	}
	if (min_length > 0 && length < min_length)
	{
		g_autofree gchar *message = g_strdup_printf("Use at least %" G_GINT64_FORMAT " characters.", min_length);

		forms_refuse(errors, key, message);
		return;
	}

	switch (kind)
	{
	case VENTURE_FORM_FIELD_EMAIL:
		if (!forms_email_valid(text))
		{
			forms_refuse(errors, key, "Enter an email address, like name@example.com.");
			return;
		}
		break;
	case VENTURE_FORM_FIELD_PHONE:
		if (!forms_phone_valid(text))
		{
			forms_refuse(errors, key, "Enter a phone number using digits, spaces, + - ( ) and dots.");
			return;
		}
		break;
	case VENTURE_FORM_FIELD_URL:
		if (!forms_url_valid(text))
		{
			forms_refuse(errors, key, "Enter a web address starting with https://.");
			return;
		}
		break;
	case VENTURE_FORM_FIELD_DATE:
		if (!forms_date_valid(text))
		{
			forms_refuse(errors, key, "Enter a real date as YYYY-MM-DD.");
			return;
		}
		break;
	case VENTURE_FORM_FIELD_NUMBER:
		{
			gdouble number;
			gchar shown[G_ASCII_DTOSTR_BUF_SIZE];

			if (!forms_number_parse(text, &number))
			{
				forms_refuse(errors, key, "Enter a number.");
				return;
			}
			if ((min != 0 || max != 0) && number < min)
			{
				g_autofree gchar *message = g_strdup_printf("Enter %g or more.", min);

				forms_refuse(errors, key, message);
				return;
			}
			if (max != 0 && number > max)
			{
				g_autofree gchar *message = g_strdup_printf("Enter %g or less.", max);

				forms_refuse(errors, key, message);
				return;
			}
			json_object_set_double_member(answers, key, number);
			g_string_append_printf(summary, "%s: %s\n", label, g_ascii_dtostr(shown, sizeof shown, number));
			return;
		}
	case VENTURE_FORM_FIELD_RATING:
		{
			gint64 low, high, rating;

			forms_rating_bounds(field, &low, &high);
			if (!g_ascii_string_to_signed(text, 10, low, high, &rating, NULL))
			{
				g_autofree gchar *message = g_strdup_printf("Choose a whole number from %" G_GINT64_FORMAT
					" to %" G_GINT64_FORMAT ".", low, high);

				forms_refuse(errors, key, message);
				return;
			}
			json_object_set_int_member(answers, key, rating);
			g_string_append_printf(summary, "%s: %" G_GINT64_FORMAT "\n", label, rating);
			return;
		}
	case VENTURE_FORM_FIELD_SINGLE_CHOICE:
		{
			g_autoptr(GPtrArray) choices = forms_field_choices(field);
			const gchar *choice = forms_choice_label(choices, text);

			if (NULL == choice)
			{
				forms_refuse(errors, key, "Choose from the options given.");
				return;
			}
			json_object_set_string_member(answers, key, text);
			g_string_append_printf(summary, "%s: %s\n", label, choice);
			return;
		}
	case VENTURE_FORM_FIELD_SHORT_TEXT:
	case VENTURE_FORM_FIELD_LONG_TEXT:
	case VENTURE_FORM_FIELD_MULTIPLE_CHOICE:
	case VENTURE_FORM_FIELD_CHECKBOX:
	case VENTURE_FORM_FIELD_HIDDEN:
	default:
		break;
	}

	if (!venture_string_is_empty(pattern) && !forms_pattern_matches(pattern, text))
	{
		forms_refuse(errors, key, "This answer is not in the expected format.");
		return;
	}
	json_object_set_string_member(answers, key, text);
	g_string_append_printf(summary, "%s: %s\n", label, text);
}

/* ==========================================================================
 * Saving a response
 * ========================================================================== */

/* Who a response is from: the answer mapped to a lead's name, else a
 * question keyed "name", else the first email, else the form. */
static gchar *
forms_response_name(VentureEntity *form, GPtrArray *fields, JsonObject *answers)
{
	g_autofree gchar *title = NULL;
	const gchar *email = NULL;
	guint pass, i;

	for (pass = 0; pass < 2; pass++)
	{
		for (i = 0; i < fields->len; i++)
		{
			VentureEntity *field = g_ptr_array_index(fields, i);
			g_autofree gchar *key = forms_string(field, "key");
			g_autofree gchar *maps = forms_string(field, "maps-to");
			JsonNode *node;

			if (!json_object_has_member(answers, key))
				continue;
			node = json_object_get_member(answers, key);
			if (!JSON_NODE_HOLDS_VALUE(node) || G_TYPE_STRING != json_node_get_value_type(node))
				continue;
			if ((0 == pass && 0 == g_strcmp0(maps, "name")) ||
			    (1 == pass && 0 == g_strcmp0(key, "name")))
				return venture_truncate(json_node_get_string(node), 200);
			if (NULL == email && VENTURE_FORM_FIELD_EMAIL == forms_kind(field))
				email = json_node_get_string(node);
		}
	}
	if (NULL != email)
		return g_strdup(email);
	title = forms_string(form, "title");
	if (venture_string_is_empty(title))
	{
		g_free(title);
		title = forms_string(form, "name");
	}
	return g_strdup_printf("Response to %s", title != NULL ? title : "a form");
}

/* The lead inputs the questions map to. Several questions mapped to notes
 * are kept, each under its question. */
static JsonObject *
forms_lead_values(GPtrArray *fields, JsonObject *answers)
{
	JsonObject *values = json_object_new();
	g_autoptr(GString) notes = g_string_new(NULL);
	guint mapped_notes = 0, i;

	for (i = 0; i < fields->len; i++)
	{
		g_autofree gchar *maps = forms_string(g_ptr_array_index(fields, i), "maps-to");

		if (0 == g_strcmp0(maps, "notes"))
			mapped_notes++;
	}
	for (i = 0; i < fields->len; i++)
	{
		VentureEntity *field = g_ptr_array_index(fields, i);
		g_autofree gchar *key = forms_string(field, "key");
		g_autofree gchar *maps = forms_string(field, "maps-to");
		g_autofree gchar *label = forms_string(field, "label");
		JsonNode *node;
		g_autofree gchar *text = NULL;

		if (venture_string_is_empty(maps) || !json_object_has_member(answers, key))
			continue;
		node = json_object_get_member(answers, key);
		if (JSON_NODE_HOLDS_VALUE(node) && G_TYPE_STRING == json_node_get_value_type(node))
			text = g_strdup(json_node_get_string(node));
		else
			text = json_to_string(node, FALSE);
		if (0 == g_strcmp0(maps, "notes"))
		{
			if (notes->len > 0)
				g_string_append(notes, "\n");
			if (mapped_notes > 1)
				g_string_append_printf(notes, "%s: %s", label, text);
			else
				g_string_append(notes, text);
		}
		else
			json_object_set_string_member(values, maps, text);
	}
	if (notes->len > 0)
		json_object_set_string_member(values, "notes", notes->str);
	return values;
}

/* Queues the thank-you email. Mail may be off; a form must still work. */
static gboolean
forms_queue_confirmation(VentureDatabase *database, VentureEntity *form, VentureEntity *submission,
	JsonObject *answers, gchar **note, GError **error)
{
	g_autofree gchar *field = forms_string(form, "confirmation-field");
	g_autofree gchar *subject = forms_string(form, "confirmation-subject");
	g_autofree gchar *body = forms_string(form, "confirmation-message");
	g_autofree gchar *success = forms_string(form, "success-message");
	g_autofree gchar *key = NULL;
	g_autoptr(VentureMailMessage) message = NULL;
	g_autoptr(VentureMailMessage) queued = NULL;
	VentureMailOutbox *outbox;
	const gchar *to;

	if (venture_string_is_empty(field) || !json_object_has_member(answers, field))
		return TRUE;
	to = json_object_get_string_member_with_default(answers, field, "");
	if (!forms_email_valid(to))
		return TRUE;
	if (G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message") ||
	    NULL == (outbox = venture_database_get_mail_outbox(database)))
	{
		*note = g_strdup("No confirmation was sent: the mail module is off.");
		return TRUE;
	}
	key = g_strdup_printf("form-response:%s", venture_entity_get_uuid(submission));
	message = venture_mail_message_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(message), venture_entity_get_organization_id(form));
	g_object_set(message, "to", to,
		"subject", venture_string_is_empty(subject) ? "We received your response" : subject,
		"text-body", !venture_string_is_empty(body) ? body :
			(!venture_string_is_empty(success) ? success : "Thank you. Your response has been received."),
		"idempotency-key", key, "related-type", "form", "related-id", venture_entity_get_id(form), NULL);
	queued = venture_mail_outbox_enqueue(outbox, message, NULL, error);
	return NULL != queued;
}

/* One attempt at the whole write: the lead, the confirmation and the
 * response, in one transaction. */
static gboolean
forms_write(VentureDatabase *database, VentureEntity *form, VentureEntity *submission,
	GPtrArray *fields, JsonObject *answers, gboolean follow_up, GDateTime *now, GError **error)
{
	g_autofree gchar *note = NULL;

	if (!venture_database_begin(database, error))
		return FALSE;

	/* Under the lock, the cap and the state are what they are: two
	 * people posting the last place at once get one response between
	 * them, not two. */
	if (!forms_is_open(database, form, now))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
		goto fail;
	}

	if (follow_up && forms_bool(form, "create-lead"))
	{
		g_autoptr(JsonObject) values = forms_lead_values(fields, answers);
		g_autoptr(VentureEntity) lead = NULL;
		g_autofree gchar *source = forms_string(form, "lead-source");
		g_autofree gchar *policy = forms_string(form, "on-duplicate");

		if (venture_string_is_empty(source))
		{
			g_free(source);
			source = forms_string(form, "title");
		}
		if (venture_string_is_empty(source))
		{
			g_free(source);
			source = forms_string(form, "name");
		}
		lead = venture_lead_service_capture_values(venture_database_get_lead_service(database),
			venture_entity_get_organization_id(form), forms_int(form, "venture-id"), source,
			forms_int(form, "campaign-id"), values, policy, error);
		if (NULL == lead)
			goto fail;
		g_object_set(submission, "lead-id", venture_entity_get_id(lead), NULL);
	}
	if (follow_up && !forms_queue_confirmation(database, form, submission, answers, &note, error))
		goto fail;
	if (NULL != note)
		g_object_set(submission, "mapping-note", note, NULL);

	if (!venture_database_save(database, submission, NULL, error))
		goto fail;
	return venture_database_commit(database, error);

fail:
	venture_database_rollback(database);
	return FALSE;
}

/* A new, unsaved response. Built afresh for a retry, because a save that
 * was rolled back has already stamped the object it was given. */
static VentureEntity *
forms_new_response(VentureEntity *form, const gchar *name, GDateTime *now, const gchar *answers,
	const gchar *summary, const gchar *origin)
{
	VentureEntity *response = VENTURE_ENTITY(venture_form_submission_new());

	venture_entity_set_organization_id(response, venture_entity_get_organization_id(form));
	g_object_set(response, "name", name, "form-id", venture_entity_get_id(form),
		"submitted-at", now, "answers", answers, "summary", summary,
		"origin", venture_string_is_empty(origin) ? NULL : origin, NULL);
	g_object_set_data(G_OBJECT(response), VENTURE_FORMS_ACCEPTING_KEY, GINT_TO_POINTER(1));
	return response;
}

gboolean
venture_forms_submit(VentureDatabase *database, VentureEntity *form, GHashTable *answers,
	const gchar *origin, GDateTime *now, VentureFormsOutcome *outcome,
	VentureEntity **submission, JsonObject **errors, GError **error)
{
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(JsonObject) stored = json_object_new();
	g_autoptr(JsonObject) refused = json_object_new();
	g_autoptr(GString) summary = g_string_new(NULL);
	g_autoptr(VentureEntity) response = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *answers_text = NULL;
	g_autoptr(GError) follow_error = NULL;
	g_autofree gchar *name = NULL;
	GHashTableIter iter;
	gpointer key;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(VENTURE_IS_FORM(form), FALSE);
	g_return_val_if_fail(answers != NULL, FALSE);
	g_return_val_if_fail(outcome != NULL, FALSE);

	if (NULL != submission)
		*submission = NULL;
	if (NULL != errors)
		*errors = NULL;
	*outcome = VENTURE_FORMS_INVALID;

	fields = venture_forms_fields(database, form, error);
	if (NULL == fields)
		return FALSE;

	/* A name that is no question on this form is refused, not dropped:
	 * the sender believes it was received. The door's own names pass. */
	g_hash_table_iter_init(&iter, answers);
	while (g_hash_table_iter_next(&iter, &key, NULL))
	{
		gboolean known = (0 == g_strcmp0(key, VENTURE_FORMS_HONEYPOT)) ||
		                 (0 == g_strcmp0(key, VENTURE_FORMS_TICKET));

		for (i = 0; !known && i < fields->len; i++)
		{
			g_autofree gchar *field_key = forms_string(g_ptr_array_index(fields, i), "key");

			known = (0 == g_strcmp0(field_key, key));
		}
		if (!known)
			forms_refuse(refused, key, "This form has no such question.");
	}

	for (i = 0; i < fields->len; i++)
	{
		VentureEntity *field = g_ptr_array_index(fields, i);
		g_autofree gchar *field_key = forms_string(field, "key");

		forms_check_field(field, g_hash_table_lookup(answers, field_key), stored, refused, summary);
	}

	if (json_object_get_size(refused) > 0)
	{
		if (NULL != errors)
			*errors = g_steal_pointer(&refused);
		return TRUE;
	}

	name = forms_response_name(form, fields, stored);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(node, stored);
	/* JSON fields are stored as their text; written once, in one form. */
	answers_text = json_to_string(node, FALSE);
	if (summary->len > 0 && '\n' == summary->str[summary->len - 1])
		g_string_truncate(summary, summary->len - 1);

	response = forms_new_response(form, name, now, answers_text, summary->str, origin);

	if (!forms_write(database, form, response, fields, stored, TRUE, now, &follow_error))
	{
		g_autofree gchar *note = NULL;

		/* A form that is no longer open refuses outright. Anything else
		 * was the lead or the email: the response is what matters, so it
		 * is kept, and says what did not happen. */
		if (g_error_matches(follow_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND) &&
		    !forms_is_open(database, form, now))
		{
			g_propagate_error(error, g_steal_pointer(&follow_error));
			return FALSE;
		}
		note = g_strdup_printf("Follow-up not done: %s", follow_error->message);
		g_clear_error(&follow_error);
		g_clear_object(&response);
		response = forms_new_response(form, name, now, answers_text, summary->str, origin);
		g_object_set(response, "mapping-note", note, NULL);
		if (!forms_write(database, form, response, fields, stored, FALSE, now, error))
			return FALSE;
	}

	*outcome = VENTURE_FORMS_ACCEPTED;
	if (NULL != submission)
		*submission = g_steal_pointer(&response);
	return TRUE;
}
