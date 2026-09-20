/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
#include <math.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#ifdef VENTURE_HAVE_POPPLER
#include <poppler.h>
#endif

struct _VentureDocumentService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *filing;
};
G_DEFINE_FINAL_TYPE(VentureDocumentService, venture_document_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureDocumentService: %s", message);
	return FALSE;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_DOCUMENT_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureDocumentService *self = VENTURE_DOCUMENT_SERVICE(object);
	if (id == 1)
	{
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	}
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VentureDocumentService *self = VENTURE_DOCUMENT_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_document_service_parent_class)->finalize(object);
}

static void
venture_document_service_class_init(VentureDocumentServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;
	g_object_class_install_property(object_class, 1,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_document_service_init(VentureDocumentService *self)
{
	(void)self;
}

VentureDocumentService *
venture_document_service_get(VentureDatabase *database)
{
	VentureDocumentService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-document-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_DOCUMENT_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-document-service", self, g_object_unref);
	}
	return self;
}

/* Path ownership is checked below the web layer, including staged/imported
 * writes. Historical paths remain editable as metadata, but never reassignable. */
static gboolean attachment_owner(VentureDocumentService *self, VentureEntity *document, const gchar *path,
	gboolean new_claim, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_DOCUMENT);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_limit(query, 0); venture_query_set_include_deleted(query, TRUE);
	{
		/* Compare identities across organizations without exposing any of
		 * their metadata or granting that scope to the actual file reader. */
		g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
		rows = venture_database_find(self->database, query, error);
	}
	if (rows == NULL) return FALSE;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *other = g_ptr_array_index(rows, i);
		g_autofree gchar *stored = NULL, *canonical = NULL;
		if (venture_entity_get_id(other) == venture_entity_get_id(document)) continue;
		g_object_get(other, "path", &stored, NULL);
		if (venture_string_is_empty(stored)) continue;
		canonical = g_canonicalize_filename(stored, NULL);
		if (g_str_equal(canonical, path) && (new_claim || venture_entity_get_organization_id(other) != venture_entity_get_organization_id(document)))
			return refuse(error, "Attachment path already belongs to another document; file a new original");
	}
	return TRUE;
}
static gboolean document_path_validate(VentureDatabase *database, VentureEntity *row, VentureEntity *previous,
	gpointer data, GError **error)
{
	VentureDocumentService *self = venture_document_service_get(database);
	g_autofree gchar *path = NULL, *old = NULL;
	(void)data;
	g_object_get(row, "path", &path, NULL);
	if (previous != NULL) g_object_get(previous, "path", &old, NULL);
	if (g_strcmp0(path, old) == 0 && (venture_string_is_empty(path) || previous == NULL ||
		venture_entity_get_organization_id(row) == venture_entity_get_organization_id(previous))) return TRUE;
	if (self->filing != row || previous != NULL)
		return refuse(error, "Attachment paths and their owning organization are assigned only by the filing service");
	return attachment_owner(self, row, path, TRUE, error);
}
void venture_document_install_validators(VentureDatabase *database)
{
	venture_database_add_save_validator(database, VENTURE_TYPE_DOCUMENT, document_path_validate, NULL, NULL);
}
static gchar *attachment_path(VentureEntity *document, const gchar *attachment_root, gchar **root, GError **error)
{
	g_autofree gchar *path = NULL, *canonical = NULL, *prefix = NULL;
	g_object_get(document, "path", &path, NULL);
	if (venture_string_is_empty(path) || venture_string_is_empty(attachment_root)) {
		refuse(error, "Document has no configured local attachment"); return NULL;
	}
	*root = g_canonicalize_filename(attachment_root, NULL);
	canonical = g_canonicalize_filename(path, NULL); prefix = g_strconcat(*root, G_DIR_SEPARATOR_S, NULL);
	if (!g_str_has_prefix(canonical, prefix)) { refuse(error, "Attachment must be filed in configured attachment storage"); return NULL; }
	return g_steal_pointer(&canonical);
}
gboolean venture_document_service_save_attachment(VentureDocumentService *self, VentureEntity *document,
	const gchar *attachment_root, const VentureActor *actor, GError **error)
{
	g_autofree gchar *path = NULL, *root = NULL;
	gboolean ok;
	g_return_val_if_fail(VENTURE_IS_DOCUMENT_SERVICE(self) && VENTURE_IS_DOCUMENT(document), FALSE);
	if (self->database == NULL || self->filing != NULL || venture_entity_get_id(document) != 0)
		return refuse(error, "File attachments as new documents");
	path = attachment_path(document, attachment_root, &root, error); if (path == NULL) return FALSE;
	g_object_set(document, "path", path, NULL);
	self->filing = document; ok = venture_database_save(self->database, document, actor, error); self->filing = NULL;
	return ok;
}
GBytes *venture_document_service_read_attachment(VentureDocumentService *self, VentureEntity *document,
	const gchar *attachment_root, gsize max_bytes, GError **error)
{
	g_autoptr(VentureEntity) live = NULL;
	g_autofree gchar *path = NULL, *root = NULL, *buffer = NULL;
	g_auto(GStrv) parts = NULL;
	guint part;
	struct stat info;
	gsize offset = 0;
	gint directory, fd;
	g_return_val_if_fail(VENTURE_IS_DOCUMENT_SERVICE(self) && VENTURE_IS_DOCUMENT(document), NULL);
	if (self->database == NULL) { refuse(error, "The document database has been closed"); return NULL; }
	live = venture_database_get(self->database, VENTURE_TYPE_DOCUMENT, venture_entity_get_id(document), error);
	if (live == NULL) return NULL;
	if (venture_entity_is_deleted(live) || venture_entity_get_organization_id(live) != venture_entity_get_organization_id(document)) {
		refuse(error, "Attachment document is unavailable in this organization"); return NULL;
	}
	path = attachment_path(live, attachment_root, &root, error); if (path == NULL) return NULL;
	if (!attachment_owner(self, live, path, FALSE, error)) return NULL;
	parts = g_strsplit(path + strlen(root) + 1, G_DIR_SEPARATOR_S, -1);
	directory = g_open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
	if (directory < 0) { refuse(error, "Attachment directory is unavailable"); return NULL; }
	for (part = 0; parts[part + 1] != NULL; part++) {
		gint next = openat(directory, parts[part], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		close(directory); directory = next;
		if (directory < 0) { refuse(error, "Attachment directory is missing or is a symbolic link"); return NULL; }
	}
	/* Nonblocking open lets fstat reject a FIFO without waiting for a writer. */
	fd = openat(directory, parts[part], O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK); close(directory);
	if (fd < 0) { refuse(error, "Attachment is missing or is a symbolic link"); return NULL; }
	if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 || (guint64)info.st_size > max_bytes) {
		close(fd); refuse(error, "Attachment is not a regular file within the allowed size"); return NULL;
	}
	buffer = g_malloc((gsize)info.st_size);
	while (offset < (gsize)info.st_size) {
		ssize_t got = read(fd, buffer + offset, (gsize)info.st_size - offset);
		if (got < 0 && errno == EINTR) continue;
		if (got <= 0) { close(fd); refuse(error, "Attachment changed or could not be read"); return NULL; }
		offset += (gsize)got;
	}
	close(fd); return g_bytes_new_take(g_steal_pointer(&buffer), offset);
}

static gchar *
next_number(VentureDocumentService *self, gint64 org, GType type, const gchar *prefix)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	gint64 count;
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	count = venture_database_count(self->database, query, NULL);
	return g_strdup_printf("%s%" G_GINT64_FORMAT, prefix, count + 1);
}

static JsonArray *
lines_of(JsonObject *spec, GError **error)
{
	JsonNode *node;
	JsonArray *lines;
	guint i;
	if (spec == NULL)
	{
		refuse(error, "a JSON object is required");
		return NULL;
	}
	node = json_object_get_member(spec, "lines");
	if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node) || json_array_get_length(json_node_get_array(node)) < 1)
	{
		refuse(error, "at least one line is required");
		return NULL;
	}
	lines = json_node_get_array(node);
	/* JSON is untrusted: typed accessors diagnose programmer errors on scalar rows. */
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		JsonNode *row = json_array_get_element(lines, i);
		if (!JSON_NODE_HOLDS_OBJECT(row))
		{
			refuse(error, "each line must be a JSON object");
			return NULL;
		}
	}
	return lines;
}

static gboolean
money_from_row(VentureEntity *line, JsonObject *row, GError **error)
{
	const gchar *price = venture_json_object_get_string(row, "unit_price", NULL);
	if (price == NULL)
		return refuse(error, "each line needs an exact unit_price");
	if (!venture_entity_set_field_from_string(line, "unit-price", price, error))
		return FALSE;
	g_object_set(line, "discount-percent", venture_json_object_get_int(row, "discount_percent", 0),
		"tax-percent", venture_json_object_get_int(row, "tax_percent", 0), NULL);
	return TRUE;
}

static gdouble
row_quantity(JsonObject *row)
{
	JsonNode *node = json_object_get_member(row, "quantity");
	if (node == NULL)
		return 1;
	if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64)
		return (gdouble)json_node_get_int(node);
	if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_DOUBLE)
		return json_node_get_double(node);
	return 0;
}

static VentureEntity *
venture_document_service_compose_invoice_impl(VentureDocumentService *self, gint64 organization_id,
	JsonObject *spec, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureInvoice) invoice = NULL;
	g_autofree gchar *number = NULL;
	g_autoptr(GDateTime) now = NULL;
	JsonArray *lines;
	guint i;
	gint64 due_days;
	g_return_val_if_fail(VENTURE_IS_DOCUMENT_SERVICE(self), NULL);
	lines = lines_of(spec, error);
	if (lines == NULL)
		return NULL;
	if (!venture_database_begin(self->database, error))
		return NULL;
	invoice = venture_invoice_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(invoice), organization_id);
	number = g_strdup(venture_json_object_get_string(spec, "number", ""));
	if (number == NULL || number[0] == '\0')
	{
		g_free(number);
		number = next_number(self, organization_id, VENTURE_TYPE_INVOICE, "INV-");
	}
	g_object_set(invoice, "number", number, "company-id",
		venture_json_object_get_int(spec, "company_id", 0),
		"contact-id", venture_json_object_get_int(spec, "contact_id", 0),
		"terms", venture_json_object_get_string(spec, "terms", ""),
		"notes", venture_json_object_get_string(spec, "notes", ""),
		"external-id", venture_json_object_get_string(spec, "external_id", ""), NULL);
	due_days = venture_json_object_get_int(spec, "due_days", 30);
	now = venture_time_now();
	if (due_days > 0)
	{
		g_autoptr(GDateTime) due = g_date_time_add_days(now, (gint)due_days);
		g_object_set(invoice, "due-at", due, NULL);
	}
	if (!venture_database_save(self->database, VENTURE_ENTITY(invoice), actor, error))
		goto fail;
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		JsonObject *row = json_array_get_object_element(lines, i);
		g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
		const gchar *description = venture_json_object_get_string(row, "description", NULL);
		gdouble quantity = row_quantity(row);
		if (description == NULL || description[0] == '\0' || !isfinite(quantity) || quantity <= 0)
		{
			refuse(error, "each line needs a description and a positive quantity");
			goto fail;
		}
		venture_entity_set_organization_id(VENTURE_ENTITY(line), organization_id);
		g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
			"description", description, "quantity", quantity, "position", (gint64)(i + 1),
			"product-id", venture_json_object_get_int(row, "product_id", 0), NULL);
		if (!money_from_row(VENTURE_ENTITY(line), row, error) ||
			!venture_database_save(self->database, VENTURE_ENTITY(line), actor, error))
			goto fail;
	}
	if (venture_json_object_get_bool(spec, "send", FALSE) &&
		!venture_settlement_service_transition(venture_settlement_service_get(self->database),
			invoice, "sent", now, actor, error))
		goto fail;
	if (!venture_database_commit(self->database, error))
		goto fail;
	return VENTURE_ENTITY(g_steal_pointer(&invoice));
fail:
	venture_database_rollback(self->database);
	return NULL;
}

VentureEntity *
venture_document_service_compose_quote(VentureDocumentService *self, gint64 organization_id,
	JsonObject *spec, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuote) quote = NULL;
	g_autofree gchar *number = NULL;
	JsonArray *lines;
	guint i;
	gint64 quote_id;
	g_return_val_if_fail(VENTURE_IS_DOCUMENT_SERVICE(self), NULL);
	lines = lines_of(spec, error);
	if (lines == NULL)
		return NULL;
	if (!venture_database_begin(self->database, error))
		return NULL;
	quote = venture_quote_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(quote), organization_id);
	number = g_strdup(venture_json_object_get_string(spec, "number", ""));
	if (number == NULL || number[0] == '\0')
	{
		g_free(number);
		number = next_number(self, organization_id, VENTURE_TYPE_QUOTE, "Q-");
	}
	g_object_set(quote, "number", number, "company-id",
		venture_json_object_get_int(spec, "company_id", 0),
		"contact-id", venture_json_object_get_int(spec, "contact_id", 0),
		"currency", venture_json_object_get_string(spec, "currency", "USD"),
		"terms", venture_json_object_get_string(spec, "terms", ""),
		"notes", venture_json_object_get_string(spec, "notes", ""),
		"billing-mode", venture_json_object_get_string(spec, "billing_mode", ""), NULL);
	if (!venture_database_save(self->database, VENTURE_ENTITY(quote), actor, error))
		goto fail;
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		JsonObject *row = json_array_get_object_element(lines, i);
		g_autoptr(VentureQuoteLine) line = venture_quote_line_new();
		const gchar *description = venture_json_object_get_string(row, "description", NULL);
		gdouble requested = row_quantity(row);
		gint64 quantity;
		if (description == NULL || description[0] == '\0' || !isfinite(requested) ||
			requested <= 0 || requested >= (gdouble)G_MAXINT64 || floor(requested) != requested)
		{
			refuse(error, "each quote line needs a description and a positive whole quantity");
			goto fail;
		}
		quantity = (gint64)requested;
		venture_entity_set_organization_id(VENTURE_ENTITY(line), organization_id);
		g_object_set(line, "quote-id", venture_entity_get_id(VENTURE_ENTITY(quote)),
			"description", description, "quantity", quantity, "position", (gint64)(i + 1),
			"product-id", venture_json_object_get_int(row, "product_id", 0), NULL);
		if (!money_from_row(VENTURE_ENTITY(line), row, error) ||
			!venture_database_save(self->database, VENTURE_ENTITY(line), actor, error))
			goto fail;
	}
	quote_id = venture_entity_get_id(VENTURE_ENTITY(quote));
	if (venture_json_object_get_bool(spec, "send", FALSE))
	{
		g_autoptr(VentureEntity) action = VENTURE_ENTITY(venture_quote_action_new());
		g_autoptr(VentureEntity) current = venture_database_get(self->database, VENTURE_TYPE_QUOTE, quote_id, error);
		if (current == NULL)
			goto fail;
		venture_entity_set_organization_id(action, organization_id);
		g_object_set(action, "quote-id", quote_id, "action", "send",
			"expected-version", venture_entity_get_version(current), NULL);
		if (!venture_quote_service_execute(venture_database_get_quote_service(self->database),
			action, "manual", NULL, actor, error))
			goto fail;
		g_object_unref(quote);
		quote = VENTURE_QUOTE(venture_database_get(self->database, VENTURE_TYPE_QUOTE, quote_id, error));
		if (quote == NULL)
			goto fail;
	}
	/* Sending validates the complete quote; refusal must roll back its header and lines. */
	if (!venture_database_commit(self->database, error))
		goto fail;
	return VENTURE_ENTITY(g_steal_pointer(&quote));
fail:
	venture_database_rollback(self->database);
	return NULL;
}

/* Bind consent before this operation creates derived rows or enters nested
 * transactions. All generated financial effects share this root proposal. */
VentureEntity *
venture_document_service_compose_invoice(VentureDocumentService *self, gint64 organization_id,
	JsonObject *spec, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureAccountingOperation) operation = NULL;
	VentureDatabase * db = self->database;
	GVariantBuilder arguments;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(JsonNode) spec_node = NULL;
	g_autofree gchar *spec_text = NULL;
	/* A draft has no ledger effect. Sending it is the posting boundary. */
	if (spec == NULL || !venture_json_object_get_bool(spec, "send", FALSE))
		return venture_document_service_compose_invoice_impl(self, organization_id, spec, actor, error);
	if (db == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Database is unavailable");
		return NULL;
	}
	if (spec != NULL)
	{
		spec_node = json_node_new(JSON_NODE_OBJECT);
		json_node_set_object(spec_node, spec);
		spec_text = venture_json_to_string(spec_node, FALSE);
	}
	g_variant_builder_init(&arguments, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&arguments, "{sv}", "organization_id", g_variant_new_int64((gint64)organization_id));
	g_variant_builder_add(&arguments, "{sv}", "spec", g_variant_new_maybe(G_VARIANT_TYPE_STRING, spec_text != NULL ? g_variant_new_string(spec_text) : NULL));
	operation = venture_accounting_operation_begin(db, "document-compose-invoice", NULL, NULL,
		g_variant_builder_end(&arguments), organization_id, actor, error);
	if (operation == NULL)
		return NULL;
	result = venture_document_service_compose_invoice_impl(self, organization_id, spec, actor, error);
	if (result == NULL)
		return NULL;
	if (!venture_accounting_operation_finish(operation, error))
		return NULL;
	return g_steal_pointer(&result);
}

/* --- Text extraction, shared by uploads and inbound mail --------------------- */

static gboolean
html_is_block(const gchar *name)
{
	static const gchar *const blocks[] = {
		"br", "p", "div", "tr", "li", "ul", "ol", "table", "blockquote", "hr", "pre",
		"h1", "h2", "h3", "h4", "h5", "h6", "section", "article", "header", "footer", "title", NULL
	};
	return g_strv_contains(blocks, name);
}

/* Appends the character a reference names, or the ampersand itself when it
 * names nothing this knows; returns how many input bytes were consumed. */
static gsize
html_append_reference(GString *text, const gchar *html, gsize size)
{
	static const struct { const gchar *name; const gchar *value; } named[] = {
		{ "&nbsp;", " " }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" },
		{ "&apos;", "'" }, { "&amp;", "&" }, { "&euro;", "\xe2\x82\xac" }, { "&pound;", "\xc2\xa3" },
		{ "&copy;", "\xc2\xa9" }, { "&ndash;", "\xe2\x80\x93" }, { "&mdash;", "\xe2\x80\x94" }
	};
	gsize i;
	if (size > 2 && html[1] == '#')
	{
		gboolean hex = (html[2] == 'x' || html[2] == 'X');
		gsize start = hex ? 3 : 2;
		gsize end = start;
		guint64 code = 0;
		while (end < size && end < start + 8 && (hex ? g_ascii_isxdigit(html[end]) : g_ascii_isdigit(html[end])))
		{
			code = code * (hex ? 16 : 10) + (guint64)(hex ? g_ascii_xdigit_value(html[end]) : g_ascii_digit_value(html[end]));
			end++;
		}
		if (end > start && end < size && html[end] == ';' && code > 0 && code <= 0x10ffff &&
		    g_unichar_validate((gunichar)code))
		{
			g_string_append_unichar(text, (gunichar)code);
			return end + 1;
		}
	}
	for (i = 0; i < G_N_ELEMENTS(named); i++)
	{
		gsize length = strlen(named[i].name);
		if (size >= length && g_ascii_strncasecmp(html, named[i].name, length) == 0)
		{
			g_string_append(text, named[i].value);
			return length;
		}
	}
	g_string_append_c(text, '&');
	return 1;
}

gchar *
venture_document_html_to_text(const gchar *html, gssize length)
{
	g_autoptr(GString) text = NULL;
	gsize size;
	gsize i;
	guint newlines = 2;
	gboolean in_space = TRUE;

	g_return_val_if_fail(html != NULL || length == 0, NULL);
	size = length < 0 ? strlen(html) : (gsize)length;
	text = g_string_new(NULL);

	for (i = 0; i < size; i++)
	{
		gchar c = html[i];

		if (c == '<' && i + 1 < size && (g_ascii_isalpha(html[i + 1]) || html[i + 1] == '/' || html[i + 1] == '!'))
		{
			const gchar *close;
			gchar name[16];
			gsize n = 0;
			gsize at = i + 1;

			/* Everything inside script, style and a comment is not content. */
			/* Bounded by what is left: callers pass attachment bytes and
			 * fetched pages that are not NUL-terminated, so an unbounded
			 * compare on a body ending in "<s" read past the buffer. */
			if ((size - i >= 7 && g_ascii_strncasecmp(html + i, "<script", 7) == 0) ||
			    (size - i >= 6 && g_ascii_strncasecmp(html + i, "<style", 6) == 0))
			{
				const gchar *end_tag = (size - i >= 7 && g_ascii_strncasecmp(html + i, "<script", 7) == 0) ? "</script" : "</style";
				gsize k;
				for (k = i + 1; k + strlen(end_tag) <= size; k++)
					if (g_ascii_strncasecmp(html + k, end_tag, strlen(end_tag)) == 0) break;
				close = k + strlen(end_tag) <= size ? memchr(html + k, '>', size - k) : NULL;
				if (close == NULL) break;
				i = (gsize)(close - html);
				continue;
			}
			if (size - i >= 4 && strncmp(html + i, "<!--", 4) == 0)
			{
				const gchar *end = g_strstr_len(html + i + 4, (gssize)(size - i - 4), "-->");
				if (end == NULL) break;
				i = (gsize)(end - html) + 2;
				continue;
			}
			close = memchr(html + i, '>', size - i);
			/* An unterminated tag: the rest is markup, not text. */
			if (close == NULL) break;
			if (html[at] == '/') at++;
			while (at < size && n + 1 < sizeof name && g_ascii_isalnum(html[at])) name[n++] = g_ascii_tolower(html[at++]);
			name[n] = '\0';
			if (html_is_block(name))
			{
				while (text->len && text->str[text->len - 1] == ' ') g_string_truncate(text, text->len - 1);
				if (newlines < 2) { g_string_append_c(text, '\n'); newlines++; }
				in_space = TRUE;
			}
			else if (!in_space && newlines == 0)
			{
				/* A tag boundary is a word boundary. */
				g_string_append_c(text, ' ');
				in_space = TRUE;
			}
			i = (gsize)(close - html);
			continue;
		}

		if (g_ascii_isspace(c))
		{
			if (!in_space && newlines == 0) { g_string_append_c(text, ' '); in_space = TRUE; }
			continue;
		}

		if (c == '&')
		{
			i += html_append_reference(text, html + i, size - i) - 1;
			in_space = FALSE;
			newlines = 0;
			continue;
		}

		g_string_append_c(text, c);
		in_space = FALSE;
		newlines = 0;
	}

	while (text->len && g_ascii_isspace(text->str[text->len - 1])) g_string_truncate(text, text->len - 1);
	return g_utf8_make_valid(text->str, (gssize)text->len);
}

gchar *
venture_document_extract_text(const gchar *content_type, const gchar *filename, GBytes *data)
{
	g_autofree gchar *lower_name = filename != NULL ? g_ascii_strdown(filename, -1) : NULL;
	g_autofree gchar *lower_type = content_type != NULL ? g_ascii_strdown(content_type, -1) : NULL;
	g_autofree gchar *converted = NULL;
	const gchar *bytes;
	gsize length = 0;
	gboolean looks_pdf, looks_html, looks_text;

	g_return_val_if_fail(data != NULL, NULL);

	looks_pdf = (lower_type != NULL && g_str_has_prefix(lower_type, "application/pdf")) ||
	            (lower_name != NULL && g_str_has_suffix(lower_name, ".pdf"));
	looks_html = (lower_type != NULL && (g_str_has_prefix(lower_type, "text/html") || g_str_has_prefix(lower_type, "application/xhtml"))) ||
	             (lower_name != NULL && (g_str_has_suffix(lower_name, ".html") || g_str_has_suffix(lower_name, ".htm")));
	looks_text = (lower_type != NULL &&
	              (g_str_has_prefix(lower_type, "text/") ||
	               g_str_has_prefix(lower_type, "application/json") ||
	               g_str_has_prefix(lower_type, "application/csv") ||
	               g_str_has_prefix(lower_type, "application/x-yaml") ||
	               g_str_has_prefix(lower_type, "application/yaml"))) ||
	             (lower_name != NULL &&
	              (g_str_has_suffix(lower_name, ".txt") || g_str_has_suffix(lower_name, ".md") ||
	               g_str_has_suffix(lower_name, ".org") || g_str_has_suffix(lower_name, ".csv") ||
	               g_str_has_suffix(lower_name, ".json") || g_str_has_suffix(lower_name, ".yaml") ||
	               g_str_has_suffix(lower_name, ".yml")));

	if (looks_pdf)
	{
#ifdef VENTURE_HAVE_POPPLER
		g_autoptr(PopplerDocument) document = poppler_document_new_from_bytes(data, NULL, NULL);
		g_autoptr(GString) text = NULL;
		gint pages;
		gint i;

		if (document == NULL)
			return NULL;
		text = g_string_new(NULL);
		/* A PDF reaches this from a stranger's email to the capture
		 * address, parsed on the main loop: a small file can declare
		 * hundreds of thousands of pages. Stop at a page and a size cap;
		 * a receipt or a manuscript fits well inside both. */
		pages = MIN(poppler_document_get_n_pages(document), VENTURE_DOCUMENT_EXTRACT_MAX_PAGES);
		for (i = 0; i < pages && text->len < VENTURE_DOCUMENT_EXTRACT_MAX_BYTES; i++)
		{
			g_autoptr(PopplerPage) page = poppler_document_get_page(document, i);
			g_autofree gchar *page_text = NULL;

			if (page == NULL)
				continue;
			page_text = poppler_page_get_text(page);
			if (venture_string_is_empty(page_text))
				continue;
			if (text->len != 0)
				g_string_append(text, "\n\n");
			g_string_append(text, page_text);
		}
		if (text->len == 0)
			return NULL;
		return g_string_free(g_steal_pointer(&text), FALSE);
#else
		/* Built without poppler: the PDF is stored, its text is not. */
		return NULL;
#endif
	}

	if (!looks_html && !looks_text)
		return NULL;

	bytes = g_bytes_get_data(data, &length);
	if (bytes == NULL || length == 0)
		return NULL;
	/* Mail without a declared charset is usually windows-1252, and a
	 * superset of Latin-1 converts every byte, so nothing is dropped. */
	if (!g_utf8_validate(bytes, (gssize)length, NULL))
	{
		converted = g_convert(bytes, (gssize)length, "UTF-8", "WINDOWS-1252", NULL, &length, NULL);
		if (converted == NULL)
			converted = g_utf8_make_valid(bytes, (gssize)length);
		bytes = converted;
		length = strlen(converted);
	}
	if (looks_html)
		return venture_document_html_to_text(bytes, (gssize)length);
	return g_strndup(bytes, length);
}
