/*
 * test-ticket-markdown.c - A ticket's words are markdown, and safe
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A ticket_comment body and a ticket description are written like a
 * comment, so the ticket page draws them through the renderer a comment
 * goes through: markdown becomes tags, everything a stranger types stays
 * text, an @mention is still a mention, a notification line is words
 * with no syntax, and the API still hands back the body as it was typed.
 */

#include <venture.h>

#include <string.h>
#include <libsoup/soup.h>

#include "venture-test-util.h"
#include "venture-test-member.h"
#include "venture-test-accounting.h"

typedef struct
{
	VentureConfig *config;
	VentureDatabase *database;
	VentureContext *context;
	VentureWebServer *server;
	gchar *state_dir;
	gint64 organization_id;
} Fixture;

static const gchar *const RICH_BODY =
	"## Plan\n"
	"\n"
	"- **bold** first\n"
	"- second\n"
	"\n"
	"Use `inline` here and:\n"
	"\n"
	"```\n"
	"make all\n"
	"```\n"
	"\n"
	"See [the docs](https://example.com/docs).";

static void
set_up(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	guint16 port;

	(void)data;

	f->config = venture_config_new();
	f->database = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	f->organization_id = venture_context_get_default_organization_id(f->context);

	f->state_dir = g_dir_make_tmp("venture-ticket-markdown-XXXXXX", &error);
	g_assert_no_error(error);
	port = 0;
	g_object_set(f->config, "state-dir", f->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)port,
	             "security-require-auth", FALSE, NULL);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
}

static void
tear_down(Fixture *f, gconstpointer data)
{
	(void)data;

	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->database);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->state_dir);
	g_free(f->state_dir);
}

typedef struct
{
	gboolean done;
	GBytes *bytes;
	GError *error;
} Reply;

static void
reply_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Reply *reply = data;

	reply->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                 result, &reply->error);
	reply->done = TRUE;
}

/* GETs @path; async on this thread's context, which the server shares. */
static gchar *
get(Fixture *f, const gchar *path)
{
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	Reply reply;
	gchar *body;

	memset(&reply, 0, sizeof(reply));
	session = soup_session_new_with_options("timeout", 15, NULL);
	url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	message = soup_message_new("GET", url);
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT,
	                                 NULL, reply_done, &reply);

	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(reply.error);
	g_assert_cmpuint(soup_message_get_status(message), ==, 200);
	body = g_strndup(g_bytes_get_data(reply.bytes, NULL),
	                 g_bytes_get_size(reply.bytes));
	g_bytes_unref(reply.bytes);

	return body;
}

static void
actor_for(VentureActor *actor, const gchar *username)
{
	actor->kind = VENTURE_ACTOR_KIND_USER;
	actor->name = username;
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

static VentureEntity *
make_ticket(
	Fixture		*f,
	const gchar	*description,
	const gchar	*assignee,
	const gchar	*by
){
	VentureTicket *ticket;
	VentureActor actor;

	if (NULL != assignee)
		venture_test_member(f->database, f->organization_id, assignee);

	if (NULL != by)
		venture_test_member(f->database, f->organization_id, by);

	actor_for(&actor, by);
	ticket = venture_ticket_new();
	g_object_set(ticket, "title", "Markdown ticket",
	             "description", description, "assignee", assignee, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(ticket),
	                                   f->organization_id);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(ticket),
	                                    (NULL != by) ? &actor : NULL, NULL));

	return VENTURE_ENTITY(ticket);
}

static void
comment_on(
	Fixture		*f,
	VentureEntity	*ticket,
	const gchar	*by,
	const gchar	*body
){
	g_autoptr(VentureTicketComment) comment = NULL;
	g_autoptr(GDateTime) now = NULL;
	VentureActor actor;

	venture_test_member(f->database, f->organization_id, by);
	actor_for(&actor, by);
	now = venture_time_now();
	comment = venture_ticket_comment_new();
	g_object_set(comment, "ticket-id", venture_entity_get_id(ticket),
	             "body", body, "author", by, "internal", FALSE,
	             "occurred-at", now, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(comment),
	                                   f->organization_id);
	g_assert_true(venture_database_save(f->database, VENTURE_ENTITY(comment),
	                                    &actor, NULL));
}

/*
 * The ticket page's page of one comment: the text of the first rendered
 * comment's element, from its opening tag to its closing div. Markdown
 * output holds no div, so the first </div> is the end of it.
 */
static gchar *
comment_region(const gchar *page)
{
	const gchar *start;
	const gchar *end;

	start = strstr(page, "<div class=\"timeline-text markdown\">");
	g_assert_nonnull(start);
	end = strstr(start, "</div>");
	g_assert_nonnull(end);

	return g_strndup(start, end - start);
}

/* The description card's content, the same way. */
static gchar *
description_region(const gchar *page)
{
	const gchar *start;
	const gchar *end;

	start = strstr(page, "<div class=\"card-body prose markdown\">");
	g_assert_nonnull(start);
	end = strstr(start, "</div>");
	g_assert_nonnull(end);

	return g_strndup(start, end - start);
}

static void
assert_rendered(const gchar *region)
{
	g_assert_nonnull(strstr(region, "<h"));
	g_assert_nonnull(strstr(region, "<ul>"));
	g_assert_nonnull(strstr(region, "<li>"));
	g_assert_nonnull(strstr(region, "<strong>bold</strong>"));
	g_assert_nonnull(strstr(region, "<code>inline</code>"));
	g_assert_nonnull(strstr(region, "<pre><code"));
	g_assert_nonnull(strstr(region, "make all"));
	g_assert_nonnull(strstr(region,
		"<a href=\"https://example.com/docs\""));
	g_assert_nonnull(strstr(region, "nofollow"));

	/* None of the syntax is left to read. */
	g_assert_null(strstr(region, "**"));
	g_assert_null(strstr(region, "```"));
	g_assert_null(strstr(region, "## "));
	g_assert_null(strstr(region, "\n- "));
	g_assert_null(strstr(region, "- "));
}

/*
 * A ticket comment with a heading, bullets, bold, inline code, a fenced
 * block and a link renders to the tags on the ticket page; a description
 * does too; and the composer says markdown works. If this regresses the
 * page shows "**bold**" again.
 */
static void
test_ticket_page_renders_markdown(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) ticket = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *comment = NULL;
	g_autofree gchar *description = NULL;

	(void)data;

	ticket = make_ticket(f, RICH_BODY, "ann", "zed");
	comment_on(f, ticket, "bob", RICH_BODY);

	path = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT,
	                       venture_entity_get_id(ticket));
	page = get(f, path);

	comment = comment_region(page);
	assert_rendered(comment);

	description = description_region(page);
	assert_rendered(description);

	g_assert_nonnull(strstr(page, "Markdown supported."));
}

/*
 * A stranger's HTML stays text: a script, an image with a handler and a
 * javascript: link are each escaped or dropped, in a comment and in a
 * description, and no live tag or javascript: href reaches the page.
 */
static void
test_ticket_page_is_safe(Fixture *f, gconstpointer data)
{
	static const gchar *const attack =
		"<script>alert(1)</script>\n"
		"\n"
		"<img src=x onerror=alert(1)>\n"
		"\n"
		"[x](javascript:alert(1)) ![p](https://evil.example/p.png)";
	g_autoptr(VentureEntity) ticket = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *comment = NULL;
	g_autofree gchar *description = NULL;
	const gchar *regions[2];
	guint i;

	(void)data;

	ticket = make_ticket(f, attack, "ann", "zed");
	comment_on(f, ticket, "bob", attack);

	path = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT,
	                       venture_entity_get_id(ticket));
	page = get(f, path);

	comment = comment_region(page);
	description = description_region(page);
	regions[0] = comment;
	regions[1] = description;

	for (i = 0; i < G_N_ELEMENTS(regions); i++)
	{
		g_assert_nonnull(strstr(regions[i], "&lt;script&gt;"));
		g_assert_null(strstr(regions[i], "<script"));
		g_assert_null(strstr(regions[i], "<img"));
		g_assert_null(strstr(regions[i], "javascript:alert(1)\""));
		g_assert_null(strstr(regions[i], "href=\"javascript"));
	}
}

/*
 * An @mention inside rendered markdown is still a mention: a chip for
 * somebody who may read the ticket, plain text for a name nobody has.
 */
static void
test_ticket_comment_mention_survives(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) ticket = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *comment = NULL;

	(void)data;

	ticket = make_ticket(f, NULL, "ann", "zed");
	venture_test_member(f->database, f->organization_id, "erin");
	comment_on(f, ticket, "bob", "**Please** look, @erin and @nobody.");

	path = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT,
	                       venture_entity_get_id(ticket));
	page = get(f, path);
	comment = comment_region(page);

	g_assert_nonnull(strstr(comment, "<strong>Please</strong>"));
	g_assert_nonnull(strstr(comment, "<span class=\"mention\""));
	g_assert_nonnull(strstr(comment, ">@erin</span>"));
	g_assert_null(strstr(comment, ">@nobody</span>"));
}

/*
 * The raw body stays the raw body: the API returns what was typed, so a
 * client that renders it itself, search and the AI read the same text.
 */
static void
test_ticket_comment_api_stays_raw(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) ticket = NULL;
	g_autofree gchar *json = NULL;

	(void)data;

	ticket = make_ticket(f, NULL, "ann", "zed");
	comment_on(f, ticket, "bob", "a **bold** word");

	json = get(f, "/api/v1/ticket_comment");
	g_assert_nonnull(strstr(json, "a **bold** word"));
}

/*
 * A notification line about a markdown comment is words: no asterisks,
 * no backticks, no tags, and the person is still told.
 */
static void
test_ticket_comment_preview_is_text(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) ticket = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gint64 ann;
	guint i;
	gboolean seen;

	(void)data;

	ann = venture_test_member(f->database, f->organization_id, "ann");
	ticket = make_ticket(f, NULL, "ann", "zed");
	comment_on(f, ticket, "bob", "**Rebooted** the `edge` box [twice](https://example.com/t)");

	rows = venture_notify_list(f->context, ann, TRUE, 0, NULL);
	g_assert_nonnull(rows);
	seen = FALSE;

	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *body = NULL;
		VentureNotificationKind kind;

		g_object_get(g_ptr_array_index(rows, i), "kind", &kind,
		             "body", &body, NULL);

		if ((VENTURE_NOTIFICATION_KIND_WATCHED != kind) || (NULL == body))
			continue;

		seen = TRUE;
		g_assert_nonnull(strstr(body, "Rebooted"));
		g_assert_null(strstr(body, "**"));
		g_assert_null(strstr(body, "`"));
		g_assert_null(strstr(body, "]("));
	}

	g_assert_true(seen);
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, set_up, func, tear_down)

	ADD("/ticket-markdown/page-renders", test_ticket_page_renders_markdown);
	ADD("/ticket-markdown/page-is-safe", test_ticket_page_is_safe);
	ADD("/ticket-markdown/mention-survives",
	    test_ticket_comment_mention_survives);
	ADD("/ticket-markdown/api-stays-raw", test_ticket_comment_api_stays_raw);
	ADD("/ticket-markdown/preview-is-text",
	    test_ticket_comment_preview_is_text);

#undef ADD

	return g_test_run();
}
