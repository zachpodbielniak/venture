/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * One preview for every outgoing email.
 *
 * A sender registers a kind: a function that builds the unsaved message it
 * would enqueue for a source record, which its own enqueue path also calls.
 * The preview renders what the SMTP transport would put on the wire from
 * that message, by the transport's own rule (venture_mail_visible_bodies()),
 * withholds every private bearer link, and runs the pre-send checks. So
 * what an operator approves is what the customer gets, short of the links
 * that must never reach anyone else's screen.
 */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "quotes/venture-quote-mail.h"

/* --- Checkers --------------------------------------------------------------- */

G_DEFINE_INTERFACE(VentureMailLinkChecker, venture_mail_link_checker, G_TYPE_OBJECT)
static void venture_mail_link_checker_default_init(VentureMailLinkCheckerInterface *iface)
{
	(void)iface;
}
gboolean venture_mail_link_checker_check(VentureMailLinkChecker *self, const gchar *url, GCancellable *cancellable, GError **error)
{
	VentureMailLinkCheckerInterface *iface;
	g_return_val_if_fail(VENTURE_IS_MAIL_LINK_CHECKER(self), FALSE);
	iface = VENTURE_MAIL_LINK_CHECKER_GET_IFACE(self);
	g_return_val_if_fail(iface->check != NULL, FALSE);
	return iface->check(self, url, cancellable, error);
}

struct _VentureHttpLinkChecker {
	GObject parent_instance;
	SoupSession *session;
};
static void http_checker_iface(VentureMailLinkCheckerInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureHttpLinkChecker, venture_http_link_checker, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_MAIL_LINK_CHECKER, http_checker_iface))
/* A HEAD, not a GET: the question is whether the address answers, and the
 * body of a landing page is not ours to download. Redirects are not
 * followed, so a public address cannot bounce the request inward past the
 * guard; a 3xx is an answer. */
static gboolean http_check(VentureMailLinkChecker *checker, const gchar *url, GCancellable *cancellable, GError **error)
{
	VentureHttpLinkChecker *self = VENTURE_HTTP_LINK_CHECKER(checker);
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GInputStream) stream = NULL;
	guint status;
	if (!g_str_has_prefix(url, "http://") && !g_str_has_prefix(url, "https://")) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Only http and https addresses are checked");
		return FALSE;
	}
	if (!venture_ai_url_is_fetchable(url, error))
		return FALSE;
	message = soup_message_new(SOUP_METHOD_HEAD, url);
	if (message == NULL) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Not a valid address");
		return FALSE;
	}
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	stream = soup_session_send(self->session, message, cancellable, error);
	if (stream == NULL)
		return FALSE;
	g_input_stream_close(stream, NULL, NULL);
	status = soup_message_get_status(message);
	if (status >= 400) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK, "answered %u", status);
		return FALSE;
	}
	return TRUE;
}
static void http_checker_iface(VentureMailLinkCheckerInterface *iface) { iface->check = http_check; }
static void http_checker_finalize(GObject *object)
{
	g_clear_object(&VENTURE_HTTP_LINK_CHECKER(object)->session);
	G_OBJECT_CLASS(venture_http_link_checker_parent_class)->finalize(object);
}
static void venture_http_link_checker_class_init(VentureHttpLinkCheckerClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = http_checker_finalize;
}
static void venture_http_link_checker_init(VentureHttpLinkChecker *self)
{
	self->session = soup_session_new_with_options("timeout", 3, "idle-timeout", 3, "max-conns", 4, NULL);
}
VentureHttpLinkChecker *venture_http_link_checker_new(void)
{
	return g_object_new(VENTURE_TYPE_HTTP_LINK_CHECKER, NULL);
}

/* --- The preview ----------------------------------------------------------- */

static void check_free(gpointer data)
{
	VentureMailCheck *check = data;
	g_free(check->id);
	g_free(check->message);
	g_free(check);
}
void venture_mail_preview_free(VentureMailPreview *preview)
{
	if (preview == NULL)
		return;
	g_free(preview->kind);
	g_free(preview->to);
	g_free(preview->subject);
	g_free(preview->text);
	g_free(preview->html);
	g_clear_pointer(&preview->headers, g_ptr_array_unref);
	g_clear_pointer(&preview->attachments, g_ptr_array_unref);
	g_clear_pointer(&preview->checks, g_ptr_array_unref);
	g_free(preview);
}
const VentureMailCheck *venture_mail_preview_get_check(const VentureMailPreview *preview, const gchar *id)
{
	guint i;
	g_return_val_if_fail(preview != NULL, NULL);
	for (i = 0; preview->checks != NULL && i < preview->checks->len; i++) {
		const VentureMailCheck *check = g_ptr_array_index(preview->checks, i);
		if (g_strcmp0(check->id, id) == 0)
			return check;
	}
	return NULL;
}
static void add_check(VentureMailPreview *preview, const gchar *id, VentureMailCheckStatus status, gchar *message)
{
	VentureMailCheck *check = g_new0(VentureMailCheck, 1);
	check->id = g_strdup(id);
	check->status = status;
	check->message = message;
	g_ptr_array_add(preview->checks, check);
}

void venture_mail_visible_bodies(VentureMailMessage *message, gchar **text, gchar **html)
{
	g_autofree gchar *public_text = NULL, *public_html = NULL, *private_text = NULL, *private_html = NULL;
	g_return_if_fail(VENTURE_IS_MAIL_MESSAGE(message));
	g_object_get(message, "text-body", &public_text, "html-body", &public_html,
		"private-text-body", &private_text, "private-html-body", &private_html, NULL);
	/* Do not let a public HTML alternative hide a bearer link: a private
	 * text body drops the public HTML, and a private HTML body, when the
	 * sender rendered one, replaces it so an HTML-only link survives. */
	if (!venture_string_is_empty(private_text)) {
		g_free(public_text);
		public_text = g_steal_pointer(&private_text);
		g_clear_pointer(&public_html, g_free);
	}
	if (!venture_string_is_empty(private_html)) {
		g_free(public_html);
		public_html = g_steal_pointer(&private_html);
	}
	*text = public_text != NULL ? g_steal_pointer(&public_text) : g_strdup("");
	*html = !venture_string_is_empty(public_html) ? g_steal_pointer(&public_html) : NULL;
}

/* --- Private links --------------------------------------------------------- */

static GRegex *url_regex(void)
{
	static GRegex *regex;
	if (g_once_init_enter(&regex)) {
		GRegex *made = g_regex_new("(?:https?|mailto):[^\\s\"'<>]+", G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
		g_once_init_leave(&regex, made);
	}
	return regex;
}
/* Sentence punctuation after a link in running text is not part of it. */
static gchar *trimmed_url(const gchar *match)
{
	gchar *url = g_strdup(match);
	gsize n = strlen(url);
	while (n > 0 && strchr(".,;:!?)]", url[n - 1]) != NULL)
		url[--n] = '\0';
	return url;
}
static void collect_urls(GHashTable *into, const gchar *body)
{
	g_autoptr(GMatchInfo) info = NULL;
	if (venture_string_is_empty(body))
		return;
	g_regex_match(url_regex(), body, 0, &info);
	while (g_match_info_matches(info)) {
		g_autofree gchar *match = g_match_info_fetch(info, 0);
		g_hash_table_add(into, trimmed_url(match));
		g_match_info_next(info, NULL);
	}
}
/* Every bearer token this install mints is 32 random bytes in hex. A link
 * that carries one is a credential wherever the sender put it -- a tracked
 * sequence step keeps its wrapped links in the public body -- so a run of
 * 32 hex digits withholds it too. A public link that happens to carry one
 * (a commit hash) is withheld in the preview as well; that is the safe way
 * to be wrong. */
static gboolean carries_token(const gchar *url)
{
	guint run = 0;
	const gchar *p;
	for (p = url; *p != '\0'; p++) {
		run = g_ascii_isxdigit(*p) ? run + 1 : 0;
		if (run >= 32)
			return TRUE;
	}
	return FALSE;
}
typedef struct {
	GHashTable *public_urls;
	const gchar *unsubscribe;
	guint withheld;
} Redaction;
static gboolean private_url(Redaction *r, const gchar *url)
{
	if (g_strcmp0(url, VENTURE_MAIL_PREVIEW_PLACEHOLDER_URL) == 0)
		return FALSE;
	if (!venture_string_is_empty(r->unsubscribe) && strstr(url, r->unsubscribe) != NULL)
		return TRUE;
	if (g_str_has_prefix(url, "mailto:") || g_str_has_prefix(url, "MAILTO:"))
		return FALSE;
	return !g_hash_table_contains(r->public_urls, url) || carries_token(url);
}
static gboolean replace_url(const GMatchInfo *info, GString *out, gpointer data)
{
	Redaction *r = data;
	g_autofree gchar *match = g_match_info_fetch(info, 0);
	g_autofree gchar *url = trimmed_url(match);
	if (private_url(r, url)) {
		g_string_append(out, VENTURE_MAIL_PREVIEW_PLACEHOLDER_URL);
		g_string_append(out, match + strlen(url));
		r->withheld++;
	} else
		g_string_append(out, match);
	return FALSE;
}
static gchar *redact(Redaction *r, const gchar *body)
{
	if (body == NULL)
		return NULL;
	return g_regex_replace_eval(url_regex(), body, -1, 0, 0, replace_url, r, NULL);
}

/* --- HTML scanning ---------------------------------------------------------- */

/* Email HTML is not parsed here as a document: the checks only need each
 * tag's attributes, and a forgiving scan reads broken markup the way a
 * mail client does rather than refusing it. */
static gchar *attribute(const gchar *tag, const gchar *name)
{
	g_autofree gchar *pattern = g_strdup_printf("[\\s/]%s\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))", name);
	g_autoptr(GRegex) regex = g_regex_new(pattern, G_REGEX_CASELESS, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;
	gint i;
	if (!g_regex_match(regex, tag, 0, &info))
		return NULL;
	for (i = 1; i <= 3; i++) {
		gint start = -1, end = -1;
		if (g_match_info_fetch_pos(info, i, &start, &end) && start >= 0)
			return g_strndup(tag + start, end - start);
	}
	return g_strdup("");
}
static GPtrArray *tags(const gchar *html, const gchar *name)
{
	GPtrArray *found = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *pattern = g_strdup_printf("<%s\\b[^>]*>", name);
	g_autoptr(GRegex) regex = g_regex_new(pattern, G_REGEX_CASELESS, 0, NULL);
	g_autoptr(GMatchInfo) info = NULL;
	if (html == NULL)
		return found;
	g_regex_match(regex, html, 0, &info);
	while (g_match_info_matches(info)) {
		g_ptr_array_add(found, g_match_info_fetch(info, 0));
		g_match_info_next(info, NULL);
	}
	return found;
}
static gchar *unescape(const gchar *value)
{
	g_autoptr(GString) out = g_string_new(value);
	g_string_replace(out, "&amp;", "&", 0);
	g_string_replace(out, "&quot;", "\"", 0);
	g_string_replace(out, "&#39;", "'", 0);
	return g_strstrip(g_string_free(g_steal_pointer(&out), FALSE));
}

/* --- The checks ------------------------------------------------------------- */

static void check_plain_text(VentureMailPreview *preview, const gchar *text)
{
	g_autofree gchar *copy = g_strstrip(g_strdup(text != NULL ? text : ""));
	glong length = g_utf8_strlen(copy, -1);
	if (length == 0)
		add_check(preview, "plain-text", VENTURE_MAIL_CHECK_FAIL, g_strdup(
			"There is no plain-text version. Text-only mail apps, some screen readers and many spam filters read only that part."));
	else if (length < 20)
		add_check(preview, "plain-text", VENTURE_MAIL_CHECK_FAIL, g_strdup_printf(
			"The plain-text version is only %ld characters, too short to stand in for the message.", length));
	else
		add_check(preview, "plain-text", VENTURE_MAIL_CHECK_OK, g_strdup_printf(
			"A plain-text version of %ld characters is included.", length));
}
static void check_image_alt(VentureMailPreview *preview, const gchar *html)
{
	g_autoptr(GPtrArray) images = tags(html, "img");
	g_autoptr(GString) missing = g_string_new(NULL);
	guint i, counted = 0, bad = 0;
	for (i = 0; i < images->len; i++) {
		const gchar *tag = g_ptr_array_index(images, i);
		g_autofree gchar *alt = attribute(tag, "alt");
		g_autofree gchar *width = attribute(tag, "width");
		g_autofree gchar *height = attribute(tag, "height");
		/* A one-pixel open-tracking image is not content, and an empty
		 * alt is exactly right for it. */
		if (g_strcmp0(width, "1") == 0 && g_strcmp0(height, "1") == 0)
			continue;
		counted++;
		if (alt != NULL && *g_strstrip(alt) != '\0')
			continue;
		bad++;
		if (bad <= 3) {
			g_autofree gchar *src = attribute(tag, "src");
			g_string_append_printf(missing, "%s%s", missing->len ? ", " : "", src ? src : "(no source)");
		}
	}
	if (counted == 0)
		add_check(preview, "image-alt", VENTURE_MAIL_CHECK_OK, g_strdup("There are no images."));
	else if (bad > 0)
		add_check(preview, "image-alt", VENTURE_MAIL_CHECK_FAIL, g_strdup_printf(
			"%u of %u images have no alt text, so a reader with images off sees nothing there: %s.", bad, counted, missing->str));
	else
		add_check(preview, "image-alt", VENTURE_MAIL_CHECK_OK, counted == 1 ? g_strdup("The image has alt text.") :
			g_strdup_printf("All %u images have alt text.", counted));
}
static gboolean well_formed(const gchar *url)
{
	g_autoptr(GUri) uri = NULL;
	if (g_ascii_strncasecmp(url, "mailto:", 7) == 0)
		return strchr(url + 7, '@') != NULL && url[7] != '@';
	if (g_ascii_strncasecmp(url, "tel:", 4) == 0)
		return url[4] != '\0';
	if (g_ascii_strncasecmp(url, "http://", 7) != 0 && g_ascii_strncasecmp(url, "https://", 8) != 0)
		return FALSE;
	uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	return uri != NULL && !venture_string_is_empty(g_uri_get_host(uri)) && g_uri_get_userinfo(uri) == NULL;
}
static gint compare_links(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}
static void check_links(VentureMailPreview *preview, const gchar *text, const gchar *html,
	VentureMailLinkChecker *checker, GCancellable *cancellable)
{
	g_autoptr(GPtrArray) anchors = tags(html, "a");
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GPtrArray) links = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GString) bad = g_string_new(NULL), unreachable = g_string_new(NULL);
	guint i, broken = 0, asked = 0, failed = 0, web = 0;
	for (i = 0; i < anchors->len; i++) {
		g_autofree gchar *href = attribute(g_ptr_array_index(anchors, i), "href");
		if (href == NULL)
			continue;
		g_ptr_array_add(links, unescape(href));
	}
	{
		g_autoptr(GHashTable) in_text = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		GHashTableIter iter;
		gpointer key;
		collect_urls(in_text, text);
		g_hash_table_iter_init(&iter, in_text);
		while (g_hash_table_iter_next(&iter, &key, NULL))
			g_ptr_array_add(links, g_strdup(key));
	}
	/* Text links are collected unordered; sort for a stable message. */
	g_ptr_array_sort(links, compare_links);
	for (i = 0; i < links->len; i++) {
		const gchar *url = g_ptr_array_index(links, i);
		if (url[0] == '#')
			continue;
		if (!g_hash_table_add(seen, g_strdup(url)))
			continue;
		if (!well_formed(url)) {
			broken++;
			if (broken <= 3)
				g_string_append_printf(bad, "%s\"%s\"", bad->len ? ", " : "", url);
			continue;
		}
		if (g_ascii_strncasecmp(url, "http", 4) != 0)
			continue;
		web++;
		if (checker == NULL || g_strcmp0(url, VENTURE_MAIL_PREVIEW_PLACEHOLDER_URL) == 0 ||
			asked >= VENTURE_MAIL_PREVIEW_MAX_REACHABILITY)
			continue;
		asked++;
		{
			g_autoptr(GError) error = NULL;
			if (!venture_mail_link_checker_check(checker, url, cancellable, &error)) {
				g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
				failed++;
				if (failed <= 3)
					g_string_append_printf(unreachable, "%s%s (%s)", unreachable->len ? ", " : "",
						uri != NULL ? g_uri_get_host(uri) : url, error != NULL ? error->message : "no answer");
			}
		}
	}
	if (broken > 0)
		add_check(preview, "links", VENTURE_MAIL_CHECK_FAIL, g_strdup_printf(
			"%u link%s cannot work as written (only http, https, mailto and tel addresses with a host open from a mail app): %s.",
			broken, broken == 1 ? "" : "s", bad->str));
	else if (failed > 0)
		add_check(preview, "links", VENTURE_MAIL_CHECK_WARN, g_strdup_printf(
			"%u of %u checked web link%s did not answer: %s.", failed, asked, asked == 1 ? "" : "s", unreachable->str));
	else if (g_hash_table_size(seen) == 0)
		add_check(preview, "links", VENTURE_MAIL_CHECK_OK, g_strdup("There are no links."));
	else if (checker == NULL)
		add_check(preview, "links", VENTURE_MAIL_CHECK_OK, g_hash_table_size(seen) == 1 ?
			g_strdup("The link is well formed; whether it answers was not checked (that is off unless asked for).") :
			g_strdup_printf("All %u links are well formed; whether they answer was not checked (that is off unless asked for).",
				g_hash_table_size(seen)));
	else
		add_check(preview, "links", VENTURE_MAIL_CHECK_OK, g_strdup_printf(
			"All %u links are well formed and the %u web link%s checked answered%s.", g_hash_table_size(seen), asked,
			asked == 1 ? "" : "s", web > asked ? "; withheld private links and links past the first ten were not asked about" : ""));
}
static void check_size(VentureMailPreview *preview, const gchar *html)
{
	gsize size = html != NULL ? strlen(html) : 0;
	if (html == NULL)
		add_check(preview, "size", VENTURE_MAIL_CHECK_OK, g_strdup("There is no HTML part, so nothing can be clipped."));
	else if (size > VENTURE_MAIL_PREVIEW_CLIP_BYTES)
		add_check(preview, "size", VENTURE_MAIL_CHECK_WARN, g_strdup_printf(
			"The HTML is %" G_GSIZE_FORMAT " KB. Above about 102 KB a major webmail client cuts the message off behind a \"view entire message\" link, and anything after the cut (often the footer and unsubscribe link) is hidden.",
			size / 1024));
	else
		add_check(preview, "size", VENTURE_MAIL_CHECK_OK, g_strdup_printf(
			"The HTML is %" G_GSIZE_FORMAT " KB, under the clipping limit of about 102 KB.", (size + 1023) / 1024));
}
static gboolean mentions_unsubscribe(const gchar *body)
{
	g_autofree gchar *folded = body != NULL ? g_utf8_casefold(body, -1) : NULL;
	return folded != NULL && strstr(folded, "unsubscribe") != NULL;
}
static void check_unsubscribe(VentureMailPreview *preview, gboolean bulk, const gchar *text, const gchar *html,
	const gchar *endpoint, gboolean header)
{
	gboolean link;
	if (!bulk) {
		add_check(preview, "unsubscribe", VENTURE_MAIL_CHECK_OK, g_strdup(
			"Not required: this is transactional mail about the reader's own business, not marketing."));
		return;
	}
	/* The endpoint itself in a body, or the word beside a link. */
	link = (!venture_string_is_empty(endpoint) && ((text && strstr(text, endpoint)) || (html && strstr(html, endpoint)))) ||
		((mentions_unsubscribe(text) || mentions_unsubscribe(html)) &&
		 (g_regex_match(url_regex(), text != NULL ? text : "", 0, NULL) || g_regex_match(url_regex(), html != NULL ? html : "", 0, NULL)));
	if (link && header)
		add_check(preview, "unsubscribe", VENTURE_MAIL_CHECK_OK, g_strdup(
			"Bulk mail: an unsubscribe link is in the body and the one-click List-Unsubscribe header is set."));
	else if (link)
		add_check(preview, "unsubscribe", VENTURE_MAIL_CHECK_WARN, g_strdup(
			"Bulk mail: the body has an unsubscribe link, but there is no List-Unsubscribe header, which large mailbox providers require of bulk senders. It needs a public HTTPS address."));
	else
		add_check(preview, "unsubscribe", VENTURE_MAIL_CHECK_FAIL, g_strdup(
			"Bulk mail must let the reader unsubscribe, and this message has no unsubscribe link or header."));
}

/* --- Rendering -------------------------------------------------------------- */

static void attachment_names(VentureMailPreview *preview, VentureMailMessage *message)
{
	g_autofree gchar *refs = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonArray *array;
	guint i;
	g_object_get(message, "attachments", &refs, NULL);
	if (venture_string_is_empty(refs))
		return;
	node = json_from_string(refs, NULL);
	if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
		return;
	array = json_node_get_array(node);
	for (i = 0; i < json_array_get_length(array); i++) {
		JsonNode *element = json_array_get_element(array, i);
		JsonObject *ref;
		if (!JSON_NODE_HOLDS_OBJECT(element))
			continue;
		ref = json_node_get_object(element);
		if (json_object_has_member(ref, "name"))
			g_ptr_array_add(preview->attachments, g_strdup(venture_json_object_get_string(ref, "name", "attachment")));
		else
			g_ptr_array_add(preview->attachments, g_strdup_printf("document #%" G_GINT64_FORMAT,
				venture_json_object_get_int(ref, "id", 0)));
	}
}
VentureMailPreview *venture_mail_preview_render(VentureMailMessage *message, const gchar *kind,
	gboolean bulk, VentureMailLinkChecker *checker, GCancellable *cancellable)
{
	VentureMailPreview *preview;
	g_autofree gchar *text = NULL, *html = NULL, *public_text = NULL, *public_html = NULL, *endpoint = NULL;
	g_autoptr(GHashTable) public_urls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	Redaction redaction;
	gboolean header = FALSE;
	g_return_val_if_fail(VENTURE_IS_MAIL_MESSAGE(message), NULL);
	preview = g_new0(VentureMailPreview, 1);
	preview->kind = g_strdup(kind != NULL ? kind : "message");
	preview->bulk = bulk;
	preview->headers = g_ptr_array_new_with_free_func(g_free);
	preview->attachments = g_ptr_array_new_with_free_func(g_free);
	preview->checks = g_ptr_array_new_with_free_func(check_free);
	g_object_get(message, "to", &preview->to, "subject", &preview->subject, "text-body", &public_text,
		"html-body", &public_html, "private-unsubscribe-url", &endpoint, NULL);
	venture_mail_visible_bodies(message, &text, &html);
	collect_urls(public_urls, public_text);
	collect_urls(public_urls, public_html);
	redaction.public_urls = public_urls;
	redaction.unsubscribe = endpoint;
	redaction.withheld = 0;
	preview->text = redact(&redaction, text);
	preview->html = redact(&redaction, html);
	/* The transport's own rule: one-click signalling needs HTTPS. */
	if (!venture_string_is_empty(endpoint) && g_str_has_prefix(endpoint, "https://")) {
		header = TRUE;
		g_ptr_array_add(preview->headers, g_strdup("List-Unsubscribe: <" VENTURE_MAIL_PREVIEW_PLACEHOLDER_URL ">"));
		g_ptr_array_add(preview->headers, g_strdup("List-Unsubscribe-Post: List-Unsubscribe=One-Click"));
	}
	attachment_names(preview, message);
	check_plain_text(preview, text);
	check_image_alt(preview, html);
	check_links(preview, preview->text, preview->html, checker, cancellable);
	check_size(preview, html);
	check_unsubscribe(preview, bulk, text, html, endpoint, header);
	return preview;
}

/* --- Kinds ------------------------------------------------------------------- */

static GPtrArray *registered;
static GMutex registry_lock;
void venture_mail_preview_register(const VentureMailPreviewKind *kind)
{
	guint i;
	g_return_if_fail(kind != NULL && kind->id != NULL && kind->build != NULL);
	g_mutex_lock(&registry_lock);
	if (registered == NULL)
		registered = g_ptr_array_new();
	for (i = 0; i < registered->len; i++)
		if (g_strcmp0(((const VentureMailPreviewKind *)g_ptr_array_index(registered, i))->id, kind->id) == 0) {
			g_mutex_unlock(&registry_lock);
			g_critical("A mail preview kind \"%s\" is already registered", kind->id);
			return;
		}
	g_ptr_array_add(registered, (gpointer)kind);
	g_mutex_unlock(&registry_lock);
}
/* Each sending module registers its own kinds; this is the one place that
 * asks them to, the way the report registry asks for reports. */
static void ensure_kinds(void)
{
	static gsize done;
	if (g_once_init_enter(&done)) {
		venture_mail_consumers_preview_register();
		venture_quote_mail_preview_register();
		venture_dunning_preview_register();
		venture_financial_documents_preview_register();
		venture_marketing_preview_register();
		venture_sequence_preview_register();
		venture_portal_preview_register();
		venture_email_design_preview_register();
		g_once_init_leave(&done, 1);
	}
}
const VentureMailPreviewKind *venture_mail_preview_lookup(const gchar *id)
{
	const VentureMailPreviewKind *found = NULL;
	guint i;
	ensure_kinds();
	g_mutex_lock(&registry_lock);
	for (i = 0; registered != NULL && i < registered->len && found == NULL; i++)
		if (g_strcmp0(((const VentureMailPreviewKind *)g_ptr_array_index(registered, i))->id, id) == 0)
			found = g_ptr_array_index(registered, i);
	g_mutex_unlock(&registry_lock);
	return found;
}
GPtrArray *venture_mail_preview_kinds(void)
{
	GPtrArray *copy = g_ptr_array_new();
	guint i;
	ensure_kinds();
	g_mutex_lock(&registry_lock);
	for (i = 0; registered != NULL && i < registered->len; i++)
		g_ptr_array_add(copy, g_ptr_array_index(registered, i));
	g_mutex_unlock(&registry_lock);
	return copy;
}
static const VentureMailPreviewKind *available(VentureContext *context, const gchar *id, GError **error)
{
	const VentureMailPreviewKind *kind = venture_mail_preview_lookup(id);
	if (kind == NULL) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"There is no previewable email kind \"%s\"", id != NULL ? id : "");
		return NULL;
	}
	if (!venture_context_module_enabled(context, "mail") ||
		(kind->module != NULL && !venture_context_module_enabled(context, kind->module)) ||
		venture_entity_registry_lookup(venture_entity_registry_get_default(), kind->source_type) == G_TYPE_INVALID) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			"%s previews need the mail and %s modules", kind->label, kind->module != NULL ? kind->module : "mail");
		return NULL;
	}
	return kind;
}
static VentureMailPreview *finish(VentureMailPreview *preview, GPtrArray *notes)
{
	guint i;
	for (i = 0; notes != NULL && i < notes->len; i++)
		add_check(preview, "source", VENTURE_MAIL_CHECK_WARN, g_strdup(g_ptr_array_index(notes, i)));
	return preview;
}
VentureMailPreview *venture_mail_preview_build(VentureContext *context, const gchar *kind_id,
	gint64 organization_id, gint64 source_id, GDateTime *now, VentureMailLinkChecker *checker, GError **error)
{
	const VentureMailPreviewKind *kind;
	g_autoptr(VentureMailMessage) message = NULL;
	g_autoptr(GPtrArray) notes = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GDateTime) clock = now != NULL ? g_date_time_ref(now) : venture_time_now();
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	kind = available(context, kind_id, error);
	if (kind == NULL)
		return NULL;
	if (organization_id <= 0 || source_id <= 0) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Record not found");
		return NULL;
	}
	message = kind->build(context, organization_id, source_id, clock, notes, error);
	if (message == NULL)
		return NULL;
	return finish(venture_mail_preview_render(message, kind->id, kind->bulk, checker, NULL), notes);
}
static gboolean bulk_related(const gchar *related)
{
	g_autoptr(GPtrArray) kinds = venture_mail_preview_kinds();
	guint i;
	for (i = 0; related != NULL && i < kinds->len; i++) {
		const VentureMailPreviewKind *kind = g_ptr_array_index(kinds, i);
		if (kind->bulk && g_strcmp0(kind->related_type, related) == 0)
			return TRUE;
	}
	return FALSE;
}
VentureMailPreview *venture_mail_preview_message(VentureContext *context, gint64 organization_id,
	gint64 message_id, VentureMailLinkChecker *checker, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autofree gchar *related = NULL, *endpoint = NULL;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message") == G_TYPE_INVALID) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "The mail module is off");
		return NULL;
	}
	if (organization_id > 0 && message_id > 0)
		row = venture_database_get(venture_context_get_database(context), VENTURE_TYPE_MAIL_MESSAGE, message_id, NULL);
	if (row == NULL || venture_entity_is_deleted(row) || venture_entity_get_organization_id(row) != organization_id) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Mail message not found");
		return NULL;
	}
	g_object_get(row, "related-type", &related, "private-unsubscribe-url", &endpoint, NULL);
	return venture_mail_preview_render(VENTURE_MAIL_MESSAGE(row), "message",
		!venture_string_is_empty(endpoint) || bulk_related(related), checker, NULL);
}

static const gchar *status_name(VentureMailCheckStatus status)
{
	return status == VENTURE_MAIL_CHECK_OK ? "ok" : status == VENTURE_MAIL_CHECK_WARN ? "warn" : "fail";
}
JsonNode *venture_mail_preview_to_json(const VentureMailPreview *preview)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	guint i;
	g_return_val_if_fail(preview != NULL, NULL);
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "kind"); json_builder_add_string_value(b, preview->kind);
	json_builder_set_member_name(b, "to"); json_builder_add_string_value(b, preview->to);
	json_builder_set_member_name(b, "subject"); json_builder_add_string_value(b, preview->subject);
	json_builder_set_member_name(b, "bulk"); json_builder_add_boolean_value(b, preview->bulk);
	json_builder_set_member_name(b, "text"); json_builder_add_string_value(b, preview->text);
	json_builder_set_member_name(b, "html");
	if (preview->html != NULL) json_builder_add_string_value(b, preview->html);
	else json_builder_add_null_value(b);
	json_builder_set_member_name(b, "headers"); json_builder_begin_array(b);
	for (i = 0; i < preview->headers->len; i++) json_builder_add_string_value(b, g_ptr_array_index(preview->headers, i));
	json_builder_end_array(b);
	json_builder_set_member_name(b, "attachments"); json_builder_begin_array(b);
	for (i = 0; i < preview->attachments->len; i++) json_builder_add_string_value(b, g_ptr_array_index(preview->attachments, i));
	json_builder_end_array(b);
	json_builder_set_member_name(b, "checks"); json_builder_begin_array(b);
	for (i = 0; i < preview->checks->len; i++) {
		const VentureMailCheck *check = g_ptr_array_index(preview->checks, i);
		json_builder_begin_object(b);
		json_builder_set_member_name(b, "id"); json_builder_add_string_value(b, check->id);
		json_builder_set_member_name(b, "status"); json_builder_add_string_value(b, status_name(check->status));
		json_builder_set_member_name(b, "message"); json_builder_add_string_value(b, check->message);
		json_builder_end_object(b);
	}
	json_builder_end_array(b);
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

/* --- Test sends -------------------------------------------------------------- */

gchar *venture_mail_preview_actor_email(VentureDatabase *database, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) user = NULL;
	g_autofree gchar *email = NULL;
	if (actor == NULL || venture_string_is_empty(actor->name)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A test send mails the acting user's own address, and this request names no user");
		return NULL;
	}
	query = venture_query_new(VENTURE_TYPE_USER);
	venture_query_add_filter_string(query, "username", VENTURE_FILTER_OP_EQ, actor->name, NULL);
	user = venture_database_find_one(database, query, NULL);
	if (user == NULL || venture_entity_is_deleted(user)) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A test send mails the acting user's own address, and \"%s\" is not a user account", actor->name);
		return NULL;
	}
	g_object_get(user, "email", &email, NULL);
	if (venture_string_is_empty(email) || strpbrk(email, "\r\n,") != NULL || strchr(email, '@') == NULL) {
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A test send mails only the acting user, and %s has no email address", actor->name);
		return NULL;
	}
	return g_strstrip(g_steal_pointer(&email));
}
VentureMailMessage *venture_mail_preview_enqueue_test(VentureDatabase *database, VentureMailMessage *message,
	const gchar *key, const gchar *related_type, gint64 related_id, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureMailPreview) preview = NULL;
	g_autoptr(VentureMailMessage) copy = NULL;
	g_autofree gchar *email = NULL, *subject = NULL, *attachments = NULL;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_MAIL_MESSAGE(message), NULL);
	if (venture_string_is_empty(key)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A test send needs an idempotency key");
		return NULL;
	}
	email = venture_mail_preview_actor_email(database, actor, error);
	if (email == NULL)
		return NULL;
	/* The rendered, redacted alternatives become the copy's public bodies:
	 * the operator's inbox gets exactly what the preview showed, and no
	 * private field leaves the customer's message. */
	preview = venture_mail_preview_render(message, NULL, FALSE, NULL, NULL);
	g_object_get(message, "attachments", &attachments, NULL);
	subject = g_strdup_printf("[TEST] %s", preview->subject != NULL ? preview->subject : "");
	copy = venture_mail_message_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(copy), venture_entity_get_organization_id(VENTURE_ENTITY(message)));
	g_object_set(copy, "to", email, "subject", subject, "text-body", preview->text, "html-body", preview->html,
		"attachments", attachments, "idempotency-key", key,
		"related-type", related_type != NULL ? related_type : "mail_preview", "related-id", related_id, NULL);
	return venture_mail_outbox_enqueue(venture_database_get_mail_outbox(database), copy, actor, error);
}
VentureMailMessage *venture_mail_preview_send_test(VentureContext *context, const gchar *kind_id,
	gint64 organization_id, gint64 source_id, const gchar *token, const VentureActor *actor, GError **error)
{
	const VentureMailPreviewKind *kind;
	g_autoptr(VentureMailMessage) message = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *key = NULL;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	if (token == NULL || !g_uuid_string_is_valid(token)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Reload the preview to send another test");
		return NULL;
	}
	kind = available(context, kind_id, error);
	if (kind == NULL)
		return NULL;
	/* Who it goes to is settled before anything is built. */
	{
		g_autofree gchar *email = venture_mail_preview_actor_email(venture_context_get_database(context), actor, error);
		if (email == NULL)
			return NULL;
	}
	message = kind->build(context, organization_id, source_id, now, NULL, error);
	if (message == NULL)
		return NULL;
	key = g_strdup_printf("mail-preview-test:%s:%" G_GINT64_FORMAT ":%s", kind->id, source_id, token);
	return venture_mail_preview_enqueue_test(venture_context_get_database(context), message, key, NULL, 0, actor, error);
}
