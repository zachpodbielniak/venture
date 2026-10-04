/*
 * venture-data-source-provider.c - Providers, their registry and the request
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The request is where every rule about reaching outside lives, so that a
 * provider -- built in, or a plugin's -- gets them by calling one function
 * rather than by remembering them:
 *
 * - an origin on the frozen allowlist, compared again on the final address;
 * - no redirect followed, ever (a 302 would carry an Authorization header
 *   to a host the answer chose);
 * - If-Modified-Since from the unit's last Last-Modified;
 * - at most the byte cap read, judged on Content-Length first and again on
 *   what actually arrives;
 * - one deadline for the whole exchange;
 * - the source's hourly request budget, at the cost the provider declares;
 * - a file only under feeds.file_roots, realpath()'d and compared against
 *   the root plus a separator.
 *
 * Nothing here reads the database or the configuration: the limits come in
 * frozen in the #VentureFeedSource.
 */

#include "venture.h"
#include "feeds/venture-feeds-private.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

G_DEFINE_QUARK(venture-feeds-error-quark, venture_feeds_error)

/* The most a 429's Retry-After may ask for: a day. A far end asking for a
 * month is broken, and the operator can sync by hand. */
#define FEED_MAX_RETRY_AFTER (86400)

/* What is read from an answer at a time. */
#define FEED_READ_CHUNK (65536)

/* --- The frozen source ------------------------------------------------------ */

G_DEFINE_BOXED_TYPE(VentureFeedSource, venture_feed_source,
                    venture_feed_source_ref, venture_feed_source_unref)

VentureFeedSource *
venture_feed_source_ref(VentureFeedSource *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	g_atomic_ref_count_inc(&self->ref);

	return self;
}

void
venture_feed_source_unref(VentureFeedSource *self)
{
	if (NULL == self)
		return;

	if (!g_atomic_ref_count_dec(&self->ref))
		return;

	if ((NULL != self->frozen) && (NULL != self->frozen_free))
		self->frozen_free(self->frozen);

	g_clear_object(&self->provider);
	g_free(self->uuid);
	g_free(self->name);
	g_free(self->provider_name);

	/* Credentials do not outlive the source in freed memory. */
	if (NULL != self->secrets_json)
	{
		memset(self->secrets_json, 0, strlen(self->secrets_json));
		g_free(self->secrets_json);
	}

	if (NULL != self->secret_values)
	{
		guint i;

		for (i = 0; i < self->secret_values->len; i++)
		{
			gchar *value = g_ptr_array_index(self->secret_values, i);

			memset(value, 0, strlen(value));
		}

		g_ptr_array_unref(self->secret_values);
	}

	g_free(self->settings_json);
	g_free(self->schedule);
	g_clear_pointer(&self->known, g_hash_table_unref);
	g_free(self->currency);
	g_free(self->venue_namespace);
	g_free(self->instrument_namespace);
	g_free(self->min_currency);
	g_strfreev(self->units);
	g_free(self->store_dir);
	g_strfreev(self->allowed_origins);
	g_strfreev(self->file_roots);
	g_strfreev(self->record_types);
	g_clear_pointer(&self->hooks, g_ptr_array_unref);
	g_free(self);
}

gint64
venture_feed_source_get_id(VentureFeedSource *self)
{
	g_return_val_if_fail(NULL != self, 0);

	return self->id;
}

const gchar *
venture_feed_source_get_uuid(VentureFeedSource *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->uuid;
}

gint64
venture_feed_source_get_organization_id(VentureFeedSource *self)
{
	g_return_val_if_fail(NULL != self, 0);

	return self->organization_id;
}

const gchar *
venture_feed_source_get_store_dir(VentureFeedSource *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->store_dir;
}

const gchar *
venture_feed_source_get_currency(VentureFeedSource *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->currency;
}

/*
 * Replaces every credential the source holds with a marker. Every string
 * the worker records -- an error, a note -- goes through this before it
 * leaves the thread, whatever produced it: a far end that echoes a query
 * string, a provider that quotes its own request. Values shorter than six
 * bytes are left alone, as the exec runtime leaves them: replacing every
 * "abc" in a price feed mangles data to hide nothing worth hiding.
 */
gchar *
venture_feed_source_redact(
	VentureFeedSource	*source,
	const gchar		*text
){
	GString *result;
	guint i;

	if (NULL == text)
		return NULL;

	if ((NULL == source) || (NULL == source->secret_values) ||
	    (0 == source->secret_values->len))
		return g_strdup(text);

	result = g_string_new(text);

	for (i = 0; i < source->secret_values->len; i++)
		g_string_replace(result, g_ptr_array_index(source->secret_values, i),
		                 VENTURE_EXEC_REDACTED, 0);

	return g_string_free(result, FALSE);
}

/* --- HTTP answers ------------------------------------------------------------ */

void
venture_feed_http_response_free(VentureFeedHttpResponse *self)
{
	if (NULL == self)
		return;

	g_clear_pointer(&self->body, g_bytes_unref);
	g_free(self->content_type);
	g_free(self);
}

/* --- The request --------------------------------------------------------------- */

struct _VentureFeedRequest
{
	GObject parent_instance;

	VentureFeedSource	*source;
	gchar			*unit;
	JsonObject		*settings;
	JsonObject		*secrets;
	SoupSession		*session;
	FeedQuota		 quota;		/* a copy; the worker takes it back */
	gboolean		 has_quota;
	gchar			*cursor;
	gint64			 if_modified_since;
	gint64			 fetched_at;
	GCancellable		*cancellable;

	/* What the fetch did, for the run. */
	gint64			 requests;
	gint64			 bytes;
	gint			 http_status;
	gint64			 retry_after;
	gint64			 deferred_until;
	gint64			 last_modified;
};

G_DEFINE_FINAL_TYPE(VentureFeedRequest, venture_feed_request, G_TYPE_OBJECT)

static void
venture_feed_request_finalize(GObject *object)
{
	VentureFeedRequest *self = VENTURE_FEED_REQUEST(object);

	g_clear_pointer(&self->source, venture_feed_source_unref);
	g_free(self->unit);
	g_clear_pointer(&self->settings, json_object_unref);
	g_clear_pointer(&self->secrets, json_object_unref);
	g_clear_object(&self->session);
	g_free(self->cursor);
	g_clear_object(&self->cancellable);

	G_OBJECT_CLASS(venture_feed_request_parent_class)->finalize(object);
}

static void
venture_feed_request_class_init(VentureFeedRequestClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_feed_request_finalize;
}

static void
venture_feed_request_init(VentureFeedRequest *self)
{
	self->if_modified_since = VENTURE_SERIES_NONE;
	self->last_modified = VENTURE_SERIES_NONE;
}

VentureFeedRequest *
venture_feed_request_new_internal(
	VentureFeedSource	*source,
	const gchar		*unit,
	JsonObject		*settings,
	JsonObject		*secrets,
	SoupSession		*session,
	const FeedQuota		*quota,
	const gchar		*cursor,
	gint64			 if_modified_since,
	gint64			 fetched_at,
	GCancellable		*cancellable
){
	VentureFeedRequest *self;

	self = g_object_new(VENTURE_TYPE_FEED_REQUEST, NULL);
	self->source = venture_feed_source_ref(source);
	self->unit = g_strdup(unit);
	self->settings = (NULL != settings) ? json_object_ref(settings) : json_object_new();
	self->secrets = (NULL != secrets) ? json_object_ref(secrets) : json_object_new();
	self->session = (NULL != session) ? g_object_ref(session) : NULL;
	if (NULL != quota)
	{
		self->quota = *quota;
		self->has_quota = TRUE;
	}
	self->cursor = g_strdup(cursor);
	self->if_modified_since = if_modified_since;
	self->fetched_at = fetched_at;

	/* The worker's, so stopping it stops every fetch in flight. */
	self->cancellable = (NULL != cancellable) ? g_object_ref(cancellable) : g_cancellable_new();

	return self;
}

VentureFeedSource *
venture_feed_request_get_source(VentureFeedRequest *self)
{
	g_return_val_if_fail(VENTURE_IS_FEED_REQUEST(self), NULL);

	return self->source;
}

const gchar *
venture_feed_request_get_unit(VentureFeedRequest *self)
{
	g_return_val_if_fail(VENTURE_IS_FEED_REQUEST(self), NULL);

	return self->unit;
}

JsonObject *
venture_feed_request_get_settings(VentureFeedRequest *self)
{
	g_return_val_if_fail(VENTURE_IS_FEED_REQUEST(self), NULL);

	return self->settings;
}

const gchar *
venture_feed_request_get_secret(
	VentureFeedRequest	*self,
	const gchar		*name
){
	JsonNode *node;

	g_return_val_if_fail(VENTURE_IS_FEED_REQUEST(self), NULL);

	if (NULL == name)
		return NULL;

	node = json_object_get_member(self->secrets, name);

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_STRING != json_node_get_value_type(node)))
		return NULL;

	return json_node_get_string(node);
}

JsonObject *
venture_feed_request_get_secrets(VentureFeedRequest *self)
{
	g_return_val_if_fail(VENTURE_IS_FEED_REQUEST(self), NULL);

	return self->secrets;
}

gpointer
venture_feed_request_get_frozen(VentureFeedRequest *self)
{
	g_return_val_if_fail(VENTURE_IS_FEED_REQUEST(self), NULL);

	return self->source->frozen;
}

const gchar *
venture_feed_request_get_cursor(VentureFeedRequest *self)
{
	g_return_val_if_fail(VENTURE_IS_FEED_REQUEST(self), NULL);

	return self->cursor;
}

gint64
venture_feed_request_get_if_modified_since(VentureFeedRequest *self)
{
	g_return_val_if_fail(VENTURE_IS_FEED_REQUEST(self), VENTURE_SERIES_NONE);

	return self->if_modified_since;
}

gint64
venture_feed_request_get_fetched_at(VentureFeedRequest *self)
{
	g_return_val_if_fail(VENTURE_IS_FEED_REQUEST(self), 0);

	return self->fetched_at;
}

gsize
venture_feed_request_get_max_bytes(VentureFeedRequest *self)
{
	g_return_val_if_fail(VENTURE_IS_FEED_REQUEST(self), 0);

	return self->source->max_response_bytes;
}

guint
venture_feed_request_get_timeout(VentureFeedRequest *self)
{
	g_return_val_if_fail(VENTURE_IS_FEED_REQUEST(self), 0);

	return self->source->request_timeout;
}

GCancellable *
venture_feed_request_get_cancellable(VentureFeedRequest *self)
{
	g_return_val_if_fail(VENTURE_IS_FEED_REQUEST(self), NULL);

	return self->cancellable;
}

gint64
venture_feed_request_get_requests(VentureFeedRequest *self)
{
	return self->requests;
}

gint64
venture_feed_request_get_bytes(VentureFeedRequest *self)
{
	return self->bytes;
}

gint
venture_feed_request_get_http_status(VentureFeedRequest *self)
{
	return self->http_status;
}

gint64
venture_feed_request_get_retry_after(VentureFeedRequest *self)
{
	return self->retry_after;
}

gint64
venture_feed_request_get_deferred_until(VentureFeedRequest *self)
{
	return self->deferred_until;
}

gint64
venture_feed_request_get_last_modified(VentureFeedRequest *self)
{
	return self->last_modified;
}

void
venture_feed_request_get_quota(
	VentureFeedRequest	*self,
	FeedQuota		*out
){
	if (self->has_quota)
		*out = self->quota;
}

gchar *
venture_feed_request_expand(
	VentureFeedRequest	 *self,
	const gchar		 *template_text,
	gboolean		  for_url,
	GError			**error
){
	g_autoptr(GString) out = NULL;
	const gchar *p;

	g_return_val_if_fail(VENTURE_IS_FEED_REQUEST(self), NULL);

	if (NULL == template_text)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "A template is missing");
		return NULL;
	}

	out = g_string_new(NULL);

	for (p = template_text; '\0' != *p; )
	{
		const gchar *close;
		g_autofree gchar *name = NULL;
		const gchar *value;

		if ('{' != *p || (NULL == (close = strchr(p, '}'))))
		{
			g_string_append_c(out, *p++);
			continue;
		}

		name = g_strndup(p + 1, (gsize)(close - p - 1));

		if (0 == g_strcmp0(name, "unit"))
			value = self->unit;
		else if (0 == g_strcmp0(name, "cursor"))
			value = (NULL != self->cursor) ? self->cursor : "";
		else if (g_str_has_prefix(name, "secret:"))
		{
			value = venture_feed_request_get_secret(self, name + strlen("secret:"));

			if (NULL == value)
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
				            "The template names the credential %s, which "
				            "has not been set for this source",
				            name + strlen("secret:"));
				return NULL;
			}
		}
		else
		{
			/* Not a placeholder: JSON in a header, a brace in a path. */
			g_string_append_c(out, *p++);
			continue;
		}

		if (for_url)
			g_string_append_uri_escaped(out, value, NULL, FALSE);
		else
			g_string_append(out, value);

		p = close + 1;
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* --- Files --------------------------------------------------------------------- */

gchar *
venture_feed_request_resolve_file(
	VentureFeedRequest	 *self,
	const gchar		 *path,
	GError			**error
){
	guint i;

	g_return_val_if_fail(VENTURE_IS_FEED_REQUEST(self), NULL);

	if (venture_string_is_empty(path) || (NULL == self->source->file_roots) ||
	    (NULL == self->source->file_roots[0]))
	{
		g_set_error_literal(error, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_FILE,
		                    "Files can be read only under feeds.file_roots, and none is set");
		return NULL;
	}

	for (i = 0; NULL != self->source->file_roots[i]; i++)
	{
		g_autofree gchar *candidate = NULL;
		g_autofree gchar *prefix = NULL;
		gchar *real_root;
		gchar *real_path;
		gchar *result;

		real_root = realpath(self->source->file_roots[i], NULL);

		if (NULL == real_root)
			continue;

		candidate = g_path_is_absolute(path)
			? g_strdup(path)
			: g_build_filename(real_root, path, NULL);
		real_path = realpath(candidate, NULL);

		/*
		 * Strictly inside, against the root plus its separator: a bare
		 * prefix test lets /srv/feeds-evil/x through for /srv/feeds,
		 * and a symlink is judged by where it lands.
		 */
		prefix = g_str_has_suffix(real_root, "/")
			? g_strdup(real_root) : g_strconcat(real_root, "/", NULL);
		result = ((NULL != real_path) && g_str_has_prefix(real_path, prefix))
			? g_strdup(real_path) : NULL;

		free(real_root);
		free(real_path);

		if (NULL != result)
			return result;
	}

	g_set_error_literal(error, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_FILE,
	                    "The file is missing or is not under any of feeds.file_roots");
	return NULL;
}

GBytes *
venture_feed_request_read_file(
	VentureFeedRequest	 *self,
	const gchar		 *path,
	GError			**error
){
	g_autofree gchar *resolved = NULL;
	g_autoptr(GMappedFile) mapped = NULL;
	g_autoptr(GError) local_error = NULL;
	GStatBuf info;

	resolved = venture_feed_request_resolve_file(self, path, error);

	if (NULL == resolved)
		return NULL;

	if ((0 != g_stat(resolved, &info)) || !S_ISREG(info.st_mode))
	{
		g_set_error_literal(error, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_FILE,
		                    "The file is not a regular file");
		return NULL;
	}

	if ((guint64)info.st_size > (guint64)self->source->max_response_bytes)
	{
		g_set_error(error, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_TOO_LARGE,
		            "The file is larger than feeds.max_response_mb (%" G_GSIZE_FORMAT " bytes)",
		            self->source->max_response_bytes);
		return NULL;
	}

	mapped = g_mapped_file_new(resolved, FALSE, &local_error);

	if (NULL == mapped)
	{
		g_set_error(error, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_FILE,
		            "The file cannot be read: %s", local_error->message);
		return NULL;
	}

	/* A file that grew past the cap between the stat and the map. */
	if (g_mapped_file_get_length(mapped) > self->source->max_response_bytes)
	{
		g_set_error_literal(error, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_TOO_LARGE,
		                    "The file is larger than feeds.max_response_mb");
		return NULL;
	}

	self->bytes += (gint64)g_mapped_file_get_length(mapped);

	return g_mapped_file_get_bytes(mapped);
}

/* --- HTTP ------------------------------------------------------------------------ */

/*
 * scheme://host[:port], the port left out when it is the scheme's own.
 * Lower case, so the allowlist compares one spelling.
 */
static gchar *
feed_origin_of(GUri *uri)
{
	g_autofree gchar *host = NULL;
	const gchar *scheme;
	gint port;

	scheme = g_uri_get_scheme(uri);
	host = g_ascii_strdown(g_uri_get_host(uri), -1);
	port = g_uri_get_port(uri);

	if (((0 == g_ascii_strcasecmp(scheme, "https")) && (443 == port)) ||
	    ((0 == g_ascii_strcasecmp(scheme, "http")) && (80 == port)))
		port = -1;

	if (strchr(host, ':') != NULL)
		return (port > 0)
			? g_strdup_printf("%s://[%s]:%d", scheme, host, port)
			: g_strdup_printf("%s://[%s]", scheme, host);

	return (port > 0)
		? g_strdup_printf("%s://%s:%d", scheme, host, port)
		: g_strdup_printf("%s://%s", scheme, host);
}

static gboolean
feed_origin_allowed(
	VentureFeedSource	*source,
	const gchar		*origin
){
	guint i;

	for (i = 0; (NULL != source->allowed_origins) && (NULL != source->allowed_origins[i]); i++)
	{
		if (0 == g_ascii_strcasecmp(source->allowed_origins[i], origin))
			return TRUE;
	}

	return FALSE;
}

typedef struct
{
	VentureFeedRequest	*request;
	SoupMessage		*message;
	GUri			*uri;
	gchar			*origin;
	GCancellable		*cancellable;	/* the deadline's, child of the request's */
	gulong			 parent_handler;
	GSource			*deadline;
	gboolean		 timed_out;
	GInputStream		*stream;
	GByteArray		*body;
} FeedHttpGet;

static void
feed_http_get_free(gpointer data)
{
	FeedHttpGet *get = data;

	if (NULL != get->deadline)
	{
		g_source_destroy(get->deadline);
		g_source_unref(get->deadline);
	}

	if (0 != get->parent_handler)
		g_cancellable_disconnect(get->request->cancellable, get->parent_handler);

	g_clear_object(&get->stream);
	g_clear_object(&get->message);
	g_clear_pointer(&get->uri, g_uri_unref);
	g_free(get->origin);
	g_clear_object(&get->cancellable);
	g_clear_pointer(&get->body, g_byte_array_unref);
	g_clear_object(&get->request);
	g_free(get);
}

static gboolean
feed_http_deadline(gpointer data)
{
	FeedHttpGet *get = data;

	get->timed_out = TRUE;
	g_cancellable_cancel(get->cancellable);

	return G_SOURCE_REMOVE;
}

static void
feed_http_parent_cancelled(
	GCancellable	*parent,
	gpointer	 data
){
	(void)parent;

	g_cancellable_cancel(G_CANCELLABLE(data));
}

/* Retry-After: delay-seconds or an HTTP date. */
static gint64
feed_parse_retry_after(const gchar *value)
{
	g_autoptr(GDateTime) when = NULL;
	gint64 seconds;
	gchar *end;

	if (NULL == value)
		return 0;

	seconds = g_ascii_strtoll(value, &end, 10);

	if ((end != value) && ('\0' == *end))
		return CLAMP(seconds, 0, FEED_MAX_RETRY_AFTER);

	when = soup_date_time_new_from_http_string(value);

	if (NULL == when)
		return 0;

	return CLAMP(g_date_time_to_unix(when) - (g_get_real_time() / G_USEC_PER_SEC),
	             0, FEED_MAX_RETRY_AFTER);
}

/*
 * Ends a fetch with an error. A cancellation the worker asked for stays a
 * cancellation; one the deadline caused is a timeout, which the run
 * records.
 */
static void
feed_http_fail(
	GTask		*task,
	FeedHttpGet	*get,
	GError		*error
){
	if (get->timed_out)
	{
		g_clear_error(&error);
		g_task_return_new_error(task, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT,
		                        "%s did not answer within %u seconds",
		                        get->origin, get->request->source->request_timeout);
		return;
	}

	g_task_return_error(task, error);
}

static void
feed_http_finish_ok(
	GTask		*task,
	FeedHttpGet	*get,
	guint		 status
){
	VentureFeedHttpResponse *response;
	const gchar *last_modified;
	const gchar *content_type;
	SoupMessageHeaders *headers;

	headers = soup_message_get_response_headers(get->message);
	response = g_new0(VentureFeedHttpResponse, 1);
	response->status = status;
	response->body = (NULL != get->body)
		? g_byte_array_free_to_bytes(g_steal_pointer(&get->body))
		: g_bytes_new(NULL, 0);
	response->last_modified = VENTURE_SERIES_NONE;

	last_modified = soup_message_headers_get_one(headers, "Last-Modified");

	if (NULL != last_modified)
	{
		g_autoptr(GDateTime) when = NULL;

		when = soup_date_time_new_from_http_string(last_modified);

		if (NULL != when)
		{
			response->last_modified = g_date_time_to_unix(when);
			get->request->last_modified = response->last_modified;
		}
	}

	content_type = soup_message_headers_get_content_type(headers, NULL);
	response->content_type = g_strdup(content_type);

	get->request->bytes += (gint64)g_bytes_get_size(response->body);

	g_task_return_pointer(task, response,
	                      (GDestroyNotify)venture_feed_http_response_free);
}

static void
feed_http_read_ready(
	GObject		*object,
	GAsyncResult	*result,
	gpointer	 user_data
);

static void
feed_http_read_next(GTask *task)
{
	FeedHttpGet *get = g_task_get_task_data(task);

	g_input_stream_read_bytes_async(get->stream, FEED_READ_CHUNK, G_PRIORITY_DEFAULT,
	                                get->cancellable, feed_http_read_ready, task);
}

static void
feed_http_read_ready(
	GObject		*object,
	GAsyncResult	*result,
	gpointer	 user_data
){
	g_autoptr(GTask) task = user_data;
	g_autoptr(GBytes) chunk = NULL;
	FeedHttpGet *get;
	GError *error = NULL;

	get = g_task_get_task_data(task);
	chunk = g_input_stream_read_bytes_finish(G_INPUT_STREAM(object), result, &error);

	if (NULL == chunk)
	{
		feed_http_fail(task, get, error);
		return;
	}

	if (0 == g_bytes_get_size(chunk))
	{
		feed_http_finish_ok(task, get, soup_message_get_status(get->message));
		return;
	}

	/* Judged on what arrives, not only on what Content-Length said: a
	 * chunked answer has no length, and a lying one would get past it. */
	if (get->body->len + g_bytes_get_size(chunk) > get->request->source->max_response_bytes)
	{
		get->request->bytes += get->body->len;
		g_task_return_new_error(task, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_TOO_LARGE,
		                        "The answer from %s is larger than feeds.max_response_mb",
		                        get->origin);
		return;
	}

	g_byte_array_append(get->body, g_bytes_get_data(chunk, NULL),
	                    (guint)g_bytes_get_size(chunk));
	feed_http_read_next(g_steal_pointer(&task));
}

static void
feed_http_sent(
	GObject		*object,
	GAsyncResult	*result,
	gpointer	 user_data
){
	g_autoptr(GTask) task = user_data;
	g_autofree gchar *requested = NULL;
	g_autofree gchar *final = NULL;
	FeedHttpGet *get;
	GError *error = NULL;
	GUri *final_uri;
	goffset length;
	guint status;

	get = g_task_get_task_data(task);
	get->stream = soup_session_send_finish(SOUP_SESSION(object), result, &error);

	if (NULL == get->stream)
	{
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		{
			/* The transport's text can quote the address with its query,
			 * which may carry a key; name the origin only. */
			g_clear_error(&error);
			g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NETWORK,
			            "Could not reach %s", get->origin);
		}

		feed_http_fail(task, get, error);
		return;
	}

	status = soup_message_get_status(get->message);
	get->request->http_status = (gint)status;

	/* Pinned: the address answered must be the address asked. With
	 * redirects off this cannot differ, and this is what says so if a
	 * future libsoup ever disagrees. */
	final_uri = soup_message_get_uri(get->message);
	requested = g_uri_to_string(get->uri);
	final = (NULL != final_uri) ? g_uri_to_string(final_uri) : NULL;

	if (0 != g_strcmp0(requested, final))
	{
		g_task_return_new_error(task, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_ORIGIN,
		                        "The answer from %s came from another address", get->origin);
		return;
	}

	if (SOUP_STATUS_NOT_MODIFIED == status)
	{
		feed_http_finish_ok(task, get, status);
		return;
	}

	if (429 == status)
	{
		get->request->retry_after = feed_parse_retry_after(
			soup_message_headers_get_one(soup_message_get_response_headers(get->message),
			                             "Retry-After"));
		g_task_return_new_error(task, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_RATE_LIMITED,
		                        "%s answered 429 Too Many Requests; retry after %"
		                        G_GINT64_FORMAT " seconds", get->origin,
		                        get->request->retry_after);
		return;
	}

	if (SOUP_STATUS_IS_REDIRECTION(status))
	{
		g_task_return_new_error(task, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_REDIRECT,
		                        "%s answered %u, a redirect; feeds never follow one",
		                        get->origin, status);
		return;
	}

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_new_error(task, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_HTTP,
		                        "%s answered HTTP %u", get->origin, status);
		return;
	}

	length = soup_message_headers_get_content_length(
		soup_message_get_response_headers(get->message));

	if ((length > 0) && ((guint64)length > (guint64)get->request->source->max_response_bytes))
	{
		g_task_return_new_error(task, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_TOO_LARGE,
		                        "The answer from %s is larger than feeds.max_response_mb",
		                        get->origin);
		return;
	}

	get->body = g_byte_array_new();
	feed_http_read_next(g_steal_pointer(&task));
}

void
venture_feed_request_http_get_async(
	VentureFeedRequest	*self,
	const gchar		*url,
	const gchar *const	*headers,
	guint			 cost,
	GAsyncReadyCallback	 callback,
	gpointer		 user_data
){
	g_autoptr(GTask) task = NULL;
	g_autoptr(GError) local_error = NULL;
	FeedHttpGet *get;
	SoupMessageHeaders *request_headers;
	gint64 now;
	guint i;

	g_return_if_fail(VENTURE_IS_FEED_REQUEST(self));

	task = g_task_new(self, self->cancellable, callback, user_data);
	g_task_set_source_tag(task, venture_feed_request_http_get_async);
	g_task_set_check_cancellable(task, FALSE);

	get = g_new0(FeedHttpGet, 1);
	get->request = g_object_ref(self);
	g_task_set_task_data(task, get, feed_http_get_free);

	if (NULL == self->session)
	{
		g_task_return_new_error(task, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
		                        "This request has no HTTP session; use venture_feed_request_http_get()");
		return;
	}

	get->uri = (NULL != url) ? g_uri_parse(url, G_URI_FLAGS_ENCODED, &local_error) : NULL;

	if ((NULL == get->uri) || (NULL == g_uri_get_host(get->uri)) ||
	    (NULL != g_uri_get_userinfo(get->uri)) ||
	    ((0 != g_ascii_strcasecmp(g_uri_get_scheme(get->uri), "https")) &&
	     (0 != g_ascii_strcasecmp(g_uri_get_scheme(get->uri), "http"))))
	{
		g_task_return_new_error(task, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_ORIGIN,
		                        "A feed address must be http or https, with no credentials in it");
		return;
	}

	get->origin = feed_origin_of(get->uri);

	if (!feed_origin_allowed(self->source, get->origin))
	{
		g_task_return_new_error(task, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_ORIGIN,
		                        "%s is not one of feeds.allowed_origins", get->origin);
		return;
	}

	/*
	 * The hourly budget, spent before the request goes out: a request that
	 * fails still cost the far end an answer. A unit that would overspend
	 * waits for the next window rather than failing, and the worker
	 * records the run as deferred.
	 */
	now = g_get_real_time() / G_USEC_PER_SEC;
	cost = MAX(cost, 1);

	if (self->has_quota && (self->source->requests_per_hour > 0))
	{
		if (now >= self->quota.window_start + 3600)
		{
			self->quota.window_start = now;
			self->quota.spent = 0;
		}

		if (self->quota.spent + (gint64)cost > (gint64)self->source->requests_per_hour)
		{
			self->deferred_until = self->quota.window_start + 3600;
			g_task_return_new_error(task, VENTURE_FEEDS_ERROR, VENTURE_FEEDS_ERROR_QUOTA,
			                        "The source's budget of %u requests an hour is spent",
			                        self->source->requests_per_hour);
			return;
		}

		self->quota.spent += cost;
	}

	get->message = soup_message_new_from_uri("GET", get->uri);
	soup_message_add_flags(get->message, SOUP_MESSAGE_NO_REDIRECT);
	request_headers = soup_message_get_request_headers(get->message);

	if (VENTURE_SERIES_NONE != self->if_modified_since)
	{
		g_autoptr(GDateTime) since = NULL;
		g_autofree gchar *text = NULL;

		since = g_date_time_new_from_unix_utc(self->if_modified_since);
		text = soup_date_time_to_string(since, SOUP_DATE_HTTP);
		soup_message_headers_replace(request_headers, "If-Modified-Since", text);
	}

	for (i = 0; (NULL != headers) && (NULL != headers[i]) && (NULL != headers[i + 1]); i += 2)
		soup_message_headers_replace(request_headers, headers[i], headers[i + 1]);

	/* One deadline for the whole exchange, through a cancellable of its
	 * own so a timeout is told apart from the worker stopping. */
	get->cancellable = g_cancellable_new();
	get->parent_handler = g_cancellable_connect(self->cancellable,
	                                            G_CALLBACK(feed_http_parent_cancelled),
	                                            g_object_ref(get->cancellable),
	                                            g_object_unref);
	get->deadline = g_timeout_source_new_seconds(MAX(self->source->request_timeout, 1));
	g_source_set_callback(get->deadline, feed_http_deadline, get, NULL);
	g_source_attach(get->deadline, g_main_context_get_thread_default());

	self->requests++;

	soup_session_send_async(self->session, get->message, G_PRIORITY_DEFAULT,
	                        get->cancellable, feed_http_sent, g_steal_pointer(&task));
}

VentureFeedHttpResponse *
venture_feed_request_http_get_finish(
	VentureFeedRequest	 *self,
	GAsyncResult		 *result,
	GError			**error
){
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

/* --- The blocking form, for a provider on a GTask thread --------------------- */

typedef struct
{
	gboolean		 done;
	VentureFeedHttpResponse	*response;
	GError			*error;
} FeedHttpWait;

static void
feed_http_wait_done(
	GObject		*object,
	GAsyncResult	*result,
	gpointer	 data
){
	FeedHttpWait *wait = data;

	wait->response = venture_feed_request_http_get_finish(VENTURE_FEED_REQUEST(object),
	                                                      result, &wait->error);
	wait->done = TRUE;
}

/*
 * A session made for this thread on a context made for this call, so no
 * unrelated source runs nested inside a provider's fetch and libsoup's
 * one-thread rule holds.
 */
SoupSession *
venture_feeds_session_new(void)
{
	return soup_session_new_with_options("user-agent", "VENTURE-feeds/" VENTURE_VERSION_S,
	                                     "max-conns-per-host", 2,
	                                     NULL);
}

VentureFeedHttpResponse *
venture_feed_request_http_get(
	VentureFeedRequest	 *self,
	const gchar		 *url,
	const gchar *const	 *headers,
	guint			  cost,
	GError			**error
){
	g_autoptr(GMainContext) context = NULL;
	SoupSession *previous;
	FeedHttpWait wait;

	g_return_val_if_fail(VENTURE_IS_FEED_REQUEST(self), NULL);

	memset(&wait, 0, sizeof(wait));
	context = g_main_context_new();
	g_main_context_push_thread_default(context);

	previous = g_steal_pointer(&self->session);
	self->session = venture_feeds_session_new();

	venture_feed_request_http_get_async(self, url, headers, cost, feed_http_wait_done, &wait);

	while (!wait.done)
		g_main_context_iteration(context, TRUE);

	g_clear_object(&self->session);
	self->session = previous;

	/* Let the session's own teardown sources run before the context goes. */
	while (g_main_context_iteration(context, FALSE))
		;

	g_main_context_pop_thread_default(context);

	if (NULL != wait.error)
		g_propagate_error(error, wait.error);

	return wait.response;
}

/* --- The interface --------------------------------------------------------------- */

G_DEFINE_INTERFACE(VentureDataSourceProvider, venture_data_source_provider, G_TYPE_OBJECT)

static void
venture_data_source_provider_default_init(VentureDataSourceProviderInterface *iface)
{
	(void)iface;
}

const gchar *
venture_data_source_provider_get_name(VentureDataSourceProvider *self)
{
	g_return_val_if_fail(VENTURE_IS_DATA_SOURCE_PROVIDER(self), NULL);

	return VENTURE_DATA_SOURCE_PROVIDER_GET_IFACE(self)->get_name(self);
}

const gchar *
venture_data_source_provider_get_label(VentureDataSourceProvider *self)
{
	VentureDataSourceProviderInterface *iface;
	const gchar *label;

	g_return_val_if_fail(VENTURE_IS_DATA_SOURCE_PROVIDER(self), NULL);

	iface = VENTURE_DATA_SOURCE_PROVIDER_GET_IFACE(self);
	label = (NULL != iface->get_label) ? iface->get_label(self) : NULL;

	return (NULL != label) ? label : venture_data_source_provider_get_name(self);
}

JsonNode *
venture_data_source_provider_dup_settings_schema(VentureDataSourceProvider *self)
{
	VentureDataSourceProviderInterface *iface;
	JsonNode *schema;

	g_return_val_if_fail(VENTURE_IS_DATA_SOURCE_PROVIDER(self), NULL);

	iface = VENTURE_DATA_SOURCE_PROVIDER_GET_IFACE(self);
	schema = (NULL != iface->dup_settings_schema) ? iface->dup_settings_schema(self) : NULL;

	if ((NULL == schema) || !JSON_NODE_HOLDS_OBJECT(schema))
	{
		g_clear_pointer(&schema, json_node_unref);
		schema = json_from_string("{\"type\":\"object\",\"properties\":{}}", NULL);
	}

	return schema;
}

gpointer
venture_data_source_provider_freeze(
	VentureDataSourceProvider	 *self,
	VentureContext			 *context,
	JsonObject			 *settings,
	GDestroyNotify			 *out_free,
	GError				**error
){
	VentureDataSourceProviderInterface *iface;

	g_return_val_if_fail(VENTURE_IS_DATA_SOURCE_PROVIDER(self), NULL);
	g_return_val_if_fail(NULL != out_free, NULL);

	*out_free = NULL;
	iface = VENTURE_DATA_SOURCE_PROVIDER_GET_IFACE(self);

	if (NULL == iface->freeze)
		return NULL;

	return iface->freeze(self, context, settings, out_free, error);
}

gchar **
venture_data_source_provider_list_units(
	VentureDataSourceProvider	 *self,
	JsonObject			 *settings,
	GError				**error
){
	VentureDataSourceProviderInterface *iface;
	g_autoptr(GPtrArray) units = NULL;
	JsonNode *node;

	g_return_val_if_fail(VENTURE_IS_DATA_SOURCE_PROVIDER(self), NULL);

	iface = VENTURE_DATA_SOURCE_PROVIDER_GET_IFACE(self);

	if (NULL != iface->list_units)
		return iface->list_units(self, settings, error);

	/* The default: the settings' units list, else one unit. */
	units = g_ptr_array_new_with_free_func(g_free);
	node = (NULL != settings) ? json_object_get_member(settings, "units") : NULL;

	if (NULL != node)
	{
		JsonArray *array;
		guint i;

		if (!JSON_NODE_HOLDS_ARRAY(node))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "units must be a list of names");
			return NULL;
		}

		array = json_node_get_array(node);

		for (i = 0; i < json_array_get_length(array); i++)
		{
			JsonNode *element = json_array_get_element(array, i);
			const gchar *unit;

			if (!JSON_NODE_HOLDS_VALUE(element))
				continue;

			/* A realm's id is a number in YAML; it is still a name. */
			if (G_TYPE_INT64 == json_node_get_value_type(element))
			{
				g_ptr_array_add(units, g_strdup_printf("%" G_GINT64_FORMAT,
				                                       json_node_get_int(element)));
				continue;
			}

			if (G_TYPE_STRING != json_node_get_value_type(element))
				continue;

			unit = json_node_get_string(element);

			if (venture_string_is_empty(unit) || (strlen(unit) > 256) ||
			    !g_utf8_validate(unit, -1, NULL))
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				                    "A unit must be a name of 1 to 256 bytes");
				return NULL;
			}

			g_ptr_array_add(units, g_strdup(unit));
		}

		if ((0 == units->len) || (units->len > 10000))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "units must name 1 to 10000 units");
			return NULL;
		}
	}
	else
		g_ptr_array_add(units, g_strdup("default"));

	g_ptr_array_add(units, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&units), FALSE);
}

void
venture_data_source_provider_fetch_async(
	VentureDataSourceProvider	*self,
	VentureFeedRequest		*request,
	GCancellable			*cancellable,
	GAsyncReadyCallback		 callback,
	gpointer			 user_data
){
	g_return_if_fail(VENTURE_IS_DATA_SOURCE_PROVIDER(self));
	g_return_if_fail(VENTURE_IS_FEED_REQUEST(request));

	VENTURE_DATA_SOURCE_PROVIDER_GET_IFACE(self)->fetch_async(self, request, cancellable,
	                                                          callback, user_data);
}

VentureFeedBatch *
venture_data_source_provider_fetch_finish(
	VentureDataSourceProvider	 *self,
	GAsyncResult			 *result,
	GError				**error
){
	VentureDataSourceProviderInterface *iface;

	g_return_val_if_fail(VENTURE_IS_DATA_SOURCE_PROVIDER(self), NULL);

	iface = VENTURE_DATA_SOURCE_PROVIDER_GET_IFACE(self);

	if (NULL != iface->fetch_finish)
		return iface->fetch_finish(self, result, error);

	return g_task_propagate_pointer(G_TASK(result), error);
}

gboolean
venture_data_source_provider_name_is_valid(const gchar *name)
{
	return (NULL != name) && (strlen(name) <= 64) &&
	       g_regex_match_simple("^[a-z][a-z0-9_]*$", name, 0, 0);
}

/* --- A provider from functions ---------------------------------------------------- */

typedef struct
{
	GObject				 parent_instance;
	gchar				*name;
	gchar				*label;
	gchar				*schema_json;
	VentureFuncDataSourceFetch	 fetch;
	gpointer			 user_data;
	GDestroyNotify			 destroy;
} VentureFuncDataSourceProvider;

typedef struct
{
	GObjectClass parent_class;
} VentureFuncDataSourceProviderClass;

static GType venture_func_data_source_provider_get_type(void);
static void venture_func_data_source_provider_iface_init(VentureDataSourceProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(VentureFuncDataSourceProvider, venture_func_data_source_provider,
                        G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(VENTURE_TYPE_DATA_SOURCE_PROVIDER,
                                              venture_func_data_source_provider_iface_init))

static void
venture_func_data_source_provider_finalize(GObject *object)
{
	VentureFuncDataSourceProvider *self = (VentureFuncDataSourceProvider *)object;

	if (NULL != self->destroy)
		self->destroy(self->user_data);

	g_free(self->name);
	g_free(self->label);
	g_free(self->schema_json);

	G_OBJECT_CLASS(venture_func_data_source_provider_parent_class)->finalize(object);
}

static void
venture_func_data_source_provider_class_init(VentureFuncDataSourceProviderClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_func_data_source_provider_finalize;
}

static void
venture_func_data_source_provider_init(VentureFuncDataSourceProvider *self)
{
	(void)self;
}

static const gchar *
func_provider_name(VentureDataSourceProvider *provider)
{
	return ((VentureFuncDataSourceProvider *)provider)->name;
}

static const gchar *
func_provider_label(VentureDataSourceProvider *provider)
{
	return ((VentureFuncDataSourceProvider *)provider)->label;
}

static JsonNode *
func_provider_schema(VentureDataSourceProvider *provider)
{
	VentureFuncDataSourceProvider *self = (VentureFuncDataSourceProvider *)provider;

	return (NULL != self->schema_json) ? json_from_string(self->schema_json, NULL) : NULL;
}

static void
func_provider_thread(
	GTask		*task,
	gpointer	 source,
	gpointer	 task_data,
	GCancellable	*cancellable
){
	VentureFuncDataSourceProvider *self = source;
	VentureFeedRequest *request = task_data;
	GError *error = NULL;
	VentureFeedBatch *batch;

	(void)cancellable;

	batch = self->fetch(request, self->user_data, &error);

	if (NULL == batch)
	{
		if (NULL == error)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			                    "The provider returned nothing and said nothing");
		g_task_return_error(task, error);
		return;
	}

	g_task_return_pointer(task, batch, (GDestroyNotify)venture_feed_batch_unref);
}

static void
func_provider_fetch_async(
	VentureDataSourceProvider	*provider,
	VentureFeedRequest		*request,
	GCancellable			*cancellable,
	GAsyncReadyCallback		 callback,
	gpointer			 user_data
){
	g_autoptr(GTask) task = NULL;

	/* A thread from GLib's pool: the function may block, and the feeds
	 * worker's loop must not. */
	task = g_task_new(provider, cancellable, callback, user_data);
	g_task_set_task_data(task, g_object_ref(request), g_object_unref);
	g_task_run_in_thread(task, func_provider_thread);
}

static void
venture_func_data_source_provider_iface_init(VentureDataSourceProviderInterface *iface)
{
	iface->get_name = func_provider_name;
	iface->get_label = func_provider_label;
	iface->dup_settings_schema = func_provider_schema;
	iface->fetch_async = func_provider_fetch_async;
}

VentureDataSourceProvider *
venture_func_data_source_provider_new(
	const gchar			*name,
	const gchar			*label,
	const gchar			*settings_schema_json,
	VentureFuncDataSourceFetch	 fetch,
	gpointer			 user_data,
	GDestroyNotify			 destroy
){
	VentureFuncDataSourceProvider *self;

	g_return_val_if_fail(NULL != name, NULL);
	g_return_val_if_fail(NULL != fetch, NULL);

	self = g_object_new(venture_func_data_source_provider_get_type(), NULL);
	self->name = g_strdup(name);
	self->label = g_strdup(label);
	self->schema_json = g_strdup(settings_schema_json);
	self->fetch = fetch;
	self->user_data = user_data;
	self->destroy = destroy;

	return VENTURE_DATA_SOURCE_PROVIDER(self);
}

/* --- The registry -------------------------------------------------------------- */

struct _VentureDataSourceProviderRegistry
{
	GObject		 parent_instance;
	GHashTable	*providers;	/* name -> provider */
};

G_DEFINE_FINAL_TYPE(VentureDataSourceProviderRegistry, venture_data_source_provider_registry,
                    G_TYPE_OBJECT)

static void
venture_data_source_provider_registry_finalize(GObject *object)
{
	VentureDataSourceProviderRegistry *self = VENTURE_DATA_SOURCE_PROVIDER_REGISTRY(object);

	g_hash_table_unref(self->providers);

	G_OBJECT_CLASS(venture_data_source_provider_registry_parent_class)->finalize(object);
}

static void
venture_data_source_provider_registry_class_init(VentureDataSourceProviderRegistryClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_data_source_provider_registry_finalize;
}

static void
venture_data_source_provider_registry_init(VentureDataSourceProviderRegistry *self)
{
	self->providers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
}

VentureDataSourceProviderRegistry *
venture_data_source_provider_registry_new(void)
{
	return g_object_new(VENTURE_TYPE_DATA_SOURCE_PROVIDER_REGISTRY, NULL);
}

gboolean
venture_data_source_provider_registry_add(
	VentureDataSourceProviderRegistry	 *self,
	VentureDataSourceProvider		 *provider,
	GError					**error
){
	const gchar *name;

	g_return_val_if_fail(VENTURE_IS_DATA_SOURCE_PROVIDER_REGISTRY(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_DATA_SOURCE_PROVIDER(provider), FALSE);

	name = venture_data_source_provider_get_name(provider);

	if (!venture_data_source_provider_name_is_valid(name))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A data source provider's name is a lower-case letter "
		                    "followed by lower-case letters, digits and underscores");
		return FALSE;
	}

	if (g_hash_table_contains(self->providers, name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "A data source provider named %s is already registered", name);
		return FALSE;
	}

	g_hash_table_insert(self->providers, g_strdup(name), g_object_ref(provider));

	return TRUE;
}

VentureDataSourceProvider *
venture_data_source_provider_registry_lookup(
	VentureDataSourceProviderRegistry	*self,
	const gchar				*name
){
	g_return_val_if_fail(VENTURE_IS_DATA_SOURCE_PROVIDER_REGISTRY(self), NULL);

	return (NULL != name) ? g_hash_table_lookup(self->providers, name) : NULL;
}

static gint
feed_compare_providers(
	gconstpointer	a,
	gconstpointer	b
){
	return g_strcmp0(venture_data_source_provider_get_name(*(VentureDataSourceProvider *const *)a),
	                 venture_data_source_provider_get_name(*(VentureDataSourceProvider *const *)b));
}

GPtrArray *
venture_data_source_provider_registry_list(VentureDataSourceProviderRegistry *self)
{
	GPtrArray *list;
	GHashTableIter iter;
	gpointer value;

	g_return_val_if_fail(VENTURE_IS_DATA_SOURCE_PROVIDER_REGISTRY(self), NULL);

	list = g_ptr_array_new();
	g_hash_table_iter_init(&iter, self->providers);

	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(list, value);

	g_ptr_array_sort(list, feed_compare_providers);

	return list;
}
