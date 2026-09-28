/*
 * venture-forms.c - Forms: validators, versions and the public intake
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A form is the one place VENTURE takes writes from strangers with no
 * session at all. The server is the authority: every answer is checked
 * here against the question's kind whatever the browser did, and an
 * unknown name is refused, never dropped, because a dropped field is a
 * silent data loss the person who filled the form in never hears about.
 *
 * What a live form asks is its published version, frozen when it was
 * published; editing the questions changes the draft and nothing public
 * until the next publish. A response is checked against, and stored
 * with, the version its sender was given.
 */

#include "venture-forms-private.h"

#include <math.h>
#include <string.h>

#define VENTURE_FORMS_STATE_KEY "venture-forms-installed"
#define VENTURE_FORMS_DEFAULT_FILL_SECONDS 3
#define VENTURE_FORMS_ANONYMISING_KEY "venture-forms-anonymising"

static gboolean forms_check_published(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, GError **error);

/* A form field record's kind. */
static VentureFormFieldKind
forms_kind(VentureEntity *field)
{
	VentureFormFieldKind kind = VENTURE_FORM_FIELD_SHORT_TEXT;

	g_object_get(field, "kind", &kind, NULL);
	return kind;
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
	g_autofree gchar *text = venture_forms_get_string(form, "allowed-origins");
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
	g_autofree gchar *token = venture_forms_get_string(entity, "public-token");
	g_autofree gchar *key = venture_forms_get_string(entity, "ticket-key");
	g_autofree gchar *slug = venture_forms_get_string(entity, "slug");
	g_autofree gchar *redirect = venture_forms_get_string(entity, "redirect-url");
	g_autofree gchar *policy = venture_forms_get_string(entity, "on-duplicate");
	g_autofree gchar *confirm = venture_forms_get_string(entity, "confirmation-field");
	g_auto(GStrv) origins = NULL;
	guint i;

	(void)user_data;

	{
		g_autofree gchar *privacy = venture_forms_get_string(entity, "privacy-url");
		g_autofree gchar *retain = venture_forms_get_string(entity, "retention-action");

		if (!venture_string_is_empty(privacy) && !forms_redirect_valid(privacy))
		{
			venture_set_error_validation(error, "Privacy notice", "must be an http:// or https:// address");
			return FALSE;
		}
		if (!venture_string_is_empty(retain) && 0 != g_strcmp0(retain, "anonymise") && 0 != g_strcmp0(retain, "purge"))
		{
			venture_set_error_validation(error, "After that", "must be anonymise or purge");
			return FALSE;
		}
		if (venture_forms_get_int(entity, "retention-days") < 0)
		{
			venture_set_error_validation(error, "Keep responses for", "cannot be negative");
			return FALSE;
		}
	}
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

	if (venture_forms_get_int(entity, "response-limit") < 0)
	{
		venture_set_error_validation(error, "Response limit", "cannot be negative");
		return FALSE;
	}
	if (venture_forms_get_int(entity, "hourly-limit") < 0)
	{
		venture_set_error_validation(error, "Responses per hour", "cannot be negative");
		return FALSE;
	}
	if (venture_forms_get_int(entity, "min-fill-seconds") < 0 || venture_forms_get_int(entity, "min-fill-seconds") > 3600)
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
	if (!venture_string_is_empty(confirm) && !venture_forms_choice_id_valid(confirm))
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
			g_autofree gchar *name = venture_forms_get_string(entity, "name");

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

	return forms_check_published(database, entity, previous, error);
}

/* The autofill tokens a question may declare: the HTML standard's, less
 * the payment and one-time-code ones a form of ours should never ask a
 * browser to fill. */
static const gchar *const forms_autocomplete_tokens[] = {
	"off", "on", "name", "honorific-prefix", "given-name", "additional-name", "family-name",
	"honorific-suffix", "nickname", "username", "organization-title", "organization",
	"street-address", "address-line1", "address-line2", "address-line3", "address-level4",
	"address-level3", "address-level2", "address-level1", "country", "country-name",
	"postal-code", "bday", "bday-day", "bday-month", "bday-year", "sex", "url", "photo",
	"tel", "tel-country-code", "tel-national", "tel-area-code", "tel-local", "tel-extension",
	"email", "impp", "language", NULL
};

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
	g_autofree gchar *key = venture_forms_get_string(entity, "key");
	g_autofree gchar *pattern = venture_forms_get_string(entity, "pattern");
	g_autofree gchar *maps = venture_forms_get_string(entity, "maps-to");
	g_autofree gchar *autocomplete = venture_forms_get_string(entity, "autocomplete");
	g_autofree gchar *choices_text = venture_forms_get_string(entity, "choices");
	VentureFormFieldKind kind = forms_kind(entity);
	gint64 form_id = venture_forms_get_int(entity, "form-id");
	gdouble min = venture_forms_get_double(entity, "min-value");
	gdouble max = venture_forms_get_double(entity, "max-value");
	gint64 min_length = venture_forms_get_int(entity, "min-length");
	gint64 max_length = venture_forms_get_int(entity, "max-length");

	(void)user_data;

	/* --- The form it belongs to, which never changes --- */

	if (NULL != previous && venture_forms_get_int(previous, "form-id") != form_id)
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
		g_autofree gchar *before = venture_forms_get_string(previous, "key");

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
		gint64 low = 1, high = 5;

		if (0 != min || 0 != max)
		{
			low = (gint64)ceil(min);
			high = (gint64)floor(max);
		}
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
	if (!venture_string_is_empty(autocomplete) && !g_strv_contains(forms_autocomplete_tokens, autocomplete))
	{
		venture_set_error_validation(error, "Autofill",
			"\"%s\" is not a browser autofill token; use one like given-name, "
			"family-name, street-address or postal-code", autocomplete);
		return FALSE;
	}
	/* A sensitive answer copied into a lead would leave the one column
	 * that keeps it in. */
	if (venture_forms_get_bool(entity, "sensitive") && !venture_string_is_empty(maps))
	{
		venture_set_error_validation(error, "Maps to",
			"a sensitive question cannot fill a lead; its answer stays on the response");
		return FALSE;
	}
	if (!venture_string_is_empty(maps) && !g_strv_contains(forms_lead_targets, maps))
	{
		venture_set_error_validation(error, "Maps to",
			"must be one of name, email, phone, company_name, website or notes");
		return FALSE;
	}

	/* --- Choices: ids written once, kept through relabelling --- */

	if (venture_forms_kind_has_choices(kind) || !venture_string_is_empty(choices_text))
	{
		g_autoptr(GPtrArray) choices = venture_forms_choices_parse(choices_text, TRUE, error);
		g_autoptr(GString) normal = NULL;
		guint i;

		if (NULL == choices)
			return FALSE;
		if (venture_forms_kind_has_choices(kind) && 0 == choices->len)
		{
			venture_set_error_validation(error, "Choices",
				"a choice question needs at least one choice, one per line");
			return FALSE;
		}
		normal = g_string_new(NULL);
		for (i = 0; i < choices->len; i++)
		{
			VentureFormsChoice *choice = g_ptr_array_index(choices, i);

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

	/* Anonymising is the one rewrite: the retention sweep empties the
	 * answers and names nobody, and marks it done. */
	if (NULL != g_object_get_data(G_OBJECT(entity), VENTURE_FORMS_ANONYMISING_KEY))
		return TRUE;

	/* What was sent is evidence. The team may mark it reviewed and add
	 * notes; they may not rewrite it. */
	{
		g_autofree gchar *before = venture_forms_get_string(previous, "answers");
		g_autofree gchar *after = venture_forms_get_string(entity, "answers");
		g_autofree gchar *secret_before = venture_forms_get_string(previous, "sensitive-answers");
		g_autofree gchar *secret_after = venture_forms_get_string(entity, "sensitive-answers");
		g_autofree gchar *summary_before = venture_forms_get_string(previous, "summary");
		g_autofree gchar *summary_after = venture_forms_get_string(entity, "summary");
		g_autofree gchar *origin_before = venture_forms_get_string(previous, "origin");
		g_autofree gchar *origin_after = venture_forms_get_string(entity, "origin");
		g_autoptr(GDateTime) at_before = NULL;
		g_autoptr(GDateTime) at_after = NULL;

		g_object_get(previous, "submitted-at", &at_before, NULL);
		g_object_get(entity, "submitted-at", &at_after, NULL);
		if (0 != g_strcmp0(before, after) || 0 != g_strcmp0(secret_before, secret_after) ||
		    venture_forms_get_int(previous, "form-id") != venture_forms_get_int(entity, "form-id") ||
		    venture_forms_get_int(previous, "version-id") != venture_forms_get_int(entity, "version-id") ||
		    venture_forms_get_int(previous, "version-number") != venture_forms_get_int(entity, "version-number") ||
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

/* A version is what a form asked, frozen: only publishing makes one and
 * nothing changes one, because responses are read against it. */
static gboolean
forms_validate_version(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	(void)database;
	(void)user_data;

	if (NULL == previous && NULL == g_object_get_data(G_OBJECT(entity), VENTURE_FORMS_ACCEPTING_KEY))
	{
		venture_set_error_validation(error, "Form",
			"versions are made by publishing the form");
		return FALSE;
	}
	if (NULL != previous)
	{
		venture_set_error_validation(error, "Questions",
			"a published version cannot be changed; edit the questions and publish again");
		return FALSE;
	}
	return TRUE;
}

/* The published version a form points at must be one of its own; its
 * number is copied from it, so the two can never disagree. Pointing back
 * at an earlier version is how a publish is undone. */
static gboolean
forms_check_published(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous,
	GError **error)
{
	g_autoptr(VentureEntity) version = NULL;
	gint64 id = venture_forms_get_int(entity, "published-version-id");

	if (id <= 0)
	{
		g_object_set(entity, "published-number", (gint64)0, NULL);
		return TRUE;
	}
	if (NULL != previous && venture_forms_get_int(previous, "published-version-id") == id)
	{
		g_object_set(entity, "published-number", venture_forms_get_int(previous, "published-number"), NULL);
		return TRUE;
	}
	version = venture_database_get(database, VENTURE_TYPE_FORM_VERSION, id, NULL);
	if (NULL == version || venture_forms_get_int(version, "form-id") != venture_entity_get_id(entity))
	{
		venture_set_error_validation(error, "Published version",
			"#%" G_GINT64_FORMAT " is not a version of this form", id);
		return FALSE;
	}
	g_object_set(entity, "published-number", venture_forms_get_int(version, "number"), NULL);
	return TRUE;
}

/* ==========================================================================
 * The publish action: the API, the CLI, MCP and the assistant
 * ========================================================================== */

static gboolean
forms_publish_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)actor;

	if (G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(), "form"))
	{
		venture_set_error_validation(error, "module", "The forms module is off");
		return FALSE;
	}
	if (!VENTURE_IS_FORM(entity))
	{
		venture_set_error_validation(error, "form", "Only a form can be published");
		return FALSE;
	}
	return TRUE;
}

static gboolean
forms_publish_allowed_type(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)entity;
	(void)actor;
	if (G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(), "form"))
	{
		venture_set_error_validation(error, "module", "The forms module is off");
		return FALSE;
	}
	return TRUE;
}

static VentureEntity *
forms_publish_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	(void)params;

	return venture_forms_publish(venture_action_get_data(action), entity, actor, error);
}

/* A type-level action: the placeholder carries the organization the
 * access policy judged, placed there from organization_id. */
static VentureEntity *
forms_sweep_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *text = NULL;
	JsonNode *limit = g_hash_table_lookup(params, "limit");
	gint64 count = (NULL != limit && JSON_NODE_HOLDS_VALUE(limit)) ? json_node_get_int(limit) : 0;
	VentureEntity *answer;

	result = venture_forms_retention_sweep(venture_action_get_data(action),
		venture_entity_get_organization_id(entity), (guint)CLAMP(count, 0, 1000), now, actor, error);
	if (NULL == result)
		return NULL;
	text = json_to_string(result, FALSE);
	answer = VENTURE_ENTITY(venture_form_new());
	venture_entity_set_organization_id(answer, venture_entity_get_organization_id(entity));
	g_object_set(answer, "name", "Retention sweep", "result", text, NULL);
	return answer;
}

static VentureEntity *
forms_erase_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	JsonNode *email = g_hash_table_lookup(params, "email");
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *text = NULL;
	VentureEntity *answer;

	result = venture_forms_erase_person(venture_action_get_data(action), venture_entity_get_organization_id(entity),
		(NULL != email && JSON_NODE_HOLDS_VALUE(email)) ? json_node_get_string(email) : NULL, actor, error);
	if (NULL == result)
		return NULL;
	text = json_to_string(result, FALSE);
	answer = VENTURE_ENTITY(venture_form_new());
	venture_entity_set_organization_id(answer, venture_entity_get_organization_id(entity));
	g_object_set(answer, "name", "Erasure", "result", text, NULL);
	return answer;
}

static VentureEntity *
forms_export_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	JsonNode *email = g_hash_table_lookup(params, "email");
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *text = NULL;
	VentureEntity *answer;

	(void)actor;
	result = venture_forms_export_person(venture_action_get_data(action), venture_entity_get_organization_id(entity),
		(NULL != email && JSON_NODE_HOLDS_VALUE(email)) ? json_node_get_string(email) : NULL, error);
	if (NULL == result)
		return NULL;
	text = json_to_string(result, FALSE);
	answer = VENTURE_ENTITY(venture_form_new());
	venture_entity_set_organization_id(answer, venture_entity_get_organization_id(entity));
	g_object_set(answer, "name", "Export", "result", text, NULL);
	return answer;
}

static void
forms_register_actions(VentureDatabase *database)
{
	g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;

	/* The database owns the registry that holds this pointer, so it is
	 * borrowed, not referenced: a reference would be a cycle. */
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "form", "name", "publish", "label", "Publish",
		"description", "Freeze the form's current questions as its next version and show that version to the public",
		"parameters", parameters, "stageable", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
		forms_publish_allowed, forms_publish_invoke, database, NULL, &error))
		g_error("Form publish action registration: %s", error->message);
	g_clear_object(&action);
	g_ptr_array_add(parameters, venture_field_spec_new("organization_id", "Organization", VENTURE_FIELD_KIND_INTEGER));
	g_ptr_array_add(parameters, venture_field_spec_new("limit", "Limit", VENTURE_FIELD_KIND_INTEGER));
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "form", "name", "sweep_retention", "label", "Apply retention",
		"description", "Anonymise or purge responses older than each form keeps them, at most limit (100) at a time",
		"parameters", parameters, "stageable", TRUE, "type-level", TRUE, "service-transaction", TRUE,
		"roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
		forms_publish_allowed_type, forms_sweep_invoke, database, NULL, &error))
		g_error("Form retention action registration: %s", error->message);
	g_clear_object(&action);
	g_ptr_array_set_size(parameters, 1);
	g_ptr_array_add(parameters, venture_field_spec_new("email", "Email", VENTURE_FIELD_KIND_STRING));
	/* Deleting what a person sent is access management, not data entry:
	 * owners only, like users and tokens. */
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "form", "name", "erase_person", "label", "Erase a person's responses",
		"description", "Delete every form response that carries this email address, cancel their queued confirmations, and record that an erasure happened",
		"parameters", parameters, "stageable", TRUE, "type-level", TRUE, "service-transaction", TRUE,
		"roles", VENTURE_USER_ROLE_OWNER, NULL);
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
		forms_publish_allowed_type, forms_erase_invoke, database, NULL, &error))
		g_error("Form erasure action registration: %s", error->message);
	g_clear_object(&action);
	/* Owners only and never staged: the answer carries sensitive answers,
	 * and a confirmation card is shown to more people than the owner. */
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "form", "name", "export_person", "label", "Export a person's responses",
		"description", "Every form response carrying this email address, sensitive answers included, for an access request",
		"parameters", parameters, "stageable", FALSE, "type-level", TRUE, "service-transaction", TRUE,
		"roles", VENTURE_USER_ROLE_OWNER, NULL);
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
		forms_publish_allowed_type, forms_export_invoke, database, NULL, &error))
		g_error("Form export action registration: %s", error->message);
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
	venture_database_add_save_validator(database, VENTURE_TYPE_FORM_VERSION,
	                                    forms_validate_version, NULL, NULL);
	forms_register_actions(database);
}

/* ==========================================================================
 * Finding forms and their questions
 * ========================================================================== */

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
	gint64 limit = venture_forms_get_int(form, "response-limit");

	if (venture_entity_is_deleted(form))
		return FALSE;
	g_object_get(form, "state", &state, "closes-at", &closes, NULL);
	if (VENTURE_FORM_LIVE != state)
		return FALSE;
	/* Live and never published has nothing to show a stranger. */
	if (venture_forms_get_int(form, "published-version-id") <= 0)
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
 * "<issued unix seconds>.<version number>.<hex HMAC-SHA256 of
 * token:seconds:version>", keyed by the form's private ticket key. The
 * time says how long the person took, and a robot cannot forge one older
 * than the form it fetched, so the minimum fill time is a real delay for
 * anything that fetches the form fresh. A snippet pasted into a static
 * page carries the ticket it was generated with, which is always old; the
 * honeypot is what guards that way in, and the docs say so.
 *
 * The version says which questions the person was given. Whoever started
 * on version 1 finishes on version 1, even if version 2 is published
 * while they type: their answers are checked against, and stored with,
 * the questions they could see.
 * ========================================================================== */

static gchar *
forms_ticket_mac(VentureEntity *form, gint64 issued, gint64 version)
{
	g_autofree gchar *key = venture_forms_get_string(form, "ticket-key");
	g_autofree gchar *token = venture_forms_get_string(form, "public-token");
	g_autofree gchar *payload = g_strdup_printf("%s:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
		token != NULL ? token : "", issued, version);

	if (venture_string_is_empty(key))
		return NULL;
	return g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)key, strlen(key), payload, -1);
}

gchar *
venture_forms_ticket_new_for_version(VentureEntity *form, gint64 version, GDateTime *issued)
{
	g_autofree gchar *mac = NULL;
	gint64 seconds;

	g_return_val_if_fail(VENTURE_IS_FORM(form), NULL);
	g_return_val_if_fail(issued != NULL, NULL);

	seconds = g_date_time_to_unix(issued);
	mac = forms_ticket_mac(form, seconds, version);
	return g_strdup_printf("%" G_GINT64_FORMAT ".%" G_GINT64_FORMAT ".%s", seconds, version,
	                       mac != NULL ? mac : "");
}

gchar *
venture_forms_ticket_new(VentureEntity *form, GDateTime *issued)
{
	g_return_val_if_fail(VENTURE_IS_FORM(form), NULL);

	return venture_forms_ticket_new_for_version(form,
		venture_forms_get_int(form, "published-number"), issued);
}

gboolean
venture_forms_ticket_parse(VentureEntity *form, const gchar *ticket, gint64 *issued, gint64 *version)
{
	g_auto(GStrv) parts = NULL;
	g_autofree gchar *expected = NULL;

	if (venture_string_is_empty(ticket) || strlen(ticket) > 120)
		return FALSE;
	parts = g_strsplit(ticket, ".", 4);
	if (3 != g_strv_length(parts) ||
	    !g_ascii_string_to_signed(parts[0], 10, 0, G_MAXINT64, issued, NULL) ||
	    !g_ascii_string_to_signed(parts[1], 10, 0, G_MAXINT64, version, NULL))
		return FALSE;
	expected = forms_ticket_mac(form, *issued, *version);
	return NULL != expected && venture_constant_time_equal(expected, parts[2]);
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
	gint64 issued = 0, version = 0, fill;

	g_return_val_if_fail(VENTURE_IS_FORM(form), FALSE);
	g_return_val_if_fail(answers != NULL, FALSE);

	if (!venture_string_is_empty(forms_first(answers, VENTURE_FORMS_HONEYPOT)))
		return FALSE;
	if (!venture_forms_ticket_parse(form, forms_first(answers, VENTURE_FORMS_TICKET), &issued, &version))
		return FALSE;
	fill = venture_forms_get_int(form, "min-fill-seconds");
	if (fill <= 0)
		fill = VENTURE_FORMS_DEFAULT_FILL_SECONDS;
	return g_date_time_to_unix(now) - issued >= fill;
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
forms_check_field(const VentureFormsField *field, GPtrArray *values, JsonObject *answers,
	JsonObject *errors, GString *summary)
{
	const gchar *key = field->key;
	const gchar *label = field->label;
	const gchar *pattern = field->pattern;
	const gchar *fallback = field->default_value;
	g_autofree gchar *text = NULL;
	VentureFormFieldKind kind = field->kind;
	gboolean required = field->required;
	gint64 min_length = field->min_length;
	gint64 max_length = field->max_length;
	gdouble min = field->min_value;
	gdouble max = field->max_value;
	glong length;
	guint i;

	if (max_length <= 0)
		max_length = forms_default_max_length(kind);

	if (VENTURE_FORM_FIELD_MULTIPLE_CHOICE == kind)
	{
		GPtrArray *choices = field->choices;
		JsonArray *picked = json_array_new();
		g_autoptr(GString) labels = g_string_new(NULL);

		for (i = 0; NULL != values && i < values->len; i++)
		{
			const gchar *id = g_ptr_array_index(values, i);
			const gchar *choice = venture_forms_choice_label(choices, id);
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

	if (VENTURE_FORM_FIELD_CHECKBOX == kind || VENTURE_FORM_FIELD_CONSENT == kind)
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
		/* A permission is the words it was given to, not a tick: keep
		 * them. An unticked box records nothing, so consent is never
		 * invented from a default. */
		if (VENTURE_FORM_FIELD_CONSENT == kind)
		{
			JsonObject *consent;

			if (!ticked)
				return;
			consent = json_object_new();
			json_object_set_boolean_member(consent, "given", TRUE);
			json_object_set_string_member(consent, "wording", label);
			if (!venture_string_is_empty(field->help))
				json_object_set_string_member(consent, "detail", field->help);
			json_object_set_object_member(answers, key, consent);
			g_string_append_printf(summary, "%s: Given\n", label);
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

			venture_forms_rating_bounds(field, &low, &high);
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
			const gchar *choice = venture_forms_choice_label(field->choices, text);

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
			const VentureFormsField *field = g_ptr_array_index(fields, i);
			JsonNode *node;

			if (!json_object_has_member(answers, field->key))
				continue;
			node = json_object_get_member(answers, field->key);
			if (!JSON_NODE_HOLDS_VALUE(node) || G_TYPE_STRING != json_node_get_value_type(node))
				continue;
			if ((0 == pass && 0 == g_strcmp0(field->maps_to, "name")) ||
			    (1 == pass && 0 == g_strcmp0(field->key, "name")))
				return venture_truncate(json_node_get_string(node), 200);
			if (NULL == email && VENTURE_FORM_FIELD_EMAIL == field->kind)
				email = json_node_get_string(node);
		}
	}
	if (NULL != email)
		return g_strdup(email);
	title = venture_forms_get_string(form, "title");
	if (venture_string_is_empty(title))
	{
		g_free(title);
		title = venture_forms_get_string(form, "name");
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
		if (0 == g_strcmp0(((const VentureFormsField *)g_ptr_array_index(fields, i))->maps_to, "notes"))
			mapped_notes++;
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);
		JsonNode *node;
		g_autofree gchar *text = NULL;

		if (venture_string_is_empty(field->maps_to) || !json_object_has_member(answers, field->key))
			continue;
		node = json_object_get_member(answers, field->key);
		if (JSON_NODE_HOLDS_VALUE(node) && G_TYPE_STRING == json_node_get_value_type(node))
			text = g_strdup(json_node_get_string(node));
		else
			text = json_to_string(node, FALSE);
		if (0 == g_strcmp0(field->maps_to, "notes"))
		{
			if (notes->len > 0)
				g_string_append(notes, "\n");
			if (mapped_notes > 1)
				g_string_append_printf(notes, "%s: %s", field->label, text);
			else
				g_string_append(notes, text);
		}
		else
			json_object_set_string_member(values, field->maps_to, text);
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
	g_autofree gchar *field = venture_forms_get_string(form, "confirmation-field");
	g_autofree gchar *subject = venture_forms_get_string(form, "confirmation-subject");
	g_autofree gchar *body = venture_forms_get_string(form, "confirmation-message");
	g_autofree gchar *success = venture_forms_get_string(form, "success-message");
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

	/* Under the lock, re-read: the cap and the state are what they are
	 * now, so two people posting the last place at once get one response
	 * between them, not two, and a form closed a moment ago is closed. */
	{
		g_autoptr(VentureEntity) fresh = venture_database_get(database, VENTURE_TYPE_FORM,
			venture_entity_get_id(form), NULL);

		if (NULL == fresh || !forms_is_open(database, fresh, now))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
			goto fail;
		}
	}

	if (follow_up && venture_forms_get_bool(form, "create-lead"))
	{
		g_autoptr(JsonObject) values = forms_lead_values(fields, answers);
		g_autoptr(VentureEntity) lead = NULL;
		g_autofree gchar *source = venture_forms_get_string(form, "lead-source");
		g_autofree gchar *policy = venture_forms_get_string(form, "on-duplicate");

		if (venture_string_is_empty(source))
		{
			g_free(source);
			source = venture_forms_get_string(form, "title");
		}
		if (venture_string_is_empty(source))
		{
			g_free(source);
			source = venture_forms_get_string(form, "name");
		}
		lead = venture_lead_service_capture_values(venture_database_get_lead_service(database),
			venture_entity_get_organization_id(form), venture_forms_get_int(form, "venture-id"), source,
			venture_forms_get_int(form, "campaign-id"), values, policy, error);
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
forms_new_response(VentureEntity *form, VentureEntity *version, const gchar *name, GDateTime *now,
	const gchar *answers, const gchar *summary, const gchar *origin)
{
	VentureEntity *response = VENTURE_ENTITY(venture_form_submission_new());

	venture_entity_set_organization_id(response, venture_entity_get_organization_id(form));
	g_object_set(response, "name", name, "form-id", venture_entity_get_id(form),
		"version-id", venture_entity_get_id(version),
		"version-number", venture_forms_get_int(version, "number"),
		"submitted-at", now, "answers", answers, "summary", summary,
		"origin", venture_string_is_empty(origin) ? NULL : origin, NULL);
	g_object_set_data(G_OBJECT(response), VENTURE_FORMS_ACCEPTING_KEY, GINT_TO_POINTER(1));
	return response;
}

VentureEntity *
venture_forms_version_for_answers(VentureDatabase *database, VentureEntity *form, GHashTable *answers)
{
	gint64 issued = 0, number = 0;

	if (venture_forms_ticket_parse(form, forms_first(answers, VENTURE_FORMS_TICKET), &issued, &number) &&
	    number > 0)
	{
		VentureEntity *version = venture_forms_version_by_number(database, form, number, NULL);

		if (NULL != version)
			return version;
	}
	return venture_forms_published_version(database, form, NULL);
}

gboolean
venture_forms_submit(VentureDatabase *database, VentureEntity *form, GHashTable *answers,
	const gchar *origin, GDateTime *now, VentureFormsOutcome *outcome,
	VentureEntity **submission, JsonObject **errors, GError **error)
{
	g_autoptr(VentureEntity) version = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(JsonObject) stored = json_object_new();
	g_autoptr(JsonObject) refused = json_object_new();
	g_autoptr(JsonObject) secret = json_object_new();
	g_autofree gchar *secret_text = NULL;
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

	version = venture_forms_version_for_answers(database, form, answers);
	if (NULL == version)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
		return FALSE;
	}
	fields = venture_forms_definition_for(database, form, version, error);
	if (NULL == fields)
		return FALSE;

	/* A name that is no question on this version is refused, not
	 * dropped: the sender believes it was received. The door's own
	 * names pass. */
	g_hash_table_iter_init(&iter, answers);
	while (g_hash_table_iter_next(&iter, &key, NULL))
	{
		if (0 == g_strcmp0(key, VENTURE_FORMS_HONEYPOT) || 0 == g_strcmp0(key, VENTURE_FORMS_TICKET))
			continue;
		if (NULL == venture_forms_definition_find(fields, key))
			forms_refuse(refused, key, "This form has no such question.");
	}

	/* A sensitive answer is checked like any other, then kept apart: in
	 * a column that never leaves the record, with only its question named
	 * in the summary that search, webhooks and the assistant read. */
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);

		if (field->sensitive)
		{
			g_autoptr(GString) hidden = g_string_new(NULL);

			forms_check_field(field, g_hash_table_lookup(answers, field->key), secret, refused, hidden);
			if (hidden->len > 0)
				g_string_append_printf(summary, "%s: (sensitive)\n", field->label);
			continue;
		}
		forms_check_field(field, g_hash_table_lookup(answers, field->key), stored, refused, summary);
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
	if (json_object_get_size(secret) > 0)
	{
		g_autoptr(JsonNode) hidden = json_node_new(JSON_NODE_OBJECT);

		json_node_set_object(hidden, secret);
		secret_text = json_to_string(hidden, FALSE);
	}
	if (summary->len > 0 && '\n' == summary->str[summary->len - 1])
		g_string_truncate(summary, summary->len - 1);

	response = forms_new_response(form, version, name, now, answers_text, summary->str, origin);
	g_object_set(response, "sensitive-answers", secret_text, NULL);
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
		response = forms_new_response(form, version, name, now, answers_text, summary->str, origin);
		g_object_set(response, "mapping-note", note, "sensitive-answers", secret_text, NULL);
		if (!forms_write(database, form, response, fields, stored, FALSE, now, error))
			return FALSE;
	}

	*outcome = VENTURE_FORMS_ACCEPTED;
	if (NULL != submission)
		*submission = g_steal_pointer(&response);
	return TRUE;
}

/* ==========================================================================
 * Publishing
 * ========================================================================== */

VentureEntity *
venture_forms_publish(VentureDatabase *database, VentureEntity *form, const VentureActor *actor,
	GError **error)
{
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureEntity) latest = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *definition = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *name = NULL;
	g_autoptr(GDateTime) published = venture_time_now();
	VentureEntity *version;
	gint64 number;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_FORM(form), NULL);

	if (!venture_database_begin(database, error))
		return NULL;

	/* Read the form under the lock: a publish from a stale page must not
	 * point the form back at an older version. */
	stored = venture_database_get(database, VENTURE_TYPE_FORM, venture_entity_get_id(form), error);
	if (NULL == stored)
		goto fail;
	fields = venture_forms_definition_from_records(database, stored, error);
	if (NULL == fields)
		goto fail;
	if (0 == fields->len)
	{
		venture_set_error_validation(error, "Questions",
			"add at least one question before publishing");
		goto fail;
	}
	definition = venture_forms_definition_to_json(fields);

	/* Publishing what is already published changes nothing and makes no
	 * version: a second click is not a second version. */
	current = venture_forms_published_version(database, stored, NULL);
	if (NULL != current)
	{
		g_autofree gchar *frozen = venture_forms_get_string(current, "definition");

		if (0 == g_strcmp0(frozen, definition))
		{
			if (!venture_database_commit(database, error))
				return NULL;
			return g_steal_pointer(&current);
		}
	}

	query = venture_query_new(VENTURE_TYPE_FORM_VERSION);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_organization(query, venture_entity_get_organization_id(stored));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(stored), NULL);
	venture_query_add_order(query, "number", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 1);
	latest = venture_database_find_one(database, query, NULL);
	number = venture_forms_get_int(latest, "number") + 1;

	title = venture_forms_get_string(stored, "title");
	if (venture_string_is_empty(title))
	{
		g_free(title);
		title = venture_forms_get_string(stored, "name");
	}
	name = g_strdup_printf("%s, version %" G_GINT64_FORMAT, title, number);
	version = VENTURE_ENTITY(venture_form_version_new());
	venture_entity_set_organization_id(version, venture_entity_get_organization_id(stored));
	g_object_set(version, "name", name, "form-id", venture_entity_get_id(stored), "number", number,
		"definition", definition, "published-at", published,
		"published-by", (NULL != actor && NULL != actor->name) ? actor->name : NULL, NULL);
	g_object_set_data(G_OBJECT(version), VENTURE_FORMS_ACCEPTING_KEY, GINT_TO_POINTER(1));
	if (!venture_database_save(database, version, actor, error))
	{
		g_object_unref(version);
		goto fail;
	}
	g_object_set(stored, "published-version-id", venture_entity_get_id(version), NULL);
	if (!venture_database_save(database, stored, actor, error))
	{
		g_object_unref(version);
		goto fail;
	}
	if (!venture_database_commit(database, error))
	{
		g_object_unref(version);
		return NULL;
	}
	return version;

fail:
	venture_database_rollback(database);
	return NULL;
}

gboolean
venture_forms_has_unpublished_changes(VentureDatabase *database, VentureEntity *form)
{
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(VentureEntity) version = NULL;
	g_autofree gchar *draft = NULL;
	g_autofree gchar *frozen = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(VENTURE_IS_FORM(form), FALSE);

	fields = venture_forms_definition_from_records(database, form, NULL);
	if (NULL == fields)
		return FALSE;
	version = venture_forms_published_version(database, form, NULL);
	if (NULL == version)
		return fields->len > 0;
	draft = venture_forms_definition_to_json(fields);
	frozen = venture_forms_get_string(version, "definition");
	return 0 != g_strcmp0(draft, frozen);
}

/* ==========================================================================
 * Retention
 *
 * A form may keep its responses for a number of days. The sweep removes
 * what is older, a bounded number at a time, when it is asked to --
 * never on a timer, because the only background thread belongs to coding
 * runs and may not touch the database. Anonymising keeps the response and
 * its version, so the counts stay right, and empties everything a person
 * typed; purging deletes it. Files and drafts, when forms have them, go
 * with the response here.
 * ========================================================================== */

static gboolean
forms_anonymise(VentureDatabase *database, VentureEntity *response, GDateTime *now,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *name = g_strdup_printf("Anonymised response #%" G_GINT64_FORMAT,
		venture_entity_get_id(response));

	g_object_set(response, "name", name, "answers", "{}", "sensitive-answers", NULL,
		"summary", "", "notes", NULL, "origin", NULL, "mapping-note", NULL,
		"anonymised-at", now, NULL);
	g_object_set_data(G_OBJECT(response), VENTURE_FORMS_ANONYMISING_KEY, GINT_TO_POINTER(1));
	return venture_database_save(database, response, actor, error);
}

JsonNode *
venture_forms_retention_sweep(VentureDatabase *database, gint64 organization_id, guint limit,
	GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) forms = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	gint64 anonymised = 0, purged = 0;
	guint budget, i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	budget = limit > 0 ? MIN(limit, 1000) : 100;
	forms = venture_query_new(VENTURE_TYPE_FORM);
	venture_query_set_organization(forms, organization_id);
	venture_query_set_limit(forms, 0);
	venture_query_add_filter_int(forms, "retention-days", VENTURE_FILTER_OP_GT, 0, NULL);
	rows = venture_database_find(database, forms, error);
	if (NULL == rows)
		return NULL;

	for (i = 0; i < rows->len && budget > 0; i++)
	{
		VentureEntity *form = g_ptr_array_index(rows, i);
		g_autofree gchar *action = venture_forms_get_string(form, "retention-action");
		g_autoptr(GDateTime) cutoff = g_date_time_add_days(now, -(gint)venture_forms_get_int(form, "retention-days"));
		g_autofree gchar *before = venture_time_to_string(cutoff);
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
		g_autoptr(GPtrArray) expired = NULL;
		gboolean purge = 0 == g_strcmp0(action, "purge");
		guint j;

		venture_query_set_organization(query, organization_id);
		venture_query_set_limit(query, budget);
		venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
		venture_query_add_filter_string(query, "submitted-at", VENTURE_FILTER_OP_LT, before, NULL);
		if (!purge)
			venture_query_add_filter_string(query, "anonymised-at", VENTURE_FILTER_OP_IS_NULL, NULL, NULL);
		venture_query_add_order(query, "submitted-at", VENTURE_SORT_ASCENDING, NULL);
		expired = venture_database_find(database, query, error);
		if (NULL == expired)
			return NULL;
		for (j = 0; j < expired->len && budget > 0; j++, budget--)
		{
			VentureEntity *response = g_ptr_array_index(expired, j);

			if (purge ? !venture_database_purge(database, response, actor, error)
			          : !forms_anonymise(database, response, now, actor, error))
				return NULL;
			if (purge)
				purged++;
			else
				anonymised++;
		}
	}

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "anonymised");
	json_builder_add_int_value(builder, anonymised);
	json_builder_set_member_name(builder, "purged");
	json_builder_add_int_value(builder, purged);
	json_builder_set_member_name(builder, "limit_reached");
	json_builder_add_boolean_value(builder, 0 == budget);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

/* ==========================================================================
 * Erasure
 *
 * "Delete everything this person sent": every response in the
 * organization whose answers carry the address, sensitive answers
 * included. Queued confirmations are cancelled so they are never sent (the
 * outbox keeps its own retained record of every message, sent or not, as
 * mail does). The responses are deleted outright, and one audit entry
 * says an erasure happened and how many -- not whose, and not what.
 * Leads made from those responses are CRM records and are erased there.
 * ========================================================================== */

/* Whether any string in @text's JSON object equals @email, ignoring case. */
static gboolean
forms_answers_mention(const gchar *text, const gchar *email)
{
	g_autoptr(JsonNode) root = NULL;
	JsonObjectIter iter;
	const gchar *member;
	JsonNode *node;

	if (venture_string_is_empty(text))
		return FALSE;
	root = json_from_string(text, NULL);
	if (NULL == root || !JSON_NODE_HOLDS_OBJECT(root))
		return FALSE;
	json_object_iter_init(&iter, json_node_get_object(root));
	while (json_object_iter_next(&iter, &member, &node))
		if (JSON_NODE_HOLDS_VALUE(node) && G_TYPE_STRING == json_node_get_value_type(node))
		{
			g_autofree gchar *value = g_strstrip(g_strdup(json_node_get_string(node)));

			if (0 == g_ascii_strcasecmp(value, email))
				return TRUE;
		}
	return FALSE;
}

JsonNode *
venture_forms_erase_person(VentureDatabase *database, gint64 organization_id, const gchar *email,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autofree gchar *wanted = NULL;
	gint64 erased = 0, cancelled = 0;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	wanted = g_strstrip(g_strdup(email != NULL ? email : ""));
	if (NULL == strchr(wanted, '@'))
	{
		venture_set_error_validation(error, "email", "name the address whose responses to erase");
		return NULL;
	}
	query = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(database, query, error);
	if (NULL == rows || !venture_database_begin(database, error))
		return NULL;

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *response = g_ptr_array_index(rows, i);
		g_autofree gchar *open = venture_forms_get_string(response, "answers");
		g_autofree gchar *hidden = venture_forms_get_string(response, "sensitive-answers");

		if (!forms_answers_mention(open, wanted) && !forms_answers_mention(hidden, wanted))
			continue;
		if (G_TYPE_INVALID != venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message") &&
		    NULL != venture_database_get_mail_outbox(database))
		{
			g_autoptr(VentureQuery) mail = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
			g_autoptr(VentureEntity) message = NULL;
			g_autofree gchar *key = g_strdup_printf("form-response:%s", venture_entity_get_uuid(response));

			venture_query_set_organization(mail, organization_id);
			venture_query_add_filter_string(mail, "idempotency-key", VENTURE_FILTER_OP_EQ, key, NULL);
			message = venture_database_find_one(database, mail, NULL);
			/* Already sent is sent; a refusal to cancel it is not a
			 * reason to keep the response. */
			if (NULL != message && venture_mail_outbox_cancel(venture_database_get_mail_outbox(database),
				organization_id, venture_entity_get_id(message), "erasure request", actor, NULL))
				cancelled++;
		}
		if (!venture_database_purge(database, response, actor, error))
			goto fail;
		erased++;
	}

	{
		g_autoptr(VentureAuditEntry) entry = NULL;
		g_autoptr(JsonNode) diff = json_from_string("{}", NULL);

		json_object_set_int_member(json_node_get_object(diff), "erased", erased);
		entry = venture_audit_entry_new_for_change(VENTURE_AUDIT_ACTION_DELETE,
			NULL != actor ? actor->kind : VENTURE_ACTOR_KIND_SYSTEM,
			NULL != actor && NULL != actor->name ? actor->name : "system", NULL, diff);
		venture_entity_set_organization_id(VENTURE_ENTITY(entry), organization_id);
		g_object_set(entry, "target-type", "form_submission", "target-label", "Erasure request", NULL);
		if (!venture_database_save(database, VENTURE_ENTITY(entry), NULL, error))
			goto fail;
	}
	if (!venture_database_commit(database, error))
		return NULL;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "erased");
	json_builder_add_int_value(builder, erased);
	json_builder_set_member_name(builder, "confirmations_cancelled");
	json_builder_add_int_value(builder, cancelled);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);

fail:
	venture_database_rollback(database);
	return NULL;
}

/* ==========================================================================
 * Export: a person's responses, for an access request
 * ========================================================================== */

JsonNode *
venture_forms_export_person(VentureDatabase *database, gint64 organization_id, const gchar *email,
	GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autofree gchar *wanted = g_strstrip(g_strdup(email != NULL ? email : ""));
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	if (NULL == strchr(wanted, '@'))
	{
		venture_set_error_validation(error, "email", "name the address whose responses to export");
		return NULL;
	}
	query = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(database, query, error);
	if (NULL == rows)
		return NULL;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "email");
	json_builder_add_string_value(builder, wanted);
	json_builder_set_member_name(builder, "responses");
	json_builder_begin_array(builder);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *response = g_ptr_array_index(rows, i);
		g_autofree gchar *open = venture_forms_get_string(response, "answers");
		g_autofree gchar *hidden = venture_forms_get_string(response, "sensitive-answers");
		g_autoptr(VentureEntity) form = NULL;
		g_autoptr(GDateTime) at = NULL;
		g_autofree gchar *when = NULL, *title = NULL;

		if (!forms_answers_mention(open, wanted) && !forms_answers_mention(hidden, wanted))
			continue;
		form = venture_database_get(database, VENTURE_TYPE_FORM, venture_forms_get_int(response, "form-id"), NULL);
		title = venture_forms_get_string(form, "title");
		g_object_get(response, "submitted-at", &at, NULL);
		when = NULL != at ? venture_time_to_string(at) : g_strdup("");
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "form");
		json_builder_add_string_value(builder, title != NULL ? title : "");
		json_builder_set_member_name(builder, "form_version");
		json_builder_add_int_value(builder, venture_forms_get_int(response, "version-number"));
		json_builder_set_member_name(builder, "received");
		json_builder_add_string_value(builder, when);
		/* Everything they sent, sensitive answers included: an access
		 * request is owed the lot. */
		json_builder_set_member_name(builder, "answers");
		json_builder_add_value(builder, json_from_string(venture_string_is_empty(open) ? "{}" : open, NULL));
		json_builder_set_member_name(builder, "sensitive_answers");
		json_builder_add_value(builder, json_from_string(venture_string_is_empty(hidden) ? "{}" : hidden, NULL));
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}
