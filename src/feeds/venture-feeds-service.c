/*
 * venture-feeds-service.c - Market data feeds, on the main thread
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything the feeds module does that touches the database happens here,
 * on the thread that opened it:
 *
 * - freezing a data_source into a #VentureFeedSource: its settings, the
 *   credentials sealed under "feed-<uuid>", the provider's frozen data,
 *   every limit from the configuration and every hook's frozen copy;
 * - keeping the worker in step: a source or its credentials saved, the
 *   configuration changed or a hook added, and every source is frozen
 *   again after the transaction that caused it;
 * - taking each finished run back and writing it -- once, as the system --
 *   then the records it carried, then `feed_synced` or `feed_failed`, then
 *   the hooks. A run that arrives while a transaction is open waits for it
 *   to close rather than joining somebody else's.
 */

#include "venture.h"
#include "feeds/venture-feeds-private.h"

#include <yaml-glib.h>
#include <string.h>

/* The most text a source's settings may hold. */
#define FEEDS_MAX_SETTINGS (64 * 1024)

/* How long a run waits between looks for a moment with no transaction. */
#define FEEDS_RETRY_MS (50)

/* Where the last context made over a database is kept, for the validator
 * and the actions, which are per database. */
#define FEEDS_CONTEXT_KEY "venture-feeds-context"
#define FEEDS_INSTALLED_KEY "venture-feeds-installed"
#define FEEDS_HOOKS_KEY "venture-feeds-hooks"

/* --- Hooks ------------------------------------------------------------------------- */

FeedHook *
feed_hook_ref(FeedHook *hook)
{
	g_atomic_ref_count_inc(&hook->ref);

	return hook;
}

void
feed_hook_unref(FeedHook *hook)
{
	if ((NULL == hook) || !g_atomic_ref_count_dec(&hook->ref))
		return;

	if (NULL != hook->destroy)
		hook->destroy(hook->user_data);

	g_free(hook->name);
	g_free(hook);
}

static void
feed_frozen_hook_free(gpointer data)
{
	FeedFrozenHook *frozen = data;

	if ((NULL != frozen->frozen) && (NULL != frozen->frozen_free))
		frozen->frozen_free(frozen->frozen);

	feed_hook_unref(frozen->hook);
	g_free(frozen);
}

static GPtrArray *
feeds_hooks(VentureContext *context)
{
	GPtrArray *hooks;

	hooks = g_object_get_data(G_OBJECT(context), FEEDS_HOOKS_KEY);

	if (NULL == hooks)
	{
		hooks = g_ptr_array_new_with_free_func((GDestroyNotify)feed_hook_unref);
		g_object_set_data_full(G_OBJECT(context), FEEDS_HOOKS_KEY, hooks,
		                       (GDestroyNotify)g_ptr_array_unref);
	}

	return hooks;
}

gboolean
venture_feeds_add_hook(
	VentureContext			*context,
	const gchar			*name,
	VentureFeedsHookFreezeFunc	 freeze,
	VentureFeedsHookCommitFunc	 commit,
	VentureFeedsHookRunFunc		 run,
	gpointer			 user_data,
	GDestroyNotify			 destroy
){
	GPtrArray *hooks;
	FeedHook *hook;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(NULL != name, FALSE);

	hooks = feeds_hooks(context);

	for (i = 0; i < hooks->len; i++)
	{
		if (0 == g_strcmp0(((FeedHook *)g_ptr_array_index(hooks, i))->name, name))
			return FALSE;
	}

	hook = g_new0(FeedHook, 1);
	g_atomic_ref_count_init(&hook->ref);
	hook->name = g_strdup(name);
	hook->freeze = freeze;
	hook->commit = commit;
	hook->run = run;
	hook->user_data = user_data;
	hook->destroy = destroy;
	g_ptr_array_add(hooks, hook);

	/* A running worker sees the hook from the next freeze on. Only a
	 * service that exists is refreshed: hooks are added while the context
	 * is built, before the schema may exist, and asking for the service
	 * there would make one and read tables that are not there yet. */
	venture_feeds_queue_refresh(context);

	return TRUE;
}

gconstpointer
venture_feed_source_get_hook_data(
	VentureFeedSource	*self,
	const gchar		*hook_name
){
	guint i;

	g_return_val_if_fail(NULL != self, NULL);

	for (i = 0; (NULL != self->hooks) && (i < self->hooks->len); i++)
	{
		FeedFrozenHook *frozen = g_ptr_array_index(self->hooks, i);

		if (0 == g_strcmp0(frozen->hook->name, hook_name))
			return frozen->frozen;
	}

	return NULL;
}

/* --- Small helpers ---------------------------------------------------------------- */

/* The last context made over @database, or NULL: a scratch database with
 * no context validates what it can without the registry. */
static VentureContext *
feeds_context_for(VentureDatabase *database)
{
	GWeakRef *ref;

	ref = g_object_get_data(G_OBJECT(database), FEEDS_CONTEXT_KEY);

	return (NULL != ref) ? g_weak_ref_get(ref) : NULL;
}

static void
feeds_weak_ref_free(gpointer data)
{
	g_weak_ref_clear(data);
	g_free(data);
}

static const VentureActor *
feeds_system_actor(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_SYSTEM;
	actor->name = "feeds";
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;

	return actor;
}

JsonObject *
venture_feeds_parse_settings(
	const gchar	 *text,
	GError		**error
){
	g_autoptr(YamlParser) parser = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autoptr(JsonNode) root = NULL;
	YamlNode *yaml_root;

	if (venture_string_is_empty(text))
		return json_object_new();

	if (strlen(text) > FEEDS_MAX_SETTINGS)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "Settings are at most %d bytes", FEEDS_MAX_SETTINGS);
		return NULL;
	}

	parser = yaml_parser_new();

	if (!yaml_parser_load_from_data(parser, text, -1, &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "Settings are not valid YAML: %s", local_error->message);
		return NULL;
	}

	yaml_root = yaml_parser_get_root(parser);

	if (NULL == yaml_root)
		return json_object_new();

	root = yaml_node_to_json_node(yaml_root);

	if ((NULL == root) || !JSON_NODE_HOLDS_OBJECT(root))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "Settings must be a YAML mapping of names to values");
		return NULL;
	}

	return json_object_ref(json_node_get_object(root));
}

gchar *
venture_feeds_store_dir(
	VentureConfig	*config,
	const gchar	*uuid
){
	g_autofree gchar *state = NULL;

	g_return_val_if_fail(VENTURE_IS_CONFIG(config), NULL);
	g_return_val_if_fail(NULL != uuid, NULL);

	g_object_get(config, "state-dir", &state, NULL);

	/* The same fallback the context uses for attachments. */
	if (venture_string_is_empty(state))
	{
		g_free(state);
		state = g_build_filename(g_get_user_data_dir(), "venture", NULL);
	}

	return g_build_filename(state, "series", uuid, NULL);
}

/* A list setting of strings, as a strv; NULL when absent. */
static gchar **
feeds_string_list(
	JsonObject	*settings,
	const gchar	*name
){
	g_autoptr(GPtrArray) list = NULL;
	JsonNode *node;
	JsonArray *array;
	guint i;

	node = json_object_get_member(settings, name);

	if ((NULL == node) || !JSON_NODE_HOLDS_ARRAY(node))
		return NULL;

	array = json_node_get_array(node);
	list = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonNode *element = json_array_get_element(array, i);

		if (!JSON_NODE_HOLDS_VALUE(element))
			continue;

		if (G_TYPE_STRING == json_node_get_value_type(element))
			g_ptr_array_add(list, g_strdup(json_node_get_string(element)));
		else if (G_TYPE_INT64 == json_node_get_value_type(element))
			g_ptr_array_add(list, g_strdup_printf("%" G_GINT64_FORMAT,
			                                      json_node_get_int(element)));
	}

	g_ptr_array_add(list, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&list), FALSE);
}

/*
 * One origin, in the spelling the request compares: scheme://host[:port],
 * lower case, no path. Plain http only for a loopback host, so a
 * credential never crosses a network in the clear.
 */
static gchar *
feeds_normalise_origin(const gchar *text)
{
	g_autoptr(GUri) uri = NULL;
	g_autofree gchar *host = NULL;
	const gchar *scheme;
	gint port;

	if (venture_string_is_empty(text))
		return NULL;

	uri = g_uri_parse(text, G_URI_FLAGS_NONE, NULL);

	if ((NULL == uri) || (NULL == g_uri_get_host(uri)) || (NULL != g_uri_get_userinfo(uri)) ||
	    ((0 != g_strcmp0(g_uri_get_path(uri), "")) && (0 != g_strcmp0(g_uri_get_path(uri), "/"))) ||
	    (NULL != g_uri_get_query(uri)))
		return NULL;

	scheme = g_uri_get_scheme(uri);
	host = g_ascii_strdown(g_uri_get_host(uri), -1);
	port = g_uri_get_port(uri);

	if (0 == g_ascii_strcasecmp(scheme, "https"))
	{
		if (443 == port)
			port = -1;
	}
	else if (0 == g_ascii_strcasecmp(scheme, "http"))
	{
		if ((0 != g_strcmp0(host, "127.0.0.1")) && (0 != g_strcmp0(host, "::1")) &&
		    (0 != g_strcmp0(host, "localhost")))
			return NULL;

		if (80 == port)
			port = -1;
	}
	else
		return NULL;

	if (NULL != strchr(host, ':'))
		return (port > 0)
			? g_strdup_printf("%s://[%s]:%d", g_ascii_strdown(scheme, -1), host, port)
			: g_strdup_printf("%s://[%s]", g_ascii_strdown(scheme, -1), host);

	return (port > 0)
		? g_strdup_printf("%s://%s:%d", (0 == g_ascii_strcasecmp(scheme, "https")) ? "https" : "http", host, port)
		: g_strdup_printf("%s://%s", (0 == g_ascii_strcasecmp(scheme, "https")) ? "https" : "http", host);
}

/*
 * The origins a source may reach: the operator's feeds.allowed_origins,
 * narrowed by the source's own `origins` when it gives them. A source
 * cannot widen the operator's list, only narrow it.
 */
static gchar **
feeds_allowed_origins(
	VentureConfig	*config,
	JsonObject	*settings
){
	g_autofree gchar *configured = NULL;
	g_auto(GStrv) entries = NULL;
	g_auto(GStrv) narrowed = NULL;
	g_autoptr(GPtrArray) list = NULL;
	guint i;

	g_object_get(config, "feeds-allowed-origins", &configured, NULL);
	entries = g_strsplit((NULL != configured) ? configured : "", ",", -1);
	narrowed = feeds_string_list(settings, "origins");
	list = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; NULL != entries[i]; i++)
	{
		g_autofree gchar *origin = feeds_normalise_origin(g_strstrip(entries[i]));
		gboolean wanted;
		guint j;

		if (NULL == origin)
			continue;

		wanted = (NULL == narrowed);

		for (j = 0; !wanted && (NULL != narrowed[j]); j++)
		{
			g_autofree gchar *mine = feeds_normalise_origin(narrowed[j]);

			wanted = (0 == g_strcmp0(mine, origin));
		}

		if (wanted)
			g_ptr_array_add(list, g_steal_pointer(&origin));
	}

	g_ptr_array_add(list, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&list), FALSE);
}

static gchar **
feeds_file_roots(VentureConfig *config)
{
	g_autofree gchar *configured = NULL;
	g_auto(GStrv) entries = NULL;
	g_autoptr(GPtrArray) list = NULL;
	guint i;

	g_object_get(config, "feeds-file-roots", &configured, NULL);
	entries = g_strsplit((NULL != configured) ? configured : "", ",", -1);
	list = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; NULL != entries[i]; i++)
	{
		const gchar *root = g_strstrip(entries[i]);

		/* Only absolute: a relative root would mean wherever the
		 * server happened to start. */
		if (g_path_is_absolute(root))
			g_ptr_array_add(list, g_strdup(root));
	}

	g_ptr_array_add(list, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&list), FALSE);
}

/* --- Records the sink may write ------------------------------------------------------ */

/*
 * The main-database types a feed may write, and the fields that find an
 * existing row when a record gives no `match`. Short on purpose: each is a
 * type whose rows are evidence of a price, never a document anybody
 * posts. A later module adds to it with venture_feeds_allow_record_type().
 */
typedef struct
{
	const gchar		*type;
	const gchar *const	*keys;
} FeedsRecordType;

static const gchar *const feeds_price_observation_keys[] = {
	"product_id", "source", "observed_at", NULL
};
static const gchar *const feeds_exchange_rate_keys[] = {
	"from_currency", "to_currency", "effective_at", "source", NULL
};

static const FeedsRecordType feeds_record_types[] = {
	{ "price_observation", feeds_price_observation_keys },
	{ "exchange_rate", feeds_exchange_rate_keys },
};

static const FeedsRecordType *
feeds_record_type(const gchar *name)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(feeds_record_types); i++)
	{
		if (0 == g_strcmp0(feeds_record_types[i].type, name))
			return &feeds_record_types[i];
	}

	return NULL;
}

/* --- Freezing ---------------------------------------------------------------------- */

/*
 * A source as the worker will see it. Read here, on the main thread, from
 * the record, its sealed credentials, the provider, the configuration and
 * every hook; nothing in it changes afterwards.
 */
static VentureFeedSource *
feeds_freeze(
	VentureContext	 *context,
	VentureEntity	 *record,
	GError		**error
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureFeedSource) spec = NULL;
	g_autoptr(JsonObject) settings = NULL;
	g_autoptr(JsonObject) secrets = NULL;
	g_autoptr(VentureMoney) min_value = NULL;
	g_autofree gchar *provider_name = NULL;
	g_autofree gchar *settings_text = NULL;
	g_autofree gchar *schedule = NULL;
	g_autofree gchar *currency = NULL;
	g_autofree gchar *venue_namespace = NULL;
	g_autofree gchar *instrument_namespace = NULL;
	g_autofree gchar *name = NULL;
	VentureConfig *config;
	VentureDataSourceProvider *provider;
	VentureDataSourceTrack track;
	GPtrArray *hooks;
	gboolean enabled;
	gint64 hourly_days;
	gint64 daily_days;
	gint64 max_store_mb;
	gint64 max_response_mb;
	gint64 request_timeout;
	gint64 max_records;
	gint64 run_window;
	guint i;

	config = venture_context_get_config(context);

	/*
	 * Read as the system. Whoever asked was judged already -- the action,
	 * the route, the rule -- and the credentials are sealed per source,
	 * not per person.
	 */
	internal = venture_access_policy_enter(
		venture_database_get_access_policy(venture_context_get_database(context)), NULL);

	g_object_get(record, "name", &name, "provider", &provider_name, "settings", &settings_text,
	             "schedule", &schedule, "enabled", &enabled, "track", &track,
	             "currency", &currency, "venue-namespace", &venue_namespace,
	             "instrument-namespace", &instrument_namespace, "min-value", &min_value,
	             NULL);

	provider = venture_data_source_provider_registry_lookup(
		venture_context_get_data_source_providers(context), provider_name);

	if (NULL == provider)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No data source provider named %s is registered", provider_name);
		return NULL;
	}

	settings = venture_feeds_parse_settings(settings_text, error);

	if (NULL == settings)
		return NULL;

	spec = g_new0(VentureFeedSource, 1);
	g_atomic_ref_count_init(&spec->ref);
	spec->id = venture_entity_get_id(record);
	spec->uuid = g_strdup(venture_entity_get_uuid(record));
	spec->organization_id = venture_entity_get_organization_id(record);
	spec->name = g_steal_pointer(&name);
	spec->provider_name = g_strdup(provider_name);
	spec->provider = g_object_ref(provider);
	spec->enabled = enabled;
	spec->schedule = g_strdup(venture_string_is_empty(schedule) ? "auto" : schedule);
	spec->currency = venture_string_is_empty(currency) ? NULL : g_ascii_strup(currency, -1);
	spec->venue_namespace = venture_string_is_empty(venue_namespace) ? NULL : g_steal_pointer(&venue_namespace);
	spec->instrument_namespace = venture_string_is_empty(instrument_namespace) ? NULL : g_steal_pointer(&instrument_namespace);
	spec->min_value = (NULL != min_value) ? venture_money_get_amount(min_value) : VENTURE_SERIES_NONE;
	spec->min_currency = (NULL != min_value) ? g_strdup(venture_money_get_currency(min_value)) : NULL;
	spec->store_dir = venture_feeds_store_dir(config, spec->uuid);

	/* Credentials, sealed under the source's own uuid. None set is an
	 * empty object; a provider that needs one says so when it fetches. */
	{
		g_autoptr(VentureIntegrationConnection) binding = NULL;
		g_autoptr(GError) lookup_error = NULL;
		g_autofree gchar *key = g_strconcat("feed-", spec->uuid, NULL);
		VentureIntegrationService *integrations;

		integrations = venture_integration_service_get(venture_context_get_database(context));
		binding = venture_integration_service_find(integrations, spec->organization_id, key,
		                                           &lookup_error);

		if (NULL != binding)
		{
			g_autoptr(JsonNode) values = NULL;

			values = venture_integration_service_resolve_version(integrations,
				spec->organization_id, venture_entity_get_id(VENTURE_ENTITY(binding)),
				venture_entity_get_version(VENTURE_ENTITY(binding)), FALSE, error);

			if (NULL == values)
				return NULL;

			if (JSON_NODE_HOLDS_OBJECT(values))
				secrets = json_object_ref(json_node_get_object(values));
		}
		else if ((NULL != lookup_error) &&
		         !g_error_matches(lookup_error, VENTURE_ERROR, VENTURE_ERROR_CONFIG))
		{
			g_propagate_error(error, g_steal_pointer(&lookup_error));
			return NULL;
		}
	}

	if (NULL == secrets)
		secrets = json_object_new();

	{
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
		JsonObjectIter iter;
		const gchar *member;
		JsonNode *value;

		json_node_set_object(node, secrets);
		spec->secrets_json = json_to_string(node, FALSE);
		spec->secret_values = g_ptr_array_new_with_free_func(g_free);
		json_object_iter_init(&iter, secrets);

		/* Every value long enough to recognise, for redaction. */
		while (json_object_iter_next(&iter, &member, &value))
		{
			if (JSON_NODE_HOLDS_VALUE(value) &&
			    (G_TYPE_STRING == json_node_get_value_type(value)) &&
			    (strlen(json_node_get_string(value)) >= 6))
				g_ptr_array_add(spec->secret_values, g_strdup(json_node_get_string(value)));
		}

		json_node_set_object(node, settings);
		spec->settings_json = json_to_string(node, FALSE);
	}

	spec->units = venture_data_source_provider_list_units(provider, settings, error);

	if (NULL == spec->units)
		return NULL;

	if (VENTURE_DATA_SOURCE_TRACK_KNOWN == track)
	{
		g_auto(GStrv) known = feeds_string_list(settings, "instruments");

		spec->track_known = TRUE;
		spec->known = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

		for (i = 0; (NULL != known) && (NULL != known[i]); i++)
			g_hash_table_add(spec->known, g_strdup(known[i]));

		/* And the instrument records filed under this source: promoting
		 * one, or typing one in with its key, is how it becomes known.
		 * Read now, on the main thread, so the worker only ever sees
		 * the frozen set. */
		{
			g_autoptr(GPtrArray) promoted = venture_marketdata_known_keys(context, record);

			for (i = 0; i < promoted->len; i++)
				g_hash_table_add(spec->known, g_strdup(g_ptr_array_index(promoted, i)));
		}
	}

	/* The limits, from the configuration now. */
	g_object_get(config, "series-hourly-days", &hourly_days, "series-daily-days", &daily_days,
	             "series-max-store-mb", &max_store_mb, "feeds-max-response-mb", &max_response_mb,
	             "feeds-request-timeout", &request_timeout,
	             "feeds-max-records-per-run", &max_records,
	             "feeds-run-window-minutes", &run_window, NULL);

	spec->hourly_days = (guint)CLAMP(hourly_days, 0, G_MAXINT32);
	spec->daily_days = (guint)CLAMP(daily_days, 0, G_MAXINT32);
	spec->max_store_bytes = (guint64)MAX(max_store_mb, 0) * 1048576;
	spec->max_response_bytes = (gsize)CLAMP(max_response_mb, 1, 4096) * 1048576;
	spec->request_timeout = (guint)CLAMP(request_timeout, 1, 3600);
	spec->max_records = (guint)CLAMP(max_records, 0, 100000);
	spec->run_window = (guint)CLAMP(run_window, 0, 1440) * 60;
	spec->allowed_origins = feeds_allowed_origins(config, settings);
	spec->file_roots = feeds_file_roots(config);

	{
		JsonNode *node = json_object_get_member(settings, "requests_per_hour");

		spec->requests_per_hour = ((NULL != node) && JSON_NODE_HOLDS_VALUE(node) &&
		                           (G_TYPE_INT64 == json_node_get_value_type(node)))
			? (guint)CLAMP(json_node_get_int(node), 0, 1000000) : 0;
	}

	{
		g_auto(GStrv) types = feeds_string_list(settings, "record_types");
		g_autoptr(GPtrArray) accepted = g_ptr_array_new_with_free_func(g_free);

		for (i = 0; (NULL != types) && (NULL != types[i]); i++)
		{
			if (NULL != feeds_record_type(types[i]))
				g_ptr_array_add(accepted, g_strdup(types[i]));
		}

		g_ptr_array_add(accepted, NULL);
		spec->record_types = (gchar **)g_ptr_array_free(g_steal_pointer(&accepted), FALSE);
	}

	/* The provider's own frozen data -- an exec plugin's program and
	 * request -- last, once everything it might read is settled. */
	{
		GError *freeze_error = NULL;

		spec->frozen = venture_data_source_provider_freeze(provider, context, settings,
		                                                   &spec->frozen_free, &freeze_error);

		if (NULL != freeze_error)
		{
			g_propagate_error(error, freeze_error);
			return NULL;
		}
	}

	hooks = feeds_hooks(context);
	spec->hooks = g_ptr_array_new_with_free_func(feed_frozen_hook_free);

	for (i = 0; i < hooks->len; i++)
	{
		FeedHook *hook = g_ptr_array_index(hooks, i);
		FeedFrozenHook *frozen = g_new0(FeedFrozenHook, 1);

		frozen->hook = feed_hook_ref(hook);

		if (NULL != hook->freeze)
			frozen->frozen = hook->freeze(context, record, hook->user_data, &frozen->frozen_free);

		g_ptr_array_add(spec->hooks, frozen);
	}

	return g_steal_pointer(&spec);
}

/* --- The service ------------------------------------------------------------------- */

struct _VentureFeedsService
{
	GObject			 parent_instance;

	VentureContext		*context;	/* not owned: the context owns us */
	VentureSeriesWorker	*worker;
	gulong			 run_handler;

	GQueue			 waiting;	/* VentureFeedRun: a transaction was open */
	guint			 waiting_source;
	guint			 refresh_source;
	gboolean		 shut_down;
};

G_DEFINE_FINAL_TYPE(VentureFeedsService, venture_feeds_service, G_TYPE_OBJECT)

static void feeds_service_run_finished(VentureSeriesWorker *worker, VentureFeedRun *run,
                                       gpointer user_data);

void
venture_feeds_service_shutdown(VentureFeedsService *self)
{
	g_return_if_fail(VENTURE_IS_FEEDS_SERVICE(self));

	self->shut_down = TRUE;

	if (0 != self->refresh_source)
	{
		g_source_remove(self->refresh_source);
		self->refresh_source = 0;
	}

	if (0 != self->waiting_source)
	{
		g_source_remove(self->waiting_source);
		self->waiting_source = 0;
	}

	g_queue_clear_full(&self->waiting, (GDestroyNotify)venture_feed_run_unref);

	if (NULL != self->worker)
	{
		g_signal_handler_disconnect(self->worker, self->run_handler);
		self->run_handler = 0;
		venture_series_worker_stop(self->worker);
		g_clear_object(&self->worker);
	}
}

static void
venture_feeds_service_dispose(GObject *object)
{
	venture_feeds_service_shutdown(VENTURE_FEEDS_SERVICE(object));

	G_OBJECT_CLASS(venture_feeds_service_parent_class)->dispose(object);
}

static void
venture_feeds_service_class_init(VentureFeedsServiceClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = venture_feeds_service_dispose;
}

static void
venture_feeds_service_init(VentureFeedsService *self)
{
	g_queue_init(&self->waiting);
}

static VentureSeriesWorker *
feeds_service_worker(VentureFeedsService *self)
{
	if ((NULL == self->worker) && !self->shut_down)
	{
		self->worker = venture_series_worker_new();
		self->run_handler = g_signal_connect(self->worker, "run-finished",
		                                     G_CALLBACK(feeds_service_run_finished), self);
	}

	return self->worker;
}

gboolean
venture_feeds_service_is_running(VentureFeedsService *self)
{
	g_return_val_if_fail(VENTURE_IS_FEEDS_SERVICE(self), FALSE);

	return NULL != self->worker;
}

static gboolean
feeds_source_scheduled(VentureEntity *record)
{
	g_autofree gchar *schedule = NULL;
	gboolean enabled;

	g_object_get(record, "enabled", &enabled, "schedule", &schedule, NULL);

	return enabled && (0 != g_strcmp0(schedule, "manual"));
}

void
venture_feeds_service_refresh(VentureFeedsService *self)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	VentureDatabase *database;
	gboolean wanted;
	guint i;

	g_return_if_fail(VENTURE_IS_FEEDS_SERVICE(self));

	if (self->shut_down || !venture_context_module_enabled(self->context, "feeds"))
		return;

	database = venture_context_get_database(self->context);

	/* Every organization's sources, read as the system: the worker serves
	 * the install, not whoever happened to save the last one. */
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	query = venture_query_new(VENTURE_TYPE_DATA_SOURCE);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(database, query, &error);

	if (NULL == rows)
	{
		g_message("feeds: could not read the data sources: %s", error->message);
		return;
	}

	wanted = FALSE;

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *record = g_ptr_array_index(rows, i);

		if (!venture_entity_is_deleted(record) && feeds_source_scheduled(record))
			wanted = TRUE;
	}

	/* Started only for a source that runs on its own; a manual source
	 * waits for its first sync. */
	if ((NULL == self->worker) && !wanted)
		return;

	feeds_service_worker(self);

	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *record = g_ptr_array_index(rows, i);
		g_autoptr(VentureFeedSource) spec = NULL;
		g_autoptr(GError) freeze_error = NULL;

		if (venture_entity_is_deleted(record))
		{
			venture_series_worker_remove_source(self->worker, venture_entity_get_id(record));
			continue;
		}

		spec = feeds_freeze(self->context, record, &freeze_error);

		if (NULL == spec)
		{
			/* Not run on its schedule; a sync says why in a run. */
			g_message("feeds: source %" G_GINT64_FORMAT " is not scheduled: %s",
			          venture_entity_get_id(record), freeze_error->message);
			venture_series_worker_remove_source(self->worker, venture_entity_get_id(record));
			continue;
		}

		venture_series_worker_set_source(self->worker, spec);
	}
}

static gboolean
feeds_service_refresh_idle(gpointer data)
{
	VentureFeedsService *self = data;

	self->refresh_source = 0;
	venture_feeds_service_refresh(self);

	return G_SOURCE_REMOVE;
}

/* After the write that caused it, on the next turn of the loop: a source
 * saved inside a transaction is frozen once the transaction has ended. */
static void
feeds_service_queue_refresh(VentureFeedsService *self)
{
	if (self->shut_down || (0 != self->refresh_source))
		return;

	self->refresh_source = g_idle_add(feeds_service_refresh_idle, self);
}

static VentureFeedsService *feeds_service_slot(VentureContext *context);

void
venture_feeds_queue_refresh(VentureContext *context)
{
	VentureFeedsService *service;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	service = feeds_service_slot(context);

	if (NULL != service)
		feeds_service_queue_refresh(service);
}

/* --- A run comes back ------------------------------------------------------------------ */

/* Fills a match filter from the candidate's own property, in the form the
 * column stores. */
static gboolean
feeds_add_match(
	VentureQuery	 *query,
	VentureEntity	 *candidate,
	const gchar	 *wire,
	GError		**error
){
	g_autofree gchar *property = NULL;
	g_auto(GValue) value = G_VALUE_INIT;
	GParamSpec *pspec;

	property = g_strdelimit(g_strdup(wire), "_", '-');
	pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(candidate), property);

	if (NULL == pspec)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s is not a field of %s", wire, venture_entity_get_entity_name(candidate));
		return FALSE;
	}

	g_value_init(&value, pspec->value_type);
	g_object_get_property(G_OBJECT(candidate), property, &value);

	if (G_VALUE_HOLDS_STRING(&value))
		return venture_query_add_filter_string(query, property, VENTURE_FILTER_OP_EQ,
		                                       g_value_get_string(&value), error);

	if (G_VALUE_HOLDS_INT64(&value))
		return venture_query_add_filter_int(query, property, VENTURE_FILTER_OP_EQ,
		                                    g_value_get_int64(&value), error);

	if (G_VALUE_HOLDS(&value, G_TYPE_DATE_TIME) && (NULL != g_value_get_boxed(&value)))
	{
		g_autofree gchar *text = venture_time_to_string(g_value_get_boxed(&value));

		return venture_query_add_filter_string(query, property, VENTURE_FILTER_OP_EQ, text, error);
	}

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
	            "%s cannot be matched on: a match field is text, a number, a reference or a time",
	            wire);
	return FALSE;
}

/*
 * One record upsert: found by its match fields in the source's
 * organization, deleted rows included -- a row somebody deleted is
 * restored rather than shadowed by a duplicate that fails a unique index
 * on every run after -- and saved as the system through every validator.
 */
static gboolean
feeds_apply_record(
	VentureFeedsService	 *self,
	VentureFeedRun		 *run,
	FeedBatchRecord		 *record,
	GError			**error
){
	g_autoptr(JsonNode) fields = NULL;
	g_autoptr(VentureEntity) candidate = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) found = NULL;
	const FeedsRecordType *allowed;
	const gchar *const *match;
	VentureDatabase *database;
	VentureActor actor;
	VentureEntity *target;
	GType type;
	guint i;

	database = venture_context_get_database(self->context);
	allowed = feeds_record_type(record->record_type);
	type = venture_entity_registry_lookup(venture_context_get_entity_registry(self->context),
	                                      record->record_type);

	if ((NULL == allowed) || (G_TYPE_INVALID == type))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s records cannot be written by a feed here (unknown, not allowed, or its module is off)",
		            record->record_type);
		return FALSE;
	}

	fields = json_from_string(record->fields_json, error);

	if (NULL == fields)
		return FALSE;

	candidate = g_object_new(type, NULL);

	if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(candidate), fields, error))
		return FALSE;

	venture_entity_set_organization_id(candidate, run->organization_id);

	query = venture_query_new(type);
	venture_query_set_organization(query, run->organization_id);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 2);
	match = ((NULL != record->match) && (NULL != record->match[0]))
		? (const gchar *const *)record->match : allowed->keys;

	for (i = 0; NULL != match[i]; i++)
	{
		if (!feeds_add_match(query, candidate, match[i], error))
			return FALSE;
	}

	found = venture_database_find(database, query, error);

	if (NULL == found)
		return FALSE;

	if (found->len > 1)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		            "%s: more than one record matches; give match fields that name one",
		            record->record_type);
		return FALSE;
	}

	feeds_system_actor(&actor);

	if (0 == found->len)
		return venture_database_save(database, candidate, &actor, error);

	target = g_ptr_array_index(found, 0);

	if (venture_entity_is_deleted(target) &&
	    !venture_database_restore(database, target, &actor, error))
		return FALSE;

	if (!venture_serializable_from_json(VENTURE_SERIALIZABLE(target), fields, error))
		return FALSE;

	return venture_database_save(database, target, &actor, error);
}

static gchar *
feeds_join_notes(VentureFeedRun *run)
{
	g_autoptr(GString) notes = g_string_new(NULL);
	guint i;

	for (i = 0; i < run->notes->len; i++)
	{
		if (notes->len > 0)
			g_string_append_c(notes, '\n');
		g_string_append(notes, g_ptr_array_index(run->notes, i));
	}

	if (run->records_dropped > 0)
	{
		if (notes->len > 0)
			g_string_append_c(notes, '\n');
		g_string_append_printf(notes, "%" G_GINT64_FORMAT " records not written: the source "
		                       "does not accept their type, or the run passed "
		                       "feeds.max_records_per_run", run->records_dropped);
	}

	return (notes->len > 0) ? g_strdup(notes->str) : NULL;
}

static void
feeds_emit(
	VentureFeedsService	*self,
	VentureFeedRun		*run,
	gint64			 run_id
){
	g_autoptr(GError) error = NULL;
	GVariantDict data;
	const gchar *event;

	event = (VENTURE_DATA_SOURCE_RUN_STATUS_FAILED == run->status) ? "feed_failed" : "feed_synced";

	g_variant_dict_init(&data, NULL);
	g_variant_dict_insert(&data, "data_source_id", "x", run->source_id);
	g_variant_dict_insert(&data, "run_id", "x", run_id);
	g_variant_dict_insert(&data, "organization_id", "x", run->organization_id);
	g_variant_dict_insert(&data, "status", "s",
	                      (VENTURE_DATA_SOURCE_RUN_STATUS_OK == run->status) ? "ok"
	                      : (VENTURE_DATA_SOURCE_RUN_STATUS_PARTIAL == run->status) ? "partial"
	                      : (VENTURE_DATA_SOURCE_RUN_STATUS_DEFERRED == run->status) ? "deferred"
	                      : "failed");
	g_variant_dict_insert(&data, "rows", "x", run->rows);
	g_variant_dict_insert(&data, "error", "s", (NULL != run->error) ? run->error : "");

	if (!venture_automation_emit(self->context, event, g_variant_dict_end(&data), &error))
		g_message("feeds: %s was not emitted: %s", event, error->message);
}

/*
 * Writes a run: the records it carried first (so the run can say how many
 * landed), then the run itself, once, as the system; then the event and
 * the hooks. Never inside somebody else's transaction -- the caller has
 * already waited for none to be open.
 */
static void
feeds_service_write_run(
	VentureFeedsService	*self,
	VentureFeedRun		*run
){
	g_autoptr(VentureDataSourceRun) record = NULL;
	g_autoptr(GDateTime) started = NULL;
	g_autoptr(GDateTime) finished = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autofree gchar *notes = NULL;
	VentureDatabase *database;
	VentureActor actor;
	GPtrArray *hooks;
	gint64 written;
	guint i;

	database = venture_context_get_database(self->context);
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	written = 0;

	for (i = 0; i < run->records->len; i++)
	{
		g_autoptr(GError) record_error = NULL;
		FeedBatchRecord *upsert = g_ptr_array_index(run->records, i);

		if (feeds_apply_record(self, run, upsert, &record_error))
		{
			written++;
			continue;
		}

		{
			g_autofree gchar *note = g_strdup_printf("%s record not written: %s",
			                                         upsert->record_type,
			                                         (NULL != record_error) ? record_error->message
			                                                                : "refused");
			venture_feed_run_add_note(run, note);
		}
	}

	started = g_date_time_new_from_unix_utc(run->started_at);
	finished = g_date_time_new_from_unix_utc(run->finished_at);
	notes = feeds_join_notes(run);

	record = venture_data_source_run_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(record), run->organization_id);
	g_object_set(record,
	             "data-source-id", run->source_id,
	             "status", run->status,
	             "trigger", run->trigger,
	             "started-at", started,
	             "finished-at", finished,
	             "units", run->units,
	             "not-modified", run->not_modified,
	             "rows", run->rows,
	             "new-instruments", run->new_instruments,
	             "refused", run->refused,
	             "records", written,
	             "requests", run->requests,
	             "bytes", run->bytes,
	             "quota-used", run->quota_used,
	             "quota-limit", run->quota_limit,
	             "http-status", (gint64)run->http_status,
	             "error", run->error,
	             "notes", notes,
	             NULL);

	if (!venture_database_save(database, VENTURE_ENTITY(record), feeds_system_actor(&actor), &error))
	{
		/* Its source deleted mid-run, most likely: the store has the
		 * data; there is nothing to hang a run from. */
		g_message("feeds: the run of source %" G_GINT64_FORMAT " was not recorded: %s",
		          run->source_id, error->message);
		g_clear_object(&record);
	}

	g_clear_object(&internal);

	feeds_emit(self, run, (NULL != record) ? venture_entity_get_id(VENTURE_ENTITY(record)) : 0);

	hooks = feeds_hooks(self->context);

	for (i = 0; i < hooks->len; i++)
	{
		FeedHook *hook = g_ptr_array_index(hooks, i);

		if (NULL != hook->run)
			hook->run(self->context, run, (NULL != record) ? VENTURE_ENTITY(record) : NULL,
			          hook->user_data);
	}
}

static gboolean feeds_service_drain(gpointer data);

static void
feeds_service_arm_drain(VentureFeedsService *self)
{
	if ((0 == self->waiting_source) && !g_queue_is_empty(&self->waiting))
		self->waiting_source = g_timeout_add(FEEDS_RETRY_MS, feeds_service_drain, self);
}

/*
 * Writes what waited. A transaction still open -- a long request, a test
 * holding one -- means waiting again, never writing inside it: a run
 * joining somebody else's transaction would be rolled back with it and
 * never told.
 */
static gboolean
feeds_service_drain(gpointer data)
{
	VentureFeedsService *self = data;

	self->waiting_source = 0;

	while (!g_queue_is_empty(&self->waiting))
	{
		VentureFeedRun *run;

		if (venture_database_has_transaction(venture_context_get_database(self->context)))
			break;

		run = g_queue_pop_head(&self->waiting);
		feeds_service_write_run(self, run);
		venture_feed_run_unref(run);
	}

	feeds_service_arm_drain(self);

	return G_SOURCE_REMOVE;
}

static void
feeds_service_run_finished(
	VentureSeriesWorker	*worker,
	VentureFeedRun		*run,
	gpointer		 user_data
){
	VentureFeedsService *self = user_data;

	(void)worker;

	if (self->shut_down)
		return;

	g_queue_push_tail(&self->waiting, venture_feed_run_ref(run));

	if (0 == self->waiting_source)
		feeds_service_drain(self);
}

/* A run that never reached the worker -- the source could not be frozen
 * -- recorded as failed, saying why. */
static void
feeds_service_record_failure(
	VentureFeedsService		*self,
	VentureEntity			*record,
	VentureDataSourceRunTrigger	 trigger,
	const gchar			*message
){
	g_autoptr(VentureFeedRun) run = NULL;
	VentureFeedSource placeholder;
	gint64 now;

	memset(&placeholder, 0, sizeof(placeholder));
	placeholder.id = venture_entity_get_id(record);
	placeholder.organization_id = venture_entity_get_organization_id(record);

	now = g_get_real_time() / G_USEC_PER_SEC;
	run = venture_feed_run_new_internal(&placeholder, trigger, now);
	run->error = g_strdup(message);
	run->failed = 1;
	venture_feed_run_finish(run, now);

	g_queue_push_tail(&self->waiting, venture_feed_run_ref(run));
	feeds_service_arm_drain(self);
}

/* --- What callers ask for -------------------------------------------------------------- */

static VentureEntity *
feeds_get_source(
	VentureFeedsService	 *self,
	gint64			  data_source_id,
	GError			**error
){
	g_autoptr(VentureEntity) record = NULL;

	record = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_DATA_SOURCE, data_source_id, error);

	if (NULL == record)
	{
		if ((NULL != error) && (NULL == *error))
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "No data source %" G_GINT64_FORMAT, data_source_id);
		return NULL;
	}

	if (venture_entity_is_deleted(record))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Data source %" G_GINT64_FORMAT " is deleted", data_source_id);
		return NULL;
	}

	return g_steal_pointer(&record);
}

gboolean
venture_feeds_service_sync(
	VentureFeedsService		 *self,
	gint64				  data_source_id,
	VentureDataSourceRunTrigger	  trigger,
	GError				**error
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureFeedSource) spec = NULL;
	g_autoptr(GError) freeze_error = NULL;

	g_return_val_if_fail(VENTURE_IS_FEEDS_SERVICE(self), FALSE);

	if (self->shut_down)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Market data feeds have stopped");
		return FALSE;
	}

	record = feeds_get_source(self, data_source_id, error);

	if (NULL == record)
		return FALSE;

	spec = feeds_freeze(self->context, record, &freeze_error);

	if (NULL == spec)
	{
		/* Asked for, so it is answered -- in a run, like any other. */
		feeds_service_record_failure(self, record, trigger, freeze_error->message);
		return TRUE;
	}

	venture_series_worker_sync(feeds_service_worker(self), spec, trigger);

	return TRUE;
}

typedef struct
{
	gboolean		 done;
	VentureFeedBatch	*batch;
	GError			*error;
} FeedsTestWait;

static void
feeds_test_done(
	GObject		*object,
	GAsyncResult	*result,
	gpointer	 data
){
	FeedsTestWait *wait = data;

	wait->batch = venture_data_source_provider_fetch_finish(VENTURE_DATA_SOURCE_PROVIDER(object),
	                                                        result, &wait->error);
	wait->done = TRUE;
}

static gboolean
feeds_test_deadline(gpointer data)
{
	g_cancellable_cancel(G_CANCELLABLE(data));

	return G_SOURCE_REMOVE;
}

gchar *
venture_feeds_service_test(
	VentureFeedsService	 *self,
	gint64			  data_source_id,
	const gchar		 *unit,
	GError			**error
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureFeedSource) spec = NULL;
	g_autoptr(GMainContext) context = NULL;
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(VentureFeedRequest) request = NULL;
	g_autoptr(GCancellable) cancellable = NULL;
	g_autoptr(JsonObject) settings = NULL;
	g_autoptr(JsonObject) secrets = NULL;
	g_autofree gchar *summary = NULL;
	g_autofree gchar *report = NULL;
	FeedsTestWait wait;
	GSource *deadline;
	const gchar *chosen;

	g_return_val_if_fail(VENTURE_IS_FEEDS_SERVICE(self), NULL);

	record = feeds_get_source(self, data_source_id, error);

	if (NULL == record)
		return NULL;

	spec = feeds_freeze(self->context, record, error);

	if (NULL == spec)
		return NULL;

	chosen = (NULL != unit) ? unit : spec->units[0];

	if (!g_strv_contains((const gchar *const *)spec->units, chosen))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "The source has no unit called %s", chosen);
		return NULL;
	}

	/*
	 * A private context, a session made on it, and the fetch iterated on
	 * it alone: nothing else of the main loop's -- an HTTP request, an
	 * automation -- runs nested inside a test fetch.
	 */
	memset(&wait, 0, sizeof(wait));
	context = g_main_context_new();
	g_main_context_push_thread_default(context);
	session = venture_feeds_session_new();
	cancellable = g_cancellable_new();
	settings = venture_feeds_parse_settings(NULL, NULL);
	g_clear_pointer(&settings, json_object_unref);
	{
		g_autoptr(JsonNode) node = json_from_string(spec->settings_json, NULL);
		g_autoptr(JsonNode) secret_node = json_from_string(spec->secrets_json, NULL);

		settings = json_object_ref(json_node_get_object(node));
		secrets = json_object_ref(json_node_get_object(secret_node));
	}

	request = venture_feed_request_new_internal(spec, chosen, settings, secrets, session, NULL,
	                                            NULL, VENTURE_SERIES_NONE,
	                                            g_get_real_time() / G_USEC_PER_SEC, cancellable);

	/* A bound past the fetch's own deadline: an exec program has its own
	 * timeout, a file has none. */
	deadline = g_timeout_source_new_seconds(spec->request_timeout + 5);
	g_source_set_callback(deadline, feeds_test_deadline, g_object_ref(cancellable), g_object_unref);
	g_source_attach(deadline, context);

	venture_data_source_provider_fetch_async(spec->provider, request, cancellable,
	                                         feeds_test_done, &wait);

	while (!wait.done)
		g_main_context_iteration(context, TRUE);

	g_source_destroy(deadline);
	g_source_unref(deadline);
	g_clear_object(&request);
	g_clear_object(&session);

	while (g_main_context_iteration(context, FALSE))
		;

	g_main_context_pop_thread_default(context);

	if (NULL == wait.batch)
	{
		g_autofree gchar *message = venture_feed_source_redact(spec, wait.error->message);

		g_set_error(error, wait.error->domain, wait.error->code, "%s: %s", chosen, message);
		g_error_free(wait.error);
		return NULL;
	}

	summary = venture_feed_batch_describe(wait.batch);
	report = g_strdup_printf("%s: %s%s%s", chosen, summary,
	                         (NULL != wait.batch->error) ? "; then: " : "",
	                         (NULL != wait.batch->error) ? wait.batch->error : "");
	venture_feed_batch_unref(wait.batch);

	return venture_feed_source_redact(spec, report);
}

VentureFeedRequest *
venture_feeds_service_open_request(
	VentureFeedsService	 *self,
	gint64			  data_source_id,
	const gchar		 *unit,
	GError			**error
){
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureFeedSource) spec = NULL;
	g_autoptr(JsonNode) settings = NULL;
	g_autoptr(JsonNode) secrets = NULL;
	const gchar *chosen;

	g_return_val_if_fail(VENTURE_IS_FEEDS_SERVICE(self), NULL);

	record = feeds_get_source(self, data_source_id, error);

	if (NULL == record)
		return NULL;

	spec = feeds_freeze(self->context, record, error);

	if (NULL == spec)
		return NULL;

	chosen = (NULL != unit) ? unit : spec->units[0];

	if (!g_strv_contains((const gchar *const *)spec->units, chosen))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "The source has no unit called %s", chosen);
		return NULL;
	}

	settings = json_from_string(spec->settings_json, NULL);
	secrets = json_from_string(spec->secrets_json, NULL);

	/* No session: the blocking helper makes one on a private context for
	 * each call, so nothing of the main loop's runs nested inside it. No
	 * quota either -- the budget belongs to the worker's runs. */
	return venture_feed_request_new_internal(spec, chosen, json_node_get_object(settings),
	                                         json_node_get_object(secrets), NULL, NULL, NULL,
	                                         VENTURE_SERIES_NONE,
	                                         g_get_real_time() / G_USEC_PER_SEC, NULL);
}

gboolean
venture_feeds_service_purge_history(
	VentureFeedsService	 *self,
	gint64			  data_source_id,
	GError			**error
){
	g_autoptr(VentureEntity) record = NULL;
	g_autofree gchar *store_dir = NULL;

	g_return_val_if_fail(VENTURE_IS_FEEDS_SERVICE(self), FALSE);

	record = feeds_get_source(self, data_source_id, error);

	if (NULL == record)
		return FALSE;

	store_dir = venture_feeds_store_dir(venture_context_get_config(self->context),
	                                    venture_entity_get_uuid(record));

	/* The thread that holds the writer deletes it; with no worker running
	 * nobody holds it, and it can go now. */
	if (NULL != self->worker)
	{
		venture_series_worker_purge(self->worker, data_source_id, store_dir);
		return TRUE;
	}

	return venture_series_worker_delete_store(store_dir, error);
}

JsonNode *
venture_feeds_service_dup_status(VentureFeedsService *self)
{
	g_return_val_if_fail(VENTURE_IS_FEEDS_SERVICE(self), NULL);

	if (NULL == self->worker)
		return json_from_string("{\"sources\":[]}", NULL);

	return venture_series_worker_dup_status(self->worker);
}

guint
venture_feeds_service_count_pending(VentureFeedsService *self)
{
	g_return_val_if_fail(VENTURE_IS_FEEDS_SERVICE(self), 0);

	return g_queue_get_length(&self->waiting) +
	       ((NULL != self->worker) ? venture_series_worker_count_live(self->worker) : 0);
}

VentureSeriesStore *
venture_feeds_service_open_reader(
	VentureFeedsService	 *self,
	gint64			  data_source_id,
	GError			**error
){
	g_autoptr(VentureEntity) record = NULL;
	g_autofree gchar *store_dir = NULL;

	g_return_val_if_fail(VENTURE_IS_FEEDS_SERVICE(self), NULL);

	record = feeds_get_source(self, data_source_id, error);

	if (NULL == record)
		return NULL;

	store_dir = venture_feeds_store_dir(venture_context_get_config(self->context),
	                                    venture_entity_get_uuid(record));

	return venture_series_store_open_reader(store_dir, error);
}

static VentureFeedsService *
feeds_service_new(VentureContext *context)
{
	VentureFeedsService *self;

	self = g_object_new(VENTURE_TYPE_FEEDS_SERVICE, NULL);
	self->context = context;

	return self;
}

/* --- The context's half --------------------------------------------------------------- */

/*
 * Kept on the context as object data rather than in its struct, so the
 * context builds the same with or without SQLite. The service is dropped
 * when feeds go off and made again when they come back; the provider
 * registry stays, because plugins registered into it at load.
 */
#define FEEDS_SERVICE_KEY "venture-feeds-service"
#define FEEDS_PROVIDERS_KEY "venture-feeds-providers"

static VentureFeedsService *
feeds_service_slot(VentureContext *context)
{
	return g_object_get_data(G_OBJECT(context), FEEDS_SERVICE_KEY);
}

VentureFeedsService *
venture_context_get_feeds_service(VentureContext *self)
{
	VentureFeedsService *service;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	if (!venture_context_module_enabled(self, "feeds"))
		return NULL;

	service = feeds_service_slot(self);

	if (NULL == service)
	{
		service = feeds_service_new(self);
		g_object_set_data_full(G_OBJECT(self), FEEDS_SERVICE_KEY, service, g_object_unref);

		/* A source that runs on its own starts the worker now; a manual
		 * one waits for its first sync. */
		venture_feeds_service_refresh(service);
	}

	return service;
}

VentureDataSourceProviderRegistry *
venture_context_get_data_source_providers(VentureContext *self)
{
	VentureDataSourceProviderRegistry *registry;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	registry = g_object_get_data(G_OBJECT(self), FEEDS_PROVIDERS_KEY);

	if (NULL == registry)
	{
		registry = venture_data_source_provider_registry_new();
		venture_feeds_register_builtin_providers(registry);
		g_object_set_data_full(G_OBJECT(self), FEEDS_PROVIDERS_KEY, registry, g_object_unref);
	}

	return registry;
}

void
venture_feeds_shutdown(VentureContext *context)
{
	VentureFeedsService *service;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	service = feeds_service_slot(context);

	if (NULL != service)
	{
		venture_feeds_service_shutdown(service);
		g_object_set_data(G_OBJECT(context), FEEDS_SERVICE_KEY, NULL);
	}
}

/* --- Validation ------------------------------------------------------------------------ */

static gboolean
feeds_check_type(
	JsonNode	 *value,
	const gchar	 *type,
	const gchar	 *name,
	GError		**error
){
	gboolean ok;

	if (NULL == type)
		return TRUE;

	if (0 == g_strcmp0(type, "string"))
		ok = JSON_NODE_HOLDS_VALUE(value) && (G_TYPE_STRING == json_node_get_value_type(value));
	else if (0 == g_strcmp0(type, "integer"))
		ok = JSON_NODE_HOLDS_VALUE(value) && (G_TYPE_INT64 == json_node_get_value_type(value));
	else if (0 == g_strcmp0(type, "boolean"))
		ok = JSON_NODE_HOLDS_VALUE(value) && (G_TYPE_BOOLEAN == json_node_get_value_type(value));
	else if (0 == g_strcmp0(type, "array"))
		ok = JSON_NODE_HOLDS_ARRAY(value);
	else if (0 == g_strcmp0(type, "object"))
		ok = JSON_NODE_HOLDS_OBJECT(value);
	else
		ok = TRUE;

	if (!ok)
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "Setting %s must be %s %s", name,
		            ((0 == g_strcmp0(type, "integer")) || (0 == g_strcmp0(type, "array")) ||
		             (0 == g_strcmp0(type, "object"))) ? "an" : "a", type);

	return ok;
}

/*
 * The settings against the provider's schema: a credential written into
 * them is refused (it belongs in the sealed store, not in a record anybody
 * with read access sees), a required setting must be there, a declared
 * one must have its type. Unknown names pass: a provider may read more
 * than it declares.
 */
static gboolean
feeds_check_settings(
	VentureDataSourceProvider	 *provider,
	JsonObject			 *settings,
	GError				**error
){
	g_autoptr(JsonNode) schema = NULL;
	JsonObject *properties;
	JsonNode *required;
	JsonObjectIter iter;
	const gchar *name;
	JsonNode *value;

	schema = venture_data_source_provider_dup_settings_schema(provider);
	properties = json_object_get_object_member(json_node_get_object(schema), "properties");
	required = json_object_get_member(json_node_get_object(schema), "required");

	json_object_iter_init(&iter, settings);

	while (json_object_iter_next(&iter, &name, &value))
	{
		JsonNode *declared;
		JsonObject *property;

		declared = (NULL != properties) ? json_object_get_member(properties, name) : NULL;

		if ((NULL == declared) || !JSON_NODE_HOLDS_OBJECT(declared))
			continue;

		property = json_node_get_object(declared);

		if (venture_json_object_get_bool(property, "x-sensitive", FALSE))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "%s is a credential: set it on the source's credentials page, "
			            "where it is sealed, never in its settings", name);
			return FALSE;
		}

		if (JSON_NODE_HOLDS_NULL(value))
			continue;

		if (!feeds_check_type(value, venture_json_object_get_string(property, "type", NULL),
		                      name, error))
			return FALSE;
	}

	if ((NULL != required) && JSON_NODE_HOLDS_ARRAY(required))
	{
		JsonArray *list = json_node_get_array(required);
		guint i;

		for (i = 0; i < json_array_get_length(list); i++)
		{
			const gchar *wanted = json_array_get_string_element(list, i);
			JsonNode *declared = (NULL != properties)
				? json_object_get_member(properties, wanted) : NULL;

			/* A required credential is checked when it is used. */
			if ((NULL != declared) && JSON_NODE_HOLDS_OBJECT(declared) &&
			    venture_json_object_get_bool(json_node_get_object(declared), "x-sensitive", FALSE))
				continue;

			if (!json_object_has_member(settings, wanted))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				            "The %s provider needs the setting %s",
				            venture_data_source_provider_get_name(provider), wanted);
				return FALSE;
			}
		}
	}

	return TRUE;
}

static gboolean
feeds_check_text(
	const gchar	 *value,
	const gchar	 *field,
	GError		**error
){
	const gchar *p;

	if (NULL == value)
		return TRUE;

	if ((strlen(value) > VENTURE_SERIES_MAX_KEY_LENGTH) || !g_utf8_validate(value, -1, NULL))
	{
		venture_set_error_validation(error, field, "must be at most 512 bytes of text");
		return FALSE;
	}

	for (p = value; '\0' != *p; p++)
	{
		if (g_ascii_iscntrl(*p))
		{
			venture_set_error_validation(error, field, "cannot hold control characters");
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean
feeds_validate_source(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(JsonObject) settings = NULL;
	g_autoptr(VentureMoney) min_value = NULL;
	g_autofree gchar *provider_name = NULL;
	g_autofree gchar *settings_text = NULL;
	g_autofree gchar *schedule = NULL;
	g_autofree gchar *currency = NULL;
	g_autofree gchar *venue_namespace = NULL;
	g_autofree gchar *instrument_namespace = NULL;
	VentureDataSourceTrack track;

	(void)previous;
	(void)user_data;

	g_object_get(entity, "provider", &provider_name, "settings", &settings_text,
	             "schedule", &schedule, "currency", &currency, "track", &track,
	             "venue-namespace", &venue_namespace,
	             "instrument-namespace", &instrument_namespace, "min-value", &min_value, NULL);

	if (!venture_data_source_provider_name_is_valid(provider_name))
	{
		venture_set_error_validation(error, "provider",
		                             "must be a provider's name: lower case, digits and underscores");
		return FALSE;
	}

	if (!venture_string_is_empty(schedule) && (0 != g_strcmp0(schedule, "auto")) &&
	    (0 != g_strcmp0(schedule, "hourly")) && (0 != g_strcmp0(schedule, "manual")))
	{
		g_autoptr(GError) cron_error = NULL;

		if (!venture_report_pack_schedule_validate(schedule, &cron_error))
		{
			venture_set_error_validation(error, "schedule",
			                             "must be auto, hourly, manual or five cron fields "
			                             "(minute hour day month weekday)");
			return FALSE;
		}
	}

	if (!venture_string_is_empty(currency) && !venture_currency_is_valid(currency))
	{
		venture_set_error_validation(error, "currency", "is not a currency code");
		return FALSE;
	}

	if (!feeds_check_text(venue_namespace, "venue-namespace", error) ||
	    !feeds_check_text(instrument_namespace, "instrument-namespace", error))
		return FALSE;

	settings = venture_feeds_parse_settings(settings_text, error);

	if (NULL == settings)
		return FALSE;

	/*
	 * With marketdata on, the instrument records filed under the source
	 * are known too, so an empty list is a source that keeps exactly
	 * those; the source is made before any instrument can name it. With
	 * it off the list is all there is, and an empty one would keep
	 * nothing at all.
	 */
	if ((VENTURE_DATA_SOURCE_TRACK_KNOWN == track) &&
	    (G_TYPE_INVALID == venture_entity_registry_lookup(venture_entity_registry_get_default(),
	                                                      "instrument")))
	{
		g_auto(GStrv) known = feeds_string_list(settings, "instruments");

		if ((NULL == known) || (NULL == known[0]))
		{
			venture_set_error_validation(error, "track",
			                             "is known, so the settings must list the "
			                             "instruments to keep under instruments");
			return FALSE;
		}
	}

	{
		g_auto(GStrv) types = feeds_string_list(settings, "record_types");
		guint i;

		for (i = 0; (NULL != types) && (NULL != types[i]); i++)
		{
			if (NULL == feeds_record_type(types[i]))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				            "A feed cannot write %s records; it may write price_observation "
				            "and exchange_rate", types[i]);
				return FALSE;
			}
		}
	}

	/* The provider's own rules, when there is a context to find it in.
	 * A provider a plugin registers may not be loaded yet on this
	 * database's scratch copy; a context that has a registry refuses an
	 * unknown one outright. */
	context = feeds_context_for(database);

	if (NULL != context)
	{
		VentureDataSourceProvider *provider;
		g_auto(GStrv) units = NULL;

		provider = venture_data_source_provider_registry_lookup(
			venture_context_get_data_source_providers(context), provider_name);

		if (NULL == provider)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "No data source provider named %s is registered", provider_name);
			return FALSE;
		}

		if (!feeds_check_settings(provider, settings, error))
			return FALSE;

		units = venture_data_source_provider_list_units(provider, settings, error);

		if (NULL == units)
			return FALSE;
	}

	return TRUE;
}

/* --- Actions ------------------------------------------------------------------------ */

static VentureFeedsService *
feeds_action_service(
	VentureAction	 *action,
	GError		**error
){
	g_autoptr(VentureContext) context = NULL;
	VentureFeedsService *service;

	context = feeds_context_for(VENTURE_DATABASE(venture_action_get_data(action)));
	service = (NULL != context) ? venture_context_get_feeds_service(context) : NULL;

	if (NULL == service)
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Market data feeds are off (feeds.enabled)");

	return service;
}

static gboolean
feeds_action_allowed(
	VentureAction		 *action,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	(void)actor;

	if (venture_entity_is_deleted(entity))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		                    "A deleted data source does not run");
		return FALSE;
	}

	return NULL != feeds_action_service(action, error);
}

static VentureEntity *
feeds_action_sync(
	VentureAction		 *action,
	VentureEntity		 *entity,
	GHashTable		 *params,
	const VentureActor	 *actor,
	GError			**error
){
	VentureFeedsService *service;

	(void)params;
	(void)actor;

	service = feeds_action_service(action, error);

	if ((NULL == service) ||
	    !venture_feeds_service_sync(service, venture_entity_get_id(entity),
	                                VENTURE_DATA_SOURCE_RUN_TRIGGER_MANUAL, error))
		return NULL;

	/* Never waits: the run is a data_source_run when it ends. */
	g_object_set(entity, "result",
	             "Sync queued; the run is recorded as a data source run when it ends", NULL);

	return g_object_ref(entity);
}

static VentureEntity *
feeds_action_test(
	VentureAction		 *action,
	VentureEntity		 *entity,
	GHashTable		 *params,
	const VentureActor	 *actor,
	GError			**error
){
	g_autofree gchar *report = NULL;
	VentureFeedsService *service;
	JsonNode *unit;

	(void)actor;

	service = feeds_action_service(action, error);

	if (NULL == service)
		return NULL;

	unit = (NULL != params) ? g_hash_table_lookup(params, "unit") : NULL;
	report = venture_feeds_service_test(service, venture_entity_get_id(entity),
	                                    ((NULL != unit) && JSON_NODE_HOLDS_VALUE(unit))
	                                    ? json_node_get_string(unit) : NULL, error);

	if (NULL == report)
		return NULL;

	g_object_set(entity, "result", report, NULL);

	return g_object_ref(entity);
}

static VentureEntity *
feeds_action_purge(
	VentureAction		 *action,
	VentureEntity		 *entity,
	GHashTable		 *params,
	const VentureActor	 *actor,
	GError			**error
){
	VentureFeedsService *service;

	(void)params;
	(void)actor;

	service = feeds_action_service(action, error);

	if ((NULL == service) ||
	    !venture_feeds_service_purge_history(service, venture_entity_get_id(entity), error))
		return NULL;

	g_object_set(entity, "result",
	             "The source's stored history is deleted; its next run starts a new store", NULL);

	return g_object_ref(entity);
}

static void
feeds_register_actions(VentureDatabase *database)
{
	static const struct
	{
		const gchar		*name;
		const gchar		*label;
		const gchar		*description;
		VentureUserRole		 role;
		VentureActionInvoke	 invoke;
		gboolean		 with_unit;
	} actions[] = {
		{ "sync", "Sync now",
		  "Fetch every unit of the source now; the run is recorded when it ends",
		  VENTURE_USER_ROLE_EDITOR, feeds_action_sync, FALSE },
		{ "test", "Test",
		  "Fetch one unit and say what came back, writing nothing",
		  VENTURE_USER_ROLE_EDITOR, feeds_action_test, TRUE },
		{ "purge_history", "Delete stored history",
		  "Delete everything the source has stored; it cannot be fetched again",
		  VENTURE_USER_ROLE_ADMIN, feeds_action_purge, FALSE },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(actions); i++)
	{
		g_autoptr(GPtrArray) parameters = NULL;
		g_autoptr(VentureAction) action = NULL;
		g_autoptr(GError) error = NULL;

		parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);

		if (actions[i].with_unit)
		{
			VentureFieldSpec *field = venture_field_spec_new("unit", "Unit", VENTURE_FIELD_KIND_STRING);

			field->help = g_strdup("Which unit to fetch; the first when left empty");
			g_ptr_array_add(parameters, field);
		}

		/*
		 * Record actions on the source, judged in its organization. None
		 * stageable: a sync reaches outside, a test writes nothing to
		 * approve, and a purge cannot be taken back.
		 */
		action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
		                      "type-name", "data_source", "name", actions[i].name,
		                      "label", actions[i].label, "description", actions[i].description,
		                      "parameters", parameters, "stageable", FALSE,
		                      "roles", actions[i].role, NULL);

		if (!venture_action_registry_register(venture_database_get_action_registry(database),
		                                      action, feeds_action_allowed, actions[i].invoke,
		                                      database, NULL, &error))
			g_error("Feeds action registration: %s", error->message);
	}
}

/* --- Automation ------------------------------------------------------------------------ */

static gboolean
feeds_automation_sync(
	VentureContext	 *context,
	const gchar	 *name,
	GVariant	 *params,
	GVariant	**result,
	gpointer	  user_data,
	GError		**error
){
	VentureFeedsService *service;
	const gchar *argument;
	gint64 id;

	(void)name;
	(void)user_data;

	argument = venture_automation_argument(params, 0);

	if ((NULL == argument) ||
	    !g_ascii_string_to_signed(argument, 10, 1, G_MAXINT64, &id, NULL))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "feeds_sync takes a data source id: feeds_sync(\"12\")");
		return FALSE;
	}

	service = venture_context_get_feeds_service(context);

	if (NULL == service)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Market data feeds are off (feeds.enabled)");
		return FALSE;
	}

	/* Queues and returns: a rule never waits on somebody else's server. */
	if (!venture_feeds_service_sync(service, id, VENTURE_DATA_SOURCE_RUN_TRIGGER_AUTOMATION, error))
		return FALSE;

	if (NULL != result)
		*result = g_variant_ref_sink(venture_automation_result_new(1, "queued", NULL));

	return TRUE;
}

void
venture_feeds_register_automation(VentureAutomationHandlerRegistry *registry)
{
	g_autoptr(GError) error = NULL;

	if (!venture_automation_handler_registry_add(registry, "feeds_sync",
	                                             "Queue a market data source's sync",
	                                             feeds_automation_sync, NULL, NULL, &error))
	{
		g_debug("feeds: %s", error->message);
		g_clear_error(&error);
	}

	if (!venture_automation_handler_registry_add_event(registry, "feed_synced",
	                                                   "A market data source's run ended", &error))
	{
		g_debug("feeds: %s", error->message);
		g_clear_error(&error);
	}

	if (!venture_automation_handler_registry_add_event(registry, "feed_failed",
	                                                   "A market data source's run failed", &error))
	{
		g_debug("feeds: %s", error->message);
		g_clear_error(&error);
	}
}

/* --- Installation ------------------------------------------------------------------- */

static void
feeds_entity_changed(
	VentureContext	*context,
	VentureEntity	*entity
){
	VentureFeedsService *service;

	/* An instrument record may make a key known to a source that tracks
	 * only those; the refresh is coalesced, so promoting a thousand is one
	 * refreeze. */
	if (!VENTURE_IS_DATA_SOURCE(entity) && !VENTURE_IS_INTEGRATION_CONNECTION(entity) &&
	    !VENTURE_IS_INSTRUMENT(entity))
		return;

	service = venture_context_get_feeds_service(context);

	if (NULL != service)
		feeds_service_queue_refresh(service);
}

static void
feeds_on_saved(
	VentureDatabase	*database,
	VentureEntity	*entity,
	gboolean	 created,
	gpointer	 user_data
){
	(void)database;
	(void)created;

	feeds_entity_changed(VENTURE_CONTEXT(user_data), entity);
}

static void
feeds_on_deleted(
	VentureDatabase	*database,
	VentureEntity	*entity,
	gpointer	 user_data
){
	(void)database;

	feeds_entity_changed(VENTURE_CONTEXT(user_data), entity);
}

static void
feeds_on_config(
	GObject		*config,
	GParamSpec	*pspec,
	gpointer	 user_data
){
	VentureContext *context = user_data;
	VentureFeedsService *service;

	(void)config;

	if (!g_str_has_prefix(pspec->name, "feeds-") && !g_str_has_prefix(pspec->name, "series-") &&
	    (0 != g_strcmp0(pspec->name, "plugins-allow-exec")) && (0 != g_strcmp0(pspec->name, "state-dir")))
		return;

	service = feeds_service_slot(context);

	if (NULL != service)
		feeds_service_queue_refresh(service);
}

/*
 * Feeds switched off: the worker stops and the service goes; switched on:
 * a fresh service freezes what is there. Nothing runs while it is off.
 */
static void
feeds_on_modules(
	VentureModuleRegistry	*modules,
	gpointer		 user_data
){
	VentureContext *context = user_data;

	if (venture_module_registry_is_enabled(modules, "feeds"))
	{
		if (NULL != feeds_service_slot(context))
			feeds_service_queue_refresh(feeds_service_slot(context));
		return;
	}

	venture_feeds_shutdown(context);
}

void
venture_feeds_install(VentureContext *context)
{
	VentureDatabase *database;
	GWeakRef *ref;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The last context over a database is the one its validator and
	 * actions ask, as for declarative venture types. */
	ref = g_new0(GWeakRef, 1);
	g_weak_ref_init(ref, context);
	g_object_set_data_full(G_OBJECT(database), FEEDS_CONTEXT_KEY, ref, feeds_weak_ref_free);

	if (NULL == g_object_get_data(G_OBJECT(database), FEEDS_INSTALLED_KEY))
	{
		g_object_set_data(G_OBJECT(database), FEEDS_INSTALLED_KEY, GINT_TO_POINTER(1));
		venture_database_add_save_validator(database, VENTURE_TYPE_DATA_SOURCE,
		                                    feeds_validate_source, NULL, NULL);
		feeds_register_actions(database);
	}

	g_signal_connect_object(database, "entity-saved", G_CALLBACK(feeds_on_saved), context, 0);
	g_signal_connect_object(database, "entity-deleted", G_CALLBACK(feeds_on_deleted), context, 0);
	g_signal_connect_object(venture_context_get_config(context), "notify",
	                        G_CALLBACK(feeds_on_config), context, 0);
	g_signal_connect_object(venture_context_get_modules(context), "changed",
	                        G_CALLBACK(feeds_on_modules), context, 0);
}
