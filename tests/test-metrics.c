/*
 * test-metrics.c - GET /metrics: what it says, and who it says it to
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A real server on port 0, scraped the way Prometheus scrapes it. Two
 * things are pinned here. That the page parses -- every sample under a
 * family declared once, labels escaped, values numbers -- because a
 * scraper that cannot parse one line drops the whole scrape, and the
 * failure is a gap in a graph nobody connects with the edit. And that
 * the route is never public: it says what the install is doing, the
 * port sits behind a public tunnel, and the tunnel connects from
 * loopback.
 */

#include <venture.h>
#include <libsoup/soup.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#include "venture-test-util.h"

typedef struct
{
	gchar			*state_dir;
	gchar			*file_root;
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	guint16			 port;
	gint64			 org;
} Fixture;

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	fixture->state_dir = g_dir_make_tmp("venture-metrics-XXXXXX", &error);
	g_assert_no_error(error);
	fixture->file_root = g_build_filename(fixture->state_dir, "files", NULL);
	g_assert_cmpint(g_mkdir_with_parents(fixture->file_root, 0700), ==, 0);

	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0,
	             "security-require-auth", TRUE,
	             "metrics-access", (NULL != user_data) ? (const gchar *)user_data : "off",
	             "feeds-enabled", TRUE,
	             "feeds-file-roots", fixture->file_root,
	             "feeds-run-window-minutes", (gint64)0,
	             NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database,
	                                       venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config, fixture->database);
	fixture->org = venture_context_get_default_organization_id(fixture->context);

	/* With this context's modules applied, the feeds tables too. */
	g_assert_true(venture_database_migrate(fixture->database,
	                                       venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);
	fixture->port = venture_web_server_get_port(fixture->server);
	fixture->session = soup_session_new();
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	if (NULL != fixture->server)
		venture_web_server_stop(fixture->server);

	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}

	g_clear_pointer(&fixture->file_root, g_free);
}

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} Outcome;

static void
request_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	Outcome *outcome = user_data;

	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &outcome->error);
	outcome->done = TRUE;
}

/*
 * One request, driving the default context by hand: the server answers on
 * it. @header and @value add one request header when set.
 */
static guint
request(
	Fixture		 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *bearer,
	const gchar	 *header,
	const gchar	 *value,
	gchar		**out_body,
	gchar		**out_type
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	Outcome outcome = { FALSE, NULL, NULL };

	url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (NULL != bearer)
	{
		g_autofree gchar *authorization = g_strconcat("Bearer ", bearer, NULL);

		soup_message_headers_append(soup_message_get_request_headers(message), "Authorization",
		                            authorization);
	}

	if (NULL != header)
		soup_message_headers_append(soup_message_get_request_headers(message), header, value);

	/* A browser's Accept would get an error page; a scraper sends this. */
	soup_message_headers_append(soup_message_get_request_headers(message), "Accept", "text/plain");

	soup_session_send_and_read_async(fixture->session, message, G_PRIORITY_DEFAULT, NULL,
	                                 request_done, &outcome);

	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);

	if (NULL != outcome.error)
		g_error("%s %s: %s", method, path, outcome.error->message);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL), g_bytes_get_size(outcome.body));

	if (NULL != out_type)
		*out_type = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message),
		                                                  "Content-Type"));

	g_clear_pointer(&outcome.body, g_bytes_unref);

	return soup_message_get_status(message);
}

/* A token for nobody in particular, with @role and @scopes. */
static gchar *
mint_token(
	Fixture		*fixture,
	VentureUserRole	 role,
	const gchar	*scopes
){
	g_autoptr(VentureApiToken) token = NULL;
	g_autoptr(GError) error = NULL;
	gchar *secret;

	token = venture_api_token_new();
	g_object_set(token, "name", "scraper", "role", role, "scopes", scopes, NULL);
	secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(token), NULL, &error));
	g_assert_no_error(error);

	return secret;
}

/* --- Parsing the exposition ---------------------------------------------------------- */

typedef struct
{
	gchar	*name;
	gchar	*labels;	/* as written between the braces, "" for none */
	gdouble	 value;
} Sample;

static void
sample_free(gpointer data)
{
	Sample *sample = data;

	g_free(sample->name);
	g_free(sample->labels);
	g_free(sample);
}

/* The family a sample belongs to: a histogram's _bucket, _sum and _count
 * are its family's. */
static gchar *
family_of(
	const gchar	*name,
	GHashTable	*types
){
	static const gchar *const suffixes[] = { "_bucket", "_sum", "_count", NULL };
	guint i;

	for (i = 0; NULL != suffixes[i]; i++)
	{
		if (g_str_has_suffix(name, suffixes[i]))
		{
			g_autofree gchar *base = g_strndup(name, strlen(name) - strlen(suffixes[i]));

			if (0 == g_strcmp0(g_hash_table_lookup(types, base), "histogram"))
				return g_steal_pointer(&base);
		}
	}

	return g_strdup(name);
}

static gboolean
valid_name(const gchar *name)
{
	const gchar *p;

	if ((NULL == name) || !(g_ascii_isalpha(name[0]) || ('_' == name[0]) || (':' == name[0])))
		return FALSE;

	for (p = name; '\0' != *p; p++)
	{
		if (!(g_ascii_isalnum(*p) || ('_' == *p) || (':' == *p)))
			return FALSE;
	}

	return TRUE;
}

/*
 * Parses a scrape strictly, failing the test at the first line a
 * Prometheus parser would reject: a sample of a family with no TYPE
 * before it, a family declared twice or split by another, a label value
 * not quoted and escaped, a value that is not a number.
 */
static GPtrArray *
parse_exposition(const gchar *text)
{
	g_autoptr(GHashTable) types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autoptr(GHashTable) closed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_auto(GStrv) lines = NULL;
	g_autofree gchar *current = NULL;
	GPtrArray *samples;
	guint i;

	samples = g_ptr_array_new_with_free_func(sample_free);
	g_assert_true(g_str_has_suffix(text, "\n"));
	lines = g_strsplit(text, "\n", -1);

	for (i = 0; NULL != lines[i]; i++)
	{
		const gchar *line = lines[i];
		const gchar *p;
		g_autofree gchar *family = NULL;
		Sample *sample;
		gchar *end;

		if ('\0' == line[0])
			continue;

		if (g_str_has_prefix(line, "# HELP "))
		{
			g_auto(GStrv) words = g_strsplit(line + 7, " ", 2);

			g_assert_true(valid_name(words[0]));
			continue;
		}

		if (g_str_has_prefix(line, "# TYPE "))
		{
			g_auto(GStrv) words = g_strsplit(line + 7, " ", -1);

			g_assert_cmpuint(g_strv_length(words), ==, 2);
			g_assert_true(valid_name(words[0]));
			g_assert_true((0 == g_strcmp0(words[1], "counter")) || (0 == g_strcmp0(words[1], "gauge")) ||
			              (0 == g_strcmp0(words[1], "histogram")));

			if (NULL != g_hash_table_lookup(types, words[0]))
				g_error("family %s is declared twice", words[0]);

			if (NULL != current)
				g_hash_table_add(closed, g_strdup(current));

			g_hash_table_insert(types, g_strdup(words[0]), g_strdup(words[1]));
			g_free(current);
			current = g_strdup(words[0]);
			continue;
		}

		g_assert_false(g_str_has_prefix(line, "#"));

		sample = g_new0(Sample, 1);
		g_ptr_array_add(samples, sample);

		for (p = line; ('\0' != *p) && ('{' != *p) && (' ' != *p); p++)
			;

		sample->name = g_strndup(line, (gsize)(p - line));
		g_assert_true(valid_name(sample->name));

		if ('{' == *p)
		{
			const gchar *start = ++p;

			/* name="value" pairs, the value quoted with \\ \" \n escapes. */
			while ('}' != *p)
			{
				const gchar *label = p;

				while (('=' != *p) && ('\0' != *p))
					p++;

				{
					g_autofree gchar *label_name = g_strndup(label, (gsize)(p - label));

					g_assert_true(valid_name(label_name));
				}

				g_assert_cmpint(*p++, ==, '=');
				g_assert_cmpint(*p++, ==, '"');

				while ('"' != *p)
				{
					g_assert_cmpint(*p, !=, '\0');
					g_assert_cmpint(*p, !=, '\n');

					if ('\\' == *p)
					{
						p++;
						g_assert_true(('\\' == *p) || ('"' == *p) || ('n' == *p));
					}

					p++;
				}

				p++;

				if (',' == *p)
					p++;
				else
					g_assert_cmpint(*p, ==, '}');
			}

			sample->labels = g_strndup(start, (gsize)(p - start));
			p++;
		}
		else
			sample->labels = g_strdup("");

		g_assert_cmpint(*p++, ==, ' ');

		if (0 == g_strcmp0(p, "NaN"))
			sample->value = NAN;
		else if (0 == g_strcmp0(p, "+Inf"))
			sample->value = INFINITY;
		else
		{
			sample->value = g_ascii_strtod(p, &end);
			g_assert_true((end != p) && ('\0' == *end));
		}

		family = family_of(sample->name, types);

		if (NULL == g_hash_table_lookup(types, family))
			g_error("sample %s has no TYPE before it", sample->name);

		if (0 != g_strcmp0(family, current))
			g_error("sample %s is not under its own family (%s is open)", sample->name, current);

		g_assert_false(g_hash_table_contains(closed, family));
	}

	return samples;
}

/* The value of @name with exactly @labels, or NAN when it is absent. */
static gdouble
sample_value(
	GPtrArray	*samples,
	const gchar	*name,
	const gchar	*labels
){
	guint i;

	for (i = 0; i < samples->len; i++)
	{
		Sample *sample = g_ptr_array_index(samples, i);

		if ((0 == g_strcmp0(sample->name, name)) && (0 == g_strcmp0(sample->labels, labels)))
			return sample->value;
	}

	return NAN;
}

static gboolean
has_family(
	GPtrArray	*samples,
	const gchar	*name
){
	guint i;

	for (i = 0; i < samples->len; i++)
	{
		if (0 == g_strcmp0(((Sample *)g_ptr_array_index(samples, i))->name, name))
			return TRUE;
	}

	return FALSE;
}

static GPtrArray *
scrape(Fixture *fixture)
{
	g_autofree gchar *body = NULL;
	g_autofree gchar *type = NULL;

	g_assert_cmpuint(request(fixture, "GET", "/metrics", NULL, NULL, NULL, &body, &type), ==, 200);
	g_assert_cmpstr(type, ==, VENTURE_METRICS_CONTENT_TYPE);

	return parse_exposition(body);
}

/* --- Who may read it ----------------------------------------------------------------- */

/*
 * Off unless asked for: a 404 to everybody, token or not.
 *
 * What breaks if this regresses: an install that never configured metrics
 * hands its request rates and every venue's freshness to whoever finds
 * the path behind the public tunnel.
 */
static void
test_metrics_off_by_default(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *owner = NULL;

	(void)user_data;

	owner = mint_token(fixture, VENTURE_USER_ROLE_OWNER, "metrics");
	g_assert_cmpuint(request(fixture, "GET", "/metrics", NULL, NULL, NULL, NULL, NULL), ==, 404);
	g_assert_cmpuint(request(fixture, "GET", "/metrics", owner, NULL, NULL, NULL, NULL), ==, 404);
}

/*
 * With the token rule: anonymous is 401, a token without the scope 403,
 * the scope or the owner 200 -- and coming from loopback buys nothing.
 *
 * What breaks if this regresses: any viewer token (handed to a dashboard,
 * a script) reads the install's activity, or the scraper's own token is
 * refused and the graphs go blank.
 */
static void
test_metrics_token(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *scoped = NULL;
	g_autofree gchar *unscoped = NULL;
	g_autofree gchar *owner = NULL;
	g_autoptr(GPtrArray) samples = NULL;
	g_autofree gchar *body = NULL;

	(void)user_data;

	scoped = mint_token(fixture, VENTURE_USER_ROLE_VIEWER, "reports, metrics");
	unscoped = mint_token(fixture, VENTURE_USER_ROLE_ADMIN, "reports,metricsx");
	owner = mint_token(fixture, VENTURE_USER_ROLE_OWNER, NULL);

	g_assert_cmpuint(request(fixture, "GET", "/metrics", NULL, NULL, NULL, NULL, NULL), ==, 401);
	g_assert_cmpuint(request(fixture, "GET", "/metrics", "vk_not-a-token", NULL, NULL, NULL, NULL), ==, 401);
	g_assert_cmpuint(request(fixture, "GET", "/metrics", unscoped, NULL, NULL, NULL, NULL), ==, 403);
	g_assert_cmpuint(request(fixture, "GET", "/metrics", scoped, NULL, NULL, &body, NULL), ==, 200);
	samples = parse_exposition(body);
	g_assert_true(has_family(samples, "venture_build_info"));
	g_assert_cmpuint(request(fixture, "GET", "/metrics", owner, NULL, NULL, NULL, NULL), ==, 200);
}

/*
 * With the loopback rule: this machine, straight, and nothing that a
 * proxy forwarded. cloudflared connects from loopback and adds its
 * headers, so a forwarded request is a stranger's.
 *
 * What breaks if this regresses: "loopback only" becomes "anyone the
 * tunnel lets in".
 */
static void
test_metrics_loopback(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	static const gchar *const headers[] = {
		"X-Forwarded-For", "Forwarded", "CF-Connecting-IP", "X-Real-IP", NULL
	};
	guint i;

	(void)user_data;

	g_assert_cmpuint(request(fixture, "GET", "/metrics", NULL, NULL, NULL, NULL, NULL), ==, 200);

	for (i = 0; NULL != headers[i]; i++)
		g_assert_cmpuint(request(fixture, "GET", "/metrics", NULL, headers[i], "203.0.113.9",
		                         NULL, NULL), ==, 404);
}

/*
 * loopback_or_token: loopback needs nothing, a forwarded request needs
 * the token.
 */
static void
test_metrics_loopback_or_token(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *scoped = NULL;

	(void)user_data;

	scoped = mint_token(fixture, VENTURE_USER_ROLE_VIEWER, "metrics");
	g_assert_cmpuint(request(fixture, "GET", "/metrics", NULL, NULL, NULL, NULL, NULL), ==, 200);
	g_assert_cmpuint(request(fixture, "GET", "/metrics", NULL, "X-Forwarded-For", "203.0.113.9",
	                         NULL, NULL), ==, 401);
	g_assert_cmpuint(request(fixture, "GET", "/metrics", scoped, "X-Forwarded-For", "203.0.113.9",
	                         NULL, NULL), ==, 200);
}

/* A value the configuration does not know is refused at startup, not read
 * as off -- or as on. */
static void
test_metrics_access_is_validated(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(GError) error = NULL;

	g_object_set(config, "metrics-access", "public", NULL);
	g_assert_false(venture_config_validate(config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "metrics.access"));
	g_clear_error(&error);

	/* What a YAML file makes of "off": still off, and still valid. */
	g_object_set(config, "metrics-access", "false", NULL);
	g_assert_true(venture_config_validate(config, &error));
	g_assert_no_error(error);
}

/* --- What it says -------------------------------------------------------------------- */

/*
 * The page parses, and carries the process, the build and the requests
 * it has answered, by class and status, with their latency.
 *
 * What breaks if this regresses: one malformed line and Prometheus drops
 * the whole scrape -- every graph goes blank at once.
 */
static void
test_metrics_format(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GPtrArray) samples = NULL;
	g_autofree gchar *version = NULL;
	gdouble health;
	gdouble bucket;
	guint i;

	(void)user_data;

	for (i = 0; i < 3; i++)
		g_assert_cmpuint(request(fixture, "GET", "/api/v1/health", NULL, NULL, NULL, NULL, NULL), ==, 200);

	g_assert_cmpuint(request(fixture, "GET", "/api/v1/feeds", NULL, NULL, NULL, NULL, NULL), ==, 401);

	samples = scrape(fixture);

	version = g_strdup_printf("version=\"%s\"", venture_get_version_string());
	g_assert_cmpfloat(sample_value(samples, "venture_build_info", version), ==, 1);
	g_assert_cmpfloat(sample_value(samples, "venture_uptime_seconds", ""), >=, 0);
	g_assert_cmpfloat(sample_value(samples, "process_start_time_seconds", ""), >, 1700000000);
	g_assert_cmpfloat(sample_value(samples, "process_resident_memory_bytes", ""), >, 0);
	g_assert_cmpfloat(sample_value(samples, "process_open_fds", ""), >, 0);

	health = sample_value(samples, "venture_http_requests_total", "class=\"health\",status=\"200\"");
	g_assert_cmpfloat(health, ==, 3);
	g_assert_cmpfloat(sample_value(samples, "venture_http_requests_total", "class=\"api\",status=\"401\""),
	                  ==, 1);

	/* The histogram is cumulative and ends at +Inf with the count. */
	bucket = sample_value(samples, "venture_http_request_duration_seconds_bucket",
	                      "class=\"health\",le=\"+Inf\"");
	g_assert_cmpfloat(bucket, ==, 3);
	g_assert_cmpfloat(sample_value(samples, "venture_http_request_duration_seconds_count", "class=\"health\""),
	                  ==, 3);
	g_assert_cmpfloat(sample_value(samples, "venture_http_request_duration_seconds_bucket",
	                               "class=\"health\",le=\"0.005\""), <=, bucket);
	g_assert_cmpfloat(sample_value(samples, "venture_http_request_duration_seconds_sum", "class=\"health\""),
	                  >=, 0);

	/* The scrape counts itself on the next one. */
	g_clear_pointer(&samples, g_ptr_array_unref);
	samples = scrape(fixture);
	g_assert_cmpfloat(sample_value(samples, "venture_http_requests_total", "class=\"metrics\",status=\"200\""),
	                  >=, 1);
}

/* The registry's own rules: labels escaped, counters never go down, and a
 * family is described once however often it is asked for. */
static void
test_metrics_registry(void)
{
	g_autoptr(VentureMetrics) metrics = venture_metrics_new();
	g_autoptr(GPtrArray) samples = NULL;
	g_autofree gchar *labels = NULL;
	g_autofree gchar *text = NULL;

	labels = venture_metrics_labels("name", "Argent \"Dawn\"\\EU\nwest", NULL);
	g_assert_cmpstr(labels, ==, "name=\"Argent \\\"Dawn\\\"\\\\EU\\nwest\"");

	venture_metrics_describe(metrics, "venture_test_total", VENTURE_METRICS_COUNTER, "A test");
	venture_metrics_describe(metrics, "venture_test_total", VENTURE_METRICS_GAUGE, "Ignored");
	venture_metrics_add(metrics, "venture_test_total", labels, 2);
	venture_metrics_add(metrics, "venture_test_total", labels, -5);
	venture_metrics_add(metrics, "venture_test_total", labels, 0.5);
	g_assert_cmpfloat(venture_metrics_get(metrics, "venture_test_total", labels), ==, 2.5);

	venture_metrics_describe(metrics, "venture_test_bytes", VENTURE_METRICS_GAUGE, "Big");
	venture_metrics_set(metrics, "venture_test_bytes", NULL, 4540633088.0);

	text = venture_metrics_render(metrics);
	g_assert_nonnull(strstr(text, "venture_test_bytes 4540633088\n"));
	samples = parse_exposition(text);
	g_assert_cmpfloat(sample_value(samples, "venture_test_total", labels), ==, 2.5);
}

/* Counters are added to from other threads: the feeds worker's
 * neighbours, the mailer. Nothing is lost. */
static gpointer
add_thousand(gpointer data)
{
	guint i;

	for (i = 0; i < 1000; i++)
		venture_metrics_add(data, "venture_test_total", "t=\"x\"", 1);

	return NULL;
}

static void
test_metrics_threads(void)
{
	g_autoptr(VentureMetrics) metrics = venture_metrics_new();
	GThread *threads[8];
	guint i;

	venture_metrics_describe(metrics, "venture_test_total", VENTURE_METRICS_COUNTER, "A test");

	for (i = 0; i < G_N_ELEMENTS(threads); i++)
		threads[i] = g_thread_new("metrics", add_thousand, metrics);

	for (i = 0; i < G_N_ELEMENTS(threads); i++)
		g_thread_join(threads[i]);

	g_assert_cmpfloat(venture_metrics_get(metrics, "venture_test_total", "t=\"x\""), ==, 8000);
}

#ifdef VENTURE_HAVE_SQLITE

static void
settle(Fixture *fixture)
{
	VentureFeedsService *service = venture_context_get_feeds_service(fixture->context);
	gint64 deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;

	while ((NULL != service) && (venture_feeds_service_count_pending(service) > 0))
	{
		if (g_get_monotonic_time() > deadline)
			g_error("the feeds worker did not settle within 30 seconds");

		if (!g_main_context_iteration(NULL, FALSE))
			g_usleep(2000);
	}

	while (g_main_context_iteration(NULL, FALSE))
		;
}

/*
 * A feed's families: runs by status, rows, the newest success, the store
 * on disk, each venue's age and the last upkeep -- labelled by source and
 * venue, never by instrument.
 *
 * What breaks if this regresses: the one question the page exists to
 * answer -- is the auction data still arriving, realm by realm -- has no
 * series to alert on.
 */
static void
test_metrics_feeds(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureDataSource) source = NULL;
	g_autoptr(GPtrArray) samples = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *by_source = NULL;
	g_autofree gchar *ok = NULL;
	g_autofree gchar *venue = NULL;
	g_autofree gchar *deleted = NULL;
	guint i;

	(void)user_data;

	path = g_build_filename(fixture->file_root, "realm.jsonl", NULL);
	g_assert_true(g_file_set_contents(path,
		"{\"type\":\"venue\",\"key\":\"argent\",\"name\":\"Argent \\\"Dawn\\\"\",\"currency\":\"GOLD\"}\n"
		"{\"type\":\"instrument\",\"key\":\"ore\",\"name\":\"Iron ore\"}\n"
		"{\"type\":\"snapshot\",\"venue\":\"argent\",\"taken_at\":\"2026-10-03T12:00:00Z\",\"complete\":true}\n"
		"{\"type\":\"listing\",\"venue\":\"argent\",\"instrument\":\"ore\",\"price\":\"1.25\",\"quantity\":20,\"id\":\"a1\"}\n",
		-1, NULL));

	source = venture_data_source_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(source), fixture->org);
	g_object_set(source, "name", "Lines", "provider", "file_jsonl", "settings", "file: realm.jsonl\n",
	             "schedule", "manual", "currency", "GOLD", NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(source), NULL, &error));
	g_assert_no_error(error);
	id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(VENTURE_ENTITY(source)));

	g_assert_true(venture_feeds_service_sync(venture_context_get_feeds_service(fixture->context),
	                                         venture_entity_get_id(VENTURE_ENTITY(source)),
	                                         VENTURE_DATA_SOURCE_RUN_TRIGGER_MANUAL, &error));
	settle(fixture);

	/* A pass's first finish runs the day's upkeep; ask for another and
	 * wait for it too, so its result is certainly kept. */
	g_assert_true(venture_feeds_service_upkeep(venture_context_get_feeds_service(fixture->context),
	                                           venture_entity_get_id(VENTURE_ENTITY(source)), FALSE,
	                                           &error));
	g_assert_no_error(error);
	settle(fixture);

	samples = scrape(fixture);
	by_source = g_strdup_printf("source=\"%s\"", id);
	ok = g_strdup_printf("source=\"%s\",status=\"ok\"", id);
	venue = g_strdup_printf("source=\"%s\",venue=\"argent\",name=\"Argent \\\"Dawn\\\"\"", id);
	deleted = g_strdup_printf("source=\"%s\",table=\"hourly\"", id);

	g_assert_cmpfloat(sample_value(samples, "venture_feed_runs_total", ok), ==, 1);
	g_assert_cmpfloat(sample_value(samples, "venture_feed_rows_total", by_source), >, 0);
	g_assert_cmpfloat(sample_value(samples, "venture_feed_units_total", by_source), ==, 1);
	g_assert_cmpfloat(sample_value(samples, "venture_feed_last_run_status", ok), ==, 1);
	g_assert_cmpfloat(sample_value(samples, "venture_feed_last_success_timestamp_seconds", by_source), >, 0);
	g_assert_cmpfloat(sample_value(samples, "venture_feed_last_run_duration_seconds", by_source), >=, 0);
	g_assert_cmpfloat(sample_value(samples, "venture_series_store_file_bytes", by_source), >, 0);
	g_assert_cmpfloat(sample_value(samples, "venture_series_store_wal_bytes", by_source), >=, 0);
	g_assert_cmpfloat(sample_value(samples, "venture_series_store_incremental_vacuum", by_source), ==, 1);
	g_assert_cmpfloat(sample_value(samples, "venture_feed_venue_last_snapshot_timestamp_seconds", venue),
	                  ==, 1791028800);
	g_assert_cmpfloat(sample_value(samples, "venture_feed_venue_snapshot_age_seconds", venue), >, 0);
	g_assert_cmpfloat(sample_value(samples, "venture_series_upkeep_last_timestamp_seconds", by_source), >, 0);
	g_assert_cmpfloat(sample_value(samples, "venture_series_upkeep_last_failed", by_source), ==, 0);
	g_assert_cmpfloat(sample_value(samples, "venture_series_upkeep_running", by_source), ==, 0);
	g_assert_false(isnan(sample_value(samples, "venture_series_upkeep_last_deleted_rows", deleted)));

	/* Nothing is labelled by instrument. */
	for (i = 0; i < samples->len; i++)
		g_assert_null(strstr(((Sample *)g_ptr_array_index(samples, i))->labels, "ore"));
}

#endif /* VENTURE_HAVE_SQLITE */

gint
main(
	gint	 argc,
	gchar	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add("/metrics/off-by-default", Fixture, NULL, fixture_set_up,
	           test_metrics_off_by_default, fixture_tear_down);
	g_test_add("/metrics/token", Fixture, "token", fixture_set_up, test_metrics_token, fixture_tear_down);
	g_test_add("/metrics/loopback", Fixture, "loopback", fixture_set_up, test_metrics_loopback,
	           fixture_tear_down);
	g_test_add("/metrics/loopback-or-token", Fixture, "loopback_or_token", fixture_set_up,
	           test_metrics_loopback_or_token, fixture_tear_down);
	g_test_add_func("/metrics/access-is-validated", test_metrics_access_is_validated);
	g_test_add("/metrics/format", Fixture, "loopback", fixture_set_up, test_metrics_format,
	           fixture_tear_down);
	g_test_add_func("/metrics/registry", test_metrics_registry);
	g_test_add_func("/metrics/threads", test_metrics_threads);
#ifdef VENTURE_HAVE_SQLITE
	g_test_add("/metrics/feeds", Fixture, "loopback", fixture_set_up, test_metrics_feeds,
	           fixture_tear_down);
#endif

	return g_test_run();
}
