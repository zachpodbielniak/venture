/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <glib/gstdio.h>
/*
 * Owner filing. The generic capture_item form asks for lifecycle fields and
 * has no photo. These pages take a picture, file it, and convert it through
 * VentureCaptureService. A post answers 303 so a refresh does not file twice.
 */

static HtmxResponse *
capture_see_other(const gchar *path)
{
	HtmxResponse *response;

	response = htmx_response_new();
	htmx_response_set_status(response, 303);
	htmx_response_add_header(response, "Location", path);
	return response;
}

static HtmxResponse *
capture_fail(VentureWebServer *self, HtmxRequest *request, const GError *error)
{
	g_autoptr(GString) content = NULL;
	const gchar *message;
	guint status;

	message = (error != NULL && error->message != NULL)
		? error->message : "Could not file that receipt";
	status = (error != NULL) ? venture_error_to_http_status((VentureError)error->code) : 500;
	if (error != NULL && error->domain != VENTURE_ERROR)
		status = 500;
	content = g_string_new("<div class=\"card\"><p>");
	venture_html_escape_append(content, message);
	g_string_append(content, "</p><p><a href=\"/capture\">Back to the inbox</a></p></div>");
	return venture_web_html_response(
		venture_web_page(self, request, "/capture", "Capture", content->str), status);
}

static HtmxResponse *
capture_gate(VentureWebServer *self, HtmxRequest *request, gboolean write)
{
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;

	gate = venture_web_ui_require_session(self, request);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "capture");
	if (gate != NULL)
		return gate;
	if (!write)
		return NULL;
	principal = venture_auth_authenticate(self->auth, request);
	if (!venture_web_hosted_auth_require(self, request, principal, VENTURE_USER_ROLE_EDITOR, &error))
		return capture_fail(self, request, error);
	return NULL;
}

/* "All businesses" is not a place a receipt can be filed. */
static gint64
capture_filing_organization(VentureWebServer *self, HtmxRequest *request)
{
	gint64 organization_id;

	organization_id = venture_web_active_organization(self, request);
	if (organization_id <= 0)
		organization_id = venture_context_get_default_organization_id(self->context);
	return organization_id;
}

static const gchar *
capture_form_text(GHashTable *form, const gchar *name)
{
	if (form == NULL)
		return NULL;
	return g_hash_table_lookup(form, name);
}

/*
 * Images other than SVG, including HEIC, plus PDF. The four vision types
 * come back canonical; anything else keeps a lowercase type with no parameters.
 */
static gchar *
capture_upload_mime(const gchar *content_type, const gchar *filename)
{
	const gchar *canon;
	const gchar *start;
	g_autofree gchar *lower = NULL;
	g_autofree gchar *bare = NULL;
	g_autofree gchar *joined = NULL;
	gsize len;

	if (filename != NULL)
		lower = g_ascii_strdown(filename, -1);
	if (content_type != NULL && g_ascii_strncasecmp(content_type, "image/svg", 9) == 0)
		return NULL;
	if (lower != NULL && g_str_has_suffix(lower, ".svg"))
		return NULL;
	canon = venture_web_image_mime_type(content_type, filename);
	if (canon != NULL)
		return g_strdup(canon);
	if ((content_type != NULL && g_ascii_strncasecmp(content_type, "application/pdf", 15) == 0) ||
	    (lower != NULL && g_str_has_suffix(lower, ".pdf")))
		return g_strdup("application/pdf");
	if (lower != NULL && (g_str_has_suffix(lower, ".heic") || g_str_has_suffix(lower, ".heif")))
		return g_strdup("image/heic");
	if (content_type != NULL &&
	    (g_ascii_strncasecmp(content_type, "image/heic", 10) == 0 ||
	     g_ascii_strncasecmp(content_type, "image/heif", 10) == 0))
		return g_strdup("image/heic");
	if (content_type == NULL || g_ascii_strncasecmp(content_type, "image/", 6) != 0)
		return NULL;
	start = content_type + 6;
	len = 0;
	while (start[len] != '\0' && start[len] != ';' && start[len] != ' ' &&
	       start[len] != '\t' && len < 64)
	{
		if (!g_ascii_isalnum(start[len]) && start[len] != '+' &&
		    start[len] != '-' && start[len] != '.')
			return NULL;
		len++;
	}
	if (len == 0 || g_ascii_strncasecmp(start, "svg", 3) == 0)
		return NULL;
	bare = g_strndup(start, len);
	joined = g_strconcat("image/", bare, NULL);
	return g_ascii_strdown(joined, -1);
}

static gint64
capture_path_id(GHashTable *params, GError **error)
{
	const gchar *text;
	gchar *end;
	gint64 id;

	end = NULL;
	text = params != NULL ? g_hash_table_lookup(params, "id") : NULL;
	if (text == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"That receipt is not in this business");
		return 0;
	}
	id = g_ascii_strtoll(text, &end, 10);
	if (end == text || id <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"That receipt is not in this business");
		return 0;
	}
	return id;
}

static VentureEntity *
capture_item_in_organization(VentureWebServer *self, HtmxRequest *request, gint64 id, GError **error)
{
	g_autoptr(VentureEntity) item = NULL;
	gint64 organization_id;

	item = venture_database_get(venture_context_get_database(self->context),
		VENTURE_TYPE_CAPTURE_ITEM, id, error);
	organization_id = capture_filing_organization(self, request);
	if (item == NULL)
	{
		if (error != NULL && *error != NULL &&
		    ((*error)->code == VENTURE_ERROR_PERMISSION_DENIED ||
		     (*error)->code == VENTURE_ERROR_NOT_FOUND))
		{
			g_clear_error(error);
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
				"That receipt is not in this business");
		}
		else if (error != NULL && *error == NULL)
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
				"That receipt is not in this business");
		return NULL;
	}
	if (organization_id <= 0 || venture_entity_get_organization_id(item) != organization_id)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"That receipt is not in this business");
		return NULL;
	}
	return g_steal_pointer(&item);
}

static HtmxResponse *
capture_api(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) output = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) result = NULL;
	VentureCaptureService *service;
	VentureActor actor;
	const gchar *action = g_hash_table_lookup(params, "action");
	HtmxResponse *gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_api(self, "capture");
	if (gate != NULL)
		return gate;
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	service = venture_capture_service_get(venture_context_get_database(self->context));
	body = htmx_request_get_json(request, NULL);
	if (g_strcmp0(action, "ingest") == 0 || action == NULL)
	{
		JsonObject *object = body != NULL && JSON_NODE_HOLDS_OBJECT(body) ? json_node_get_object(body) : NULL;
		g_autoptr(VentureMoney) amount = NULL;
		g_autoptr(GDateTime) when = NULL;
		const gchar *amount_text;
		if (object == NULL)
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Expected a capture object");
			return venture_web_error_response(error);
		}
		amount_text = venture_json_object_get_string(object, "amount", NULL);
		if (amount_text != NULL)
		{
			/* A bare amount is in the organization's book currency. */
			g_autofree gchar *book = venture_database_get_book_currency(
				venture_context_get_database(self->context), venture_web_active_organization(self, request));
			amount = venture_money_from_string(amount_text, book, NULL);
		}
		if (venture_json_object_get_string(object, "occurred_at", NULL) != NULL)
			when = g_date_time_new_from_iso8601(venture_json_object_get_string(object, "occurred_at", NULL), NULL);
		item = venture_capture_service_ingest_for_organization(service, venture_web_active_organization(self, request),
			venture_json_object_get_string(object, "kind", "receipt"),
			venture_json_object_get_string(object, "title", NULL),
			venture_json_object_get_string(object, "source", "upload"),
			venture_json_object_get_int(object, "document_id", 0),
			venture_json_object_get_string(object, "vendor", NULL),
			amount, when, venture_json_object_get_string(object, "notes", NULL),
			&actor, &error);
		if (item == NULL)
			return venture_web_error_response(error);
		output = venture_serializable_to_json(VENTURE_SERIALIZABLE(item), FALSE);
		return venture_web_json_response(output, 201);
	}
	item = venture_database_get(venture_context_get_database(self->context),
		VENTURE_TYPE_CAPTURE_ITEM, g_ascii_strtoll(g_hash_table_lookup(params, "id"), NULL, 10), &error);
	if (item == NULL)
		return venture_web_error_response(error);
	if (g_strcmp0(action, "convert") == 0)
	{
		JsonObject *object = body != NULL && JSON_NODE_HOLDS_OBJECT(body) ? json_node_get_object(body) : NULL;
		result = venture_capture_service_convert(service, item,
			object != NULL ? venture_json_object_get_string(object, "as", "expense") : "expense",
			object, &actor, &error);
		if (result == NULL)
			return venture_web_error_response(error);
		output = venture_serializable_to_json(VENTURE_SERIALIZABLE(result), FALSE);
		return venture_web_json_response(output, 201);
	}
	if (g_strcmp0(action, "reject") == 0)
	{
		const gchar *reason = NULL;
		if (body != NULL && JSON_NODE_HOLDS_OBJECT(body))
			reason = venture_json_object_get_string(json_node_get_object(body), "reason", NULL);
		if (!venture_capture_service_reject(service, item, reason, &actor, &error))
			return venture_web_error_response(error);
		output = venture_serializable_to_json(VENTURE_SERIALIZABLE(item), FALSE);
		return venture_web_json_response(output, 200);
	}
	g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Unknown capture action");
	return venture_web_error_response(error);
}

static HtmxResponse *
capture_ui(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GString) content = g_string_new("<div class=\"page-head\"><h1>Capture inbox</h1><a class=\"btn\" href=\"/capture/file\">Snap a receipt</a></div>");
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	HtmxResponse *gate = venture_web_ui_require_session(self, request);
	(void)params;
	if (gate != NULL)
		return gate;
	gate = venture_web_require_module_ui(self, request, "capture");
	if (gate != NULL)
		return gate;
	{
		g_autoptr(VentureAuthPrincipal) principal = venture_auth_authenticate(self->auth, request);
		g_autoptr(VentureEntity) subject = VENTURE_ENTITY(venture_capture_item_new());
		venture_entity_set_organization_id(subject, venture_web_active_organization(self, request));
		venture_web_append_record_actions(self, content, subject, principal);
	}
	query = venture_query_new(VENTURE_TYPE_CAPTURE_ITEM);
	venture_query_set_organization(query, venture_web_active_organization(self, request));
	venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, "inbox", NULL);
	rows = venture_database_find(venture_context_get_database(self->context), query, &error);
	if (rows == NULL)
		return venture_web_error_response(error);
	g_string_append(content, "<div class=\"card\"><div class=\"card-head\"><h2>Waiting</h2></div><table><thead><tr><th>Title</th><th>Kind</th></tr></thead><tbody>");
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *title = NULL;
		g_autofree gchar *kind = NULL;
		g_object_get(g_ptr_array_index(rows, i), "title", &title, "kind", &kind, NULL);
		g_string_append_printf(content, "<tr><td><a href=\"/capture/items/%" G_GINT64_FORMAT "\">",
			venture_entity_get_id(g_ptr_array_index(rows, i)));
		venture_html_escape_append(content, title);
		g_string_append(content, "</a></td><td>");
		venture_html_escape_append(content, kind);
		g_string_append(content, "</td></tr>");
	}
	g_string_append(content, "</tbody></table></div>");
	return venture_web_html_response(venture_web_page(self, request, "/capture", "Capture inbox", content->str), 200);
}

static HtmxResponse *
capture_file_get(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(GString) content = NULL;
	HtmxResponse *gate;

	(void)params;
	gate = capture_gate(self, request, FALSE);
	if (gate != NULL)
		return gate;
	content = g_string_new(
		"<div class=\"page-head\"><h1>Snap a receipt</h1></div>"
		"<form class=\"card\" method=\"post\" action=\"/capture/file\" enctype=\"multipart/form-data\">"
		"<label>Photo <input id=\"receipt-photo\" type=\"file\" name=\"photo\" "
		"accept=\"image/*,application/pdf,.heic,.heif\" capture=\"environment\"></label>"
		"<img id=\"receipt-preview\" alt=\"Receipt preview\">"
		"<label>What it was <input name=\"title\" type=\"text\"></label>"
		"<label>Who you paid <input name=\"vendor\" type=\"text\"></label>"
		"<label>Amount <input name=\"amount\" type=\"text\" inputmode=\"decimal\"></label>"
		"<label>Date <input name=\"date\" type=\"date\"></label>"
		"<fieldset><legend>What this is</legend>"
		"<label><input type=\"radio\" name=\"kind\" value=\"receipt\" checked> Receipt</label>"
		"<label><input type=\"radio\" name=\"kind\" value=\"supplier_invoice\"> Supplier bill</label>"
		"</fieldset>"
		"<button class=\"btn\" type=\"submit\">File this receipt</button>"
		"</form>"
		"<script>"
		"document.getElementById('receipt-photo').addEventListener('change', function (event) {"
		"var file = event.target.files && event.target.files[0];"
		"var preview = document.getElementById('receipt-preview');"
		"var reader;"
		"if (!file || !window.FileReader) return;"
		"reader = new FileReader();"
		"reader.onload = function () { preview.src = reader.result; };"
		"reader.readAsDataURL(file);"
		"});"
		"</script>");
	return venture_web_html_response(
		venture_web_page(self, request, "/capture", "Snap a receipt", content->str), 200);
}

static HtmxUploadedFile *
capture_photo_part(GPtrArray *files)
{
	HtmxUploadedFile *photo;
	guint i;

	photo = NULL;
	if (files == NULL)
		return NULL;
	for (i = 0; i < files->len; i++)
	{
		HtmxUploadedFile *candidate;

		candidate = g_ptr_array_index(files, i);
		if (g_strcmp0(htmx_uploaded_file_get_field_name(candidate), "photo") == 0)
			photo = candidate;
	}
	return photo;
}

static HtmxResponse *
capture_file_post(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GPtrArray) files = NULL;
	g_autoptr(GHashTable) form = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(GDateTime) when = NULL;
	g_autoptr(VentureEntity) document = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autofree gchar *book = NULL;
	g_autofree gchar *mime = NULL;
	g_autofree gchar *directory = NULL;
	g_autofree gchar *safe_name = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *stored_name = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *checksum = NULL;
	g_autofree gchar *extracted = NULL;
	g_autofree gchar *location = NULL;
	HtmxResponse *gate;
	HtmxUploadedFile *photo;
	VentureActor actor;
	GBytes *bytes;
	const gchar *title;
	const gchar *kind;
	const gchar *vendor;
	const gchar *amount_text;
	const gchar *date_text;
	const gchar *filename;
	gint64 organization_id;

	(void)params;
	gate = capture_gate(self, request, TRUE);
	if (gate != NULL)
		return gate;
	files = htmx_uploaded_file_parse_multipart(
		htmx_request_get_content_type(request),
		htmx_request_get_body_bytes(request), &form, &error);
	if (files == NULL)
		return capture_fail(self, request, error);
	photo = capture_photo_part(files);
	if (photo == NULL)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Choose a photo of the receipt");
		return capture_fail(self, request, error);
	}
	if (htmx_uploaded_file_get_size(photo) == 0 ||
	    htmx_uploaded_file_get_size(photo) > VENTURE_WEB_ATTACHMENT_MAX_BYTES)
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"That file is empty or over the %d MB limit",
			VENTURE_WEB_ATTACHMENT_MAX_BYTES / (1024 * 1024));
		return capture_fail(self, request, error);
	}
	filename = htmx_uploaded_file_get_filename(photo);
	if (venture_string_is_empty(filename))
		filename = "receipt";
	mime = capture_upload_mime(htmx_uploaded_file_get_content_type(photo), filename);
	if (mime == NULL)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Use a photo or a PDF. SVG files are not accepted");
		return capture_fail(self, request, error);
	}
	kind = capture_form_text(form, "kind");
	if (venture_string_is_empty(kind))
		kind = "receipt";
	if (g_strcmp0(kind, "receipt") != 0 && g_strcmp0(kind, "supplier_invoice") != 0)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"File this as a receipt or a supplier bill");
		return capture_fail(self, request, error);
	}
	title = capture_form_text(form, "title");
	if (venture_string_is_empty(title))
		title = filename;
	if (venture_string_is_empty(title))
		title = "Receipt";
	vendor = capture_form_text(form, "vendor");
	organization_id = capture_filing_organization(self, request);
	if (organization_id <= 0)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"Pick a business before filing a receipt");
		return capture_fail(self, request, error);
	}
	amount_text = capture_form_text(form, "amount");
	if (!venture_string_is_empty(amount_text))
	{
		book = venture_database_get_book_currency(
			venture_context_get_database(self->context), organization_id);
		amount = venture_money_from_string(amount_text, book, &error);
		if (amount == NULL)
			return capture_fail(self, request, error);
	}
	date_text = capture_form_text(form, "date");
	if (venture_string_is_empty(date_text))
		date_text = capture_form_text(form, "occurred_at");
	if (!venture_string_is_empty(date_text))
	{
		when = venture_time_from_string(date_text, &error);
		if (when == NULL)
			return capture_fail(self, request, error);
	}
	/* Bytes land only after the picture, amount and date are acceptable. */
	safe_name = g_strcanon(g_strdup(filename),
		"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-", '_');
	token = venture_generate_token(8);
	stored_name = g_strdup_printf("%s_%s", token, safe_name);
	directory = g_build_filename(
		venture_config_get_state_dir(venture_context_get_config(self->context)),
		"attachments", NULL);
	if (g_mkdir_with_parents(directory, 0700) != 0)
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "Cannot create %s", directory);
		return capture_fail(self, request, error);
	}
	path = g_build_filename(directory, stored_name, NULL);
	if (!htmx_uploaded_file_save(photo, path, &error))
		return capture_fail(self, request, error);
	bytes = htmx_uploaded_file_get_data(photo);
	checksum = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, bytes);
	extracted = venture_document_extract_text(
		htmx_uploaded_file_get_content_type(photo), filename, bytes);
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	document = VENTURE_ENTITY(venture_document_new());
	g_object_set(document,
		"title", title,
		"kind", g_strcmp0(kind, "supplier_invoice") == 0 ? "invoice" : "receipt",
		"path", path,
		"mime-type", mime,
		"size-bytes", (gint64)htmx_uploaded_file_get_size(photo),
		"hash", checksum,
		"extracted-text", extracted,
		NULL);
	venture_entity_set_organization_id(document, organization_id);
	if (!venture_document_service_save_attachment(
		venture_document_service_get(venture_context_get_database(self->context)),
		document, directory, &actor, &error))
	{
		g_unlink(path);
		return capture_fail(self, request, error);
	}
	item = venture_capture_service_ingest_for_organization(
		venture_capture_service_get(venture_context_get_database(self->context)),
		organization_id, kind, title, "upload", venture_entity_get_id(document),
		venture_string_is_empty(vendor) ? NULL : vendor,
		amount, when, NULL, &actor, &error);
	if (item == NULL)
		return capture_fail(self, request, error);
	location = g_strdup_printf("/capture/items/%" G_GINT64_FORMAT, venture_entity_get_id(item));
	return capture_see_other(location);
}

static HtmxResponse *
capture_item_get(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) document = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) content = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *kind = NULL;
	g_autofree gchar *status = NULL;
	g_autofree gchar *result_type = NULL;
	g_autofree gchar *vendor = NULL;
	g_autofree gchar *mime = NULL;
	HtmxResponse *gate;
	gint64 id;
	gint64 document_id;
	const gchar *heading;

	gate = capture_gate(self, request, FALSE);
	if (gate != NULL)
		return gate;
	id = capture_path_id(params, &error);
	if (id <= 0)
		return capture_fail(self, request, error);
	item = capture_item_in_organization(self, request, id, &error);
	if (item == NULL)
		return capture_fail(self, request, error);
	g_object_get(item, "title", &title, "kind", &kind, "status", &status,
		"result-type", &result_type, "vendor", &vendor, "document-id", &document_id, NULL);
	if (g_strcmp0(status, "converted") == 0 && g_strcmp0(result_type, "expense") == 0)
		heading = "Filed as an expense";
	else if (g_strcmp0(status, "converted") == 0 && g_strcmp0(result_type, "vendor_bill") == 0)
		heading = "Filed as a supplier bill";
	else if (g_strcmp0(kind, "supplier_invoice") == 0)
		heading = "Supplier bill filed";
	else
		heading = "Receipt filed";
	if (document_id > 0)
	{
		document = venture_database_get(venture_context_get_database(self->context),
			VENTURE_TYPE_DOCUMENT, document_id, NULL);
		if (document != NULL)
			g_object_get(document, "mime-type", &mime, NULL);
	}
	content = g_string_new("<div class=\"page-head\"><h1>");
	venture_html_escape_append(content, heading);
	g_string_append(content, "</h1></div><div class=\"card\">");
	if (mime != NULL && g_str_has_prefix(mime, "image/"))
		g_string_append_printf(content,
			"<img alt=\"Receipt photo\" src=\"/capture/items/%" G_GINT64_FORMAT "/photo\">", id);
	else if (document_id > 0)
		g_string_append_printf(content,
			"<p><a href=\"/capture/items/%" G_GINT64_FORMAT "/photo\">Open the receipt</a></p>", id);
	g_string_append(content, "<p>");
	venture_html_escape_append(content, title);
	if (!venture_string_is_empty(vendor))
	{
		g_string_append(content, " / ");
		venture_html_escape_append(content, vendor);
	}
	g_string_append(content, "</p>");
	if (g_strcmp0(status, "inbox") == 0)
	{
		g_string_append_printf(content,
			"<form method=\"post\" action=\"/capture/items/%" G_GINT64_FORMAT "/file-as\">"
			"<input type=\"hidden\" name=\"as\" value=\"expense\">"
			"<button class=\"btn\" type=\"submit\">File it as an expense</button></form>"
			"<form method=\"post\" action=\"/capture/items/%" G_GINT64_FORMAT "/file-as\">"
			"<input type=\"hidden\" name=\"as\" value=\"vendor_bill\">"
			"<button class=\"btn\" type=\"submit\">File it as a supplier bill</button></form>",
			id, id);
	}
	g_string_append(content, "<p><a href=\"/capture\">Back to the inbox</a></p></div>");
	return venture_web_html_response(
		venture_web_page(self, request, "/capture", heading, content->str), 200);
}

static HtmxResponse *
capture_item_photo(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) document = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *stored_mime = NULL;
	g_autofree gchar *mime = NULL;
	g_autofree gchar *directory = NULL;
	HtmxResponse *gate;
	HtmxResponse *response;
	gint64 id;
	gint64 document_id;

	gate = capture_gate(self, request, FALSE);
	if (gate != NULL)
		return gate;
	id = capture_path_id(params, &error);
	if (id <= 0)
		return capture_fail(self, request, error);
	item = capture_item_in_organization(self, request, id, &error);
	if (item == NULL)
		return capture_fail(self, request, error);
	g_object_get(item, "document-id", &document_id, NULL);
	if (document_id <= 0)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"That receipt has no photo");
		return capture_fail(self, request, error);
	}
	document = venture_database_get(venture_context_get_database(self->context),
		VENTURE_TYPE_DOCUMENT, document_id, &error);
	if (document == NULL ||
	    venture_entity_get_organization_id(document) != venture_entity_get_organization_id(item))
	{
		g_clear_error(&error);
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"That receipt is not in this business");
		return capture_fail(self, request, error);
	}
	g_object_get(document, "mime-type", &stored_mime, NULL);
	mime = capture_upload_mime(stored_mime, NULL);
	if (mime == NULL)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"That file cannot be shown");
		return capture_fail(self, request, error);
	}
	directory = g_build_filename(
		venture_config_get_state_dir(venture_context_get_config(self->context)),
		"attachments", NULL);
	bytes = venture_document_service_read_attachment(
		venture_document_service_get(venture_context_get_database(self->context)),
		document, directory, VENTURE_WEB_ATTACHMENT_MAX_BYTES, &error);
	if (bytes == NULL)
		return capture_fail(self, request, error);
	response = htmx_response_new();
	htmx_response_set_bytes(response, bytes);
	htmx_response_set_content_type(response, mime);
	htmx_response_set_status(response, 200);
	htmx_response_add_header(response, "X-Content-Type-Options", "nosniff");
	htmx_response_add_header(response, "Cache-Control", "private, no-store");
	return response;
}

static HtmxResponse *
capture_item_file_as(HtmxRequest *request, GHashTable *params, gpointer data)
{
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *location = NULL;
	HtmxResponse *gate;
	VentureActor actor;
	const gchar *as;
	gint64 id;

	gate = capture_gate(self, request, TRUE);
	if (gate != NULL)
		return gate;
	id = capture_path_id(params, &error);
	if (id <= 0)
		return capture_fail(self, request, error);
	item = capture_item_in_organization(self, request, id, &error);
	if (item == NULL)
		return capture_fail(self, request, error);
	as = htmx_request_get_form_value(request, "as");
	if (g_strcmp0(as, "expense") != 0 && g_strcmp0(as, "vendor_bill") != 0)
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"File this as an expense or a supplier bill");
		return capture_fail(self, request, error);
	}
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	result = venture_capture_service_convert(
		venture_capture_service_get(venture_context_get_database(self->context)),
		item, as, NULL, &actor, &error);
	if (result == NULL)
		return capture_fail(self, request, error);
	location = g_strdup_printf("/capture/items/%" G_GINT64_FORMAT, id);
	return capture_see_other(location);
}
