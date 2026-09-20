/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>

/* --- Items ---------------------------------------------------------------- */
G_DEFINE_BOXED_TYPE(VentureCalDavItem, venture_caldav_item, venture_caldav_item_copy, venture_caldav_item_free)
VentureCalDavItem *venture_caldav_item_new(const gchar *href, const gchar *etag)
{
	VentureCalDavItem *item = g_new0(VentureCalDavItem, 1);
	item->href = g_strdup(href);
	item->etag = g_strdup(etag ? etag : "");
	return item;
}
VentureCalDavItem *venture_caldav_item_copy(const VentureCalDavItem *item)
{
	g_return_val_if_fail(item != NULL, NULL);
	return venture_caldav_item_new(item->href, item->etag);
}
void venture_caldav_item_free(VentureCalDavItem *item)
{
	if (!item) return;
	g_free(item->href);
	g_free(item->etag);
	g_free(item);
}

/* --- Interface ------------------------------------------------------------ */
G_DEFINE_INTERFACE(VentureCalDavClient, venture_caldav_client, G_TYPE_OBJECT)
static void venture_caldav_client_default_init(VentureCalDavClientInterface *iface) { (void)iface; }
gboolean venture_caldav_client_connect(VentureCalDavClient *self, const gchar *url, const gchar *username, const gchar *secret, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_CALDAV_CLIENT(self), FALSE);
	g_return_val_if_fail(VENTURE_CALDAV_CLIENT_GET_IFACE(self)->connect != NULL, FALSE);
	return VENTURE_CALDAV_CLIENT_GET_IFACE(self)->connect(self, url, username, secret, cancellable, error);
}
gchar *venture_caldav_client_get_token(VentureCalDavClient *self, const gchar *calendar_path, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_CALDAV_CLIENT(self), NULL);
	g_return_val_if_fail(VENTURE_CALDAV_CLIENT_GET_IFACE(self)->get_token != NULL, NULL);
	return VENTURE_CALDAV_CLIENT_GET_IFACE(self)->get_token(self, calendar_path, cancellable, error);
}
GPtrArray *venture_caldav_client_list(VentureCalDavClient *self, const gchar *calendar_path, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_CALDAV_CLIENT(self), NULL);
	g_return_val_if_fail(VENTURE_CALDAV_CLIENT_GET_IFACE(self)->list != NULL, NULL);
	return VENTURE_CALDAV_CLIENT_GET_IFACE(self)->list(self, calendar_path, cancellable, error);
}
gchar *venture_caldav_client_fetch(VentureCalDavClient *self, const gchar *href, gchar **etag, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_CALDAV_CLIENT(self), NULL);
	g_return_val_if_fail(VENTURE_CALDAV_CLIENT_GET_IFACE(self)->fetch != NULL, NULL);
	return VENTURE_CALDAV_CLIENT_GET_IFACE(self)->fetch(self, href, etag, cancellable, error);
}
gchar *venture_caldav_client_put(VentureCalDavClient *self, const gchar *href, const gchar *ics, const gchar *etag, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_CALDAV_CLIENT(self), NULL);
	g_return_val_if_fail(VENTURE_CALDAV_CLIENT_GET_IFACE(self)->put != NULL, NULL);
	return VENTURE_CALDAV_CLIENT_GET_IFACE(self)->put(self, href, ics, etag, cancellable, error);
}
void venture_caldav_client_disconnect(VentureCalDavClient *self)
{
	g_return_if_fail(VENTURE_IS_CALDAV_CLIENT(self));
	g_return_if_fail(VENTURE_CALDAV_CLIENT_GET_IFACE(self)->disconnect != NULL);
	VENTURE_CALDAV_CLIENT_GET_IFACE(self)->disconnect(self);
}

/* --- Fake: one collection of (href, ics, etag) in memory ------------------ */
typedef struct { gchar *ics; gchar *etag; } FakeEvent;
struct _VentureFakeCalDavClient {
	GObject parent_instance;
	GHashTable *events; /* href -> FakeEvent */
	guint serial;       /* every change moves it; the ctag is derived from it */
	gint connects, puts, fetches;
	gboolean connected;
};
static void fake_iface_init(VentureCalDavClientInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureFakeCalDavClient, venture_fake_caldav_client, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_CALDAV_CLIENT, fake_iface_init))
static void fake_event_free(gpointer data)
{
	FakeEvent *e = data;
	g_free(e->ics); g_free(e->etag); g_free(e);
}
static void fake_finalize(GObject *object)
{
	VentureFakeCalDavClient *self = VENTURE_FAKE_CALDAV_CLIENT(object);
	g_hash_table_unref(self->events);
	G_OBJECT_CLASS(venture_fake_caldav_client_parent_class)->finalize(object);
}
static guint fake_response_ready;
static void venture_fake_caldav_client_class_init(VentureFakeCalDavClientClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = fake_finalize;
	/**
	 * VentureFakeCalDavClient::response-ready:
	 * @self: fake transport
	 *
	 * Emitted synchronously after copying a successful fetch response, before
	 * returning it to the consumer. Fixtures may revoke authorization here
	 * to exercise state changes while a real provider request was pending.
	 */
	fake_response_ready = g_signal_new("response-ready", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void venture_fake_caldav_client_init(VentureFakeCalDavClient *self)
{
	self->events = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, fake_event_free);
}
static void fake_store(VentureFakeCalDavClient *self, const gchar *href, const gchar *ics)
{
	FakeEvent *e = g_new0(FakeEvent, 1);
	self->serial++;
	e->ics = g_strdup(ics);
	e->etag = g_strdup_printf("\"%u\"", self->serial);
	g_hash_table_replace(self->events, g_strdup(href), e);
}
static gboolean fake_connect(VentureCalDavClient *client, const gchar *url, const gchar *username, const gchar *secret, GCancellable *cancellable, GError **error)
{
	VentureFakeCalDavClient *self = VENTURE_FAKE_CALDAV_CLIENT(client);
	(void)url; (void)username; (void)cancellable;
	if (!secret || !*secret) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "CalDAV secret is empty"); return FALSE; }
	self->connects++;
	self->connected = TRUE;
	return TRUE;
}
static gboolean fake_ready(VentureFakeCalDavClient *self, GError **error)
{
	if (self->connected) return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "CalDAV client is not connected");
	return FALSE;
}
static gchar *fake_get_token(VentureCalDavClient *client, const gchar *calendar_path, GCancellable *cancellable, GError **error)
{
	VentureFakeCalDavClient *self = VENTURE_FAKE_CALDAV_CLIENT(client);
	(void)calendar_path; (void)cancellable;
	if (!fake_ready(self, error)) return NULL;
	return g_strdup_printf("ctag-%u", self->serial);
}
static GPtrArray *fake_list(VentureCalDavClient *client, const gchar *calendar_path, GCancellable *cancellable, GError **error)
{
	VentureFakeCalDavClient *self = VENTURE_FAKE_CALDAV_CLIENT(client);
	GPtrArray *items;
	GHashTableIter iter;
	gpointer key, value;
	(void)calendar_path; (void)cancellable;
	if (!fake_ready(self, error)) return NULL;
	items = g_ptr_array_new_with_free_func((GDestroyNotify)venture_caldav_item_free);
	g_hash_table_iter_init(&iter, self->events);
	while (g_hash_table_iter_next(&iter, &key, &value))
		g_ptr_array_add(items, venture_caldav_item_new(key, ((FakeEvent *)value)->etag));
	return items;
}
static gchar *fake_fetch(VentureCalDavClient *client, const gchar *href, gchar **etag, GCancellable *cancellable, GError **error)
{
	VentureFakeCalDavClient *self = VENTURE_FAKE_CALDAV_CLIENT(client);
	FakeEvent *e;
	gchar *response;
	(void)cancellable;
	if (!fake_ready(self, error)) return NULL;
	e = g_hash_table_lookup(self->events, href);
	if (!e) { g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No event at %s", href); return NULL; }
	self->fetches++;
	if (etag) *etag = g_strdup(e->etag);
	response = g_strdup(e->ics);
	g_signal_emit(client, fake_response_ready, 0);
	return response;
}
static gchar *fake_put(VentureCalDavClient *client, const gchar *href, const gchar *ics, const gchar *etag, GCancellable *cancellable, GError **error)
{
	VentureFakeCalDavClient *self = VENTURE_FAKE_CALDAV_CLIENT(client);
	FakeEvent *e;
	(void)cancellable;
	if (!fake_ready(self, error)) return NULL;
	e = g_hash_table_lookup(self->events, href);
	/* The same preconditions a server enforces: create must not clobber,
	 * replace must match what the caller last saw. */
	if (!etag && e) { g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "An event already exists at %s", href); return NULL; }
	if (etag && (!e || g_strcmp0(e->etag, etag))) { g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "The event at %s changed on the server", href); return NULL; }
	self->puts++;
	fake_store(self, href, ics);
	return g_strdup(((FakeEvent *)g_hash_table_lookup(self->events, href))->etag);
}
static void fake_disconnect(VentureCalDavClient *client) { VENTURE_FAKE_CALDAV_CLIENT(client)->connected = FALSE; }
static void fake_iface_init(VentureCalDavClientInterface *iface)
{
	iface->connect = fake_connect;
	iface->get_token = fake_get_token;
	iface->list = fake_list;
	iface->fetch = fake_fetch;
	iface->put = fake_put;
	iface->disconnect = fake_disconnect;
}
VentureFakeCalDavClient *venture_fake_caldav_client_new(void) { return g_object_new(VENTURE_TYPE_FAKE_CALDAV_CLIENT, NULL); }
void venture_fake_caldav_client_set_remote(VentureFakeCalDavClient *self, const gchar *href, const gchar *ics)
{
	g_return_if_fail(VENTURE_IS_FAKE_CALDAV_CLIENT(self));
	g_return_if_fail(href != NULL && ics != NULL);
	fake_store(self, href, ics);
}
void venture_fake_caldav_client_remove_remote(VentureFakeCalDavClient *self, const gchar *href)
{
	g_return_if_fail(VENTURE_IS_FAKE_CALDAV_CLIENT(self));
	if (g_hash_table_remove(self->events, href)) self->serial++;
}
const gchar *venture_fake_caldav_client_get_remote(VentureFakeCalDavClient *self, const gchar *href)
{
	FakeEvent *e;
	g_return_val_if_fail(VENTURE_IS_FAKE_CALDAV_CLIENT(self), NULL);
	e = g_hash_table_lookup(self->events, href);
	return e ? e->ics : NULL;
}
GStrv venture_fake_caldav_client_get_hrefs(VentureFakeCalDavClient *self)
{
	GPtrArray *hrefs;
	GHashTableIter iter;
	gpointer key;
	g_return_val_if_fail(VENTURE_IS_FAKE_CALDAV_CLIENT(self), NULL);
	hrefs = g_ptr_array_new();
	g_hash_table_iter_init(&iter, self->events);
	while (g_hash_table_iter_next(&iter, &key, NULL)) g_ptr_array_add(hrefs, g_strdup(key));
	g_ptr_array_sort_values(hrefs, (GCompareFunc)g_strcmp0);
	g_ptr_array_add(hrefs, NULL);
	return (GStrv)g_ptr_array_free(hrefs, FALSE);
}
gint venture_fake_caldav_client_get_connects(VentureFakeCalDavClient *self) { g_return_val_if_fail(VENTURE_IS_FAKE_CALDAV_CLIENT(self), 0); return self->connects; }
gint venture_fake_caldav_client_get_puts(VentureFakeCalDavClient *self) { g_return_val_if_fail(VENTURE_IS_FAKE_CALDAV_CLIENT(self), 0); return self->puts; }
gint venture_fake_caldav_client_get_fetches(VentureFakeCalDavClient *self) { g_return_val_if_fail(VENTURE_IS_FAKE_CALDAV_CLIENT(self), 0); return self->fetches; }

/* --- libsoup: PROPFIND, GET and PUT with basic authentication ------------- */
#define SOUP_CALDAV_LIMIT (20u * 1024u * 1024u)
struct _VentureSoupCalDavClient {
	GObject parent_instance;
	SoupSession *session;
	gchar *base;          /* scheme://host[:port], no trailing slash */
	gchar *authorization; /* "Basic ..." for the session; cleared on disconnect */
};
static void soup_iface_init(VentureCalDavClientInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureSoupCalDavClient, venture_soup_caldav_client, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_CALDAV_CLIENT, soup_iface_init))
static void soup_forget(VentureSoupCalDavClient *self)
{
	if (self->authorization) memset(self->authorization, 0, strlen(self->authorization));
	g_clear_pointer(&self->authorization, g_free);
	g_clear_pointer(&self->base, g_free);
}
static void soup_finalize(GObject *object)
{
	VentureSoupCalDavClient *self = VENTURE_SOUP_CALDAV_CLIENT(object);
	soup_forget(self);
	g_clear_object(&self->session);
	G_OBJECT_CLASS(venture_soup_caldav_client_parent_class)->finalize(object);
}
static void venture_soup_caldav_client_class_init(VentureSoupCalDavClientClass *klass) { G_OBJECT_CLASS(klass)->finalize = soup_finalize; }
static void venture_soup_caldav_client_init(VentureSoupCalDavClient *self)
{
	self->session = soup_session_new();
	soup_session_set_timeout(self->session, 30);
	soup_session_set_user_agent(self->session, "VENTURE/" VENTURE_VERSION_S " ");
}
static gboolean has_control(const gchar *s)
{
	return s && (strchr(s, '\r') || strchr(s, '\n'));
}
static gboolean soup_connect(VentureCalDavClient *client, const gchar *url, const gchar *username, const gchar *secret, GCancellable *cancellable, GError **error)
{
	VentureSoupCalDavClient *self = VENTURE_SOUP_CALDAV_CLIENT(client);
	g_autoptr(GUri) uri = NULL;
	g_autofree gchar *pair = NULL, *encoded = NULL;
	gint port;
	(void)cancellable;
	soup_forget(self);
	if (!url || !*url) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "The account has no CalDAV URL"); return FALSE; }
	uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	if (!uri || (g_strcmp0(g_uri_get_scheme(uri), "https") && g_strcmp0(g_uri_get_scheme(uri), "http")) || !g_uri_get_host(uri)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "The CalDAV URL must be an http or https URL"); return FALSE;
	}
	/* A username or secret carrying a line break could smuggle a header. */
	if (has_control(username) || has_control(secret)) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "CalDAV credentials cannot contain line breaks"); return FALSE; }
	port = g_uri_get_port(uri);
	self->base = port > 0 ? g_strdup_printf("%s://%s:%d", g_uri_get_scheme(uri), g_uri_get_host(uri), port)
		: g_strdup_printf("%s://%s", g_uri_get_scheme(uri), g_uri_get_host(uri));
	pair = g_strdup_printf("%s:%s", username ? username : "", secret ? secret : "");
	encoded = g_base64_encode((const guchar *)pair, strlen(pair));
	memset(pair, 0, strlen(pair));
	self->authorization = g_strdup_printf("Basic %s", encoded);
	return TRUE;
}
/* Every request the service makes goes through here: one place for the
 * credential, the size cap and the status-to-error mapping. Server bodies
 * are never copied into error messages. */
static GBytes *soup_request(VentureSoupCalDavClient *self, const gchar *method, const gchar *href, const gchar *depth, const gchar *content_type, const gchar *body,
	const gchar *if_match, const gchar *if_none_match, guint *status_out, gchar **etag_out, GCancellable *cancellable, GError **error)
{
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	g_autoptr(GBytes) response = NULL;
	SoupMessageHeaders *headers;
	guint status;
	if (!self->authorization || !self->base) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "CalDAV client is not connected"); return NULL; }
	if (!href || *href != '/' || has_control(href)) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "A CalDAV path must be absolute"); return NULL; }
	url = g_strconcat(self->base, href, NULL);
	message = soup_message_new(method, url);
	if (!message) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "The CalDAV URL is not valid"); return NULL; }
	headers = soup_message_get_request_headers(message);
	soup_message_headers_append(headers, "Authorization", self->authorization);
	if (depth) soup_message_headers_append(headers, "Depth", depth);
	if (if_match) soup_message_headers_append(headers, "If-Match", if_match);
	if (if_none_match) soup_message_headers_append(headers, "If-None-Match", if_none_match);
	if (body) {
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}
	response = soup_session_send_and_read(self->session, message, cancellable, error);
	if (!response) return NULL;
	status = soup_message_get_status(message);
	if (status_out) *status_out = status;
	if (g_bytes_get_size(response) > SOUP_CALDAV_LIMIT) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "CalDAV response larger than 20 MiB refused"); return NULL; }
	if (status == 401 || status == 403) { g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "The CalDAV server refused the credential"); return NULL; }
	if (status == 404) { g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "The CalDAV server has nothing at %s", href); return NULL; }
	if (status == 412) { g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "The event at %s changed on the server", href); return NULL; }
	if (status < 200 || status >= 300) { g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "The CalDAV server answered %u to %s", status, method); return NULL; }
	if (etag_out) *etag_out = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "ETag"));
	return g_steal_pointer(&response);
}
/* A multistatus reduced to what the service reads: per response, its href,
 * getetag, getctag/sync-token and whether it is a collection. Prefixes are
 * whatever the server chose, so elements are matched by local name. */
typedef struct { gchar *href, *etag, *token; gboolean collection; } DavResponse;
typedef struct { GPtrArray *responses; DavResponse *current; GString *text; gboolean in_resourcetype; } DavParse;
static void dav_response_free(gpointer data)
{
	DavResponse *r = data;
	g_free(r->href); g_free(r->etag); g_free(r->token); g_free(r);
}
static const gchar *local_name(const gchar *element)
{
	const gchar *colon = strrchr(element, ':');
	return colon ? colon + 1 : element;
}
static void dav_start(GMarkupParseContext *context, const gchar *element, const gchar **names, const gchar **values, gpointer data, GError **error)
{
	DavParse *p = data;
	const gchar *name = local_name(element);
	(void)context; (void)names; (void)values; (void)error;
	if (!g_strcmp0(name, "response")) { p->current = g_new0(DavResponse, 1); g_ptr_array_add(p->responses, p->current); }
	else if (!g_strcmp0(name, "resourcetype")) p->in_resourcetype = TRUE;
	else if (p->in_resourcetype && !g_strcmp0(name, "collection") && p->current) p->current->collection = TRUE;
	g_string_truncate(p->text, 0);
}
static void dav_end(GMarkupParseContext *context, const gchar *element, gpointer data, GError **error)
{
	DavParse *p = data;
	const gchar *name = local_name(element);
	g_autofree gchar *value = g_strstrip(g_strdup(p->text->str));
	(void)context; (void)error;
	if (!g_strcmp0(name, "resourcetype")) p->in_resourcetype = FALSE;
	if (!p->current) return;
	if (!g_strcmp0(name, "href") && !p->current->href) p->current->href = g_steal_pointer(&value);
	else if (!g_strcmp0(name, "getetag")) { g_free(p->current->etag); p->current->etag = g_steal_pointer(&value); }
	else if ((!g_strcmp0(name, "getctag") || !g_strcmp0(name, "sync-token")) && !p->current->token) p->current->token = g_steal_pointer(&value);
	else if (!g_strcmp0(name, "response")) p->current = NULL;
	g_string_truncate(p->text, 0);
}
static void dav_text(GMarkupParseContext *context, const gchar *text, gsize length, gpointer data, GError **error)
{
	DavParse *p = data;
	(void)context; (void)error;
	g_string_append_len(p->text, text, (gssize)length);
}
static GPtrArray *dav_parse(GBytes *body, GError **error)
{
	static const GMarkupParser parser = { dav_start, dav_end, dav_text, NULL, NULL };
	g_autoptr(GMarkupParseContext) context = NULL;
	DavParse p;
	gsize length;
	const gchar *data = g_bytes_get_data(body, &length);
	p.responses = g_ptr_array_new_with_free_func(dav_response_free);
	p.current = NULL;
	p.text = g_string_new(NULL);
	p.in_resourcetype = FALSE;
	context = g_markup_parse_context_new(&parser, 0, &p, NULL);
	if (!g_markup_parse_context_parse(context, data ? data : "", (gssize)length, NULL) || !g_markup_parse_context_end_parse(context, NULL)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "The CalDAV server answered with something other than a multistatus");
		g_ptr_array_unref(p.responses);
		g_string_free(p.text, TRUE);
		return NULL;
	}
	g_string_free(p.text, TRUE);
	return p.responses;
}
static gchar *soup_propfind_first(VentureSoupCalDavClient *self, const gchar *href, const gchar *props, gboolean want_etag, GCancellable *cancellable, GError **error)
{
	g_autofree gchar *body = g_strdup_printf("<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\" xmlns:cs=\"http://calendarserver.org/ns/\"><d:prop>%s</d:prop></d:propfind>", props);
	g_autoptr(GBytes) response = soup_request(self, "PROPFIND", href, "0", "application/xml; charset=utf-8", body, NULL, NULL, NULL, NULL, cancellable, error);
	g_autoptr(GPtrArray) responses = NULL;
	guint i;
	if (!response) return NULL;
	responses = dav_parse(response, error);
	if (!responses) return NULL;
	for (i = 0; i < responses->len; i++) {
		DavResponse *r = g_ptr_array_index(responses, i);
		const gchar *value = want_etag ? r->etag : r->token;
		if (value && *value) return g_strdup(value);
	}
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "The CalDAV server reported no %s for %s", want_etag ? "etag" : "ctag", href);
	return NULL;
}
static gchar *soup_get_token(VentureCalDavClient *client, const gchar *calendar_path, GCancellable *cancellable, GError **error)
{
	return soup_propfind_first(VENTURE_SOUP_CALDAV_CLIENT(client), calendar_path, "<cs:getctag/><d:sync-token/>", FALSE, cancellable, error);
}
static GPtrArray *soup_list(VentureCalDavClient *client, const gchar *calendar_path, GCancellable *cancellable, GError **error)
{
	VentureSoupCalDavClient *self = VENTURE_SOUP_CALDAV_CLIENT(client);
	static const gchar *body = "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:getetag/><d:resourcetype/></d:prop></d:propfind>";
	g_autoptr(GBytes) response = soup_request(self, "PROPFIND", calendar_path, "1", "application/xml; charset=utf-8", body, NULL, NULL, NULL, NULL, cancellable, error);
	g_autoptr(GPtrArray) responses = NULL;
	GPtrArray *items;
	guint i;
	if (!response) return NULL;
	responses = dav_parse(response, error);
	if (!responses) return NULL;
	items = g_ptr_array_new_with_free_func((GDestroyNotify)venture_caldav_item_free);
	for (i = 0; i < responses->len; i++) {
		DavResponse *r = g_ptr_array_index(responses, i);
		g_autofree gchar *path = NULL;
		if (r->collection || !r->href || !r->etag || !*r->etag) continue;
		/* Servers answer with either a path or a full URL; keep the path. */
		if (g_str_has_prefix(r->href, "http://") || g_str_has_prefix(r->href, "https://")) {
			g_autoptr(GUri) uri = g_uri_parse(r->href, G_URI_FLAGS_NONE, NULL);
			if (!uri) continue;
			path = g_strdup(g_uri_get_path(uri));
		} else path = g_strdup(r->href);
		g_ptr_array_add(items, venture_caldav_item_new(path, r->etag));
	}
	return items;
}
static gchar *soup_fetch(VentureCalDavClient *client, const gchar *href, gchar **etag, GCancellable *cancellable, GError **error)
{
	g_autoptr(GBytes) response = soup_request(VENTURE_SOUP_CALDAV_CLIENT(client), "GET", href, NULL, NULL, NULL, NULL, NULL, NULL, etag, cancellable, error);
	gsize length;
	const gchar *data;
	if (!response) return NULL;
	data = g_bytes_get_data(response, &length);
	if (data && !g_utf8_validate(data, (gssize)length, NULL)) { g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "The event at %s is not UTF-8", href); return NULL; }
	return g_strndup(data ? data : "", length);
}
static gchar *soup_put(VentureCalDavClient *client, const gchar *href, const gchar *ics, const gchar *etag, GCancellable *cancellable, GError **error)
{
	VentureSoupCalDavClient *self = VENTURE_SOUP_CALDAV_CLIENT(client);
	g_autofree gchar *new_etag = NULL;
	g_autoptr(GBytes) response = soup_request(self, "PUT", href, NULL, "text/calendar; charset=utf-8", ics, etag, etag ? NULL : "*", NULL, &new_etag, cancellable, error);
	if (!response) return NULL;
	/* Not every server returns the new tag on PUT; ask for it. */
	if (new_etag && *new_etag) return g_steal_pointer(&new_etag);
	return soup_propfind_first(self, href, "<d:getetag/>", TRUE, cancellable, error);
}
static void soup_disconnect(VentureCalDavClient *client) { soup_forget(VENTURE_SOUP_CALDAV_CLIENT(client)); }
static void soup_iface_init(VentureCalDavClientInterface *iface)
{
	iface->connect = soup_connect;
	iface->get_token = soup_get_token;
	iface->list = soup_list;
	iface->fetch = soup_fetch;
	iface->put = soup_put;
	iface->disconnect = soup_disconnect;
}
VentureSoupCalDavClient *venture_soup_caldav_client_new(void) { return g_object_new(VENTURE_TYPE_SOUP_CALDAV_CLIENT, NULL); }
