/*
 * venture-data-source-provider.h - Where market data comes from
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A provider turns one unit of a data source -- a realm, a page, a file --
 * into a #VentureFeedBatch. It is registered by name on the context
 * (venture_context_get_data_source_providers()): http_json, csv and
 * file_jsonl ship, an exec plugin adds one through its manifest's
 * `data_source_provider` entry, and a native or crispy plugin registers one
 * in code, with venture_func_data_source_provider_new() when it would rather
 * not write a GObject.
 *
 * The threading contract, which is the whole reason this is an interface
 * rather than a callback:
 *
 * - get_name, get_label, dup_settings_schema and list_units are pure and
 *   may run on any thread.
 * - freeze runs on the main thread when a source is (re)loaded and may
 *   read anything there -- a plugin's settings, a program to run. What it
 *   returns is handed to another thread and must be immutable.
 * - fetch_async runs on the feeds worker thread (or, for a test fetch, on
 *   a private context on the main thread) and must touch nothing but its
 *   #VentureFeedRequest: never the database, the entity registry, the
 *   configuration or a plugin's settings. Network goes through the
 *   request's HTTP helper, which enforces the operator's origin allowlist,
 *   the body cap, the deadline, the request budget and refuses redirects;
 *   files go through venture_feed_request_resolve_file(), which enforces
 *   feeds.file_roots.
 *
 * A provider object is shared by every source that names it and by both
 * threads at once, so it holds no per-fetch state.
 */

#ifndef VENTURE_DATA_SOURCE_PROVIDER_H
#define VENTURE_DATA_SOURCE_PROVIDER_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* --- Errors ------------------------------------------------------------------ */

/**
 * VENTURE_FEEDS_ERROR:
 *
 * The error domain for what a fetch can run into that the worker treats
 * differently from a plain failure.
 */
#define VENTURE_FEEDS_ERROR (venture_feeds_error_quark())

GQuark venture_feeds_error_quark(void);

/**
 * VentureFeedsError:
 * @VENTURE_FEEDS_ERROR_ORIGIN: the address is not on feeds.allowed_origins
 *   or the source's own origins
 * @VENTURE_FEEDS_ERROR_QUOTA: the source's request budget for the hour is
 *   spent; the unit waits for the next hour and the run is deferred, not
 *   failed
 * @VENTURE_FEEDS_ERROR_RATE_LIMITED: the far end answered 429; the unit
 *   waits its Retry-After
 * @VENTURE_FEEDS_ERROR_HTTP: the far end answered with a status that is
 *   neither success nor not-modified
 * @VENTURE_FEEDS_ERROR_REDIRECT: the far end answered with a redirect,
 *   which is never followed
 * @VENTURE_FEEDS_ERROR_TOO_LARGE: the answer or file passed
 *   feeds.max_response_mb
 * @VENTURE_FEEDS_ERROR_FILE: the file is not under feeds.file_roots, or
 *   cannot be read
 *
 * Why a fetch stopped, when the reason changes what happens next.
 */
typedef enum
{
	VENTURE_FEEDS_ERROR_ORIGIN = 0,
	VENTURE_FEEDS_ERROR_QUOTA,
	VENTURE_FEEDS_ERROR_RATE_LIMITED,
	VENTURE_FEEDS_ERROR_HTTP,
	VENTURE_FEEDS_ERROR_REDIRECT,
	VENTURE_FEEDS_ERROR_TOO_LARGE,
	VENTURE_FEEDS_ERROR_FILE
} VentureFeedsError;

/* --- The frozen source and the request ---------------------------------------- */

/**
 * VentureFeedSource:
 *
 * A data source as the worker sees it: read on the main thread from the
 * record, its credentials, the configuration and every registered hook,
 * and never changed afterwards. Reference-counted atomically.
 */
typedef struct _VentureFeedSource VentureFeedSource;

#define VENTURE_TYPE_FEED_SOURCE (venture_feed_source_get_type())

GType venture_feed_source_get_type(void) G_GNUC_CONST;

/**
 * venture_feed_source_ref:
 * @self: a source
 *
 * Returns: (transfer full): @self
 */
VentureFeedSource *
venture_feed_source_ref(VentureFeedSource *self);

/**
 * venture_feed_source_unref:
 * @self: (transfer full): a source
 */
void
venture_feed_source_unref(VentureFeedSource *self);

/**
 * venture_feed_source_get_id:
 * @self: a source
 *
 * Returns: the data_source record's id
 */
gint64
venture_feed_source_get_id(VentureFeedSource *self);

/**
 * venture_feed_source_get_uuid:
 * @self: a source
 *
 * Returns: (transfer none): the record's uuid, which names its store
 */
const gchar *
venture_feed_source_get_uuid(VentureFeedSource *self);

/**
 * venture_feed_source_get_organization_id:
 * @self: a source
 *
 * Returns: the organization the record belongs to
 */
gint64
venture_feed_source_get_organization_id(VentureFeedSource *self);

/**
 * venture_feed_source_get_store_dir:
 * @self: a source
 *
 * Returns: (transfer none): <state_dir>/series/<uuid>, derived, never stored
 */
const gchar *
venture_feed_source_get_store_dir(VentureFeedSource *self);

/**
 * venture_feed_source_get_currency:
 * @self: a source
 *
 * Returns: (transfer none) (nullable): what a price naming no currency is in
 */
const gchar *
venture_feed_source_get_currency(VentureFeedSource *self);

/**
 * venture_feed_source_get_hook_data:
 * @self: a source
 * @hook_name: a hook registered with venture_feeds_add_hook()
 *
 * Returns: (transfer none) (nullable): what the hook's freeze function
 *   made for this source
 */
gconstpointer
venture_feed_source_get_hook_data(
	VentureFeedSource	*self,
	const gchar		*hook_name
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureFeedSource, venture_feed_source_unref)

/**
 * VentureFeedHttpResponse:
 * @status: the HTTP status: 200, or 304 for not modified
 * @body: the answer; empty for a 304
 * @last_modified: its Last-Modified as Unix seconds, or
 *   %VENTURE_SERIES_NONE
 * @content_type: (nullable): its Content-Type
 * @headers: (array zero-terminated=1): every response header as name,
 *   value, name, value..., names in lower case; read them with
 *   venture_feed_http_response_get_header()
 *
 * A successful answer -- or, asked for with
 * %VENTURE_FEED_HTTP_ANY_STATUS, any final answer that is neither a
 * redirect nor a 429. Anything else is a #GError.
 */
typedef struct
{
	guint	 status;
	GBytes	*body;
	gint64	 last_modified;
	gchar	*content_type;
	gchar  **headers;
} VentureFeedHttpResponse;

/**
 * venture_feed_http_response_free:
 * @self: (transfer full) (nullable): a response
 */
void
venture_feed_http_response_free(VentureFeedHttpResponse *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureFeedHttpResponse, venture_feed_http_response_free)

/**
 * venture_feed_http_response_get_header:
 * @self: a response
 * @name: a header name, in any case
 *
 * Returns: (transfer none) (nullable): the header's first value
 */
const gchar *
venture_feed_http_response_get_header(
	const VentureFeedHttpResponse	*self,
	const gchar			*name
);

/**
 * VentureFeedHttpFlags:
 * @VENTURE_FEED_HTTP_DEFAULT: the unit's own request: If-Modified-Since
 *   goes out when the unit has a Last-Modified, and the answer's
 *   Last-Modified and status become the unit's
 * @VENTURE_FEED_HTTP_UNCONDITIONAL: a side request -- a token, an index, a
 *   name lookup -- that is not the unit's answer: no If-Modified-Since is
 *   added, and its Last-Modified and status are left out of the unit's
 *   (a provider may still send its own If-Modified-Since header)
 * @VENTURE_FEED_HTTP_ANY_STATUS: hand back any final answer other than a
 *   redirect or a 429 as a response, body included, so the provider can
 *   say what a 401 or a 404 means for it; without it those are
 *   %VENTURE_FEEDS_ERROR_HTTP
 *
 * How one request of a fetch is made.
 */
typedef enum
{
	VENTURE_FEED_HTTP_DEFAULT	= 0,
	VENTURE_FEED_HTTP_UNCONDITIONAL	= 1 << 0,
	VENTURE_FEED_HTTP_ANY_STATUS	= 1 << 1
} VentureFeedHttpFlags;

#define VENTURE_TYPE_FEED_REQUEST (venture_feed_request_get_type())

G_DECLARE_FINAL_TYPE(VentureFeedRequest, venture_feed_request, VENTURE, FEED_REQUEST, GObject)

/**
 * venture_feed_request_get_source:
 * @self: a request
 *
 * Returns: (transfer none): the frozen source
 */
VentureFeedSource *
venture_feed_request_get_source(VentureFeedRequest *self);

/**
 * venture_feed_request_get_unit:
 * @self: a request
 *
 * Returns: (transfer none): the unit being fetched
 */
const gchar *
venture_feed_request_get_unit(VentureFeedRequest *self);

/**
 * venture_feed_request_get_settings:
 * @self: a request
 *
 * The source's settings, as a JSON object this thread owns. Read it; never
 * keep it past the fetch.
 *
 * Returns: (transfer none): the settings
 */
JsonObject *
venture_feed_request_get_settings(VentureFeedRequest *self);

/**
 * venture_feed_request_get_secret:
 * @self: a request
 * @name: a setting the provider's schema marks sensitive
 *
 * A credential, sealed in the integration store and resolved on the main
 * thread when the source was frozen. Put it in a header or a request body;
 * never in an error, a note or the batch -- the worker redacts every
 * secret it can find from what it records, but a value it is not shown
 * cannot be found.
 *
 * Returns: (transfer none) (nullable): the value
 */
const gchar *
venture_feed_request_get_secret(
	VentureFeedRequest	*self,
	const gchar		*name
);

/**
 * venture_feed_request_get_secrets:
 * @self: a request
 *
 * Returns: (transfer none): every credential, possibly an empty object
 */
JsonObject *
venture_feed_request_get_secrets(VentureFeedRequest *self);

/**
 * venture_feed_request_get_frozen:
 * @self: a request
 *
 * Returns: (transfer none) (nullable): what the provider's freeze returned
 */
gpointer
venture_feed_request_get_frozen(VentureFeedRequest *self);

/**
 * venture_feed_request_get_cursor:
 * @self: a request
 *
 * Returns: (transfer none) (nullable): where the unit's last fetch said to
 *   resume
 */
const gchar *
venture_feed_request_get_cursor(VentureFeedRequest *self);

/**
 * venture_feed_request_get_if_modified_since:
 * @self: a request
 *
 * Returns: the unit's last Last-Modified as Unix seconds, or
 *   %VENTURE_SERIES_NONE
 */
gint64
venture_feed_request_get_if_modified_since(VentureFeedRequest *self);

/**
 * venture_feed_request_get_fetched_at:
 * @self: a request
 *
 * Returns: when the fetch began, Unix seconds
 */
gint64
venture_feed_request_get_fetched_at(VentureFeedRequest *self);

/**
 * venture_feed_request_get_max_bytes:
 * @self: a request
 *
 * Returns: the most bytes an answer or file may hold
 */
gsize
venture_feed_request_get_max_bytes(VentureFeedRequest *self);

/**
 * venture_feed_request_get_timeout:
 * @self: a request
 *
 * Returns: the seconds one fetch may take
 */
guint
venture_feed_request_get_timeout(VentureFeedRequest *self);

/**
 * venture_feed_request_get_cancellable:
 * @self: a request
 *
 * Returns: (transfer none): cancelled when the worker stops
 */
GCancellable *
venture_feed_request_get_cancellable(VentureFeedRequest *self);

/**
 * venture_feed_request_expand:
 * @self: a request
 * @template_text: text with `{unit}`, `{cursor}` and `{secret:NAME}`
 *   placeholders
 * @for_url: whether to percent-escape what is put in
 * @error: (out) (optional): return location for a #GError
 *
 * Fills in a URL or header template. A secret named that the source has
 * not been given is refused rather than left empty.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL on error
 */
gchar *
venture_feed_request_expand(
	VentureFeedRequest	 *self,
	const gchar		 *template_text,
	gboolean		  for_url,
	GError			**error
);

/**
 * venture_feed_request_resolve_file:
 * @self: a request
 * @path: a path, absolute or relative to one of feeds.file_roots
 * @error: (out) (optional): return location for a #GError
 *
 * Resolves a file a provider may read: realpath()'d and compared against
 * each root plus a separator, so a symlink out of the root and a sibling
 * named `<root>-evil` are both refused (%VENTURE_FEEDS_ERROR_FILE). No
 * roots configured means no file is readable.
 *
 * Returns: (transfer full) (nullable): the resolved path
 */
gchar *
venture_feed_request_resolve_file(
	VentureFeedRequest	 *self,
	const gchar		 *path,
	GError			**error
);

/**
 * venture_feed_request_read_file:
 * @self: a request
 * @path: a path for venture_feed_request_resolve_file()
 * @error: (out) (optional): return location for a #GError
 *
 * Resolves and reads a whole file, refusing one past the request's byte
 * cap before reading it. Blocks: call it from a thread that may block (a
 * #GTask worker), never from the feeds worker's own loop.
 *
 * Returns: (transfer full) (nullable): the contents
 */
GBytes *
venture_feed_request_read_file(
	VentureFeedRequest	 *self,
	const gchar		 *path,
	GError			**error
);

/**
 * venture_feed_request_http_get_async:
 * @self: a request
 * @url: an absolute http or https address
 * @headers: (nullable) (array zero-terminated=1): name, value, name,
 *   value... added to the request
 * @cost: what the request costs against the source's hourly budget;
 *   0 counts as 1
 * @callback: (scope async): called on the calling thread's context
 * @user_data: (closure): data for @callback
 *
 * GETs @url the way every feed fetch must: only from an origin on the
 * frozen allowlist, compared again on the final address; never following
 * a redirect; with If-Modified-Since when the unit has a Last-Modified;
 * reading at most the byte cap; inside the deadline; and against the
 * source's request budget. A 429's Retry-After is kept for the worker.
 * Run it on the thread that owns the request's session -- the feeds worker,
 * or the private context of a test fetch.
 */
void
venture_feed_request_http_get_async(
	VentureFeedRequest	*self,
	const gchar		*url,
	const gchar *const	*headers,
	guint			 cost,
	GAsyncReadyCallback	 callback,
	gpointer		 user_data
);

/**
 * venture_feed_request_http_get_finish:
 * @self: a request
 * @result: the result
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): a 200 or 304 answer, or %NULL with
 *   a #VENTURE_FEEDS_ERROR, %VENTURE_ERROR_TIMEOUT or
 *   %VENTURE_ERROR_NETWORK
 */
VentureFeedHttpResponse *
venture_feed_request_http_get_finish(
	VentureFeedRequest	 *self,
	GAsyncResult		 *result,
	GError			**error
);

/**
 * venture_feed_request_http_get:
 * @self: a request
 * @url: an absolute http or https address
 * @headers: (nullable) (array zero-terminated=1): name, value pairs
 * @cost: the request's cost against the budget
 * @error: (out) (optional): return location for a #GError
 *
 * The same, blocking: for a provider whose fetch runs on a #GTask worker
 * (venture_func_data_source_provider_new()). It iterates a private main
 * context with a session of its own, never the thread's default context.
 *
 * Returns: (transfer full) (nullable): the answer
 */
VentureFeedHttpResponse *
venture_feed_request_http_get(
	VentureFeedRequest	 *self,
	const gchar		 *url,
	const gchar *const	 *headers,
	guint			  cost,
	GError			**error
);

/**
 * venture_feed_request_http_send_async:
 * @self: a request
 * @method: "GET" or "POST"
 * @url: an absolute http or https address
 * @headers: (nullable) (array zero-terminated=1): name, value pairs
 * @body: (nullable): a POST's body; refused on a GET
 * @content_type: (nullable): the body's type, e.g.
 *   "application/x-www-form-urlencoded"
 * @cost: the request's cost against the source's hourly budget; 0 is 1
 * @flags: how the request is made
 * @callback: (scope async): called on the calling thread's context
 * @user_data: (closure): data for @callback
 *
 * The general form of venture_feed_request_http_get_async(), under the
 * same rules -- the allowlist, no redirects, the pinned address, the body
 * cap, the deadline and the budget -- for a provider that has to POST (an
 * OAuth token) or make requests that are not the unit's own answer.
 */
void
venture_feed_request_http_send_async(
	VentureFeedRequest	*self,
	const gchar		*method,
	const gchar		*url,
	const gchar *const	*headers,
	GBytes			*body,
	const gchar		*content_type,
	guint			 cost,
	VentureFeedHttpFlags	 flags,
	GAsyncReadyCallback	 callback,
	gpointer		 user_data
);

/**
 * venture_feed_request_http_send_finish:
 * @self: a request
 * @result: the result
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the answer
 */
VentureFeedHttpResponse *
venture_feed_request_http_send_finish(
	VentureFeedRequest	 *self,
	GAsyncResult		 *result,
	GError			**error
);

/**
 * venture_feed_request_http_send:
 * @self: a request
 * @method: "GET" or "POST"
 * @url: an absolute http or https address
 * @headers: (nullable) (array zero-terminated=1): name, value pairs
 * @body: (nullable): a POST's body
 * @content_type: (nullable): the body's type
 * @cost: the request's cost against the budget
 * @flags: how the request is made
 * @error: (out) (optional): return location for a #GError
 *
 * The same, blocking, on a private main context with a session of its
 * own: for a provider on a #GTask worker, or for a main-thread action
 * that must reach the far end (it then blocks the main loop for at most
 * the deadline, and nothing else of the main loop runs nested inside).
 *
 * Returns: (transfer full) (nullable): the answer
 */
VentureFeedHttpResponse *
venture_feed_request_http_send(
	VentureFeedRequest	 *self,
	const gchar		 *method,
	const gchar		 *url,
	const gchar *const	 *headers,
	GBytes			 *body,
	const gchar		 *content_type,
	guint			  cost,
	VentureFeedHttpFlags	  flags,
	GError			**error
);

/**
 * venture_feed_request_redact:
 * @self: a request
 * @text: (nullable): text a provider is about to put in an error or a note
 *
 * Replaces every credential the source holds (six bytes or longer) with a
 * marker. The worker does this to everything it records; a provider that
 * builds a message from something the far end said -- or from a URL that
 * carries a key in its query, as some APIs insist on -- should do it too,
 * so the message is safe wherever else it goes.
 *
 * Returns: (transfer full) (nullable): the redacted text
 */
gchar *
venture_feed_request_redact(
	VentureFeedRequest	*self,
	const gchar		*text
);

/* --- The provider interface ------------------------------------------------------ */

#define VENTURE_TYPE_DATA_SOURCE_PROVIDER (venture_data_source_provider_get_type())

G_DECLARE_INTERFACE(VentureDataSourceProvider, venture_data_source_provider,
                    VENTURE, DATA_SOURCE_PROVIDER, GObject)

/**
 * VentureDataSourceProviderInterface:
 * @parent_iface: the parent interface
 * @get_name: the registry key: lower case, digits and underscores
 * @get_label: what a person reads
 * @dup_settings_schema: a JSON schema of the settings; properties marked
 *   `"x-sensitive": true` are credentials, kept in the integration store
 * @freeze: optional; main thread; make what a fetch needs from main-thread
 *   state, immutable, freed with *@out_free on any thread
 * @list_units: optional; the units a source's settings name; the default
 *   is the settings' `units` list, else one unit called "default"
 * @fetch_async: fetch one unit; complete on the calling thread's context
 * @fetch_finish: optional; the default propagates a #GTask's pointer
 *
 * A source of market data. See the header comment for which thread runs
 * which method.
 */
struct _VentureDataSourceProviderInterface
{
	GTypeInterface parent_iface;

	const gchar *	(*get_name)		(VentureDataSourceProvider *self);
	const gchar *	(*get_label)		(VentureDataSourceProvider *self);
	JsonNode *	(*dup_settings_schema)	(VentureDataSourceProvider *self);
	gpointer	(*freeze)		(VentureDataSourceProvider *self,
						 VentureContext *context,
						 JsonObject *settings,
						 GDestroyNotify *out_free,
						 GError **error);
	gchar **	(*list_units)		(VentureDataSourceProvider *self,
						 JsonObject *settings,
						 GError **error);
	void		(*fetch_async)		(VentureDataSourceProvider *self,
						 VentureFeedRequest *request,
						 GCancellable *cancellable,
						 GAsyncReadyCallback callback,
						 gpointer user_data);
	VentureFeedBatch * (*fetch_finish)	(VentureDataSourceProvider *self,
						 GAsyncResult *result,
						 GError **error);

	gpointer padding[8];
};

/**
 * venture_data_source_provider_get_name:
 * @self: a provider
 *
 * Returns: (transfer none): its registry key
 */
const gchar *
venture_data_source_provider_get_name(VentureDataSourceProvider *self);

/**
 * venture_data_source_provider_get_label:
 * @self: a provider
 *
 * Returns: (transfer none): what a person reads; the name when it has none
 */
const gchar *
venture_data_source_provider_get_label(VentureDataSourceProvider *self);

/**
 * venture_data_source_provider_dup_settings_schema:
 * @self: a provider
 *
 * Returns: (transfer full): a JSON schema object; an empty one when the
 *   provider declares none
 */
JsonNode *
venture_data_source_provider_dup_settings_schema(VentureDataSourceProvider *self);

/**
 * venture_data_source_provider_freeze:
 * @self: a provider
 * @context: the context, main thread only
 * @settings: the source's settings
 * @out_free: (out): how to free what is returned
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the frozen data; %NULL with no
 *   error when the provider needs none
 */
gpointer
venture_data_source_provider_freeze(
	VentureDataSourceProvider	 *self,
	VentureContext			 *context,
	JsonObject			 *settings,
	GDestroyNotify			 *out_free,
	GError				**error
);

/**
 * venture_data_source_provider_list_units:
 * @self: a provider
 * @settings: the source's settings
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (array zero-terminated=1) (nullable): at least
 *   one unit, or %NULL on error
 */
gchar **
venture_data_source_provider_list_units(
	VentureDataSourceProvider	 *self,
	JsonObject			 *settings,
	GError				**error
);

/**
 * venture_data_source_provider_fetch_async:
 * @self: a provider
 * @request: what to fetch
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called on the calling thread's context
 * @user_data: (closure): data for @callback
 */
void
venture_data_source_provider_fetch_async(
	VentureDataSourceProvider	*self,
	VentureFeedRequest		*request,
	GCancellable			*cancellable,
	GAsyncReadyCallback		 callback,
	gpointer			 user_data
);

/**
 * venture_data_source_provider_fetch_finish:
 * @self: a provider
 * @result: the result
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the batch
 */
VentureFeedBatch *
venture_data_source_provider_fetch_finish(
	VentureDataSourceProvider	 *self,
	GAsyncResult			 *result,
	GError				**error
);

/* --- A provider from functions --------------------------------------------------- */

/**
 * VentureFuncDataSourceFetch:
 * @request: what to fetch
 * @user_data: the data given at construction
 * @error: (out) (optional): return location for a #GError
 *
 * Fetches one unit, blocking. Runs on a #GTask worker thread, never the
 * feeds worker's loop, so it may block on venture_feed_request_http_get()
 * or venture_feed_request_read_file(). It must touch nothing but @request.
 *
 * Returns: (transfer full) (nullable): the batch
 */
typedef VentureFeedBatch *(*VentureFuncDataSourceFetch) (
	VentureFeedRequest	 *request,
	gpointer		  user_data,
	GError			**error
);

/**
 * venture_func_data_source_provider_new:
 * @name: the registry key
 * @label: (nullable): what a person reads
 * @settings_schema_json: (nullable): a JSON schema object, as text
 * @fetch: (scope notified): the fetch
 * @user_data: (closure): data for @fetch, shared by every thread
 * @destroy: (nullable): frees @user_data with the provider
 *
 * A provider without the GObject boilerplate, for a crispy script or a
 * small native plugin. @user_data is read from several threads at once
 * and must not change.
 *
 * Returns: (transfer full): the provider
 */
VentureDataSourceProvider *
venture_func_data_source_provider_new(
	const gchar			*name,
	const gchar			*label,
	const gchar			*settings_schema_json,
	VentureFuncDataSourceFetch	 fetch,
	gpointer			 user_data,
	GDestroyNotify			 destroy
);

/* --- The registry -------------------------------------------------------------- */

#define VENTURE_TYPE_DATA_SOURCE_PROVIDER_REGISTRY (venture_data_source_provider_registry_get_type())

G_DECLARE_FINAL_TYPE(VentureDataSourceProviderRegistry, venture_data_source_provider_registry,
                     VENTURE, DATA_SOURCE_PROVIDER_REGISTRY, GObject)

/**
 * venture_data_source_provider_registry_new:
 *
 * Returns: (transfer full): an empty registry
 */
VentureDataSourceProviderRegistry *
venture_data_source_provider_registry_new(void);

/**
 * venture_data_source_provider_registry_add:
 * @self: a registry
 * @provider: the provider; the registry takes a reference
 * @error: (out) (optional): return location for a #GError
 *
 * A held name is never replaced: two plugins would each believe they own
 * the sources that name it.
 *
 * Returns: %TRUE when it was added; %VENTURE_ERROR_ALREADY_EXISTS for a
 *   name already held, %VENTURE_ERROR_INVALID_ARGUMENT for a bad one
 */
gboolean
venture_data_source_provider_registry_add(
	VentureDataSourceProviderRegistry	 *self,
	VentureDataSourceProvider		 *provider,
	GError					**error
);

/**
 * venture_data_source_provider_registry_lookup:
 * @self: a registry
 * @name: (nullable): a provider's name
 *
 * Returns: (transfer none) (nullable): the provider
 */
VentureDataSourceProvider *
venture_data_source_provider_registry_lookup(
	VentureDataSourceProviderRegistry	*self,
	const gchar				*name
);

/**
 * venture_data_source_provider_registry_list:
 * @self: a registry
 *
 * Returns: (transfer container) (element-type VentureDataSourceProvider):
 *   every provider, by name
 */
GPtrArray *
venture_data_source_provider_registry_list(VentureDataSourceProviderRegistry *self);

/**
 * venture_data_source_provider_name_is_valid:
 * @name: (nullable): a candidate name
 *
 * Returns: whether it is `[a-z][a-z0-9_]*`, at most 64 bytes
 */
gboolean
venture_data_source_provider_name_is_valid(const gchar *name);

G_END_DECLS

#endif /* VENTURE_DATA_SOURCE_PROVIDER_H */
