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
#include <stripe-glib.h>
#include <string.h>

#define VENTURE_FORMS_STATE_KEY "venture-forms-installed"
#define VENTURE_FORMS_DEFAULT_FILL_SECONDS 3
#define VENTURE_FORMS_ANONYMISING_KEY "venture-forms-anonymising"

static gboolean forms_answers_mention(const gchar *text, const gchar *email);
static gboolean forms_bound_email(VentureDatabase *database, VentureEntity *response, const gchar *email);
static void forms_payment_context_free(gpointer data);
static VentureEntity *forms_payment_recover_invoke(VentureAction *action, VentureEntity *form,
	GHashTable *params, const VentureActor *actor, GError **error);
static gboolean forms_payment_validate(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error);
static gboolean forms_payment_limits(VentureDatabase *database, VentureEntity *form,
	VentureEntity *response, JsonObject *refused, GError **error);
static gboolean forms_payment_begin(VentureDatabase *database, VentureEntity *form, VentureEntity *version,
	VentureEntity *response, GPtrArray *fields, JsonObject *answers, const gchar *nonce,
	JsonArray *lines, VentureMoney *total, GDateTime *now, JsonObject *refused, GError **error);

static gboolean forms_validate_draft(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error);

static gboolean forms_resume_save(VentureDatabase *database, VentureEntity *form, VentureEntity *draft,
	VentureFormsStep *step, const gchar *email, GDateTime *now, GError **error);
static gboolean forms_working_copy_purge(VentureDatabase *database, VentureEntity *copy, const gchar *type,
	const VentureActor *actor, gint64 *cancelled, GError **error);

static gboolean forms_check_published(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, GError **error);

static gboolean forms_validate_pending(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error);
static gboolean forms_optin_definition(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GError **error);
static gboolean forms_optin_store(VentureDatabase *database, VentureEntity *form, VentureEntity *version,
	GHashTable *answers, GPtrArray *fields, JsonObject *checked, const gchar *origin,
	GDateTime *now, JsonObject *refused, GError **error);
static gboolean forms_optin_follow_up(VentureDatabase *database, VentureEntity *form, VentureEntity *response,
	VentureEntity *pending, GPtrArray *fields, JsonObject *answers, GDateTime *now, GError **error);

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

	{
		g_autoptr(GDateTime) opens = NULL;
		g_autoptr(GDateTime) closes = NULL;

		g_object_get(entity, "opens-at", &opens, "closes-at", &closes, NULL);
		if (opens != NULL && closes != NULL && g_date_time_compare(opens, closes) >= 0)
		{
			venture_set_error_validation(error, "Closes at", "must be after Opens at");
			return FALSE;
		}
	}
	if (venture_forms_get_int(entity, "upload-quota-bytes") < 0 || venture_forms_get_int(entity, "upload-quota-bytes") > G_GINT64_CONSTANT(1099511627776) ||
	    venture_forms_get_int(entity, "upload-client-hourly-bytes") < 0 || venture_forms_get_int(entity, "upload-client-hourly-bytes") > G_GINT64_CONSTANT(1099511627776))
	{ venture_set_error_validation(error, "Upload quota", "must be 0..1 TiB"); return FALSE; }
	if (venture_forms_get_int(entity, "resume-days") < 0 || venture_forms_get_int(entity, "resume-days") > 30)
	{
		venture_set_error_validation(error, "Saved draft lifetime", "must be 0..30 days");
		return FALSE;
	}
	if (venture_forms_get_bool(entity, "allow-resume"))
	{
		g_autofree gchar *origin = venture_forms_get_string(entity, "public-origin");
		if (!venture_forms_public_origin_valid(origin, error)) return FALSE;
	}
	if (venture_forms_get_int(entity, "draft-minutes") < 0 || venture_forms_get_int(entity, "draft-minutes") > 1440)
	{
		venture_set_error_validation(error, "Draft lifetime", "must be 0..1440 minutes");
		return FALSE;
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

	{
		g_autofree gchar *language = venture_forms_get_string(entity, "default-language");
		g_autofree gchar *normalized = NULL;
		if (!venture_string_is_empty(language) && !venture_forms_language_valid(language))
		{ venture_set_error_validation(error, "Default language", "use a language tag such as en or fr-ca"); return FALSE; }
		normalized = venture_string_is_empty(language) ? g_strdup("en") : g_ascii_strdown(language, -1);
		g_object_set(entity, "default-language", normalized, NULL);
	}
	return venture_forms_validate_piping(database, entity, error) &&
	       forms_check_published(database, entity, previous, error);
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

	{
		g_autofree gchar *contact = venture_forms_get_string(entity, "contact-field");
		gboolean query = venture_forms_get_bool(entity, "allow-prefill");
		const gchar *allowed[] = { "name", "email", "phone", "role", "website", "address", NULL };
		if (query && (g_strcmp0(key, "personal") == 0 || g_strcmp0(key, "style") == 0))
		{
			venture_set_error_validation(error, "Prefill", "personal and style are reserved public query parameters");
			return FALSE;
		}

		if (!venture_string_is_empty(contact) && !g_strv_contains(allowed, contact))
		{
			venture_set_error_validation(error, "Contact prefill", "choose name, email, phone, role, website or address");
			return FALSE;
		}
		if ((query || !venture_string_is_empty(contact)) && (kind == VENTURE_FORM_FIELD_CONSENT ||
		    kind == VENTURE_FORM_FIELD_PAGE_BREAK || kind == VENTURE_FORM_FIELD_BOOKING || venture_forms_get_bool(entity, "sensitive") ||
		    venture_forms_get_int(entity, "group-id") != 0))
		{
			venture_set_error_validation(error, "Prefill", "consent, sensitive questions, page breaks and repeated questions cannot be prefilled");
			return FALSE;
		}
	}

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

	{
		gint64 group_id = venture_forms_get_int(entity, "group-id");
		if (group_id != 0 && (previous == NULL || group_id != venture_forms_get_int(previous, "group-id")))
		{
			g_autoptr(VentureEntity) group = venture_database_get(database, VENTURE_TYPE_FORM_GROUP, group_id, NULL);
			if (group == NULL || venture_forms_get_int(group, "form-id") != form_id ||
			    venture_entity_get_organization_id(group) != venture_entity_get_organization_id(entity))
			{
				venture_set_error_validation(error, "Repeat group", "choose a group on this form");
				return FALSE;
			}
		}
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
		if (!venture_forms_key_valid(key))
		{
			venture_set_error_validation(error, "Key",
				"must start with a lowercase letter and use only lowercase letters, "
				"digits and _ (at most %d)", VENTURE_FORMS_KEY_MAX);
			return FALSE;
		}
		{
			g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_GROUP);
			g_autoptr(GPtrArray) groups = NULL;
			venture_query_set_organization(query, venture_entity_get_organization_id(entity));
			venture_query_set_include_deleted(query, TRUE);
			venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, form_id, NULL);
			venture_query_add_filter_string(query, "key", VENTURE_FILTER_OP_EQ, key, NULL);
			venture_query_set_limit(query, 1);
			groups = venture_database_find(database, query, error);
			if (groups == NULL) return FALSE;
			if (groups->len > 0)
			{
				venture_set_error_validation(error, "Key", "this name belongs to a repeat group");
				return FALSE;
			}
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

	if (kind == VENTURE_FORM_FIELD_PAGE_BREAK &&
	    (venture_forms_get_bool(entity, "required") || !venture_string_is_empty(maps)))
	{
		venture_set_error_validation(error, "Page break", "cannot be required or mapped to a lead");
		return FALSE;
	}
	if (venture_forms_get_bool(entity, "sensitive"))
	{
		g_autofree gchar *initial = venture_forms_get_string(entity, "default-value");
		if (!venture_string_is_empty(initial))
		{ venture_set_error_validation(error, "Sensitive question", "cannot publish a default answer"); return FALSE; }
	}
	if (kind == VENTURE_FORM_FIELD_CONSENT)
	{
		g_autofree gchar *default_value = venture_forms_get_string(entity, "default-value");

		if (!venture_string_is_empty(default_value))
		{
			venture_set_error_validation(error, "Default", "a consent question cannot have a default answer");
			return FALSE;
		}
	}

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
	if (venture_forms_get_bool(entity, "marketing-consent") &&
	    (kind != VENTURE_FORM_FIELD_CONSENT || venture_forms_get_bool(entity, "sensitive")))
	{
		venture_set_error_validation(error, "Marketing permission",
			"only a non-sensitive consent question may grant marketing permission");
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

	{
		VentureFormsField *field = venture_forms_field_from_record(entity);
		gboolean valid = venture_forms_quiz_field_valid(field, error);
		venture_forms_field_free(field);
		if (!valid) return FALSE;
	}
	return venture_forms_upload_field_validate(entity, error) && venture_forms_validate_piping(database, entity, error);
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
		g_autofree gchar *personal_before = venture_forms_get_string(previous, "personal-hash");
		g_autofree gchar *personal_after = venture_forms_get_string(entity, "personal-hash");
		g_autofree gchar *omitted_before = venture_forms_get_string(previous, "not-shown");
		g_autofree gchar *omitted_after = venture_forms_get_string(entity, "not-shown");
		g_autofree gchar *summary_before = venture_forms_get_string(previous, "summary");
		g_autofree gchar *summary_after = venture_forms_get_string(entity, "summary");
		g_autofree gchar *language_before = venture_forms_get_string(previous, "language");
		g_autofree gchar *language_after = venture_forms_get_string(entity, "language");
		g_autofree gchar *origin_before = venture_forms_get_string(previous, "origin");
		g_autofree gchar *origin_after = venture_forms_get_string(entity, "origin");
		g_autoptr(GDateTime) at_before = NULL;
		g_autoptr(GDateTime) at_after = NULL;

		g_autofree gchar *result_before = venture_forms_get_string(previous, "result-message"), *result_after = venture_forms_get_string(entity, "result-message");
		g_autofree gchar *key_before = venture_forms_get_string(previous, "result-key"), *key_after = venture_forms_get_string(entity, "result-key");
		g_autofree gchar *url_before = venture_forms_get_string(previous, "result-url"), *url_after = venture_forms_get_string(entity, "result-url");
		g_object_get(previous, "submitted-at", &at_before, NULL);
		g_object_get(entity, "submitted-at", &at_after, NULL);
		if (venture_forms_get_int(previous, "invoice-id") != venture_forms_get_int(entity, "invoice-id") ||
		    venture_forms_get_int(previous, "booking-id") != venture_forms_get_int(entity, "booking-id") ||
		    venture_forms_get_bool(previous, "scored") != venture_forms_get_bool(entity, "scored") ||
		    venture_forms_get_int(previous, "score") != venture_forms_get_int(entity, "score") ||
		    g_strcmp0(result_before, result_after) != 0 || g_strcmp0(key_before, key_after) != 0 || g_strcmp0(url_before, url_after) != 0 ||
		    0 != g_strcmp0(before, after) || 0 != g_strcmp0(secret_before, secret_after) ||
		    0 != g_strcmp0(omitted_before, omitted_after) ||
		    0 != g_strcmp0(personal_before, personal_after) ||
		    venture_forms_get_int(previous, "contact-id") != venture_forms_get_int(entity, "contact-id") ||
		    venture_forms_get_int(previous, "form-id") != venture_forms_get_int(entity, "form-id") ||
		    venture_forms_get_int(previous, "version-id") != venture_forms_get_int(entity, "version-id") ||
		    venture_forms_get_int(previous, "version-number") != venture_forms_get_int(entity, "version-number") ||
		    0 != g_strcmp0(summary_before, summary_after) ||
		    0 != g_strcmp0(origin_before, origin_after) ||
		    0 != g_strcmp0(language_before, language_after) ||
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
	(void)previous;
	version = venture_database_get(database, VENTURE_TYPE_FORM_VERSION, id, NULL);
	if (NULL == version || venture_forms_get_int(version, "form-id") != venture_entity_get_id(entity))
	{
		venture_set_error_validation(error, "Published version",
			"#%" G_GINT64_FORMAT " is not a version of this form", id);
		return FALSE;
	}
	{
		g_autofree gchar *key = venture_forms_get_string(entity, "unique-email-field");
		if (!venture_string_is_empty(key))
		{
			g_autoptr(GPtrArray) fields = venture_forms_definition_for(database, entity, version, error);
			const VentureFormsField *field;
			if (fields == NULL) return FALSE;
			field = venture_forms_definition_find(fields, key);
			if (field == NULL || field->kind != VENTURE_FORM_FIELD_EMAIL || !field->required || field->group_key != NULL)
			{
				venture_set_error_validation(error, "One response per email", "must name a required email question in the published version");
				return FALSE;
			}
		}
	}
	{
		g_autofree gchar *key = venture_forms_get_string(entity, "confirmation-field");
		if (!venture_string_is_empty(key))
		{
			g_autoptr(GPtrArray) fields = venture_forms_definition_for(database, entity, version, error);
			const VentureFormsField *field;
			if (fields == NULL) return FALSE;
			field = venture_forms_definition_find(fields, key);
			if (field != NULL && field->group_key != NULL)
			{
				venture_set_error_validation(error, "Confirmation recipient", "must name a non-repeated email question");
				return FALSE;
			}
		}
	}

	if (venture_forms_get_bool(entity, "double-opt-in"))
	{
		g_autoptr(GPtrArray) fields = venture_forms_definition_for(database, entity, version, error);
		if (fields == NULL || !forms_optin_definition(database, entity, fields, error)) return FALSE;
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
	JsonNode *email = g_hash_table_lookup(params, "email"), *contact = g_hash_table_lookup(params, "contact_id");
	gboolean has_contact = contact != NULL && !JSON_NODE_HOLDS_NULL(contact);
	gint64 contact_id = has_contact ? json_node_get_int(contact) : 0;
	const gchar *address = email != NULL && JSON_NODE_HOLDS_VALUE(email) ? json_node_get_string(email) : NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *text = NULL;
	VentureEntity *answer;

	if (has_contact && contact_id <= 0)
	{ venture_set_error_validation(error, "Person", "contact_id must be positive"); return NULL; }
	if (contact_id > 0 && !venture_string_is_empty(address))
	{ venture_set_error_validation(error, "Person", "choose email or contact_id, not both"); return NULL; }
	result = contact_id > 0 ? venture_forms_erase_contact(venture_action_get_data(action), venture_entity_get_organization_id(entity), contact_id, actor, error) :
		venture_forms_erase_person(venture_action_get_data(action), venture_entity_get_organization_id(entity), address, actor, error);
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
	JsonNode *email = g_hash_table_lookup(params, "email"), *contact = g_hash_table_lookup(params, "contact_id");
	gboolean has_contact = contact != NULL && !JSON_NODE_HOLDS_NULL(contact);
	gint64 contact_id = has_contact ? json_node_get_int(contact) : 0;
	const gchar *address = email != NULL && JSON_NODE_HOLDS_VALUE(email) ? json_node_get_string(email) : NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *text = NULL;
	VentureEntity *answer;

	(void)actor;
	if (has_contact && contact_id <= 0)
	{ venture_set_error_validation(error, "Person", "contact_id must be positive"); return NULL; }
	if (contact_id > 0 && !venture_string_is_empty(address))
	{ venture_set_error_validation(error, "Person", "choose email or contact_id, not both"); return NULL; }
	result = contact_id > 0 ? venture_forms_export_contact(venture_action_get_data(action), venture_entity_get_organization_id(entity), contact_id, error) :
		venture_forms_export_person(venture_action_get_data(action), venture_entity_get_organization_id(entity), address, error);
	if (NULL == result)
		return NULL;
	text = json_to_string(result, FALSE);
	answer = VENTURE_ENTITY(venture_form_new());
	venture_entity_set_organization_id(answer, venture_entity_get_organization_id(entity));
	g_object_set(answer, "name", "Export", "result", text, NULL);
	return answer;
}

/* Bulk generation returns capabilities only to an owner, never to a staged
 * card. Campaign/sequence services use the same link constructor privately. */
static VentureEntity *
forms_personal_links_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	JsonNode *ids = g_hash_table_lookup(params, "contact_ids");
	JsonNode *origin_node = g_hash_table_lookup(params, "origin");
	JsonNode *days_node = g_hash_table_lookup(params, "days");
	const gchar *origin = origin_node != NULL && JSON_NODE_HOLDS_VALUE(origin_node) &&
		json_node_get_value_type(origin_node) == G_TYPE_STRING ? json_node_get_string(origin_node) : NULL;
	gint64 days = days_node != NULL && JSON_NODE_HOLDS_VALUE(days_node) &&
		json_node_get_value_type(days_node) == G_TYPE_INT64 ? json_node_get_int(days_node) : 7;
	g_autoptr(GDateTime) now = venture_time_now(), expires = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *text = NULL;
	VentureEntity *answer;
	guint i;
	(void)actor;
	if (ids == NULL || !JSON_NODE_HOLDS_ARRAY(ids) || json_array_get_length(json_node_get_array(ids)) == 0 ||
	    json_array_get_length(json_node_get_array(ids)) > 200 || days < 1 || days > 30)
	{
		venture_set_error_validation(error, "Personal links", "name 1..200 contact IDs and 1..30 days");
		return NULL;
	}
	expires = g_date_time_add_days(now, (gint)days);
	json_builder_begin_array(builder);
	for (i = 0; i < json_array_get_length(json_node_get_array(ids)); i++)
	{
		JsonNode *id = json_array_get_element(json_node_get_array(ids), i);
		g_autoptr(VentureEntity) contact = NULL;
		g_autofree gchar *url = NULL;
		if (!JSON_NODE_HOLDS_VALUE(id) || json_node_get_value_type(id) != G_TYPE_INT64 || json_node_get_int(id) <= 0)
		{
			venture_set_error_validation(error, "Personal links", "contact IDs must be positive integers");
			return NULL;
		}
		contact = venture_database_get(database, VENTURE_TYPE_CONTACT, json_node_get_int(id), error);
		if (contact == NULL) return NULL;
		url = venture_forms_personal_link(database, entity, contact, origin, expires, now, error);
		if (url == NULL) return NULL;
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "contact_id"); json_builder_add_int_value(builder, venture_entity_get_id(contact));
		json_builder_set_member_name(builder, "url"); json_builder_add_string_value(builder, url);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder); result = json_builder_get_root(builder); text = json_to_string(result, FALSE);
	answer = VENTURE_ENTITY(venture_form_new());
	venture_entity_set_organization_id(answer, venture_entity_get_organization_id(entity));
	g_object_set(answer, "name", "Personal links", "result", text, NULL);
	return answer;
}

static VentureEntity *
forms_summary_invoke(VentureAction *action, VentureEntity *form, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	GWeakRef *reference = g_object_get_data(G_OBJECT(database), "venture-forms-payment-context");
	g_autoptr(VentureContext) context = reference != NULL ? g_weak_ref_get(reference) : NULL;
	g_autoptr(VentureEntity) version = NULL, result = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(JsonNode) summary = NULL;
	g_autofree gchar *text = NULL;
	JsonNode *first_node = g_hash_table_lookup(params, "first_version"), *last_node = g_hash_table_lookup(params, "last_version");
	JsonNode *period_node = g_hash_table_lookup(params, "period"), *question_node = g_hash_table_lookup(params, "question"), *limit_node = g_hash_table_lookup(params, "limit");
	gint64 first = first_node != NULL && !JSON_NODE_HOLDS_NULL(first_node) ? json_node_get_int(first_node) : 1;
	gint64 last = last_node != NULL && !JSON_NODE_HOLDS_NULL(last_node) ? json_node_get_int(last_node) : 0;
	gint64 limit = limit_node != NULL && !JSON_NODE_HOLDS_NULL(limit_node) ? json_node_get_int(limit_node) : 100;
	(void)actor;
	if (context == NULL) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Form context unavailable"); return NULL; }
	if (last == 0)
	{
		version = venture_database_get(database, VENTURE_TYPE_FORM_VERSION, venture_forms_get_int(form, "published-version-id"), error);
		if (version == NULL) return NULL;
		last = venture_forms_get_int(version, "number");
	}
	if (limit < 1 || limit > 200) { venture_set_error_validation(error, "Limit", "choose 1 through 200 responses"); return NULL; }
	period = venture_context_parse_period(context, period_node != NULL && !JSON_NODE_HOLDS_NULL(period_node) ? json_node_get_string(period_node) : "all", error);
	if (period == NULL) return NULL;
	summary = venture_forms_summarize(context, venture_entity_get_organization_id(form), venture_entity_get_id(form), first, last,
		question_node != NULL && !JSON_NODE_HOLDS_NULL(question_node) ? json_node_get_string(question_node) : NULL, period, (guint)limit, error);
	if (summary == NULL) return NULL;
	text = json_to_string(summary, FALSE);
	result = VENTURE_ENTITY(venture_form_new()); venture_entity_set_organization_id(result, venture_entity_get_organization_id(form));
	g_object_set(result, "name", "Proposed answer summary", "result", text, NULL);
	return g_steal_pointer(&result);
}

static VentureEntity *
forms_definition_invoke(VentureAction *action, VentureEntity *form, GHashTable *params,
	const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	GWeakRef *reference = g_object_get_data(G_OBJECT(database), "venture-forms-payment-context");
	g_autoptr(VentureContext) context = reference != NULL ? g_weak_ref_get(reference) : NULL;
	g_autoptr(JsonNode) definition = NULL;
	g_autofree gchar *text = NULL, *name = NULL;
	JsonNode *input = g_hash_table_lookup(params, "definition"), *chosen = g_hash_table_lookup(params, "template"), *format = g_hash_table_lookup(params, "format"), *bindings = g_hash_table_lookup(params, "bindings");
	VentureEntity *result;
	g_object_get(action, "name", &name, NULL);
	if (context == NULL) { venture_set_error_validation(error, "Form", "context unavailable"); return NULL; }
	if (g_str_equal(name, "import_definition"))
	{
		const gchar *source = input != NULL && !JSON_NODE_HOLDS_NULL(input) ? json_node_get_string(input) : NULL;
		const gchar *template_name = chosen != NULL && !JSON_NODE_HOLDS_NULL(chosen) ? json_node_get_string(chosen) : NULL;
		guint i;
		if (bindings != NULL && !JSON_NODE_HOLDS_NULL(bindings) && !JSON_NODE_HOLDS_OBJECT(bindings))
		{ venture_set_error_validation(error, "Bindings", "expected an object of destination IDs"); return NULL; }
		if (venture_string_is_empty(source) == venture_string_is_empty(template_name))
		{ venture_set_error_validation(error, "Definition", "provide either definition text or a template name"); return NULL; }
		if (!venture_string_is_empty(template_name))
		{
			definition = venture_forms_templates();
			for (i = 0; i < json_array_get_length(json_node_get_array(definition)); i++)
			{
				JsonObject *item = json_array_get_object_element(json_node_get_array(definition), i);
				if (g_strcmp0(template_name, json_object_get_string_member(item, "name")) == 0)
				{ text = json_to_string(json_object_get_member(item, "definition"), FALSE); break; }
			}
			if (text == NULL) { venture_set_error_validation(error, "Template", "unknown form template"); return NULL; }
			source = text;
		}
		return venture_forms_import_definition(context, venture_entity_get_organization_id(form), source,
			bindings != NULL && JSON_NODE_HOLDS_OBJECT(bindings) ? json_node_get_object(bindings) : NULL, actor, error);
	}
	if (g_str_equal(name, "templates")) definition = venture_forms_templates();
	else
	{
		g_autoptr(JsonNode) exported = venture_forms_export_definition(database, form, error);
		JsonObject *object;
		if (exported == NULL) return NULL;
		text = venture_forms_definition_format(exported, format != NULL && !JSON_NODE_HOLDS_NULL(format) ? json_node_get_string(format) : "json", error);
		if (text == NULL) return NULL;
		definition = json_node_new(JSON_NODE_OBJECT); object = json_object_new();
		json_object_set_string_member(object, "text", text); json_object_set_member(object, "definition", g_steal_pointer(&exported));
		json_node_take_object(definition, object); g_clear_pointer(&text, g_free);
	}
	text = json_to_string(definition, FALSE); result = VENTURE_ENTITY(venture_form_new());
	venture_entity_set_organization_id(result, venture_entity_get_organization_id(form));
	g_object_set(result, "name", "Form definition", "result", text, NULL); return result;
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
	g_ptr_array_add(parameters, venture_field_spec_new("contact_id", "Contact", VENTURE_FIELD_KIND_INTEGER));
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
	g_clear_object(&action);
	g_ptr_array_set_size(parameters, 0);
	g_ptr_array_add(parameters, venture_field_spec_new("contact_ids", "Contact IDs", VENTURE_FIELD_KIND_JSON));
	g_ptr_array_add(parameters, venture_field_spec_new("origin", "Public origin", VENTURE_FIELD_KIND_STRING));
	g_ptr_array_add(parameters, venture_field_spec_new("days", "Days", VENTURE_FIELD_KIND_INTEGER));
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "form", "name", "personal_links", "label", "Generate personal links",
		"description", "Generate private form links for 1..200 contacts, expiring after 1..30 days (default 7)",
		"parameters", parameters, "stageable", FALSE, "service-transaction", TRUE,
		"roles", VENTURE_USER_ROLE_OWNER, NULL);
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
		forms_publish_allowed, forms_personal_links_invoke, database, NULL, &error))
		g_error("Form personal link action registration: %s", error->message);

	g_clear_object(&action);
	g_ptr_array_set_size(parameters, 0);
	g_ptr_array_add(parameters, venture_field_spec_new("invoice_id", "Invoice", VENTURE_FIELD_KIND_INTEGER));
	g_ptr_array_add(parameters, venture_field_spec_new("booking_start", "Replacement booking start", VENTURE_FIELD_KIND_STRING));
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "form", "name", "reconcile_payment", "label", "Recover paid response",
		"description", "Complete a retained invoice payment; optionally select an available replacement slot for an expired paid booking hold",
		"parameters", parameters, "stageable", FALSE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_OWNER, NULL);
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
		forms_publish_allowed, forms_payment_recover_invoke, database, NULL, &error))
		g_error("Form payment recovery registration: %s", error->message);

	g_clear_object(&action); g_ptr_array_set_size(parameters, 0);
	g_ptr_array_add(parameters, venture_field_spec_new("first_version", "First version", VENTURE_FIELD_KIND_INTEGER));
	g_ptr_array_add(parameters, venture_field_spec_new("last_version", "Last version", VENTURE_FIELD_KIND_INTEGER));
	g_ptr_array_add(parameters, venture_field_spec_new("period", "Period", VENTURE_FIELD_KIND_STRING));
	g_ptr_array_add(parameters, venture_field_spec_new("question", "Question key", VENTURE_FIELD_KIND_STRING));
	g_ptr_array_add(parameters, venture_field_spec_new("limit", "Response limit", VENTURE_FIELD_KIND_INTEGER));
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "form", "name", "summarize", "label", "Summarize free-text answers",
		"description", "Propose themes and verified quotes from bounded nonsensitive answers; no response is changed",
		"parameters", parameters, "stageable", FALSE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
		forms_publish_allowed, forms_summary_invoke, database, NULL, &error))
		g_error("Form summary registration: %s", error->message);

	g_clear_object(&action); g_ptr_array_set_size(parameters, 0);
	g_ptr_array_add(parameters, venture_field_spec_new("format", "Format", VENTURE_FIELD_KIND_STRING));
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "form", "name", "export_definition", "label", "Export form definition",
		"description", "Export editable structure as portable JSON or YAML without responses or tokens",
		"parameters", parameters, "stageable", FALSE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
		forms_publish_allowed, forms_definition_invoke, database, NULL, &error)) g_error("Form export registration: %s", error->message);
	g_clear_object(&action); g_ptr_array_set_size(parameters, 0);
	g_ptr_array_add(parameters, venture_field_spec_new("organization_id", "Organization", VENTURE_FIELD_KIND_INTEGER));
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "form", "name", "templates", "label", "List form templates",
		"description", "Show the built-in portable form templates",
		"parameters", parameters, "stageable", FALSE, "type-level", TRUE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
		forms_publish_allowed_type, forms_definition_invoke, database, NULL, &error)) g_error("Form templates registration: %s", error->message);
	g_clear_object(&action);
	g_ptr_array_add(parameters, venture_field_spec_new("definition", "JSON or YAML definition", VENTURE_FIELD_KIND_TEXT));
	g_ptr_array_add(parameters, venture_field_spec_new("template", "Template name", VENTURE_FIELD_KIND_STRING));
	g_ptr_array_add(parameters, venture_field_spec_new("bindings", "Destination reference bindings", VENTURE_FIELD_KIND_JSON));
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "form", "name", "import_definition", "label", "Import form definition",
		"description", "Create a draft from portable JSON/YAML or a built-in template; map external record references explicitly",
		"parameters", parameters, "stageable", TRUE, "type-level", TRUE, "service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
		forms_publish_allowed_type, forms_definition_invoke, database, NULL, &error)) g_error("Form import registration: %s", error->message);

}

void
venture_forms_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);
	{
		GWeakRef *reference = g_new0(GWeakRef, 1);
		g_weak_ref_init(reference, context);
		g_object_set_data_full(G_OBJECT(database), "venture-forms-payment-context", reference, forms_payment_context_free);
	}


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
	venture_database_add_save_validator(database, VENTURE_TYPE_FORM_PAYMENT, forms_payment_validate, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_FORM_PRICE, venture_forms_price_validate, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_FORM_RESULT_BAND, venture_forms_quiz_validate_band, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_FORM_DRAFT_RECORD,
	                                    forms_validate_draft, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_FORM_RULE,
	                                    venture_forms_validate_rule, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_FORM_TRANSLATION,
	                                    venture_forms_validate_translation, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_FORM_PENDING,
	                                    forms_validate_pending, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_FORM_GROUP,
	                                    venture_forms_validate_group, NULL, NULL);
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
	venture_query_set_include_deleted(query, TRUE);
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
	g_autoptr(GDateTime) opens = NULL;
	VentureFormState state = VENTURE_FORM_DRAFT;
	gint64 limit = venture_forms_get_int(form, "response-limit");

	if (venture_entity_is_deleted(form))
		return FALSE;
	g_object_get(form, "state", &state, "opens-at", &opens, "closes-at", &closes, NULL);
	if (VENTURE_FORM_LIVE != state)
		return FALSE;
	/* Live and never published has nothing to show a stranger. */
	if (venture_forms_get_int(form, "published-version-id") <= 0)
		return FALSE;
	if (NULL != opens && g_date_time_compare(now, opens) < 0)
		return FALSE;
	if (NULL != closes && g_date_time_compare(now, closes) >= 0)
		return FALSE;
	if (limit > 0)
	{
		gint64 count = forms_response_count(database, form);
		/* A failed count is not evidence that a place remains. */
		if (count < 0 || count >= limit) return FALSE;
	}
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
	g_autoptr(JsonObject) flat = NULL;
	JsonObjectIter iter;
	const gchar *name;
	JsonNode *node;

	g_return_val_if_fail(object != NULL, NULL);

	flat = venture_forms_flatten_json_answers(object, error);
	if (flat == NULL) return NULL;
	object = flat;

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

/* Working-copy state may retain these two authenticated capabilities. The
 * ordinary answer serializer deliberately excludes all internal controls. */
JsonObject *
venture_forms_answers_state(GHashTable *answers)
{
	JsonObject *values = venture_forms_answers_to_json(answers);
	const gchar *keys[] = { VENTURE_FORMS_PERSONAL, VENTURE_FORMS_PREFILL, VENTURE_FORMS_PAYMENT_NONCE };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(keys); i++)
	{
		const gchar *value = forms_first(answers, keys[i]);
		if (value != NULL) json_object_set_string_member(values, keys[i], value);
	}
	return values;
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
forms_check_field(VentureDatabase *database, VentureEntity *form, GDateTime *now, const VentureFormsField *field, GPtrArray *values, JsonObject *answers,
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

	if (field->group_boundary != VENTURE_FORMS_GROUP_NONE)
	{
		if (field->group_boundary == VENTURE_FORMS_GROUP_ROW_START)
		{
			if (values != NULL && (values->len != 1 || g_strcmp0(g_ptr_array_index(values, 0), "1") != 0))
				forms_refuse(errors, field->group_key, "This row marker is not valid.");
		}
		else if (values != NULL && values->len > 0)
			forms_refuse(errors, field->group_key, "A group heading does not accept an answer.");
		if (field->group_boundary == VENTURE_FORMS_GROUP_START)
			g_string_append_printf(summary, "%s: %u rows\n", field->group_label, field->row_count);
		return;
	}
	if (kind == VENTURE_FORM_FIELD_PAGE_BREAK)
	{
		if (values != NULL && values->len > 0) forms_refuse(errors, key, "A page heading does not accept an answer.");
		return;
	}
	if (kind == VENTURE_FORM_FIELD_FILE)
	{
		venture_forms_upload_check(database, form, now, field, values, answers, errors, summary); return;
	}
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
			g_string_append_printf(summary, "%s: %s\n", label, venture_forms_field_text(field, "message.given", "Given"));
			return;
		}
		json_object_set_boolean_member(answers, key, ticked);
		g_string_append_printf(summary, "%s: %s\n", label, venture_forms_field_text(field, ticked ? "message.yes" : "message.no", ticked ? "Yes" : "No"));
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

		forms_refuse(errors, key, venture_forms_field_text(field, "message.max_length", message));
		return;
	}
	if (min_length > 0 && length < min_length)
	{
		g_autofree gchar *message = g_strdup_printf("Use at least %" G_GINT64_FORMAT " characters.", min_length);

		forms_refuse(errors, key, venture_forms_field_text(field, "message.min_length", message));
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
	case VENTURE_FORM_FIELD_BOOKING:
		{
			g_autoptr(GDateTime) when = g_date_time_new_from_iso8601(text, NULL);
			if (when == NULL) { forms_refuse(errors, key, "Choose an offered booking time."); return; }
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

				forms_refuse(errors, key, venture_forms_field_text(field, "message.minimum", message));
				return;
			}
			if (max != 0 && number > max)
			{
				g_autofree gchar *message = g_strdup_printf("Enter %g or less.", max);

				forms_refuse(errors, key, venture_forms_field_text(field, "message.maximum", message));
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

				forms_refuse(errors, key, venture_forms_field_text(field, "message.rating", message));
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

/* Piping sees validated public data, even while a page is redisplaying an
 * invalid submission. Neither sensitive nor branch-hidden answers enter it. */
JsonObject *
venture_forms_pipe_values(GPtrArray *fields, JsonObject *raw)
{
	g_autoptr(JsonObject) visible = json_object_new(), checked = json_object_new(), errors = json_object_new();
	g_autoptr(GHashTable) hidden = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) answers = NULL;
	g_autoptr(GString) summary = g_string_new(NULL);
	JsonObject *values = json_object_new();
	guint i;
	if (raw != NULL)
	{
		JsonObjectIter iter;
		const gchar *key;
		JsonNode *node;
		json_object_iter_init(&iter, raw);
		while (json_object_iter_next(&iter, &key, &node)) json_object_set_member(visible, key, json_node_copy(node));
	}
	venture_forms_rules_filter(fields, visible, hidden);
	answers = venture_forms_answers_from_json(visible, NULL);
	if (answers == NULL) return values;
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *source = g_ptr_array_index(fields, i);
		VentureFormsField field = *source;
		g_autofree gchar *text = NULL;
		if (field.sensitive || field.group_boundary != VENTURE_FORMS_GROUP_NONE ||
		    field.kind == VENTURE_FORM_FIELD_PAGE_BREAK || field.kind == VENTURE_FORM_FIELD_FILE || g_hash_table_contains(hidden, field.key)) continue;
		if (!venture_forms_field_active(source, visible, &field.required)) continue;
		forms_check_field(NULL, NULL, NULL, &field, g_hash_table_lookup(answers, field.key), checked, errors, summary);
		if (json_object_has_member(errors, field.key)) continue;
		text = venture_forms_pipe_answer(&field, json_object_get_member(checked, field.key));
		json_object_set_string_member(values, field.key, text);
	}
	venture_forms_pipe_counts(fields, visible, values);
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);
		if (field->group_boundary == VENTURE_FORMS_GROUP_START && g_hash_table_contains(hidden, field->key))
		{
			g_autofree gchar *key = g_strdup_printf("%s.count", field->group_key);
			json_object_remove_member(values, key);
		}
	}
	return values;
}

/* Prefill is untrusted input, even when it originated from a contact.
 * Invalid values disappear rather than bypassing the ordinary validator. */
JsonObject *
venture_forms_prefill_values(VentureDatabase *database, VentureEntity *form,
	VentureEntity *version, GHashTable *query, const gchar *personal, GDateTime *now, GError **error)
{
	g_autoptr(GPtrArray) fields = venture_forms_definition_for(database, form, version, error);
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(JsonObject) result = json_object_new();
	guint i;
	if (fields == NULL) return NULL;
	if (personal != NULL)
	{
		contact = venture_forms_personal_contact(database, form, personal, now, error);
		if (contact == NULL) return NULL;
		json_object_set_string_member(result, VENTURE_FORMS_PERSONAL, personal);
	}
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		g_autofree gchar *mapped = NULL;
		const gchar *value = NULL;
		g_autoptr(GPtrArray) raw = g_ptr_array_new();
		g_autoptr(JsonObject) checked = json_object_new(), refused = json_object_new();
		g_autoptr(GString) summary = g_string_new(NULL);
		JsonNode *node;
		if (field->sensitive || field->kind == VENTURE_FORM_FIELD_CONSENT ||
		    field->kind == VENTURE_FORM_FIELD_PAGE_BREAK || field->kind == VENTURE_FORM_FIELD_FILE || field->group_key != NULL) continue;
		if (field->allow_prefill && query != NULL) value = g_hash_table_lookup(query, field->key);
		if (contact != NULL && !venture_string_is_empty(field->contact_field))
		{
			const gchar *allowed[] = { "name", "email", "phone", "role", "website", "address", NULL };
			if (!g_strv_contains(allowed, field->contact_field)) continue;
			mapped = venture_forms_get_string(contact, field->contact_field);
			value = mapped;
		}
		if (value == NULL || strlen(value) > 32768) continue;
		g_ptr_array_add(raw, (gpointer)value);
		forms_check_field(database, form, now, field, raw, checked, refused, summary);
		node = json_object_get_member(checked, field->key);
		if (node != NULL && json_object_get_size(refused) == 0)
		{
			if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) != G_TYPE_STRING)
			{
				g_autofree gchar *text = json_to_string(node, FALSE);
				json_object_set_string_member(result, field->key, text);
			}
			else json_object_set_member(result, field->key, json_node_copy(node));
		}
	}
	return g_steal_pointer(&result);
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
	g_autofree gchar *subject = NULL, *body = NULL, *success = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(JsonObject) values = NULL;
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
	fields = venture_forms_record_definition(database, form, submission, error);
	if (fields == NULL) return FALSE;
	values = venture_forms_pipe_stored_values(fields, answers);
	subject = venture_forms_pipe_text(venture_forms_text(fields, "form.confirmation_subject",
		venture_forms_text(fields, "message.mail_subject", "We received your response")), fields, NULL, values);
	g_strdelimit(subject, "\r\n", ' ');
	body = venture_forms_pipe_text(venture_forms_text(fields, "form.confirmation_message", ""), fields, NULL, values);
	success = venture_forms_success_message(database, form, submission);
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

/* Opting into an intake/privacy statement is not permission to market.
 * Only the published question's explicit mapping can grant that permission;
 * the shared service retains suppression and normalized-address rules. */
static gboolean
forms_record_marketing_consent(VentureDatabase *database, VentureEntity *form,
	VentureEntity *submission, VentureEntity *lead, GPtrArray *fields,
	JsonObject *answers, GDateTime *now, GError **error)
{
	guint i;

	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);
		g_autoptr(VentureMarketingConsent) consent = NULL;
		g_autofree gchar *key = NULL;
		g_autofree gchar *source = NULL;
		g_autofree gchar *evidence = NULL;
		JsonNode *answer;

		if (!field->marketing_consent || field->sensitive ||
		    field->kind != VENTURE_FORM_FIELD_CONSENT ||
		    !json_object_has_member(answers, field->key))
			continue;
		answer = json_object_get_member(answers, field->key);
		if (!JSON_NODE_HOLDS_OBJECT(answer) ||
		    !json_object_get_boolean_member_with_default(json_node_get_object(answer), "given", FALSE))
			continue;
		key = g_strdup_printf("form:%s:%s", venture_entity_get_uuid(submission), field->key);
		source = g_strdup_printf("Form #%" G_GINT64_FORMAT " version %" G_GINT64_FORMAT,
			venture_entity_get_id(form), venture_forms_get_int(submission, "version-number"));
		evidence = g_strdup_printf("%s; question %s\n%s%s%s", source, field->key,
			field->label, venture_string_is_empty(field->help) ? "" : "\n",
			field->help != NULL ? field->help : "");
		consent = venture_marketing_service_consent(venture_marketing_service_get(database),
			venture_entity_get_organization_id(form), lead, source, evidence, key, now, NULL, error);
		if (NULL == consent)
			return FALSE;
	}
	return TRUE;
}

/* Reuse stored answers instead of keeping a second identity after erasure.
 * The caller holds the save lock. Read in bounded batches so enabling the
 * limit also covers responses collected before it was configured. */
static gchar *
forms_response_email(VentureEntity *response, const gchar *key)
{
	const gchar *columns[] = { "answers", "sensitive-answers" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(columns); i++)
	{
		g_autofree gchar *text = venture_forms_get_string(response, columns[i]);
		g_autoptr(JsonParser) parser = json_parser_new();
		JsonNode *root, *value;
		if (venture_string_is_empty(text) || !json_parser_load_from_data(parser, text, -1, NULL))
			continue;
		root = json_parser_get_root(parser);
		if (!JSON_NODE_HOLDS_OBJECT(root)) continue;
		value = json_object_get_member(json_node_get_object(root), key);
		if (value != NULL && JSON_NODE_HOLDS_VALUE(value) && json_node_get_value_type(value) == G_TYPE_STRING)
		{
			g_autofree gchar *address = g_strdup(json_node_get_string(value));
			return g_ascii_strdown(g_strstrip(address), -1);
		}
	}
	return NULL;
}

static gboolean
forms_check_email_limit(VentureDatabase *database, VentureEntity *form,
	VentureEntity *submission, GPtrArray *fields, JsonObject *refused, GError **error)
{
	g_autofree gchar *key = venture_forms_get_string(form, "unique-email-field");
	g_autofree gchar *email = NULL;
	const VentureFormsField *field;
	gint64 after = 0;
	if (venture_string_is_empty(key)) return TRUE;
	field = venture_forms_definition_find(fields, key);
	email = forms_response_email(submission, key);
	if (field == NULL || field->kind != VENTURE_FORM_FIELD_EMAIL || venture_string_is_empty(email))
	{
		forms_refuse(refused, key, "An email address is required for this form.");
		return FALSE;
	}
	for (;;)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
		g_autoptr(GPtrArray) rows = NULL;
		guint i;
		venture_query_set_organization(query, venture_entity_get_organization_id(form));
		venture_query_set_include_deleted(query, TRUE);
		venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
		venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_GT, after, NULL);
		venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
		venture_query_set_limit(query, 100);
		rows = venture_database_find(database, query, error);
		if (rows == NULL) return FALSE;
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(rows, i);
			g_autofree gchar *previous = forms_response_email(row, key);
			after = venture_entity_get_id(row);
			if (g_strcmp0(email, previous) == 0)
			{
				forms_refuse(refused, key, "A response for this email address has already been received.");
				return FALSE;
			}
		}
		if (rows->len < 100) return TRUE;
	}
}

/* One attempt at the whole write: the lead, the confirmation and the
 * response, in one transaction. */
#include "venture-forms-receipts.inc"

typedef struct {
	VentureFormsRelayCheck check;
	gpointer data;
	gboolean failed;
} FormsRelayGuard;

static gboolean
forms_relay_check(VentureEntity *form, GError **error)
{
	FormsRelayGuard *guard = g_object_get_data(G_OBJECT(form), "venture-forms-relay-guard");
	if (guard == NULL || guard->check == NULL) return TRUE;
	if (guard->check(guard->data, error)) return TRUE;
	guard->failed = TRUE;
	return FALSE;
}

static gboolean
forms_write(VentureDatabase *database, VentureEntity *form, VentureEntity **submission_out,
	GPtrArray *fields, JsonObject *answers, gboolean follow_up, GDateTime *now, JsonObject *refused, GError **error)
{
	g_autofree gchar *note = NULL;
	VentureEntity *submission = *submission_out;
	g_autoptr(VentureEntity) booking_hold = NULL;
	g_autoptr(VentureEntity) receipt = NULL;
	g_autoptr(GPtrArray) uploads = NULL;

	if (!venture_database_begin(database, error))
		return FALSE;

	if (!forms_relay_check(form, error)) goto fail;

	/* Recheck under the write lock before any follow-up can run. */
	{
		const gchar *nonce = g_object_get_data(G_OBJECT(submission), FORMS_RECEIPT_NONCE);
		if (nonce != NULL)
		{
			g_autoptr(GHashTable) identity = venture_forms_answers_new();
			g_autoptr(VentureEntity) prior = NULL;
			gint replay;
			venture_forms_answers_add(identity, VENTURE_FORMS_PAYMENT_NONCE, nonce);
			replay = venture_forms_receipt_replay(database, form, identity,
				g_object_get_data(G_OBJECT(submission), FORMS_RECEIPT_DIGEST), FALSE, &prior, error);
			if (replay < 0) goto fail;
			if (replay > 0)
			{
				g_object_unref(*submission_out); *submission_out = g_steal_pointer(&prior);
				return venture_database_commit(database, error);
			}
		}
	}

	/* Under the lock, re-read: the cap and the state are what they are
	 * now, so two people posting the last place at once get one response
	 * between them, not two, and a form closed a moment ago is closed. */
	{
		g_autoptr(VentureEntity) fresh = venture_database_get(database, VENTURE_TYPE_FORM,
			venture_entity_get_id(form), NULL);

		if (NULL == fresh || (g_object_get_data(G_OBJECT(form), VENTURE_FORMS_PAYMENT_SETTLING) == NULL && !forms_is_open(database, fresh, now)))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
			goto fail;
		}
		if (g_object_get_data(G_OBJECT(form), VENTURE_FORMS_PAYMENT_SETTLING) == NULL &&
		    (!venture_forms_personal_bind(database, fresh, submission, now, refused, error) ||
		     !forms_check_email_limit(database, fresh, submission, fields, refused, error) ||
		     !forms_payment_limits(database, fresh, submission, refused, error))) goto fail;
	}

	if (!forms_receipt_reserve(database, form, submission, &receipt, error)) goto fail;

	if (g_object_get_data(G_OBJECT(form), VENTURE_FORMS_PAYMENT_SETTLING) != NULL)
	{
		VentureEntity *pending = g_object_get_data(G_OBJECT(form), VENTURE_FORMS_PAYMENT_SETTLING);
		gint64 hold_id = venture_forms_get_int(pending, "reservation-id");
		if (hold_id > 0)
		{
			booking_hold = venture_database_get(database, VENTURE_TYPE_BOOKING_RESERVATION, hold_id, error);
			if (booking_hold == NULL) goto fail;
		}
	}
	else if (!venture_forms_booking_prepare(database, form, fields, answers, now, &booking_hold, refused, error)) goto fail;
	if (g_object_get_data(G_OBJECT(form), VENTURE_FORMS_CONFIRMING) != NULL)
	{
		if (!forms_optin_follow_up(database, form, submission,
			g_object_get_data(G_OBJECT(form), VENTURE_FORMS_CONFIRMING), fields, answers, now, error)) goto fail;
	}
	else if (follow_up && venture_forms_get_bool(form, "create-lead"))
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
		if (venture_forms_get_bool(submission, "scored") && venture_forms_quiz(fields) != NULL &&
		    json_object_get_boolean_member_with_default(venture_forms_quiz(fields), "lead_input", FALSE))
			lead = venture_lead_service_capture_scored_values(venture_database_get_lead_service(database),
				venture_entity_get_organization_id(form), venture_forms_get_int(form, "venture-id"), source,
				venture_forms_get_int(form, "campaign-id"), values, policy, venture_forms_get_int(submission, "score"), error);
		else
			lead = venture_lead_service_capture_values(venture_database_get_lead_service(database),
			venture_entity_get_organization_id(form), venture_forms_get_int(form, "venture-id"), source,
			venture_forms_get_int(form, "campaign-id"), values, policy, error);
		if (NULL == lead)
			goto fail;
		/* Duplicate capture may return a contact or company. Its numeric ID
		 * is not a lead reference and must not bind an unrelated lead. */
		if (VENTURE_IS_LEAD(lead))
			g_object_set(submission, "lead-id", venture_entity_get_id(lead), NULL);
		else
			g_object_set(submission, "mapping-note",
				"Capture matched an existing CRM record; no new lead was created.", NULL);
		if (!forms_record_marketing_consent(database, form, submission, lead, fields, answers, now, error))
			goto fail;
	}
	if (follow_up && !forms_queue_confirmation(database, form, submission, answers, &note, error))
		goto fail;
	if (NULL != note)
		g_object_set(submission, "mapping-note", note, NULL);

	if (!venture_forms_booking_finish(database, form, fields, answers, booking_hold, submission, now, refused, error)) goto fail;

	if (!venture_forms_upload_claim(database, form, submission, fields, now, &uploads, error)) goto fail;
	if (!venture_database_save(database, submission, NULL, error)) goto fail;
	if (!venture_forms_upload_bind_response(database, submission, uploads, error)) goto fail;
	if (receipt != NULL)
	{
		g_object_set(receipt, "response-id", venture_entity_get_id(submission), NULL);
		if (!venture_database_save(database, receipt, NULL, error)) goto fail;
	}
	if (!forms_relay_check(form, error)) goto fail;
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
	g_autoptr(GPtrArray) fields = NULL, base_fields = NULL;
	g_autoptr(JsonObject) stored = json_object_new();
	g_autoptr(JsonObject) refused = json_object_new();
	g_autoptr(JsonObject) secret = json_object_new();
	g_autoptr(JsonObject) visible = NULL;
	g_autoptr(GHashTable) filtered = NULL;
	g_autoptr(GHashTable) not_shown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(JsonArray) price_lines = NULL;
	g_autoptr(VentureMoney) price_total = NULL;
	g_autofree gchar *payment_nonce = g_strdup(forms_first(answers, VENTURE_FORMS_PAYMENT_NONCE));
	g_autofree gchar *receipt_digest = NULL;
	g_autofree gchar *secret_text = NULL;
	g_autofree gchar *omitted_text = NULL;
	g_autofree gchar *personal = g_strdup(forms_first(answers, VENTURE_FORMS_PERSONAL));
	g_autofree gchar *language = NULL;
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

	if (payment_nonce != NULL && venture_forms_payment_nonce_valid(form, payment_nonce))
	{
		gint replay;
		receipt_digest = venture_forms_request_digest(form, answers, NULL);
		replay = venture_forms_receipt_replay(database, form, answers, receipt_digest, FALSE, submission, error);
		if (replay < 0) return FALSE;
		if (replay > 0) { *outcome = VENTURE_FORMS_ACCEPTED; return TRUE; }
	}

	version = venture_forms_version_for_answers(database, form, answers);
	if (NULL == version)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
		return FALSE;
	}
	if (personal != NULL)
	{
		g_autoptr(VentureEntity) contact = venture_forms_personal_contact(database, form, personal, now, error);
		if (contact == NULL) return FALSE;
	}
	if (g_hash_table_contains(answers, VENTURE_FORMS_PREFILL))
	{
		g_autoptr(JsonObject) seed = venture_forms_prefill_unpack(form, version,
			forms_first(answers, VENTURE_FORMS_PREFILL), error);
		if (seed == NULL) return FALSE;
		language = g_strdup(json_object_get_string_member_with_default(seed, VENTURE_FORMS_LANGUAGE, NULL));
	}

	base_fields = venture_forms_definition_for(database, form, version, error);
	if (NULL == base_fields)
		return FALSE;
	venture_forms_localize(base_fields, language);
	visible = venture_forms_answers_state(answers);
	fields = venture_forms_expand_groups(base_fields, visible, FALSE, refused);

	/* A name that is no question on this version is refused, not
	 * dropped: the sender believes it was received. The door's own
	 * names pass. */
	g_hash_table_iter_init(&iter, answers);
	while (g_hash_table_iter_next(&iter, &key, NULL))
	{
		if (0 == g_strcmp0(key, VENTURE_FORMS_HONEYPOT) || 0 == g_strcmp0(key, VENTURE_FORMS_TICKET) ||
		    0 == g_strcmp0(key, VENTURE_FORMS_PERSONAL) || 0 == g_strcmp0(key, VENTURE_FORMS_PREFILL) ||
		    0 == g_strcmp0(key, VENTURE_FORMS_PAYMENT_NONCE))
			continue;
		if (NULL == venture_forms_definition_find(fields, key))
			forms_refuse(refused, key, "This form has no such question.");
	}

	venture_forms_rules_filter(fields, visible, not_shown);
	venture_forms_groups_validate(fields, not_shown, refused);
	filtered = venture_forms_answers_from_json(visible, error);
	if (filtered == NULL) return FALSE;
	answers = filtered;
	{
		g_autoptr(JsonObject) pipe_values = venture_forms_pipe_values(fields, visible);
		venture_forms_piping_apply(fields, pipe_values);
	}

	/* A sensitive answer is checked like any other, then kept apart: in
	 * a column that never leaves the record, with only its question named
	 * in the summary that search, webhooks and the assistant read. */
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *source = g_ptr_array_index(fields, i);
		VentureFormsField effective = *source;
		const VentureFormsField *field = &effective;
		if (g_hash_table_contains(not_shown, field->key)) continue;
		venture_forms_field_active(source, visible, &effective.required);

		if (field->sensitive)
		{
			g_autoptr(GString) hidden = g_string_new(NULL);

			forms_check_field(database, form, now, field, g_hash_table_lookup(answers, field->key), secret, refused, hidden);
			if (hidden->len > 0)
				g_string_append_printf(summary, "%s: (sensitive)\n", field->label);
			continue;
		}
		forms_check_field(database, form, now, field, g_hash_table_lookup(answers, field->key), stored, refused, summary);
	}

	venture_forms_translation_errors(fields, refused);
	if (json_object_get_size(refused) > 0)
	{
		if (NULL != errors)
			*errors = g_steal_pointer(&refused);
		return TRUE;
	}

	if (venture_forms_get_bool(form, "double-opt-in") &&
	    g_object_get_data(G_OBJECT(form), VENTURE_FORMS_CONFIRMING) == NULL)
	{
		if (!forms_optin_store(database, form, version, answers, fields, stored, origin, now, refused, error)) return FALSE;
		venture_forms_translation_errors(fields, refused);
		if (json_object_get_size(refused) > 0)
		{
			if (errors != NULL) *errors = g_steal_pointer(&refused);
		}
		else *outcome = VENTURE_FORMS_PENDING;
		return TRUE;
	}

	name = forms_response_name(form, fields, stored);
	response = forms_new_response(form, version, name, now, "{}", "", origin);
	if (!venture_forms_quiz_apply(fields, stored, response, error)) return FALSE;
	if (venture_forms_payment(fields) != NULL && json_object_get_boolean_member(venture_forms_payment(fields), "enabled"))
	{
		g_autoptr(GError) price_error = NULL;
		price_total = venture_forms_price_total(fields, stored, &price_lines, &price_error);
		if (price_total == NULL)
		{
			forms_refuse(refused, "_form", "Choose a positive order with whole quantities within the allowed range.");
			venture_forms_translation_errors(fields, refused);
			if (errors != NULL) *errors = g_steal_pointer(&refused);
			return TRUE;
		}
	}

	venture_forms_groups_fold(fields, stored, not_shown, FALSE);
	venture_forms_groups_fold(fields, secret, not_shown, TRUE);
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

	g_object_set(response, "answers", answers_text, "summary", summary->str, NULL);
	g_object_set(response, "language", venture_forms_language(fields), NULL);
	g_object_set_data_full(G_OBJECT(response), VENTURE_FORMS_PERSONAL_WRITE, g_strdup(personal), g_free);
	g_object_set(response, "sensitive-answers", secret_text, NULL);
	{
		g_autoptr(JsonObject) omitted = json_object_new();
		g_autoptr(JsonNode) omitted_node = json_node_new(JSON_NODE_OBJECT);
		g_hash_table_iter_init(&iter, not_shown);
		while (g_hash_table_iter_next(&iter, &key, NULL)) json_object_set_boolean_member(omitted, key, TRUE);
		json_node_set_object(omitted_node, omitted);
		omitted_text = json_to_string(omitted_node, FALSE);
		g_object_set(response, "not-shown", omitted_text, NULL);
	}
	if (price_total != NULL)
	{
		if (!forms_payment_begin(database, form, version, response, fields, stored, payment_nonce,
		    price_lines, price_total, now, refused, error))
		{
			venture_forms_translation_errors(fields, refused);
			if (json_object_get_size(refused) > 0)
			{
				if (errors != NULL) *errors = g_steal_pointer(&refused);
				return TRUE;
			}
			return FALSE;
		}
		*outcome = VENTURE_FORMS_PAYMENT;
		if (submission != NULL) *submission = g_steal_pointer(&response);
		return TRUE;
	}
	if (payment_nonce != NULL && !venture_forms_payment_nonce_valid(form, payment_nonce))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Form not found");
		return FALSE;
	}
	g_object_set_data_full(G_OBJECT(response), FORMS_RECEIPT_NONCE, g_strdup(payment_nonce), g_free);
	g_object_set_data_full(G_OBJECT(response), FORMS_RECEIPT_DIGEST, g_strdup(receipt_digest), g_free);
	if (!forms_write(database, form, &response, fields, stored, TRUE, now, refused, &follow_error))
	{
		g_autofree gchar *note = NULL;
		FormsRelayGuard *guard = g_object_get_data(G_OBJECT(form), "venture-forms-relay-guard");

		/* An authority failure is never an optional follow-up failure. */
		if (guard != NULL && guard->failed)
		{
			g_propagate_error(error, g_steal_pointer(&follow_error));
			return FALSE;
		}
		venture_forms_translation_errors(fields, refused);
		if (json_object_get_size(refused) > 0)
		{
			if (errors != NULL) *errors = g_steal_pointer(&refused);
			return TRUE;
		}
		if (g_error_matches(follow_error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT) ||
		    g_object_get_data(G_OBJECT(form), VENTURE_FORMS_CONFIRMING) != NULL)
		{
			g_propagate_error(error, g_steal_pointer(&follow_error));
			return FALSE;
		}
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
		{
			g_autoptr(VentureEntity) graded = g_steal_pointer(&response);
			g_autofree gchar *result_key = venture_forms_get_string(graded, "result-key");
			g_autofree gchar *result_message = venture_forms_get_string(graded, "result-message");
			g_autofree gchar *result_url = venture_forms_get_string(graded, "result-url");
			/* Retry storage, not grading: repeated answers are folded now,
			 * and the result already describes the validated published input. */
			response = forms_new_response(form, version, name, now, answers_text, summary->str, origin);
			g_object_set(response, "scored", venture_forms_get_bool(graded, "scored"),
				"score", venture_forms_get_int(graded, "score"), "result-key", result_key,
				"result-message", result_message, "result-url", result_url, NULL);
		}
		g_object_set(response, "language", venture_forms_language(fields), NULL);
		g_object_set_data_full(G_OBJECT(response), VENTURE_FORMS_PERSONAL_WRITE, g_strdup(personal), g_free);
		g_object_set(response, "mapping-note", note, "sensitive-answers", secret_text, "not-shown", omitted_text, NULL);
		g_object_set_data_full(G_OBJECT(response), FORMS_RECEIPT_NONCE, g_strdup(payment_nonce), g_free);
		g_object_set_data_full(G_OBJECT(response), FORMS_RECEIPT_DIGEST, g_strdup(receipt_digest), g_free);
		if (!forms_write(database, form, &response, fields, stored, FALSE, now, refused, error))
		{
			venture_forms_translation_errors(fields, refused);
			if (json_object_get_size(refused) > 0)
			{
				if (errors != NULL) *errors = g_steal_pointer(&refused);
				return TRUE;
			}
			return FALSE;
		}
	}

	*outcome = VENTURE_FORMS_ACCEPTED;
	if (NULL != submission)
		*submission = g_steal_pointer(&response);
	return TRUE;
}

/* Private intermediate-state service; shares field validation and final intake. */
#include "venture-forms-relay.inc"
#include "venture-forms-pages.inc"
#include "venture-forms-resume.inc"
#include "venture-forms-optin.inc"
#include "venture-forms-payments.inc"

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
	{
		g_autofree gchar *success = venture_forms_get_string(stored, "success-message");
		if (!venture_forms_piping_definition(fields, success, error)) goto fail;
	}
	if (!forms_optin_definition(database, stored, fields, error)) goto fail;
	if (0 == fields->len)
	{
		venture_set_error_validation(error, "Questions",
			"add at least one question before publishing");
		goto fail;
	}
	{
		guint i;
		gboolean empty = TRUE;
		if (venture_forms_page_count(fields) > 32)
		{
			venture_set_error_validation(error, "Pages", "at most 32 pages are supported");
			goto fail;
		}
		for (i = 0; i < fields->len; i++)
		{
			VentureFormsField *field = g_ptr_array_index(fields, i);
			if (field->kind == VENTURE_FORM_FIELD_PAGE_BREAK)
			{
				if (empty)
				{
					venture_set_error_validation(error, "Pages", "every page needs a question");
					goto fail;
				}
				empty = TRUE;
			}
			else empty = FALSE;
		}
		if (empty)
		{
			venture_set_error_validation(error, "Pages", "the final page needs a question");
			goto fail;
		}
	}
	if (!venture_forms_translations_check(fields, error)) goto fail;
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

	g_object_set(response, "name", name, "answers", "{}", "sensitive-answers", NULL, "not-shown", NULL,
		"summary", "", "notes", NULL, "origin", NULL, "mapping-note", NULL,
		"anonymised-at", now, "contact-id", (gint64)0, "personal-hash", NULL,
		"invoice-id", (gint64)0, "booking-id", (gint64)0, "lead-id", (gint64)0,
		"scored", FALSE, "score", (gint64)0, "result-key", "", "result-message", "", "result-url", "", NULL);
	g_object_set_data(G_OBJECT(response), VENTURE_FORMS_ANONYMISING_KEY, GINT_TO_POINTER(1));
	return venture_database_save(database, response, actor, error);
}

static gboolean
forms_forget_mail_rows(VentureDatabase *database, VentureQuery *query, const VentureActor *actor,
	gint64 *cancelled, GError **error)
{
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(OrmInspector) inspector = orm_inspector_new(venture_database_get_connection(database), error);
	g_autoptr(GError) inspection_error = NULL;
	gboolean exists;
	guint i;
	if (inspector == NULL) return FALSE;
	exists = orm_inspector_has_table(inspector, "mail_messages", NULL, &inspection_error);
	if (inspection_error != NULL) { g_propagate_error(error, g_steal_pointer(&inspection_error)); return FALSE; }
	if (!exists) return TRUE;
	rows = venture_database_find(database, query, error);
	if (rows == NULL) return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *message = g_ptr_array_index(rows, i);
		g_autofree gchar *state = venture_forms_get_string(message, "state");
		if (g_strcmp0(state, "queued") && g_strcmp0(state, "failed") && g_strcmp0(state, "dead") && g_strcmp0(state, "cancelled")) continue;
		if (!venture_mail_outbox_forget_pending(venture_database_get_mail_outbox(database),
			venture_entity_get_organization_id(message), venture_entity_get_id(message), actor, error)) return FALSE;
		if (cancelled != NULL) (*cancelled)++;
	}
	return TRUE;
}

static gboolean
forms_forget_response_mail(VentureDatabase *database, VentureEntity *response, const VentureActor *actor,
	gint64 *cancelled, GError **error)
{
	g_autoptr(VentureQuery) mail = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE), reservations = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *key = g_strdup_printf("form-response:%s", venture_entity_get_uuid(response));
	gint64 organization = venture_entity_get_organization_id(response), booking = venture_forms_get_int(response, "booking-id");
	guint i;
	venture_query_set_organization(mail, organization); venture_query_set_limit(mail, 0);
	venture_query_add_filter_string(mail, "idempotency-key", VENTURE_FILTER_OP_EQ, key, NULL);
	if (!forms_forget_mail_rows(database, mail, actor, cancelled, error)) return FALSE;
	if (booking <= 0) return TRUE;
	reservations = venture_query_new(VENTURE_TYPE_BOOKING_RESERVATION);
	venture_query_set_organization(reservations, organization); venture_query_set_limit(reservations, 0);
	venture_query_add_filter_int(reservations, "activity-id", VENTURE_FILTER_OP_EQ, booking, NULL);
	rows = venture_database_find(database, reservations, error); if (rows == NULL) return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		g_clear_object(&mail); mail = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
		venture_query_set_organization(mail, organization); venture_query_set_limit(mail, 0);
		venture_query_add_filter_string(mail, "related-type", VENTURE_FILTER_OP_EQ, "booking_reservation", NULL);
		venture_query_add_filter_int(mail, "related-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(g_ptr_array_index(rows, i)), NULL);
		if (!forms_forget_mail_rows(database, mail, actor, cancelled, error)) return FALSE;
	}
	return TRUE;
}

JsonNode *
venture_forms_retention_sweep(VentureDatabase *database, gint64 organization_id, guint limit,
	GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) forms = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	gint64 anonymised = 0, purged = 0, drafts = 0, pending = 0, payment_payloads = 0, files = 0;
	guint budget, i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	budget = limit > 0 ? MIN(limit, 1000) : 100;
	forms = venture_query_new(VENTURE_TYPE_FORM);
	venture_query_set_organization(forms, organization_id);
	venture_query_set_include_deleted(forms, TRUE);
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
		venture_query_set_include_deleted(query, TRUE);
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

			if (!forms_forget_response_mail(database, response, actor, NULL, error) || !venture_forms_upload_purge_source(database, response, error) || !forms_payment_forget_response(database, response, error) || !forms_receipt_forget(database, response, error)) return NULL;
			if (purge ? !venture_database_purge(database, response, actor, error)
			          : !forms_anonymise(database, response, now, actor, error))
				return NULL;
			if (purge)
				purged++;
			else
				anonymised++;
		}
		{
			gint removed = forms_payment_expire_payloads(database, organization_id, venture_entity_get_id(form), cutoff, budget, error);
			if (removed < 0) return NULL;
			budget -= (guint)removed; payment_payloads += removed;
		}

	}

	if (budget > 0)
	{
		g_autoptr(VentureQuery) expired_query = venture_query_new(VENTURE_TYPE_FORM_DRAFT_RECORD);
		g_autoptr(GPtrArray) expired = NULL;
		g_autofree gchar *cutoff = venture_time_to_string(now);
		venture_query_set_organization(expired_query, organization_id);
		venture_query_set_include_deleted(expired_query, TRUE);
		venture_query_set_limit(expired_query, budget);
		venture_query_add_filter_string(expired_query, "expires-at", VENTURE_FILTER_OP_LTE, cutoff, NULL);
		expired = venture_database_find(database, expired_query, error);
		if (expired == NULL) return NULL;
		for (i = 0; i < expired->len; i++, budget--)
		{
			if (!forms_working_copy_purge(database, g_ptr_array_index(expired, i), "form_draft", actor, NULL, error)) return NULL;
			drafts++;
		}
	}

	if (budget > 0)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_PENDING);
		g_autoptr(GPtrArray) expired = NULL;
		g_autofree gchar *cutoff = venture_time_to_string(now);
		venture_query_set_organization(query, organization_id);
		venture_query_set_include_deleted(query, TRUE);
		venture_query_set_limit(query, budget);
		venture_query_add_filter_string(query, "expires-at", VENTURE_FILTER_OP_LTE, cutoff, NULL);
		expired = venture_database_find(database, query, error);
		if (expired == NULL) return NULL;
		for (i = 0; i < expired->len; i++, budget--)
		{
			if (!forms_pending_purge(database, g_ptr_array_index(expired, i), actor, NULL, error)) return NULL;
			pending++;
		}
	}

	if (budget > 0)
	{
		g_autoptr(GDateTime) oldest = g_date_time_add_days(now, -30);
		gint removed = forms_payment_expire_payloads(database, organization_id, 0, oldest, budget, error);
		if (removed < 0) return NULL;
		budget -= (guint)removed; payment_payloads += removed;
	}

	json_builder_begin_object(builder);
	if (budget > 0)
	{
		files = venture_forms_upload_sweep(database, organization_id, budget, now, error);
		if (files < 0) return NULL;
		budget -= (guint)files;
	}
	json_builder_set_member_name(builder, "expired_uploads"); json_builder_add_int_value(builder, files);
	json_builder_set_member_name(builder, "expired_payment_payloads");
	json_builder_add_int_value(builder, payment_payloads);
	json_builder_set_member_name(builder, "expired_signups");
	json_builder_add_int_value(builder, pending);
	json_builder_set_member_name(builder, "expired_drafts");
	json_builder_add_int_value(builder, drafts);
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

/* Row arrays and private drafts participate in the same erasure search. */
static gboolean
forms_node_mentions(JsonNode *node, const gchar *email, guint depth)
{
	if (depth > 8) return FALSE;
	if (JSON_NODE_HOLDS_OBJECT(node))
	{
		JsonObjectIter iter;
		const gchar *member;
		JsonNode *child;
		json_object_iter_init(&iter, json_node_get_object(node));
		while (json_object_iter_next(&iter, &member, &child))
			if (forms_node_mentions(child, email, depth + 1)) return TRUE;
	}
	else if (JSON_NODE_HOLDS_ARRAY(node))
	{
		JsonArray *array = json_node_get_array(node);
		guint i;
		for (i = 0; i < json_array_get_length(array); i++)
			if (forms_node_mentions(json_array_get_element(array, i), email, depth + 1)) return TRUE;
	}
	else if (JSON_NODE_HOLDS_VALUE(node) && G_TYPE_STRING == json_node_get_value_type(node))
	{
		g_autofree gchar *value = g_strstrip(g_strdup(json_node_get_string(node)));
		return g_ascii_strcasecmp(value, email) == 0;
	}
	return FALSE;
}

static gboolean
forms_answers_mention(const gchar *text, const gchar *email)
{
	g_autoptr(JsonNode) root = NULL;
	if (venture_string_is_empty(text)) return FALSE;
	root = json_from_string(text, NULL);
	return root != NULL && JSON_NODE_HOLDS_OBJECT(root) && forms_node_mentions(root, email, 0);
}

static gboolean
forms_bound_email(VentureDatabase *database, VentureEntity *response, const gchar *email)
{
	gint64 id = venture_forms_get_int(response, "contact-id");
	g_autoptr(VentureEntity) contact = NULL;
	g_autofree gchar *address = NULL;
	if (id <= 0) return FALSE;
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTACT);
		venture_query_set_organization(query, venture_entity_get_organization_id(response));
		venture_query_set_include_deleted(query, TRUE);
		venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_EQ, id, NULL);
		contact = venture_database_find_one(database, query, NULL);
	}
	if (contact == NULL || venture_entity_get_organization_id(contact) != venture_entity_get_organization_id(response)) return FALSE;
	address = venture_forms_get_string(contact, "email");
	return address != NULL && g_ascii_strcasecmp(g_strstrip(address), email) == 0;
}

static gboolean
forms_privacy_matches(VentureDatabase *database, VentureEntity *row, const gchar *open,
	const gchar *secret, const gchar *email, gint64 contact_id)
{
	return (contact_id > 0 && venture_forms_get_int(row, "contact-id") == contact_id) ||
		(!venture_string_is_empty(email) && (forms_answers_mention(open, email) ||
		 forms_answers_mention(secret, email) || forms_bound_email(database, row, email)));
}

static gboolean
forms_payment_privacy_matches(VentureDatabase *database, VentureEntity *pending,
	const gchar *email, gint64 contact_id, GError **error)
{
	g_autofree gchar *payload = venture_forms_get_string(pending, "payload");
	if (venture_string_is_empty(payload)) return FALSE;
	if (contact_id > 0 && venture_forms_get_int(pending, "contact-id") == contact_id) return TRUE;
	return !venture_string_is_empty(email) && forms_payment_mentions(database, pending, email, error);
}

static JsonNode *
forms_erase_matching(VentureDatabase *database, gint64 organization_id, const gchar *email,
	gint64 contact_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autofree gchar *wanted = NULL;
	gint64 erased = 0, cancelled = 0;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	wanted = g_strstrip(g_strdup(email != NULL ? email : ""));
	if (contact_id <= 0 && NULL == strchr(wanted, '@'))
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

		if (!forms_privacy_matches(database, response, open, hidden, wanted, contact_id))
			continue;
		if (!forms_forget_response_mail(database, response, actor, &cancelled, error)) goto fail;
		if (!forms_payment_forget_response(database, response, error) || !forms_receipt_forget(database, response, error)) goto fail;
		if (!venture_forms_upload_purge_source(database, response, error) || !venture_database_purge(database, response, actor, error))
			goto fail;
		erased++;
	}

	{
		g_autoptr(VentureQuery) payments_query = venture_query_new(VENTURE_TYPE_FORM_PAYMENT);
		g_autoptr(GPtrArray) payments = NULL;
		venture_query_set_organization(payments_query, organization_id);
		venture_query_set_limit(payments_query, 0);
		payments = venture_database_find(database, payments_query, error); if (payments == NULL) goto fail;
		for (i = 0; i < payments->len; i++)
		{
			VentureEntity *pending_payment = g_ptr_array_index(payments, i);
			if (!forms_payment_privacy_matches(database, pending_payment, wanted, contact_id, error))
			{ if (error != NULL && *error != NULL) goto fail; continue; }
			if (!forms_payment_forget(database, pending_payment, error)) goto fail;
			erased++;
		}
	}

	{
		g_autoptr(VentureQuery) draft_query = venture_query_new(VENTURE_TYPE_FORM_DRAFT_RECORD);
		g_autoptr(GPtrArray) drafts = NULL;
		venture_query_set_organization(draft_query, organization_id);
		venture_query_set_include_deleted(draft_query, TRUE);
		venture_query_set_limit(draft_query, 0);
		drafts = venture_database_find(database, draft_query, error);
		if (drafts == NULL) goto fail;
		for (i = 0; i < drafts->len; i++)
		{
			VentureEntity *draft = g_ptr_array_index(drafts, i);
			g_autofree gchar *text = venture_forms_get_string(draft, "answers");
			g_autofree gchar *address = venture_forms_get_string(draft, "resume-email");
			if ((venture_string_is_empty(wanted) || g_ascii_strcasecmp(address != NULL ? address : "", wanted) != 0) && !forms_privacy_matches(database, draft, text, NULL, wanted, contact_id)) continue;
			if (!forms_working_copy_purge(database, draft, "form_draft", actor, &cancelled, error)) goto fail;
			erased++;
		}
	}

	{
		g_autoptr(VentureQuery) pending_query = venture_query_new(VENTURE_TYPE_FORM_PENDING);
		g_autoptr(GPtrArray) pending = NULL;
		venture_query_set_organization(pending_query, organization_id);
		venture_query_set_include_deleted(pending_query, TRUE);
		venture_query_set_limit(pending_query, 0);
		pending = venture_database_find(database, pending_query, error);
		if (pending == NULL) goto fail;
		for (i = 0; i < pending->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(pending, i);
			g_autofree gchar *address = venture_forms_get_string(row, "email");
			g_autofree gchar *text = venture_forms_get_string(row, "answers");
			if ((venture_string_is_empty(wanted) || g_ascii_strcasecmp(address != NULL ? address : "", wanted) != 0) && !forms_privacy_matches(database, row, text, NULL, wanted, contact_id)) continue;
			if (!forms_pending_purge(database, row, actor, &cancelled, error)) goto fail;
			erased++;
		}
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

static JsonNode *
forms_export_matching(VentureDatabase *database, gint64 organization_id, const gchar *email,
	gint64 contact_id, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autofree gchar *wanted = g_strstrip(g_strdup(email != NULL ? email : ""));
	guint i;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	if (contact_id <= 0 && NULL == strchr(wanted, '@'))
	{
		venture_set_error_validation(error, "email", "name the address whose responses to export");
		return NULL;
	}
	query = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
	venture_query_set_organization(query, organization_id);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(database, query, error);
	if (NULL == rows)
		return NULL;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "email");
	json_builder_add_string_value(builder, wanted);
	if (contact_id > 0) { json_builder_set_member_name(builder, "contact_id"); json_builder_add_int_value(builder, contact_id); }
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

		if (!forms_privacy_matches(database, response, open, hidden, wanted, contact_id))
			continue;
		form = venture_database_get(database, VENTURE_TYPE_FORM, venture_forms_get_int(response, "form-id"), NULL);
		title = form != NULL ? venture_forms_get_string(form, "title") : NULL;
		g_object_get(response, "submitted-at", &at, NULL);
		when = NULL != at ? venture_time_to_string(at) : g_strdup("");
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "form");
		json_builder_add_string_value(builder, title != NULL ? title : "");
		json_builder_set_member_name(builder, "form_version");
		json_builder_add_int_value(builder, venture_forms_get_int(response, "version-number"));
		json_builder_set_member_name(builder, "received");
		json_builder_add_string_value(builder, when);
		if (venture_forms_get_bool(response, "scored"))
		{
			g_autofree gchar *result = venture_forms_get_string(response, "result-message");
			g_autofree gchar *band = venture_forms_get_string(response, "result-key");
			json_builder_set_member_name(builder, "score"); json_builder_add_int_value(builder, venture_forms_get_int(response, "score"));
			json_builder_set_member_name(builder, "result_key"); json_builder_add_string_value(builder, band != NULL ? band : "");
			json_builder_set_member_name(builder, "result"); json_builder_add_string_value(builder, result != NULL ? result : "");
		}
		/* Everything they sent, sensitive answers included: an access
		 * request is owed the lot. */
		json_builder_set_member_name(builder, "answers");
		json_builder_add_value(builder, json_from_string(venture_string_is_empty(open) ? "{}" : open, NULL));
		json_builder_set_member_name(builder, "sensitive_answers");
		json_builder_add_value(builder, json_from_string(venture_string_is_empty(hidden) ? "{}" : hidden, NULL));
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	{
		g_autoptr(VentureQuery) payments_query = venture_query_new(VENTURE_TYPE_FORM_PAYMENT);
		g_autoptr(GPtrArray) payments = NULL;
		venture_query_set_organization(payments_query, organization_id); venture_query_set_limit(payments_query, 0);
		payments = venture_database_find(database, payments_query, error); if (payments == NULL) return NULL;
		json_builder_set_member_name(builder, "pending_payments"); json_builder_begin_array(builder);
		for (i = 0; i < payments->len; i++)
		{
			VentureEntity *pending_payment = g_ptr_array_index(payments, i);
			g_autoptr(VentureEntity) response = NULL;
			g_autoptr(JsonNode) open_values = NULL, hidden_values = NULL;
			g_autofree gchar *open = NULL, *hidden = NULL, *state = NULL;
			if (!forms_payment_privacy_matches(database, pending_payment, wanted, contact_id, error))
			{ if (error != NULL && *error != NULL) return NULL; continue; }
			response = forms_payment_restore(pending_payment, error); if (response == NULL) return NULL;
			open = venture_forms_get_string(response, "answers"); hidden = venture_forms_get_string(response, "sensitive-answers");
			state = venture_forms_get_string(pending_payment, "state");
			open_values = json_from_string(venture_string_is_empty(open) ? "{}" : open, error);
			hidden_values = json_from_string(venture_string_is_empty(hidden) ? "{}" : hidden, error);
			if (open_values == NULL || hidden_values == NULL ||
			    !venture_forms_upload_export(database, venture_forms_get_int(pending_payment, "form-id"), open_values, error) ||
			    !venture_forms_upload_export(database, venture_forms_get_int(pending_payment, "form-id"), hidden_values, error)) return NULL;
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "form_id"); json_builder_add_int_value(builder, venture_forms_get_int(pending_payment, "form-id"));
			json_builder_set_member_name(builder, "invoice_id"); json_builder_add_int_value(builder, venture_forms_get_int(pending_payment, "invoice-id"));
			json_builder_set_member_name(builder, "state"); json_builder_add_string_value(builder, state);
			json_builder_set_member_name(builder, "answers"); json_builder_add_value(builder, g_steal_pointer(&open_values));
			json_builder_set_member_name(builder, "sensitive_answers"); json_builder_add_value(builder, g_steal_pointer(&hidden_values));
			json_builder_end_object(builder);
		}
		json_builder_end_array(builder);
	}

	{
		g_autoptr(VentureQuery) draft_query = venture_query_new(VENTURE_TYPE_FORM_DRAFT_RECORD);
		g_autoptr(GPtrArray) drafts = NULL;
		venture_query_set_organization(draft_query, organization_id);
		venture_query_set_include_deleted(draft_query, TRUE);
		venture_query_set_limit(draft_query, 0);
		drafts = venture_database_find(database, draft_query, error);
		if (drafts == NULL) return NULL;
		json_builder_set_member_name(builder, "drafts");
		json_builder_begin_array(builder);
		for (i = 0; i < drafts->len; i++)
		{
			VentureEntity *draft = g_ptr_array_index(drafts, i);
			g_autofree gchar *text = venture_forms_get_string(draft, "answers");
			g_autofree gchar *address = venture_forms_get_string(draft, "resume-email");
			if ((venture_string_is_empty(wanted) || g_ascii_strcasecmp(address != NULL ? address : "", wanted) != 0) && !forms_privacy_matches(database, draft, text, NULL, wanted, contact_id)) continue;
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "form_id");
			json_builder_add_int_value(builder, venture_forms_get_int(draft, "form-id"));
			json_builder_set_member_name(builder, "answers");
			{
				JsonNode *value = json_from_string(text, NULL);
				if (value != NULL && JSON_NODE_HOLDS_OBJECT(value))
				{
					json_object_remove_member(json_node_get_object(value), VENTURE_FORMS_PERSONAL);
					json_object_remove_member(json_node_get_object(value), VENTURE_FORMS_PREFILL);
					json_object_remove_member(json_node_get_object(value), VENTURE_FORMS_PAYMENT_NONCE);
				}
				if (!venture_forms_upload_export(database, venture_forms_get_int(draft, "form-id"), value, error))
				{ if (value != NULL) json_node_unref(value); return NULL; }
				json_builder_add_value(builder, value);
			}
			json_builder_end_object(builder);
		}
		json_builder_end_array(builder);
	}

	{
		g_autoptr(VentureQuery) pending_query = venture_query_new(VENTURE_TYPE_FORM_PENDING);
		g_autoptr(GPtrArray) pending = NULL;
		venture_query_set_organization(pending_query, organization_id);
		venture_query_set_include_deleted(pending_query, TRUE);
		venture_query_set_limit(pending_query, 0);
		pending = venture_database_find(database, pending_query, error);
		if (pending == NULL) return NULL;
		json_builder_set_member_name(builder, "unconfirmed_signups");
		json_builder_begin_array(builder);
		for (i = 0; i < pending->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(pending, i);
			g_autofree gchar *text = venture_forms_get_string(row, "answers");
			g_autoptr(JsonNode) answers = json_from_string(text, NULL);
			g_autofree gchar *address = venture_forms_get_string(row, "email");
			if ((venture_string_is_empty(wanted) || g_ascii_strcasecmp(address != NULL ? address : "", wanted) != 0) && !forms_privacy_matches(database, row, text, NULL, wanted, contact_id)) continue;
			if (answers == NULL || !JSON_NODE_HOLDS_OBJECT(answers)) continue;
			json_object_remove_member(json_node_get_object(answers), VENTURE_FORMS_TICKET);
			json_object_remove_member(json_node_get_object(answers), VENTURE_FORMS_PERSONAL);
			json_object_remove_member(json_node_get_object(answers), VENTURE_FORMS_PREFILL);
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "form_id");
			json_builder_add_int_value(builder, venture_forms_get_int(row, "form-id"));
			json_builder_set_member_name(builder, "answers");
			if (!venture_forms_upload_export(database, venture_forms_get_int(row, "form-id"), answers, error)) return NULL;
			json_builder_add_value(builder, g_steal_pointer(&answers));
			json_builder_end_object(builder);
		}
		json_builder_end_array(builder);
	}

	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

/* Contact identity covers a personal link even when its submitted email was
 * edited or the CRM contact has since lost its email address. */
static gchar *
forms_privacy_contact_email(VentureDatabase *database, gint64 organization, gint64 id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTACT);
	g_autoptr(VentureEntity) contact = NULL;
	g_autofree gchar *address = NULL;
	venture_query_set_organization(query, organization); venture_query_set_include_deleted(query, TRUE);
	venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_EQ, id, NULL);
	contact = id > 0 ? venture_database_find_one(database, query, error) : NULL;
	if (contact == NULL)
	{
		if (error == NULL || *error == NULL) g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Contact not found");
		return NULL;
	}
	address = venture_forms_get_string(contact, "email");
	return address != NULL ? g_strstrip(g_steal_pointer(&address)) : g_strdup("");
}

JsonNode *
venture_forms_erase_person(VentureDatabase *database, gint64 organization, const gchar *email,
	const VentureActor *actor, GError **error)
{
	return forms_erase_matching(database, organization, email, 0, actor, error);
}

JsonNode *
venture_forms_erase_contact(VentureDatabase *database, gint64 organization, gint64 contact_id,
	const VentureActor *actor, GError **error)
{
	g_autofree gchar *email = forms_privacy_contact_email(database, organization, contact_id, error);
	return email != NULL ? forms_erase_matching(database, organization, email, contact_id, actor, error) : NULL;
}

JsonNode *
venture_forms_export_person(VentureDatabase *database, gint64 organization, const gchar *email, GError **error)
{
	return forms_export_matching(database, organization, email, 0, error);
}

JsonNode *
venture_forms_export_contact(VentureDatabase *database, gint64 organization, gint64 contact_id, GError **error)
{
	g_autofree gchar *email = forms_privacy_contact_email(database, organization, contact_id, error);
	return email != NULL ? forms_export_matching(database, organization, email, contact_id, error) : NULL;
}
