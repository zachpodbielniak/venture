/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Designed email on the records: a mail_template's design is the source
 * and its text and HTML bodies are built from it on every save, by every
 * writer, so the one template renderer -- and every sender that already
 * uses it -- sends what the design says. Versions are kept by this
 * service alone, so undo can trust them byte for byte.
 */
#include "venture-email-design-private.h"
#include <string.h>

#define CONTEXT_KEY "venture-email-design-context"
#define INSTALLED_KEY "venture-email-design-installed"

static void context_free(gpointer data)
{
	g_weak_ref_clear(data);
	g_free(data);
}
VentureContext *venture_email_design_context(VentureDatabase *database)
{
	GWeakRef *reference = g_object_get_data(G_OBJECT(database), CONTEXT_KEY);
	VentureContext *context = reference != NULL ? g_weak_ref_get(reference) : NULL;
	/* Borrowed: the context owns the database, so it outlives this call. */
	if (context != NULL)
		g_object_unref(context);
	return context;
}
gboolean venture_email_design_require_manager(VentureContext *context, gint64 organization_id, GError **error)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureAccessPolicy *policy = venture_database_get_access_policy(venture_context_get_database(context));
	const VentureAuthPrincipal *actor = venture_access_policy_get_actor(policy);
	if (actor == NULL)
		return TRUE;
	if (venture_access_policy_has_organization_role(policy, actor, organization_id, roles, G_N_ELEMENTS(roles)))
		return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		"Designing email needs an owner or administrator of the organization");
	return FALSE;
}
void venture_email_design_result_free(VentureEmailDesignResult *result)
{
	if (result == NULL) return;
	g_clear_object(&result->template);
	g_clear_object(&result->version);
	g_clear_pointer(&result->stripped, g_ptr_array_unref);
	g_free(result->ask);
	g_free(result);
}
static gchar *get_string(gpointer row, const gchar *name)
{
	gchar *value = NULL;
	g_object_get(row, name, &value, NULL);
	return value;
}
static gint64 get_int(gpointer row, const gchar *name)
{
	gint64 value = 0;
	g_object_get(row, name, &value, NULL);
	return value;
}

/* --- The brand ------------------------------------------------------------------ */

static VentureEntity *find_brand(VentureDatabase *database, gint64 organization_id)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_EMAIL_BRAND);
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_set_organization(query, organization_id);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 1);
	rows = venture_database_find(database, query, NULL);
	return rows != NULL && rows->len > 0 ? g_object_ref(g_ptr_array_index(rows, 0)) : NULL;
}
static gchar *organization_name(VentureDatabase *database, gint64 organization_id)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	g_autoptr(VentureEntity) organization = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, organization_id, NULL);
	return organization != NULL ? get_string(organization, "name") : NULL;
}
static VentureEntity *find_image(VentureDatabase *database, gint64 organization_id, gint64 image_id)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	VentureEntity *image = image_id > 0 ? venture_database_get(database, VENTURE_TYPE_HOSTED_IMAGE, image_id, NULL) : NULL;
	if (image != NULL && (venture_entity_is_deleted(image) || venture_entity_get_organization_id(image) != organization_id))
		g_clear_object(&image);
	return image;
}
static void take(gchar **slot, gchar *value)
{
	if (venture_string_is_empty(value)) { g_free(value); return; }
	g_free(*slot);
	*slot = value;
}
VentureEmailLook *venture_email_look_load(VentureContext *context, gint64 organization_id)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureEntity) brand = find_brand(database, organization_id);
	g_autofree gchar *name = organization_name(database, organization_id);
	VentureEmailLook *look = venture_email_look_new_default(name);
	if (brand != NULL) {
		g_autoptr(VentureEntity) logo = find_image(database, organization_id, get_int(brand, "logo-image-id"));
		g_autofree gchar *font = get_string(brand, "font");
		take(&look->name, get_string(brand, "name"));
		take(&look->primary, get_string(brand, "primary-color"));
		take(&look->accent, get_string(brand, "accent-color"));
		take(&look->text, get_string(brand, "text-color"));
		take(&look->background, get_string(brand, "background-color"));
		take(&look->website, get_string(brand, "website-url"));
		take(&look->address, get_string(brand, "footer-address"));
		/* The dark colours follow the light ones unless the brand says. */
		g_clear_pointer(&look->dark_primary, g_free);
		g_clear_pointer(&look->dark_card, g_free);
		take(&look->dark_primary, get_string(brand, "dark-primary-color"));
		take(&look->dark_text, get_string(brand, "dark-text-color"));
		{
			g_autofree gchar *dark = get_string(brand, "dark-background-color");
			if (!venture_string_is_empty(dark)) {
				g_free(look->dark_background);
				look->dark_background = g_steal_pointer(&dark);
			} else
				look->dark_card = g_strdup("#1b1f24");
		}
		if (venture_email_font_stack(font) != NULL) {
			g_free(look->font);
			look->font = g_strdup(venture_email_font_stack(font));
		}
		if (logo != NULL) {
			look->logo_url = venture_hosted_image_url(context, logo);
			look->logo_width = (gint)get_int(logo, "width");
			look->logo_height = (gint)get_int(logo, "height");
		}
	}
	venture_email_look_finish(look);
	return look;
}

static gboolean web_address(const gchar *value)
{
	g_autoptr(GUri) uri = g_uri_parse(value, G_URI_FLAGS_NONE, NULL);
	const gchar *scheme = uri != NULL ? g_uri_get_scheme(uri) : NULL;
	return scheme != NULL && (g_ascii_strcasecmp(scheme, "http") == 0 || g_ascii_strcasecmp(scheme, "https") == 0) &&
		!venture_string_is_empty(g_uri_get_host(uri)) && strpbrk(value, " \"'<>{}") == NULL;
}
static gboolean colour_ok(const gchar *value)
{
	guint i;
	if (venture_string_is_empty(value)) return TRUE;
	if (strlen(value) != 7 || value[0] != '#') return FALSE;
	for (i = 1; i < 7; i++)
		if (!g_ascii_isxdigit(value[i])) return FALSE;
	return TRUE;
}
static gboolean brand_validate(VentureDatabase *database, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	static const gchar *const colours[] = { "primary-color", "accent-color", "text-color", "background-color",
		"dark-primary-color", "dark-text-color", "dark-background-color", NULL };
	g_autofree gchar *font = get_string(row, "font"), *site = get_string(row, "website-url"), *ask = get_string(row, "logo-ask");
	gint64 logo = get_int(row, "logo-image-id");
	guint i;
	(void)previous; (void)data;
	for (i = 0; colours[i]; i++) {
		g_autofree gchar *value = get_string(row, colours[i]);
		if (!colour_ok(value)) {
			venture_set_error_validation(error, colours[i], "write a colour as #rrggbb, for example #c2410c");
			return FALSE;
		}
	}
	if (!venture_string_is_empty(font) && venture_email_font_stack(font) == NULL) {
		venture_set_error_validation(error, "font", "choose sans, serif, rounded, system or mono");
		return FALSE;
	}
	if (!venture_string_is_empty(site) && !web_address(site)) {
		venture_set_error_validation(error, "website-url", "must be an http or https address");
		return FALSE;
	}
	if (!venture_string_is_empty(ask) && g_strcmp0(ask, "asked") != 0 && g_strcmp0(ask, "dismissed") != 0 && g_strcmp0(ask, "answered") != 0) {
		venture_set_error_validation(error, "logo-ask", "is asked, dismissed or answered");
		return FALSE;
	}
	if (logo > 0) {
		g_autoptr(VentureEntity) image = find_image(database, venture_entity_get_organization_id(row), logo);
		if (image == NULL) {
			venture_set_error_validation(error, "logo-image-id", "choose one of this organization's hosted images");
			return FALSE;
		}
		/* A logo answers the designer's request for one, so it is never asked. */
		if (g_strcmp0(ask, "answered") != 0)
			g_object_set(row, "logo-ask", "answered", NULL);
	}
	{
		g_autoptr(VentureEntity) existing = find_brand(database, venture_entity_get_organization_id(row));
		if (existing != NULL && venture_entity_get_id(existing) != venture_entity_get_id(row)) {
			venture_set_error_validation(error, "Brand", "this organization already has an email brand; change that one");
			return FALSE;
		}
	}
	return TRUE;
}

/* --- Sections and images, for the renderer ------------------------------------------ */

typedef struct {
	VentureContext *context;
	gint64 organization_id;
	gint64 override_id;
	const gchar *override_design;
} Resolve;

static gchar *resolve_section(gint64 id, gpointer data, GError **error)
{
	Resolve *resolve = data;
	VentureDatabase *database = venture_context_get_database(resolve->context);
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) section = NULL;
	gchar *design;
	if (resolve->override_id > 0 && id == resolve->override_id)
		return g_strdup(resolve->override_design);
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	section = venture_database_get(database, VENTURE_TYPE_EMAIL_SECTION, id, NULL);
	if (section == NULL || venture_entity_is_deleted(section) || venture_entity_get_organization_id(section) != resolve->organization_id) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "The design names section %" G_GINT64_FORMAT ", which this organization does not have", id);
		return NULL;
	}
	design = get_string(section, "design");
	if (venture_string_is_empty(design)) {
		g_free(design);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Section %" G_GINT64_FORMAT " has no blocks", id);
		return NULL;
	}
	return design;
}
static gboolean resolve_image(gint64 id, gpointer data, gchar **url, gint *width, gint *height, GError **error)
{
	Resolve *resolve = data;
	g_autoptr(VentureEntity) image = find_image(venture_context_get_database(resolve->context), resolve->organization_id, id);
	if (image == NULL) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "The design names image %" G_GINT64_FORMAT ", which this organization does not have", id);
		return FALSE;
	}
	*url = venture_hosted_image_url(resolve->context, image);
	*width = (gint)get_int(image, "width");
	*height = (gint)get_int(image, "height");
	return TRUE;
}
/* Builds a template's bodies from its design, with the organization's
 * current brand, sections and images. */
static gboolean compile(VentureContext *context, VentureEntity *template, gchar **html, gchar **text, GError **error)
{
	g_autofree gchar *design = get_string(template, "design"), *subject = get_string(template, "subject");
	g_autoptr(VentureEmailLook) look = venture_email_look_load(context, venture_entity_get_organization_id(template));
	Resolve resolve;
	VentureEmailResolver resolver;
	resolve.context = context;
	resolve.organization_id = venture_entity_get_organization_id(template);
	resolve.override_id = 0;
	resolve.override_design = NULL;
	resolver.section = resolve_section;
	resolver.image = resolve_image;
	resolver.data = &resolve;
	return venture_email_design_render(design, look, &resolver, subject, TRUE, html, text, error);
}
static gboolean template_validate(VentureDatabase *database, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	VentureContext *context = venture_email_design_context(database);
	g_autofree gchar *design = get_string(row, "design"), *html = NULL, *text = NULL;
	(void)previous; (void)data;
	if (venture_string_is_empty(design))
		return TRUE;
	if (context == NULL)
		return venture_email_design_validate(design, error);
	if (!compile(context, row, &html, &text, error))
		return FALSE;
	g_object_set(row, "html-body", html, "text-body", text, NULL);
	return TRUE;
}
static gboolean section_validate(VentureDatabase *database, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	VentureContext *context = venture_email_design_context(database);
	g_autofree gchar *design = get_string(row, "design"), *html = NULL, *text = NULL;
	g_autoptr(VentureEmailLook) look = venture_email_look_new_default(NULL);
	Resolve resolve;
	VentureEmailResolver resolver;
	(void)previous; (void)data;
	if (!venture_email_design_validate(design, error))
		return FALSE;
	if (context == NULL)
		return TRUE;
	/* Rendered once with this very design standing in for the stored
	 * one, so a loop it would close is found before it is saved. */
	resolve.context = context;
	resolve.organization_id = venture_entity_get_organization_id(row);
	resolve.override_id = venture_entity_get_id(row);
	resolve.override_design = design;
	resolver.section = resolve_section;
	resolver.image = resolve_image;
	resolver.data = &resolve;
	if (venture_entity_get_id(row) > 0) {
		g_autofree gchar *wrapped = g_strdup_printf("{\"blocks\":[{\"type\":\"section\",\"section_id\":%" G_GINT64_FORMAT "}]}", venture_entity_get_id(row));
		return venture_email_design_render(wrapped, look, &resolver, NULL, TRUE, &html, &text, error);
	}
	return venture_email_design_render(design, look, &resolver, NULL, TRUE, &html, &text, error);
}
/* The history is written by venture_email_design_save() alone: an edited
 * or invented version would make undo restore something nobody saved. */
static gboolean version_validate(VentureDatabase *database, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	(void)previous; (void)data;
	if (g_object_get_data(G_OBJECT(database), VENTURE_EMAIL_DESIGN_FILING) == row)
		return TRUE;
	venture_set_error_validation(error, "Version", "design versions are kept by Email design and cannot be written directly");
	return FALSE;
}

/* --- Saving, undo and rebuilding ---------------------------------------------------- */

static gint64 last_version(VentureDatabase *database, gint64 template_id, gint64 below)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_EMAIL_DESIGN_VERSION);
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_add_filter_int(query, "template-id", VENTURE_FILTER_OP_EQ, template_id, NULL);
	if (below > 0)
		venture_query_add_filter_int(query, "number", VENTURE_FILTER_OP_LT, below, NULL);
	venture_query_add_order(query, "number", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 1);
	rows = venture_database_find(database, query, NULL);
	return rows != NULL && rows->len > 0 ? venture_entity_get_id(g_ptr_array_index(rows, 0)) : 0;
}
VentureEntity *venture_email_design_save(VentureContext *context, VentureEntity *template, const gchar *design,
	const gchar *source, const gchar *note, const VentureActor *actor, GError **error)
{
	VentureDatabase *database;
	g_autoptr(VentureEntity) version = NULL;
	gint64 number = 1, org;
	gboolean ok;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context) && VENTURE_IS_MAIL_TEMPLATE(template), NULL);
	database = venture_context_get_database(context);
	org = venture_entity_get_organization_id(template);
	if (org <= 0) {
		org = venture_context_get_default_organization_id(context);
		venture_entity_set_organization_id(template, org);
	}
	if (!venture_email_design_require_manager(context, org, error))
		return NULL;
	if (!venture_email_design_validate(design, error))
		return NULL;
	if (venture_entity_get_id(template) > 0) {
		gint64 last = last_version(database, venture_entity_get_id(template), 0);
		if (last > 0) {
			g_autoptr(VentureEntity) row = venture_database_get(database, VENTURE_TYPE_EMAIL_DESIGN_VERSION, last, NULL);
			number = row != NULL ? get_int(row, "number") + 1 : 1;
		}
	}
	if (!venture_database_begin(database, error))
		return NULL;
	g_object_set(template, "design", design, "design-version", number, NULL);
	ok = venture_database_save(database, template, actor, error);
	if (ok) {
		version = g_object_new(VENTURE_TYPE_EMAIL_DESIGN_VERSION, "organization-id", org,
			"template-id", venture_entity_get_id(template), "number", number, "design", design,
			"source", source != NULL ? source : "manual", "note", note, NULL);
		g_object_set_data(G_OBJECT(database), VENTURE_EMAIL_DESIGN_FILING, version);
		ok = venture_database_save(database, version, actor, error);
		g_object_set_data(G_OBJECT(database), VENTURE_EMAIL_DESIGN_FILING, NULL);
	}
	if (ok)
		ok = venture_database_commit(database, error);
	else
		venture_database_rollback(database);
	return ok ? g_steal_pointer(&version) : NULL;
}
static VentureEntity *load_template(VentureContext *context, gint64 organization_id, gint64 template_id, gboolean designed, GError **error)
{
	VentureEntity *template = template_id > 0
		? venture_database_get(venture_context_get_database(context), VENTURE_TYPE_MAIL_TEMPLATE, template_id, NULL) : NULL;
	g_autofree gchar *design = NULL;
	if (template == NULL || venture_entity_is_deleted(template) || venture_entity_get_organization_id(template) != organization_id) {
		g_clear_object(&template);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Mail template not found");
		return NULL;
	}
	design = get_string(template, "design");
	if (designed && venture_string_is_empty(design)) {
		g_clear_object(&template);
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "This template has no design yet");
		return NULL;
	}
	return template;
}
VentureEntity *venture_email_design_undo(VentureContext *context, gint64 organization_id, gint64 template_id,
	const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureEntity) template = NULL, previous = NULL;
	g_autofree gchar *design = NULL;
	gint64 id;
	if (!venture_email_design_require_manager(context, organization_id, error))
		return NULL;
	template = load_template(context, organization_id, template_id, TRUE, error);
	if (template == NULL)
		return NULL;
	id = last_version(database, template_id, get_int(template, "design-version"));
	previous = id > 0 ? venture_database_get(database, VENTURE_TYPE_EMAIL_DESIGN_VERSION, id, NULL) : NULL;
	if (previous == NULL) {
		venture_set_error_validation(error, "Undo", "there is no earlier version of this design");
		return NULL;
	}
	design = get_string(previous, "design");
	g_object_set(template, "design", design, "design-version", get_int(previous, "number"), NULL);
	if (!venture_database_save(database, template, actor, error))
		return NULL;
	return g_steal_pointer(&previous);
}
/* After a brand or a section changes: every designed template of the
 * organization, rebuilt with what they now say. */
static gint venture_email_design_rebuild(VentureContext *context, gint64 organization_id, const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_TEMPLATE);
	g_autoptr(GPtrArray) rows = NULL;
	gint changed = 0;
	guint i;
	if (!venture_email_design_require_manager(context, organization_id, error))
		return -1;
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 1000);
	rows = venture_database_find(database, query, error);
	if (rows == NULL)
		return -1;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *template = g_ptr_array_index(rows, i);
		g_autofree gchar *design = get_string(template, "design"), *html = NULL, *text = NULL, *old_html = NULL, *old_text = NULL;
		if (venture_string_is_empty(design))
			continue;
		if (!compile(context, template, &html, &text, error))
			return -1;
		old_html = get_string(template, "html-body");
		old_text = get_string(template, "text-body");
		if (g_strcmp0(html, old_html) == 0 && g_strcmp0(text, old_text) == 0)
			continue;
		g_object_set(template, "html-body", html, "text-body", text, NULL);
		if (!venture_database_save(database, template, actor, error))
			return -1;
		changed++;
	}
	return changed;
}
gboolean venture_email_design_dismiss_logo_ask(VentureContext *context, gint64 organization_id,
	const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureEntity) brand = NULL;
	if (!venture_email_design_require_manager(context, organization_id, error))
		return FALSE;
	brand = find_brand(database, organization_id);
	if (brand == NULL) {
		g_autofree gchar *name = organization_name(database, organization_id);
		brand = g_object_new(VENTURE_TYPE_EMAIL_BRAND, "organization-id", organization_id,
			"name", !venture_string_is_empty(name) ? name : "My business", NULL);
	} else {
		g_autofree gchar *ask = get_string(brand, "logo-ask");
		if (g_strcmp0(ask, "answered") == 0 || g_strcmp0(ask, "dismissed") == 0)
			return TRUE;
	}
	g_object_set(brand, "logo-ask", "dismissed", NULL);
	return venture_database_save(database, brand, actor, error);
}
/* Asks for a logo once per organization: the first design made without
 * one asks, and nothing asks again once asked, dismissed or answered. */
gchar *venture_email_design_logo_ask(VentureContext *context, gint64 organization_id, const VentureActor *actor)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureEntity) brand = find_brand(database, organization_id);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ask = NULL;
	if (brand != NULL) {
		ask = get_string(brand, "logo-ask");
		if (!venture_string_is_empty(ask) || get_int(brand, "logo-image-id") > 0)
			return NULL;
	} else {
		g_autofree gchar *name = organization_name(database, organization_id);
		brand = g_object_new(VENTURE_TYPE_EMAIL_BRAND, "organization-id", organization_id,
			"name", !venture_string_is_empty(name) ? name : "My business", NULL);
	}
	g_object_set(brand, "logo-ask", "asked", NULL);
	if (!venture_database_save(database, brand, actor, &error)) {
		g_message("Email design: the logo request was not recorded: %s", error->message);
		return NULL;
	}
	return g_strdup("This email uses a plain look because there is no logo yet. Upload your logo on the Email design page "
		"(Upload an image), then choose it as the logo in Email brand; every design after that carries it.");
}

/* --- The preview kind ------------------------------------------------------------------ */

static void placeholders(JsonObject *values, const gchar *body)
{
	const gchar *p = body;
	while (p != NULL && (p = strchr(p, '{')) != NULL) {
		const gchar *q = p + 1;
		if (p[1] == '{') { p += 2; continue; }
		while (g_ascii_islower(*q) || g_ascii_isdigit(*q) || *q == '_') q++;
		if (*q == '}' && q > p + 1) {
			g_autofree gchar *key = g_strndup(p + 1, q - p - 1);
			g_autofree gchar *shown = g_strdup_printf("{%s}", key);
			json_object_set_string_member(values, key, shown);
		}
		p = q;
	}
}
/* The template as it would be sent, with each recipient's placeholders
 * shown as themselves: the bodies are the stored ones every sender uses. */
static VentureMailMessage *design_preview_build(VentureContext *context, gint64 organization_id, gint64 source_id,
	GDateTime *now, GPtrArray *notes, GError **error)
{
	g_autoptr(VentureEntity) template = load_template(context, organization_id, source_id, TRUE, error);
	g_autoptr(JsonObject) values = json_object_new();
	g_autofree gchar *subject = NULL, *text = NULL, *html = NULL;
	VentureMailMessage *message;
	(void)now;
	if (template == NULL)
		return NULL;
	g_object_get(template, "subject", &subject, "text-body", &text, "html-body", &html, NULL);
	placeholders(values, subject);
	placeholders(values, text);
	placeholders(values, html);
	if (json_object_get_size(values) > 0)
		g_ptr_array_add(notes, g_strdup("Placeholders such as {first_name} are shown as themselves; each is filled from the recipient's record when the email is sent."));
	message = venture_mail_template_render_values(VENTURE_MAIL_TEMPLATE(template), values, error);
	if (message != NULL)
		g_object_set(message, "to", "recipient@example.com", "related-type", "mail_template", "related-id", source_id, NULL);
	return message;
}
void venture_email_design_preview_register(void)
{
	static const VentureMailPreviewKind kind = {
		"email-design", "Designed email", "mail", "mail_template", "mail_template", FALSE, design_preview_build
	};
	venture_mail_preview_register(&kind);
}

/* --- Actions the assistant is offered -------------------------------------------------- */

static VentureContext *action_context(VentureAction *action, GError **error)
{
	VentureContext *context = venture_email_design_context(venture_action_get_data(action));
	if (context == NULL)
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Email design is unavailable");
	return context;
}
static const gchar *param_string(GHashTable *params, const gchar *name)
{
	JsonNode *node = g_hash_table_lookup(params, name);
	return node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}
static gint64 param_int(GHashTable *params, const gchar *name)
{
	JsonNode *node = g_hash_table_lookup(params, name);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node)) return 0;
	if (json_node_get_value_type(node) == G_TYPE_INT64) return json_node_get_int(node);
	if (json_node_get_value_type(node) == G_TYPE_STRING) return g_ascii_strtoll(json_node_get_string(node), NULL, 10);
	return 0;
}
static gboolean design_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)entity; (void)actor;
	return action_context(action, error) != NULL;
}
static VentureEntity *design_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureContext *context = action_context(action, error);
	g_autofree gchar *name = NULL;
	g_autoptr(VentureEmailDesignResult) result = NULL;
	gint64 org;
	if (context == NULL)
		return NULL;
	g_object_get(action, "name", &name, NULL);
	org = venture_entity_get_organization_id(entity);
	if (org <= 0)
		org = param_int(params, "organization_id") > 0 ? param_int(params, "organization_id") : venture_context_get_default_organization_id(context);
	if (g_str_equal(name, "design_email"))
		result = venture_email_design_from_brief(context, org, param_int(params, "template_id"), param_string(params, "name"),
			param_string(params, "brief"), actor, error);
	else if (g_str_equal(name, "edit_design"))
		result = venture_email_design_edit(context, org, venture_entity_get_id(entity), param_string(params, "instruction"), actor, error);
	else if (g_str_equal(name, "undo_design")) {
		g_autoptr(VentureEntity) version = venture_email_design_undo(context, org, venture_entity_get_id(entity), actor, error);
		return version != NULL ? load_template(context, org, venture_entity_get_id(entity), TRUE, error) : NULL;
	} else if (g_str_equal(name, "rebuild_designs")) {
		gint changed = venture_email_design_rebuild(context, org, actor, error);
		VentureEntity *answer;
		g_autofree gchar *summary = NULL;
		if (changed < 0)
			return NULL;
		summary = g_strdup_printf("Rebuilt %d designed template%s", changed, changed == 1 ? "" : "s");
		answer = g_object_new(VENTURE_TYPE_MAIL_TEMPLATE, "organization-id", org, "name", summary, NULL);
		return answer;
	}
	if (result == NULL)
		return NULL;
	return g_object_ref(result->template);
}
static void register_action(VentureDatabase *database, const gchar *name, const gchar *label, const gchar *description,
	gboolean type_level, gboolean stageable, GPtrArray *parameters)
{
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		"type-name", "mail_template", "name", name, "label", label, "description", description,
		"parameters", parameters, "stageable", stageable, "type-level", type_level,
		"service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
		design_allowed, design_invoke, database, NULL, &error))
		g_error("Email design action %s: %s", name, error->message);
}
static VentureFieldSpec *parameter(const gchar *name, const gchar *label, VentureFieldKind kind, gboolean required)
{
	VentureFieldSpec *spec = venture_field_spec_new(name, label, kind);
	spec->required = required;
	return spec;
}
static void register_actions(VentureDatabase *database)
{
	g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_ptr_array_add(parameters, parameter("organization_id", "Organization", VENTURE_FIELD_KIND_INTEGER, FALSE));
	g_ptr_array_add(parameters, parameter("brief", "What the email is for, in plain words", VENTURE_FIELD_KIND_TEXT, TRUE));
	g_ptr_array_add(parameters, parameter("template_id", "A template to redesign; leave out for a new one", VENTURE_FIELD_KIND_INTEGER, FALSE));
	g_ptr_array_add(parameters, parameter("name", "A new template's name", VENTURE_FIELD_KIND_STRING, FALSE));
	register_action(database, "design_email", "Design an email from a brief",
		"Design an email from a plain-words brief using the organization's brand, logo, images and products; "
		"prices, offers and dates come only from the brief and the records. Saved as a new design version", TRUE, TRUE, parameters);
	g_ptr_array_set_size(parameters, 0);
	g_ptr_array_add(parameters, parameter("instruction", "The change, in plain words: \"make the button bigger\"", VENTURE_FIELD_KIND_TEXT, TRUE));
	register_action(database, "edit_design", "Change the design by words",
		"Change a template's email design as described; saved as a new version that undo_design reverses", FALSE, TRUE, parameters);
	g_ptr_array_set_size(parameters, 0);
	register_action(database, "undo_design", "Undo the last design change",
		"Put a template's design back to its previous version exactly", FALSE, TRUE, parameters);
	g_ptr_array_add(parameters, parameter("organization_id", "Organization", VENTURE_FIELD_KIND_INTEGER, FALSE));
	register_action(database, "rebuild_designs", "Rebuild designed emails",
		"Rebuild every designed template of the organization with its current brand, sections and images", TRUE, FALSE, parameters);
}

void venture_email_design_install(VentureContext *context)
{
	VentureDatabase *database;
	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	database = venture_context_get_database(context);
	{
		GWeakRef *reference = g_new0(GWeakRef, 1);
		g_weak_ref_init(reference, context);
		g_object_set_data_full(G_OBJECT(database), CONTEXT_KEY, reference, context_free);
	}
	/* Several contexts over one database (the tests) add nothing twice. */
	if (g_object_get_data(G_OBJECT(database), INSTALLED_KEY) != NULL)
		return;
	g_object_set_data(G_OBJECT(database), INSTALLED_KEY, GINT_TO_POINTER(1));
	venture_database_add_save_validator(database, VENTURE_TYPE_MAIL_TEMPLATE, template_validate, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_EMAIL_SECTION, section_validate, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_EMAIL_BRAND, brand_validate, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_EMAIL_DESIGN_VERSION, version_validate, NULL, NULL);
	venture_hosted_image_install(database);
	register_actions(database);
}
