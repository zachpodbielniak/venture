/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Included by the web server, after the mail preview pages whose frames
 * it reuses: the email builder, image upload and the public image address.
 *
 * Every page and write is for an owner or administrator of the template's
 * organization. The image address is the exception, on purpose: a mail
 * app fetching a picture sends no cookie, so the unguessable token in the
 * address is the only credential, and it works only under its own
 * organization. */

static const gint venture_email_design_web_roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
static const gchar *const venture_email_design_web_types[] = {
	"header", "text", "image", "button", "columns", "divider", "social", "video", "footer", "section", NULL
};

/* The person, an owner or administrator of @org, or a refusal. */
static gboolean
venture_email_design_web_manager(VentureWebServer *self, HtmxRequest *request, gint64 org,
	VentureAuthPrincipal **out, GError **error)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = venture_auth_authenticate(self->auth, request);
	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(database), principal, org,
		venture_email_design_web_roles, G_N_ELEMENTS(venture_email_design_web_roles))) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
			"Designing email needs an owner or administrator of the organization");
		return FALSE;
	}
	*out = g_steal_pointer(&principal);
	return TRUE;
}

/* The template in the path, and its organization's manager. */
static VentureEntity *
venture_email_design_web_template(VentureWebServer *self, HtmxRequest *request, GHashTable *params,
	VentureAuthPrincipal **principal, GError **error)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	const gchar *text = g_hash_table_lookup(params, "id");
	g_autoptr(VentureEntity) template = NULL;
	gint64 id = 0;
	if (text != NULL && g_ascii_string_to_signed(text, 10, 1, G_MAXINT64, &id, NULL))
		template = venture_database_get(database, VENTURE_TYPE_MAIL_TEMPLATE, id, NULL);
	if (template == NULL || venture_entity_is_deleted(template)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Mail template not found");
		return NULL;
	}
	if (!venture_email_design_web_manager(self, request, venture_entity_get_organization_id(template), principal, error))
		return NULL;
	return g_steal_pointer(&template);
}

static const gchar *
venture_email_design_web_label(const gchar *type)
{
	static const gchar *const labels[] = { "Header", "Text", "Image", "Button", "Columns", "Divider", "Social links", "Video", "Footer", "Section", NULL };
	guint i;
	for (i = 0; venture_email_design_web_types[i]; i++)
		if (g_strcmp0(venture_email_design_web_types[i], type) == 0)
			return labels[i];
	return "Block";
}

/* A form posting to the builder: htmx swaps the builder in place, and
 * without script it is an ordinary post that comes back to the page. */
static void
venture_email_design_web_form(GString *content, gint64 id, const gchar *action, const gchar *extra)
{
	g_string_append_printf(content, "<form method=\"post\" action=\"/email-design/%" G_GINT64_FORMAT "/%s\" "
		"hx-post=\"/email-design/%" G_GINT64_FORMAT "/%s\" hx-target=\"#email-builder\" hx-select=\"#email-builder\" hx-swap=\"outerHTML\"%s>",
		id, action, id, action, extra != NULL ? extra : "");
}

static void
venture_email_design_web_block(GString *content, gint64 id, JsonNode *block, guint index, guint count)
{
	JsonObject *object = json_node_get_object(block);
	const gchar *type = json_object_get_string_member(object, "type");
	const gchar *summary = NULL;
	g_autofree gchar *pretty = NULL;
	{
		g_autoptr(JsonGenerator) generator = json_generator_new();
		json_generator_set_root(generator, block);
		json_generator_set_pretty(generator, TRUE);
		json_generator_set_indent(generator, 2);
		pretty = json_generator_to_data(generator, NULL);
	}
	if (json_object_has_member(object, "heading")) summary = json_object_get_string_member(object, "heading");
	else if (json_object_has_member(object, "label")) summary = json_object_get_string_member(object, "label");
	else if (json_object_has_member(object, "alt")) summary = json_object_get_string_member(object, "alt");
	else if (json_object_has_member(object, "body")) summary = json_object_get_string_member(object, "body");
	g_string_append_printf(content, "<li class=\"email-block card\" data-block-index=\"%u\" style=\"padding:.6rem .8rem;margin:0 0 .6rem\">"
		"<div style=\"display:flex;gap:.5rem;align-items:center;flex-wrap:wrap\"><strong>%u. ", index, index + 1);
	venture_html_escape_append(content, venture_email_design_web_label(type));
	g_string_append(content, "</strong> <span class=\"muted\" style=\"flex:1;min-width:8rem;overflow:hidden;text-overflow:ellipsis;white-space:nowrap\">");
	if (summary != NULL) venture_html_escape_append(content, summary);
	g_string_append(content, "</span>");
	if (index > 0) {
		venture_email_design_web_form(content, id, "blocks", " style=\"display:inline\"");
		g_string_append_printf(content, "<input type=\"hidden\" name=\"op\" value=\"up\"><input type=\"hidden\" name=\"index\" value=\"%u\">"
			"<button class=\"btn btn-sm\" type=\"submit\" aria-label=\"Move block %u up\">&uarr;</button></form>", index, index + 1);
	}
	if (index + 1 < count) {
		venture_email_design_web_form(content, id, "blocks", " style=\"display:inline\"");
		g_string_append_printf(content, "<input type=\"hidden\" name=\"op\" value=\"down\"><input type=\"hidden\" name=\"index\" value=\"%u\">"
			"<button class=\"btn btn-sm\" type=\"submit\" aria-label=\"Move block %u down\">&darr;</button></form>", index, index + 1);
	}
	venture_email_design_web_form(content, id, "blocks", " style=\"display:inline\"");
	g_string_append_printf(content, "<input type=\"hidden\" name=\"op\" value=\"remove\"><input type=\"hidden\" name=\"index\" value=\"%u\">"
		"<button class=\"btn btn-sm\" type=\"submit\" aria-label=\"Remove block %u\">Remove</button></form></div>", index, index + 1);
	g_string_append(content, "<details><summary>Edit</summary>");
	venture_email_design_web_form(content, id, "blocks", NULL);
	g_string_append_printf(content, "<input type=\"hidden\" name=\"op\" value=\"edit\"><input type=\"hidden\" name=\"index\" value=\"%u\">"
		"<label>Block %u as JSON<textarea name=\"block\" rows=\"8\" spellcheck=\"false\" style=\"width:100%%;font-family:monospace\">", index, index + 1);
	venture_html_escape_append(content, pretty);
	g_string_append(content, "</textarea></label><button class=\"btn btn-sm\" type=\"submit\">Save block</button></form></details></li>");
}

static HtmxResponse *
venture_email_design_web_page(VentureWebServer *self, HtmxRequest *request, VentureEntity *template, const gchar *notice,
	gboolean negative, const gchar *ask)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(GString) content = g_string_new(NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMailPreview) preview = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) images = NULL;
	g_autoptr(VentureEntity) brand = NULL;
	g_autofree gchar *name = NULL, *subject = NULL, *design = NULL, *title = NULL;
	gint64 id = venture_entity_get_id(template), org = venture_entity_get_organization_id(template), version = 0;
	JsonArray *blocks = NULL;
	guint i;
	g_object_get(template, "name", &name, "subject", &subject, "design", &design, "design-version", &version, NULL);
	node = !venture_string_is_empty(design) ? json_from_string(design, NULL) : NULL;
	if (node != NULL && JSON_NODE_HOLDS_OBJECT(node))
		blocks = json_object_get_array_member(json_node_get_object(node), "blocks");
	g_string_append(content, "<div id=\"email-builder\" class=\"email-builder\">");
	g_string_append(content, "<h1>Email design: ");
	venture_html_escape_append(content, name);
	g_string_append(content, "</h1><p class=\"muted\">Subject: ");
	venture_html_escape_append(content, subject);
	if (version > 0)
		g_string_append_printf(content, " &middot; version %" G_GINT64_FORMAT, version);
	g_string_append_printf(content, " &middot; <a href=\"/mail/preview/email-design/%" G_GINT64_FORMAT "\">Every width, light and dark, and the checks</a>"
		" &middot; <a href=\"/e/mail_template/%" G_GINT64_FORMAT "\">Template record</a> &middot; ", id, id);
	{
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
		g_autoptr(VentureQuery) brands = venture_query_new(VENTURE_TYPE_EMAIL_BRAND);
		g_autoptr(GPtrArray) found = NULL;
		venture_query_set_organization(brands, org);
		venture_query_set_limit(brands, 1);
		found = venture_database_find(database, brands, NULL);
		if (found != NULL && found->len > 0)
			brand = g_object_ref(g_ptr_array_index(found, 0));
	}
	if (brand != NULL)
		g_string_append_printf(content, "<a href=\"/e/email_brand/%" G_GINT64_FORMAT "\">Brand: logo, colours, font</a></p>", venture_entity_get_id(brand));
	else
		g_string_append(content, "<a href=\"/e/email_brand/new\">Set up your brand: logo, colours, font</a></p>");
	if (notice != NULL) {
		g_string_append_printf(content, "<div class=\"notice%s\" role=\"%s\">", negative ? " negative" : "", negative ? "alert" : "status");
		venture_html_escape_append(content, notice);
		g_string_append(content, "</div>");
	}
	if (ask != NULL) {
		g_string_append(content, "<div class=\"notice\" role=\"status\"><p>");
		venture_html_escape_append(content, ask);
		g_string_append_printf(content, "</p><form method=\"post\" action=\"/email-design/logo-ask/dismiss\">"
			"<input type=\"hidden\" name=\"organization_id\" value=\"%" G_GINT64_FORMAT "\">"
			"<input type=\"hidden\" name=\"return_to\" value=\"/email-design/%" G_GINT64_FORMAT "\">"
			"<button class=\"btn btn-sm\" type=\"submit\">No logo, thanks</button></form></div>", org, id);
	}
	g_string_append(content, "<div class=\"email-builder-grid\" style=\"display:grid;grid-template-columns:repeat(auto-fit,minmax(340px,1fr));gap:1.5rem;align-items:start\">");
	/* The controls. */
	g_string_append(content, "<section class=\"email-builder-controls\">");
	g_string_append(content, "<h2>Design it from a brief</h2>");
	venture_email_design_web_form(content, id, "brief", NULL);
	g_string_append(content, "<label>What is this email for, in plain words?"
		"<textarea name=\"brief\" rows=\"3\" maxlength=\"2000\" placeholder=\"fall furnace tune-up, 10% off, ends Nov 15\" style=\"width:100%\"></textarea></label>"
		"<button class=\"btn\" type=\"submit\">Design it</button> <small>Uses your brand, images and products. Prices, offers and dates come only from your words and your records.</small></form>");
	if (blocks != NULL) {
		g_string_append(content, "<h2>Change it by words</h2>");
		venture_email_design_web_form(content, id, "edit", NULL);
		g_string_append(content, "<label>What should change?<input type=\"text\" name=\"instruction\" maxlength=\"1000\" "
			"placeholder=\"make the button bigger\" style=\"width:100%\"></label><button class=\"btn\" type=\"submit\">Change it</button></form>");
		venture_email_design_web_form(content, id, "undo", " style=\"margin-top:.5rem\"");
		g_string_append(content, "<button class=\"btn btn-sm\" type=\"submit\">Undo the last change</button></form>");
	}
	g_string_append(content, "<h2>Blocks</h2><ol class=\"email-blocks\" style=\"list-style:none;padding:0\">");
	for (i = 0; blocks != NULL && i < json_array_get_length(blocks); i++)
		venture_email_design_web_block(content, id, json_array_get_element(blocks, i), i, json_array_get_length(blocks));
	g_string_append(content, "</ol>");
	venture_email_design_web_form(content, id, "blocks", NULL);
	g_string_append(content, "<input type=\"hidden\" name=\"op\" value=\"add\"><label>Add a block <select name=\"type\">");
	for (i = 0; venture_email_design_web_types[i]; i++)
		g_string_append_printf(content, "<option value=\"%s\">%s</option>", venture_email_design_web_types[i],
			venture_email_design_web_label(venture_email_design_web_types[i]));
	g_string_append(content, "</select></label> <button class=\"btn btn-sm\" type=\"submit\">Add</button></form>");
	/* Images. */
	g_string_append(content, "<h2>Images</h2>");
	g_string_append_printf(content, "<form method=\"post\" action=\"/email-design/images\" enctype=\"multipart/form-data\">"
		"<input type=\"hidden\" name=\"organization_id\" value=\"%" G_GINT64_FORMAT "\">"
		"<input type=\"hidden\" name=\"return_to\" value=\"/email-design/%" G_GINT64_FORMAT "\">"
		"<label>Picture (PNG, JPEG, GIF or WebP, up to %d MB)<input type=\"file\" name=\"file\" accept=\"image/png,image/jpeg,image/gif,image/webp\"></label>"
		"<label>What it shows<input type=\"text\" name=\"alt\" maxlength=\"300\"></label>"
		"<button class=\"btn btn-sm\" type=\"submit\">Upload an image</button></form>", org, id, VENTURE_HOSTED_IMAGE_MAX_BYTES / (1024 * 1024));
	query = venture_query_new(VENTURE_TYPE_HOSTED_IMAGE);
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 12);
	images = venture_database_find(database, query, NULL);
	if (images != NULL && images->len > 0) {
		g_string_append(content, "<ul class=\"email-images\" style=\"list-style:none;padding:0;display:flex;flex-wrap:wrap;gap:.6rem\">");
		for (i = 0; i < images->len; i++) {
			VentureEntity *image = g_ptr_array_index(images, i);
			g_autofree gchar *url = venture_hosted_image_url(self->context, image), *alt = NULL;
			g_object_get(image, "alt-text", &alt, NULL);
			g_string_append(content, "<li style=\"width:110px;font-size:.8rem\"><img src=\"");
			venture_html_escape_append(content, url);
			g_string_append(content, "\" alt=\"");
			venture_html_escape_append(content, alt);
			g_string_append_printf(content, "\" width=\"110\" style=\"display:block;width:110px;height:72px;object-fit:contain;background:#8881;border-radius:4px\">"
				"image_id %" G_GINT64_FORMAT "</li>", venture_entity_get_id(image));
		}
		g_string_append(content, "</ul>");
	}
	g_string_append(content, "</section>");
	/* The live preview: lane 1's frames, so what is shown is what is sent. */
	g_string_append(content, "<section class=\"email-builder-preview\"><h2>Preview</h2>");
	preview = blocks != NULL ? venture_mail_preview_build(self->context, "email-design", org, id, NULL, NULL, &error) : NULL;
	if (preview != NULL) {
		g_autofree gchar *base = g_strdup_printf("/mail/preview/email-design/%" G_GINT64_FORMAT, id);
		g_string_append(content, "<ul class=\"mail-preview-checks\">");
		for (i = 0; i < preview->checks->len; i++) {
			const VentureMailCheck *check = g_ptr_array_index(preview->checks, i);
			g_string_append_printf(content, "<li data-check=\"%s\" data-status=\"%s\"><strong>%s</strong> ",
				check->id, venture_web_mail_preview_status(check->status), venture_web_mail_preview_status(check->status));
			venture_html_escape_append(content, check->message);
			g_string_append(content, "</li>");
		}
		g_string_append(content, "</ul>");
		venture_web_mail_preview_frame(content, preview, 390, "Phone", "light", base);
		venture_web_mail_preview_frame(content, preview, 390, "Phone", "dark", base);
	} else {
		g_string_append(content, "<p>");
		venture_html_escape_append(content, error != NULL ? error->message : "Add a block, or design it from a brief, to see it here.");
		g_string_append(content, "</p>");
	}
	g_string_append(content, "</section></div></div>");
	title = g_strdup_printf("Email design: %s", name != NULL ? name : "");
	return venture_web_html_response(venture_web_page(self, request, "/email-design", title, content->str), 200);
}

/* The answer to a builder post: the page again for htmx, which swaps it
 * in; a redirect back to it for an ordinary form. */
static HtmxResponse *
venture_email_design_web_after(VentureWebServer *self, HtmxRequest *request, gint64 id, const gchar *notice, const gchar *ask)
{
	g_autofree gchar *location = g_strdup_printf("/email-design/%" G_GINT64_FORMAT, id);
	HtmxResponse *response;
	if (htmx_request_is_htmx(request)) {
		g_autoptr(VentureEntity) template = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_MAIL_TEMPLATE, id, NULL);
		if (template != NULL)
			return venture_email_design_web_page(self, request, template, notice, FALSE, ask);
	}
	response = htmx_response_new();
	htmx_response_set_status(response, 303);
	htmx_response_add_header(response, "Location", location);
	return response;
}
static HtmxResponse *
venture_email_design_web_refused(VentureWebServer *self, HtmxRequest *request, VentureEntity *template, const GError *error)
{
	if (htmx_request_is_htmx(request) && template != NULL && error != NULL && error->code != VENTURE_ERROR_PERMISSION_DENIED) {
		HtmxResponse *response = venture_email_design_web_page(self, request, template, error->message, TRUE, NULL);
		return response;
	}
	return venture_web_error_response(error);
}

/* GET /email-design: the designed templates, and a new one. */
static HtmxResponse *
venture_email_design_web_list(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GString) content = g_string_new(NULL);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	guint i, shown = 0;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "mail");
	if (gate != NULL) return gate;
	query = venture_query_new(VENTURE_TYPE_MAIL_TEMPLATE);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 200);
	rows = venture_database_find(venture_context_get_database(self->context), query, NULL);
	g_string_append(content, "<h1>Email design</h1><p>Designed emails are mail templates built from blocks, in your brand. "
		"Write a brief and let the assistant design one, or build it block by block. "
		"<a href=\"/e/email_brand\">Brand</a> &middot; <a href=\"/e/email_section\">Shared sections</a> &middot; <a href=\"/e/hosted_image\">Images</a></p>"
		"<form method=\"post\" action=\"/email-design/new\"><label>New design, named <input type=\"text\" name=\"name\" required maxlength=\"200\"></label> ");
	venture_web_ai_append_organization(self, content, "email-design-organization", "in ");
	g_string_append(content, " <button class=\"btn\" type=\"submit\">Start</button></form><table><thead><tr><th>Template</th><th>Subject</th></tr></thead><tbody>");
	for (i = 0; rows != NULL && i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *name = NULL, *subject = NULL, *design = NULL;
		g_object_get(row, "name", &name, "subject", &subject, "design", &design, NULL);
		if (venture_string_is_empty(design)) continue;
		shown++;
		g_string_append_printf(content, "<tr><td><a href=\"/email-design/%" G_GINT64_FORMAT "\">", venture_entity_get_id(row));
		venture_html_escape_append(content, name);
		g_string_append(content, "</a></td><td>");
		venture_html_escape_append(content, subject);
		g_string_append(content, "</td></tr>");
	}
	if (shown == 0)
		g_string_append(content, "<tr><td colspan=\"2\">No designed emails yet.</td></tr>");
	g_string_append(content, "</tbody></table><p><small>After changing the brand or a shared section, rebuild the designs that use it: "
		"<code>venturectl act mail_template rebuild_designs organization_id=N</code>, or open each and save a change.</small></p>");
	return venture_web_html_response(venture_web_page(self, request, "/email-design", "Email design", content->str), 200);
}

/* POST /email-design/new: a template with a plain starting design. */
static HtmxResponse *
venture_email_design_web_new(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(VentureEntity) template = NULL, version = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *name = htmx_request_get_form_value(request, "name");
	gint64 org;
	VentureActor actor;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "mail");
	if (gate != NULL) return gate;
	org = venture_web_ai_organization(self, htmx_request_get_form_value(request, "organization_id"), &error);
	if (org <= 0 || !venture_email_design_web_manager(self, request, org, &principal, &error))
		return venture_web_error_response(error);
	if (venture_string_is_empty(name)) {
		venture_set_error_validation(&error, "name", "give the design a name");
		return venture_web_error_response(error);
	}
	scope = venture_access_policy_enter_organization(venture_database_get_access_policy(database), principal, org);
	venture_auth_to_actor(principal, &actor);
	template = g_object_new(VENTURE_TYPE_MAIL_TEMPLATE, "organization-id", org, "name", name, "subject", name, NULL);
	version = venture_email_design_save(self->context, template,
		"{\"blocks\":[{\"type\":\"header\"},{\"type\":\"text\",\"heading\":\"A heading\",\"body\":\"Write your message here.\"},{\"type\":\"footer\"}]}",
		"manual", "Started", &actor, &error);
	if (version == NULL)
		return venture_web_error_response(error);
	return venture_email_design_web_after(self, request, venture_entity_get_id(template), NULL, NULL);
}

/* GET /email-design/:id - the builder. */
static HtmxResponse *
venture_email_design_web_builder(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) template = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "mail");
	if (gate != NULL) return gate;
	template = venture_email_design_web_template(self, request, params, &principal, &error);
	if (template == NULL)
		return venture_web_error_response(error);
	return venture_email_design_web_page(self, request, template, htmx_request_get_query_param(request, "notice"), FALSE, NULL);
}

/* A block a person adds starts from what the organization has: its
 * newest image, its website. Nothing is made up to fill a field. */
static JsonNode *
venture_email_design_web_new_block(VentureWebServer *self, gint64 org, const gchar *type, GError **error)
{
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureEmailLook) look = venture_email_look_load(self->context, org);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *json = NULL;
	gint64 image = 0, section = 0;
	g_autofree gchar *alt = NULL;
	query = venture_query_new(VENTURE_TYPE_HOSTED_IMAGE);
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, 1);
	rows = venture_database_find(database, query, NULL);
	if (rows != NULL && rows->len > 0) {
		image = venture_entity_get_id(g_ptr_array_index(rows, 0));
		g_object_get(g_ptr_array_index(rows, 0), "alt-text", &alt, NULL);
	}
	g_clear_pointer(&rows, g_ptr_array_unref);
	g_clear_object(&query);
	query = venture_query_new(VENTURE_TYPE_EMAIL_SECTION);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 1);
	rows = venture_database_find(database, query, NULL);
	if (rows != NULL && rows->len > 0)
		section = venture_entity_get_id(g_ptr_array_index(rows, 0));
	if (venture_string_is_empty(alt)) {
		g_free(alt);
		alt = g_strdup("Picture");
	}
	{
		g_autofree gchar *quoted_alt = NULL, *site = NULL;
		g_autoptr(JsonNode) a = json_node_new(JSON_NODE_VALUE), s = json_node_new(JSON_NODE_VALUE);
		json_node_set_string(a, alt);
		quoted_alt = json_to_string(a, FALSE);
		json_node_set_string(s, look->website != NULL ? look->website : "");
		site = json_to_string(s, FALSE);
		if (g_str_equal(type, "header")) json = g_strdup("{\"type\":\"header\"}");
		else if (g_str_equal(type, "text")) json = g_strdup("{\"type\":\"text\",\"heading\":\"A heading\",\"body\":\"Write your message here.\"}");
		else if (g_str_equal(type, "divider")) json = g_strdup("{\"type\":\"divider\"}");
		else if (g_str_equal(type, "footer")) json = g_strdup("{\"type\":\"footer\"}");
		else if (g_str_equal(type, "columns"))
			json = g_strdup("{\"type\":\"columns\",\"columns\":[{\"blocks\":[{\"type\":\"text\",\"heading\":\"Left\",\"body\":\"Write here.\"}]},"
				"{\"blocks\":[{\"type\":\"text\",\"heading\":\"Right\",\"body\":\"Write here.\"}]}]}");
		else if (g_str_equal(type, "image") && image > 0)
			json = g_strdup_printf("{\"type\":\"image\",\"image_id\":%" G_GINT64_FORMAT ",\"alt\":%s}", image, quoted_alt);
		else if (g_str_equal(type, "button") && look->website != NULL)
			json = g_strdup_printf("{\"type\":\"button\",\"label\":\"Visit us\",\"url\":%s}", site);
		else if (g_str_equal(type, "social") && look->website != NULL)
			json = g_strdup_printf("{\"type\":\"social\",\"links\":[{\"network\":\"website\",\"url\":%s}]}", site);
		else if (g_str_equal(type, "video") && image > 0 && look->website != NULL)
			json = g_strdup_printf("{\"type\":\"video\",\"url\":%s,\"poster_image_id\":%" G_GINT64_FORMAT ",\"alt\":%s}", site, image, quoted_alt);
		else if (g_str_equal(type, "section") && section > 0)
			json = g_strdup_printf("{\"type\":\"section\",\"section_id\":%" G_GINT64_FORMAT "}", section);
	}
	if (json == NULL) {
		if (g_str_equal(type, "image") || g_str_equal(type, "video"))
			venture_set_error_validation(error, "Block", "upload an image first%s", g_str_equal(type, "video") ? ", and set your website in the brand" : "");
		else if (g_str_equal(type, "section"))
			venture_set_error_validation(error, "Block", "save a shared section first");
		else if (g_str_equal(type, "button") || g_str_equal(type, "social"))
			venture_set_error_validation(error, "Block", "set your website in the brand first; a button needs an address to go to");
		else
			venture_set_error_validation(error, "Block", "there is no block type \"%s\"", type != NULL ? type : "");
		return NULL;
	}
	return json_from_string(json, NULL);
}

/* POST /email-design/:id/blocks - add, remove, move or edit one block;
 * every change is a version. */
static HtmxResponse *
venture_email_design_web_blocks(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(VentureEntity) template = NULL, version = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *design = NULL, *changed = NULL, *note = NULL;
	const gchar *op = htmx_request_get_form_value(request, "op");
	const gchar *index_text = htmx_request_get_form_value(request, "index");
	JsonArray *blocks;
	gint64 index = -1;
	guint count;
	VentureActor actor;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "mail");
	if (gate != NULL) return gate;
	template = venture_email_design_web_template(self, request, params, &principal, &error);
	if (template == NULL)
		return venture_web_error_response(error);
	g_object_get(template, "design", &design, NULL);
	node = json_from_string(!venture_string_is_empty(design) ? design : "{\"blocks\":[]}", NULL);
	if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node) || !json_object_has_member(json_node_get_object(node), "blocks")) {
		g_clear_pointer(&node, json_node_unref);
		node = json_from_string("{\"blocks\":[]}", NULL);
	}
	blocks = json_object_get_array_member(json_node_get_object(node), "blocks");
	count = json_array_get_length(blocks);
	if (index_text != NULL && !g_ascii_string_to_signed(index_text, 10, 0, G_MAXINT32, &index, NULL))
		index = -1;
	if (g_strcmp0(op, "add") == 0) {
		JsonNode *block = venture_email_design_web_new_block(self, venture_entity_get_organization_id(template),
			htmx_request_get_form_value(request, "type"), &error);
		if (block == NULL)
			return venture_email_design_web_refused(self, request, template, error);
		json_array_add_element(blocks, block);
		note = g_strdup_printf("Added a %s block", htmx_request_get_form_value(request, "type"));
	} else if (index < 0 || (guint)index >= count) {
		venture_set_error_validation(&error, "Block", "there is no block %" G_GINT64_FORMAT, index + 1);
		return venture_email_design_web_refused(self, request, template, error);
	} else if (g_strcmp0(op, "remove") == 0) {
		json_array_remove_element(blocks, (guint)index);
		note = g_strdup_printf("Removed block %" G_GINT64_FORMAT, index + 1);
	} else if (g_strcmp0(op, "up") == 0 || g_strcmp0(op, "down") == 0) {
		gint64 other = g_strcmp0(op, "up") == 0 ? index - 1 : index + 1;
		JsonNode *a, *b;
		if (other < 0 || (guint)other >= count) {
			venture_set_error_validation(&error, "Block", "block %" G_GINT64_FORMAT " cannot move that way", index + 1);
			return venture_email_design_web_refused(self, request, template, error);
		}
		a = json_node_copy(json_array_get_element(blocks, (guint)index));
		b = json_node_copy(json_array_get_element(blocks, (guint)other));
		{
			/* JsonArray has no set: rebuild it in the new order. */
			JsonArray *reordered = json_array_new();
			guint i;
			for (i = 0; i < count; i++) {
				if (i == (guint)index) json_array_add_element(reordered, json_node_copy(b));
				else if (i == (guint)other) json_array_add_element(reordered, json_node_copy(a));
				else json_array_add_element(reordered, json_node_copy(json_array_get_element(blocks, i)));
			}
			json_object_set_array_member(json_node_get_object(node), "blocks", reordered);
		}
		json_node_unref(a);
		json_node_unref(b);
		note = g_strdup_printf("Moved block %" G_GINT64_FORMAT " %s", index + 1, op);
	} else if (g_strcmp0(op, "edit") == 0) {
		JsonNode *block = json_from_string(htmx_request_get_form_value(request, "block"), NULL);
		JsonArray *edited = json_array_new();
		guint i;
		if (block == NULL || !JSON_NODE_HOLDS_OBJECT(block)) {
			if (block != NULL) json_node_unref(block);
			json_array_unref(edited);
			venture_set_error_validation(&error, "Block", "write the block as a JSON object");
			return venture_email_design_web_refused(self, request, template, error);
		}
		for (i = 0; i < count; i++)
			json_array_add_element(edited, i == (guint)index ? json_node_ref(block) : json_node_copy(json_array_get_element(blocks, i)));
		json_node_unref(block);
		json_object_set_array_member(json_node_get_object(node), "blocks", edited);
		note = g_strdup_printf("Edited block %" G_GINT64_FORMAT, index + 1);
	} else {
		venture_set_error_validation(&error, "Block", "add, remove, up, down or edit");
		return venture_email_design_web_refused(self, request, template, error);
	}
	changed = json_to_string(node, FALSE);
	scope = venture_access_policy_enter_organization(venture_database_get_access_policy(database), principal,
		venture_entity_get_organization_id(template));
	venture_auth_to_actor(principal, &actor);
	version = venture_email_design_save(self->context, template, changed, "manual", note, &actor, &error);
	if (version == NULL)
		return venture_email_design_web_refused(self, request, template, error);
	return venture_email_design_web_after(self, request, venture_entity_get_id(template), note, NULL);
}

/* POST /email-design/:id/brief, /edit and /undo - the designer, from the page. */
static HtmxResponse *
venture_email_design_web_ai(HtmxRequest *request, GHashTable *params, gpointer data, const gchar *what)
{
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(VentureEntity) template = NULL, version = NULL;
	g_autoptr(VentureEmailDesignResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) notice = g_string_new(NULL);
	gint64 id, org;
	guint i;
	VentureActor actor;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "mail");
	if (gate != NULL) return gate;
	template = venture_email_design_web_template(self, request, params, &principal, &error);
	if (template == NULL)
		return venture_web_error_response(error);
	id = venture_entity_get_id(template);
	org = venture_entity_get_organization_id(template);
	scope = venture_access_policy_enter_organization(venture_database_get_access_policy(database), principal, org);
	venture_auth_to_actor(principal, &actor);
	if (g_str_equal(what, "undo")) {
		version = venture_email_design_undo(self->context, org, id, &actor, &error);
		if (version == NULL)
			return venture_email_design_web_refused(self, request, template, error);
		return venture_email_design_web_after(self, request, id, "Undone: the previous version is back", NULL);
	}
	if (g_str_equal(what, "brief"))
		result = venture_email_design_from_brief(self->context, org, id, NULL, htmx_request_get_form_value(request, "brief"), &actor, &error);
	else
		result = venture_email_design_edit(self->context, org, id, htmx_request_get_form_value(request, "instruction"), &actor, &error);
	if (result == NULL)
		return venture_email_design_web_refused(self, request, template, error);
	g_string_append(notice, g_str_equal(what, "brief") ? "Designed from your brief." : "Changed as asked.");
	g_string_append(notice, " Undo puts the previous version back.");
	for (i = 0; i < result->stripped->len; i++) {
		g_string_append(notice, i == 0 ? " Removed because it was not in your words or your records: " : "; ");
		g_string_append(notice, g_ptr_array_index(result->stripped, i));
	}
	if (!htmx_request_is_htmx(request) && result->ask == NULL && result->stripped->len == 0)
		return venture_email_design_web_after(self, request, id, notice->str, NULL);
	if (!htmx_request_is_htmx(request)) {
		/* An ordinary post that has something to say shows the page now:
		 * the ask is made once and would be lost on a redirect. */
		g_autoptr(VentureEntity) fresh = venture_database_get(database, VENTURE_TYPE_MAIL_TEMPLATE, id, NULL);
		return venture_email_design_web_page(self, request, fresh != NULL ? fresh : template, notice->str, FALSE, result->ask);
	}
	return venture_email_design_web_after(self, request, id, notice->str, result->ask);
}
static HtmxResponse *
venture_email_design_web_brief(HtmxRequest *request, GHashTable *params, gpointer data)
{
	return venture_email_design_web_ai(request, params, data, "brief");
}
static HtmxResponse *
venture_email_design_web_edit(HtmxRequest *request, GHashTable *params, gpointer data)
{
	return venture_email_design_web_ai(request, params, data, "edit");
}
static HtmxResponse *
venture_email_design_web_undo(HtmxRequest *request, GHashTable *params, gpointer data)
{
	return venture_email_design_web_ai(request, params, data, "undo");
}

/* POST /email-design/logo-ask/dismiss - "no logo, thanks", remembered. */
static HtmxResponse *
venture_email_design_web_dismiss(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *back = htmx_request_get_form_value(request, "return_to");
	gint64 org;
	VentureActor actor;
	HtmxResponse *response, *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL) return gate;
	org = venture_web_ai_organization(self, htmx_request_get_form_value(request, "organization_id"), &error);
	if (org <= 0 || !venture_email_design_web_manager(self, request, org, &principal, &error))
		return venture_web_error_response(error);
	scope = venture_access_policy_enter_organization(venture_database_get_access_policy(venture_context_get_database(self->context)), principal, org);
	venture_auth_to_actor(principal, &actor);
	if (!venture_email_design_dismiss_logo_ask(self->context, org, &actor, &error))
		return venture_web_error_response(error);
	response = htmx_response_new();
	htmx_response_set_status(response, 303);
	htmx_response_add_header(response, "Location", back != NULL && g_str_has_prefix(back, "/email-design") ? back : "/email-design");
	return response;
}

/* POST /email-design/images - multipart: file, alt, organization_id. */
static HtmxResponse *
venture_email_design_web_upload(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GPtrArray) files = NULL;
	g_autoptr(GHashTable) form = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) image = NULL;
	g_autoptr(JsonObject) object = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *url = NULL, *type = NULL;
	HtmxUploadedFile *file = NULL;
	const gchar *back;
	gint64 org, width = 0, height = 0;
	guint i;
	VentureActor actor;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL) return gate;
	gate = venture_web_require_module_ui(self, request, "mail");
	if (gate != NULL) return gate;
	files = htmx_uploaded_file_parse_multipart(htmx_request_get_content_type(request), htmx_request_get_body_bytes(request), &form, &error);
	if (files == NULL) {
		if (error == NULL)
			venture_set_error_validation(&error, "Image", "no file arrived");
		return venture_web_error_response(error);
	}
	org = venture_web_ai_organization(self, form != NULL ? g_hash_table_lookup(form, "organization_id") : NULL, &error);
	if (org <= 0 || !venture_email_design_web_manager(self, request, org, &principal, &error))
		return venture_web_error_response(error);
	for (i = 0; i < files->len && file == NULL; i++)
		if (g_strcmp0(htmx_uploaded_file_get_field_name(g_ptr_array_index(files, i)), "file") == 0)
			file = g_ptr_array_index(files, i);
	if (file == NULL) {
		venture_set_error_validation(&error, "Image", "no file arrived");
		return venture_web_error_response(error);
	}
	scope = venture_access_policy_enter_organization(venture_database_get_access_policy(venture_context_get_database(self->context)), principal, org);
	venture_auth_to_actor(principal, &actor);
	image = venture_hosted_image_store(self->context, org, htmx_uploaded_file_get_filename(file), htmx_uploaded_file_get_data(file),
		form != NULL ? g_hash_table_lookup(form, "alt") : NULL, &actor, &error);
	if (image == NULL)
		return venture_web_error_response(error);
	back = form != NULL ? g_hash_table_lookup(form, "return_to") : NULL;
	if (back != NULL && g_str_has_prefix(back, "/email-design/")) {
		HtmxResponse *response = htmx_response_new();
		htmx_response_set_status(response, 303);
		htmx_response_add_header(response, "Location", back);
		return response;
	}
	url = venture_hosted_image_url(self->context, image);
	g_object_get(image, "content-type", &type, "width", &width, "height", &height, NULL);
	object = json_object_new();
	json_object_set_int_member(object, "id", venture_entity_get_id(image));
	json_object_set_string_member(object, "url", url);
	json_object_set_string_member(object, "content_type", type);
	json_object_set_int_member(object, "width", width);
	json_object_set_int_member(object, "height", height);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(node, object);
	return venture_web_json_response(node, 201);
}

/* GET /i/:org/:file - a hosted image, to anyone who has its address. */
static HtmxResponse *
venture_email_design_web_image(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	const gchar *org_text = g_hash_table_lookup(params, "org"), *content_type = NULL;
	HtmxResponse *response;
	gint64 org = 0;
	(void)request;
	if (org_text != NULL && g_ascii_string_to_signed(org_text, 10, 1, G_MAXINT64, &org, NULL))
		bytes = venture_hosted_image_read(self->context, org, g_hash_table_lookup(params, "file"), &content_type, &error);
	if (bytes == NULL) {
		response = htmx_response_new_with_content("Not found");
		htmx_response_set_status(response, 404);
		htmx_response_set_content_type(response, "text/plain; charset=utf-8");
		return response;
	}
	response = htmx_response_new();
	htmx_response_set_status(response, 200);
	htmx_response_set_bytes(response, bytes);
	htmx_response_set_content_type(response, content_type);
	/* An image never changes under its address (a new upload is a new
	 * token), so every cache may keep it; and the bytes are only ever an
	 * image, whatever a browser would guess. */
	htmx_response_add_header(response, "Cache-Control", "public, max-age=31536000, immutable");
	htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");
	htmx_response_add_header(response, "Content-Security-Policy", "default-src 'none'; sandbox");
	return response;
}

static void
venture_email_design_pages_register(VentureWebServer *self)
{
	static const struct {
		HtmxMethod method;
		const gchar *path;
		HtmxRouteCallback handler;
	} routes[] = {
		{ HTMX_METHOD_GET, "/i/:org/:file", venture_email_design_web_image },
		{ HTMX_METHOD_GET, "/email-design", venture_email_design_web_list },
		{ HTMX_METHOD_POST, "/email-design/new", venture_email_design_web_new },
		{ HTMX_METHOD_POST, "/email-design/images", venture_email_design_web_upload },
		{ HTMX_METHOD_POST, "/email-design/logo-ask/dismiss", venture_email_design_web_dismiss },
		{ HTMX_METHOD_GET, "/email-design/:id", venture_email_design_web_builder },
		{ HTMX_METHOD_POST, "/email-design/:id/blocks", venture_email_design_web_blocks },
		{ HTMX_METHOD_POST, "/email-design/:id/brief", venture_email_design_web_brief },
		{ HTMX_METHOD_POST, "/email-design/:id/edit", venture_email_design_web_edit },
		{ HTMX_METHOD_POST, "/email-design/:id/undo", venture_email_design_web_undo },
	};
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(routes); i++)
		venture_web_server_add_classified_route(self, routes[i].method, routes[i].path,
			VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE, routes[i].handler, self);
}
