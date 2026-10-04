/*
 * test-comments.c - A discussion on any record
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every record type takes comments unless it opted out, and a comment is a
 * piece of its record: whoever may read the record may read and add to its
 * discussion, nobody else may learn it exists, only the author may change
 * the words, and the people it names are told only when they may read
 * what it is about. These tests pin those rules at the service, at the
 * access policy every door shares, and at the routes a browser, venturectl
 * and a script reach.
 */

#include <venture.h>

#include <string.h>
#include <libsoup/soup.h>

#include "venture-test-util.h"

typedef struct
{
	VentureConfig *config;
	VentureDatabase *database;
	VentureContext *context;
	VentureWebServer *server;
	gchar *state_dir;
	gint64 org;
	gint64 other_org;
	gint64 owner_id;
	gint64 editor_id;
	gint64 viewer_id;
	gint64 admin_id;
	gint64 outsider_id;
	gint64 editor_token_id;
	gchar *editor_token;
	gchar *outsider_token;
	gchar *admin_token;
	VentureEntity *company;		/* the viewer's own company */
	VentureEntity *other_company;	/* the organization's, not the viewer's */
	VentureEntity *foreign_company;	/* another organization's */
} Fixture;

/* The principals as their sessions present them. */
static gchar owner_name[] = "olive";
static gchar editor_name[] = "erin";
static gchar viewer_name[] = "vic";
static gchar admin_name[] = "ada";
static gchar outsider_name[] = "otto";

static void
principal_for(
	VentureAuthPrincipal	*principal,
	gint64			 user_id,
	VentureUserRole		 role,
	gchar			*name
){
	principal->user_id = user_id;
	principal->token_id = 0;
	principal->role = role;
	principal->name = name;
	principal->authenticated = TRUE;
}

static void
save(Fixture *f, VentureEntity *record)
{
	g_autoptr(GError) error = NULL;

	if (!venture_database_save(f->database, record, NULL, &error))
		g_error("save %s: %s", G_OBJECT_TYPE_NAME(record), error->message);
}

static gint64
make_user(
	Fixture		*f,
	const gchar	*username,
	const gchar	*display,
	VentureUserRole	 role
){
	g_autoptr(VentureEntity) user = NULL;

	user = g_object_new(VENTURE_TYPE_USER, "username", username,
	                    "display-name", display, "role", role, "active", TRUE,
	                    NULL);
	save(f, user);

	return venture_entity_get_id(user);
}

static void
make_member(
	Fixture			*f,
	gint64			 user_id,
	gint64			 org,
	VentureOrganizationRole	 role
){
	g_autoptr(VentureEntity) member = NULL;

	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP,
	                      "user-id", user_id, "organization-id", org,
	                      "role", role, "active", TRUE, NULL);
	save(f, member);
}

static gchar *
make_token(
	Fixture	*f,
	gint64	 user_id,
	gint64	*out_id
){
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;

	g_object_set(token, "name", "a name nobody should ever see",
	             "user-id", user_id, "role", VENTURE_USER_ROLE_EDITOR, NULL);
	secret = venture_api_token_generate(token);
	save(f, VENTURE_ENTITY(token));

	if (NULL != out_id)
		*out_id = venture_entity_get_id(VENTURE_ENTITY(token));

	return secret;
}

static VentureEntity *
make_company(
	Fixture		*f,
	const gchar	*name,
	gint64		 org,
	gint64		 owner_user_id
){
	VentureEntity *company;

	company = g_object_new(VENTURE_TYPE_COMPANY, "name", name,
	                       "owner-user-id", owner_user_id, NULL);
	venture_entity_set_organization_id(company, org);
	save(f, company);

	return company;
}

/*
 * One fixture for everything: a server with sign-in on (or off, for the
 * walk over every type), two organizations, and five people in them --
 * the install's owner, an editor, a viewer who owns one company, an
 * organization administrator, and somebody from the other organization.
 */
static void
fixture_set_up(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) other = NULL;
	gboolean open = (NULL != data);

	g_setenv("VENTURE_TEST_SESSION_SECRET", "test-secret-value", TRUE);
	f->state_dir = g_dir_make_tmp("venture-comments-XXXXXX", &error);
	g_assert_no_error(error);

	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)0,
	             "security-require-auth", !open,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
	             NULL);

	f->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->database);
	f->org = venture_context_get_default_organization_id(f->context);

	other = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Other Ltd",
	                     "slug", "other", NULL);
	save(f, other);
	f->other_org = venture_entity_get_id(other);

	f->owner_id = make_user(f, owner_name, "Olive Owner", VENTURE_USER_ROLE_OWNER);
	f->editor_id = make_user(f, editor_name, "Erin Edits", VENTURE_USER_ROLE_EDITOR);
	f->viewer_id = make_user(f, viewer_name, "Vic Views", VENTURE_USER_ROLE_VIEWER);
	f->admin_id = make_user(f, admin_name, "Ada Admin", VENTURE_USER_ROLE_EDITOR);
	f->outsider_id = make_user(f, outsider_name, "Otto Outside", VENTURE_USER_ROLE_EDITOR);

	make_member(f, f->owner_id, f->org, VENTURE_ORGANIZATION_ROLE_OWNER);
	make_member(f, f->editor_id, f->org, VENTURE_ORGANIZATION_ROLE_EDITOR);
	make_member(f, f->viewer_id, f->org, VENTURE_ORGANIZATION_ROLE_VIEWER);
	make_member(f, f->admin_id, f->org, VENTURE_ORGANIZATION_ROLE_ADMIN);
	make_member(f, f->outsider_id, f->other_org, VENTURE_ORGANIZATION_ROLE_EDITOR);

	f->editor_token = make_token(f, f->editor_id, &f->editor_token_id);
	f->outsider_token = make_token(f, f->outsider_id, NULL);
	f->admin_token = make_token(f, f->admin_id, NULL);

	f->company = make_company(f, "Bellhaven Books", f->org, f->viewer_id);
	f->other_company = make_company(f, "Harbor Paper", f->org, 0);
	f->foreign_company = make_company(f, "Outside Co", f->other_org, 0);

	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
}

static void
fixture_tear_down(Fixture *f, gconstpointer data)
{
	(void)data;

	venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	g_clear_object(&f->company);
	g_clear_object(&f->other_company);
	g_clear_object(&f->foreign_company);
	g_clear_object(&f->context);
	g_clear_object(&f->database);
	g_clear_object(&f->config);
	g_free(f->editor_token);
	g_free(f->outsider_token);
	g_free(f->admin_token);
	venture_test_remove_tree(f->state_dir);
	g_free(f->state_dir);
	g_unsetenv("VENTURE_TEST_SESSION_SECRET");
}

/* --- Speaking HTTP ---------------------------------------------------------- */

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

/*
 * One request, answered on this thread's main context, which is where the
 * server runs. Redirects are not followed, so a test sees where it was
 * sent.
 */
static guint
request(
	Fixture		 *f,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *token,
	const gchar	 *content_type,
	const gchar	 *body,
	gboolean	  discussion,
	gchar		**out_body,
	gchar		**out_location,
	gchar		**out_comment
){
	g_autoptr(SoupSession) session = NULL;
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	SoupMessageHeaders *headers;
	Reply reply;

	memset(&reply, 0, sizeof(reply));
	session = soup_session_new_with_options("timeout", 15, NULL);
	url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	headers = soup_message_get_request_headers(message);

	if (NULL != token)
	{
		g_autofree gchar *bearer = g_strconcat("Bearer ", token, NULL);

		soup_message_headers_replace(headers, "Authorization", bearer);
	}

	if (discussion)
	{
		soup_message_headers_replace(headers, "X-Venture-Discussion", "1");
		soup_message_headers_replace(headers, "X-Venture-Inline", "1");
	}

	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));

		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}

	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT,
	                                 NULL, reply_done, &reply);

	while (!reply.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(reply.error);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(reply.bytes, NULL),
		                      g_bytes_get_size(reply.bytes));

	g_bytes_unref(reply.bytes);

	if (NULL != out_location)
		*out_location = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Location"));

	if (NULL != out_comment)
		*out_comment = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "X-Venture-Comment"));

	return soup_message_get_status(message);
}

static guint
api(
	Fixture		 *f,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *token,
	const gchar	 *json,
	JsonNode	**out
){
	g_autofree gchar *body = NULL;
	guint status;

	status = request(f, method, path, token, "application/json", json, FALSE,
	                 &body, NULL, NULL);

	if (NULL != out)
		*out = venture_json_parse(body, NULL);

	return status;
}

/* --- Helpers over the service --------------------------------------------- */

static VentureEntity *
comment_as(
	Fixture				*f,
	const VentureAuthPrincipal	*who,
	VentureEntity			*subject,
	gint64				 parent,
	const gchar			*body
){
	g_autoptr(GError) error = NULL;
	VentureEntity *comment;

	comment = venture_comment_create(f->context, who,
		venture_entity_get_entity_name(subject), venture_entity_get_id(subject),
		parent, body, &error);
	g_assert_no_error(error);
	g_assert_nonnull(comment);

	return comment;
}

/* Every notification one person has, newest first. */
static GPtrArray *
inbox_of(Fixture *f, gint64 user_id)
{
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;

	rows = venture_notify_list(f->context, user_id, FALSE, 0, &error);
	g_assert_no_error(error);

	return rows;
}

static guint
count_kind(GPtrArray *rows, VentureNotificationKind wanted)
{
	guint i;
	guint n = 0;

	for (i = 0; i < rows->len; i++)
	{
		VentureNotificationKind kind;

		g_object_get(g_ptr_array_index(rows, i), "kind", &kind, NULL);

		if (kind == wanted)
			n++;
	}

	return n;
}

/* --- The model ------------------------------------------------------------- */

/*
 * Which types take comments is metadata on the type, not a list. If this
 * regresses, the audit log grows a discussion, a ticket has two
 * conversations, or a type added tomorrow takes none.
 */
static void
test_types(void)
{
	static GType (*const refused[])(void) = {
		venture_comment_get_type, venture_ticket_get_type,
		venture_ticket_comment_get_type, venture_ticket_relation_get_type,
		venture_audit_entry_get_type, venture_notification_get_type,
		venture_watch_get_type, venture_chat_thread_get_type,
		venture_chat_message_get_type, venture_webhook_delivery_get_type,
		venture_kb_chunk_get_type, venture_kb_link_get_type,
		venture_record_link_get_type, venture_ledger_entry_get_type,
		venture_data_source_run_get_type, NULL
	};
	static GType (*const accepted[])(void) = {
		venture_company_get_type, venture_contact_get_type,
		venture_invoice_get_type, venture_arbitrage_trade_get_type,
		venture_venture_get_type, venture_release_get_type,
		venture_product_get_type, venture_journal_get_type,
		venture_category_get_type, NULL
	};
	gsize i;

	for (i = 0; NULL != refused[i]; i++)
	{
		g_autoptr(GError) error = NULL;

		g_assert_false(venture_comment_type_accepts(refused[i](), &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	}

	for (i = 0; NULL != accepted[i]; i++)
		g_assert_true(venture_comment_type_accepts(accepted[i](), NULL));
}

/*
 * Anybody who may read a record may comment on it -- a viewer too, with
 * no proposal to approve -- and only the author may change the words. If
 * this regresses, either a viewer's question waits for an approver and
 * arrives under the approver's name, or somebody edits somebody else's
 * words and the thread lies about who said what.
 */
static void
test_permissions(Fixture *f, gconstpointer data)
{
	VentureAuthPrincipal owner;
	VentureAuthPrincipal editor;
	VentureAuthPrincipal viewer;
	VentureAuthPrincipal admin;
	VentureAuthPrincipal token;
	g_autoptr(VentureEntity) by_viewer = NULL;
	g_autoptr(VentureEntity) by_editor = NULL;
	g_autoptr(VentureEntity) edited = NULL;
	g_autoptr(VentureEntity) refused = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *author = NULL;
	gint64 author_user_id = 0;

	(void)data;

	principal_for(&owner, f->owner_id, VENTURE_USER_ROLE_OWNER, owner_name);
	principal_for(&editor, f->editor_id, VENTURE_USER_ROLE_EDITOR, editor_name);
	principal_for(&viewer, f->viewer_id, VENTURE_USER_ROLE_VIEWER, viewer_name);
	principal_for(&admin, f->admin_id, VENTURE_USER_ROLE_EDITOR, admin_name);

	/* A viewer comments on a record they may read, directly. */
	by_viewer = comment_as(f, &viewer, f->company, 0, "Can we chase this?");
	g_object_get(by_viewer, "author", &author, "author-user-id", &author_user_id, NULL);
	g_assert_cmpstr(author, ==, viewer_name);
	g_assert_cmpint(author_user_id, ==, f->viewer_id);
	g_assert_cmpint(venture_entity_get_organization_id(by_viewer), ==, f->org);

	by_editor = comment_as(f, &editor, f->company, 0, "On it.");

	/* The viewer may not change the editor's words; the editor may. */
	refused = venture_comment_edit(f->context, &viewer,
		venture_entity_get_id(by_editor), "Not on it.", &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);

	/* Nor may the install's owner: an owner may remove words, never put
	 * them in somebody's mouth. */
	refused = venture_comment_edit(f->context, &owner,
		venture_entity_get_id(by_editor), "Owner says so.", &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);

	edited = venture_comment_edit(f->context, &editor,
		venture_entity_get_id(by_editor), "On it, today.", &error);
	g_assert_no_error(error);
	g_assert_nonnull(edited);

	/* The viewer may not delete the editor's comment; an organization
	 * administrator may. */
	g_assert_false(venture_comment_delete(f->context, &viewer,
		venture_entity_get_id(by_editor), &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_true(venture_comment_delete(f->context, &admin,
		venture_entity_get_id(by_editor), &error));
	g_assert_no_error(error);

	/* The viewer deletes their own. */
	g_assert_true(venture_comment_delete(f->context, &viewer,
		venture_entity_get_id(by_viewer), &error));
	g_assert_no_error(error);

	/* A read-only token stays read-only. */
	principal_for(&token, f->editor_id, VENTURE_USER_ROLE_VIEWER, editor_name);
	token.token_id = f->editor_token_id;
	refused = venture_comment_create(f->context, &token, "company",
		venture_entity_get_id(f->company), 0, "From a read-only token", &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

/*
 * A record the caller may not read is not found -- to comment on, to read
 * the discussion of, or to open one comment of. If this regresses, a
 * comment route tells somebody from another organization that a record
 * exists, or hands them what was said about it.
 */
static void
test_unreadable(Fixture *f, gconstpointer data)
{
	VentureAuthPrincipal viewer;
	VentureAuthPrincipal outsider;
	VentureAuthPrincipal editor;
	g_autoptr(VentureEntity) inside = NULL;
	g_autoptr(VentureEntity) refused = NULL;
	g_autoptr(JsonNode) thread = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;

	principal_for(&viewer, f->viewer_id, VENTURE_USER_ROLE_VIEWER, viewer_name);
	principal_for(&outsider, f->outsider_id, VENTURE_USER_ROLE_EDITOR, outsider_name);
	principal_for(&editor, f->editor_id, VENTURE_USER_ROLE_EDITOR, editor_name);

	inside = comment_as(f, &editor, f->other_company, 0, "Internal only.");

	/* Another organization's member. */
	refused = venture_comment_create(f->context, &outsider, "company",
		venture_entity_get_id(f->other_company), 0, "Hello?", &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	thread = venture_comment_thread(f->context, &outsider, "company",
		venture_entity_get_id(f->other_company), &error);
	g_assert_null(thread);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	refused = venture_comment_get(f->context, &outsider,
		venture_entity_get_id(inside), &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* A viewer may read only what they own: the organization's other
	 * company is not theirs, and neither is its discussion. */
	refused = venture_comment_create(f->context, &viewer, "company",
		venture_entity_get_id(f->other_company), 0, "Me too", &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	refused = venture_comment_get(f->context, &viewer,
		venture_entity_get_id(inside), &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

/*
 * The validator holds every writer to the record: it must exist, must not
 * be deleted, must take comments, the comment lives in its organization,
 * a reply answers a comment on the same record and lands in its thread,
 * and who wrote it is whoever the write is made as. If this regresses, a
 * client can post a comment "by" somebody else, park one in an
 * organization that cannot see its record, or thread a reply under a
 * conversation about something else.
 */
static void
test_validator(Fixture *f, gconstpointer data)
{
	VentureAuthPrincipal editor;
	g_autoptr(VentureEntity) top = NULL;
	g_autoptr(VentureEntity) reply = NULL;
	g_autoptr(VentureEntity) deeper = NULL;
	g_autoptr(VentureEntity) elsewhere = NULL;
	g_autoptr(VentureEntity) gone = NULL;
	g_autoptr(VentureEntity) forged = NULL;
	g_autoptr(VentureEntity) refused = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) edited_at = NULL;
	g_autofree gchar *author = NULL;
	gint64 parent = 0;
	VentureActor actor;

	(void)data;

	principal_for(&editor, f->editor_id, VENTURE_USER_ROLE_EDITOR, editor_name);

	top = comment_as(f, &editor, f->company, 0, "Top.");
	reply = comment_as(f, &editor, f->company, venture_entity_get_id(top), "Reply.");

	/* An answer to an answer joins the thread it is in. */
	deeper = comment_as(f, &editor, f->company, venture_entity_get_id(reply),
	                    "Reply to the reply.");
	g_object_get(deeper, "parent-id", &parent, NULL);
	g_assert_cmpint(parent, ==, venture_entity_get_id(top));

	/* Never edited until the words change. */
	g_object_get(top, "edited-at", &edited_at, NULL);
	g_assert_null(edited_at);

	/* A reply names a comment on the same record. */
	elsewhere = comment_as(f, &editor, f->other_company, 0, "Elsewhere.");
	refused = venture_comment_create(f->context, &editor, "company",
		venture_entity_get_id(f->company), venture_entity_get_id(elsewhere),
		"Cross-thread", &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	/* Nothing to say, or too much. */
	refused = venture_comment_create(f->context, &editor, "company",
		venture_entity_get_id(f->company), 0, "   \n  ", &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);

	{
		g_autofree gchar *huge = g_strnfill(VENTURE_COMMENT_MAX_LENGTH + 1, 'x');

		refused = venture_comment_create(f->context, &editor, "company",
			venture_entity_get_id(f->company), 0, huge, &error);
		g_assert_null(refused);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
	}

	/* A type that takes no comments, and a record that is not there. */
	refused = venture_comment_create(f->context, &editor, "audit_entry", 1, 0,
	                                 "Hm.", &error);
	g_assert_null(refused);
	g_assert_nonnull(error);
	g_clear_error(&error);

	refused = venture_comment_create(f->context, &editor, "company", 9999, 0,
	                                 "Hm.", &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* A deleted record takes no new comment; its old ones stay editable. */
	gone = make_company(f, "Closed Co", f->org, 0);
	{
		g_autoptr(VentureEntity) before = comment_as(f, &editor, gone, 0, "Before.");
		g_autoptr(VentureEntity) after_edit = NULL;

		g_assert_true(venture_database_delete(f->database, gone, NULL, &error));
		g_assert_no_error(error);

		refused = venture_comment_create(f->context, &editor, "company",
			venture_entity_get_id(gone), 0, "After.", &error);
		g_assert_null(refused);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
		g_clear_error(&error);

		after_edit = venture_comment_edit(f->context, &editor,
			venture_entity_get_id(before), "Before, corrected.", &error);
		g_assert_no_error(error);
		g_assert_nonnull(after_edit);
	}

	/*
	 * A writer that is not the service -- a plugin, an approved change
	 * -- saving a comment with somebody else's name and organization on
	 * it: the save takes the author from the principal and the
	 * organization from the record.
	 */
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->database),
	                                    &editor);
	venture_auth_to_actor(&editor, &actor);
	forged = g_object_new(VENTURE_TYPE_COMMENT, "subject-type", "company",
	                      "subject-id", venture_entity_get_id(f->company),
	                      "body", "Totally from the owner.", "author", "olive",
	                      "author-user-id", f->owner_id, NULL);
	venture_entity_set_organization_id(forged, f->org);
	venture_database_save(f->database, forged, &actor, &error);
	g_assert_no_error(error);
	g_object_get(forged, "author", &author, NULL);
	g_assert_cmpstr(author, ==, editor_name);

	/* And once written, the author does not change. */
	g_object_set(forged, "author", "olive", NULL);
	g_assert_false(venture_database_save(f->database, forged, &actor, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_clear_object(&scope);

	/* The edit stamped its time. */
	{
		g_autoptr(VentureEntity) again = NULL;

		again = venture_comment_edit(f->context, &editor,
			venture_entity_get_id(top), "Top, edited.", &error);
		g_assert_no_error(error);
		g_clear_pointer(&edited_at, g_date_time_unref);
		g_object_get(again, "edited-at", &edited_at, NULL);
		g_assert_nonnull(edited_at);
	}
}

/*
 * A deleted comment with replies stays in its thread as a placeholder
 * with no words; one without replies goes. If this regresses, either the
 * replies hang under nothing, or the deleted words are still handed out.
 */
static void
test_thread_shape(Fixture *f, gconstpointer data)
{
	VentureAuthPrincipal editor;
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureEntity) answer = NULL;
	g_autoptr(VentureEntity) lonely = NULL;
	g_autoptr(JsonNode) thread = NULL;
	g_autoptr(GError) error = NULL;
	JsonArray *threads;
	JsonObject *placeholder;

	(void)data;

	principal_for(&editor, f->editor_id, VENTURE_USER_ROLE_EDITOR, editor_name);

	first = comment_as(f, &editor, f->company, 0, "Secret words.");
	answer = comment_as(f, &editor, f->company, venture_entity_get_id(first), "Answer.");
	lonely = comment_as(f, &editor, f->company, 0, "Nobody answered.");

	g_assert_true(venture_comment_delete(f->context, &editor,
		venture_entity_get_id(first), &error));
	g_assert_true(venture_comment_delete(f->context, &editor,
		venture_entity_get_id(lonely), &error));
	g_assert_no_error(error);

	thread = venture_comment_thread(f->context, &editor, "company",
		venture_entity_get_id(f->company), &error);
	g_assert_no_error(error);
	threads = json_node_get_array(thread);

	g_assert_cmpuint(json_array_get_length(threads), ==, 1);
	placeholder = json_array_get_object_element(threads, 0);
	g_assert_true(json_object_get_boolean_member(placeholder, "deleted"));
	g_assert_true(json_node_is_null(json_object_get_member(placeholder, "body")));
	g_assert_true(json_node_is_null(json_object_get_member(placeholder, "html")));
	g_assert_cmpuint(json_array_get_length(
		json_object_get_array_member(placeholder, "replies")), ==, 1);
	g_assert_cmpint(venture_comment_count(f->context, "company",
		venture_entity_get_id(f->company)), ==, 1);

	{
		g_autofree gchar *text = venture_json_to_string(thread, FALSE);

		g_assert_null(strstr(text, "Secret words"));
		g_assert_null(strstr(text, "Nobody answered"));
	}
}

/*
 * A mention tells the person named -- if, and only if, they may read the
 * record; watchers are told; a reply tells whoever it answers; an edit
 * tells only the people it newly names; and every one of those lines
 * opens the comment's permalink. If this regresses, an inbox line hands
 * somebody from another organization the name of a record and what was
 * said about it, or a reply goes unheard.
 */
static void
test_mentions(Fixture *f, gconstpointer data)
{
	VentureAuthPrincipal editor;
	VentureAuthPrincipal admin;
	g_autoptr(VentureEntity) comment = NULL;
	g_autoptr(VentureEntity) reply = NULL;
	g_autoptr(VentureEntity) edited = NULL;
	g_autoptr(GPtrArray) viewer_inbox = NULL;
	g_autoptr(GPtrArray) outsider_inbox = NULL;
	g_autoptr(GPtrArray) owner_inbox = NULL;
	g_autoptr(GPtrArray) editor_inbox = NULL;
	g_autoptr(GPtrArray) admin_inbox = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) as_json = NULL;
	g_autofree gchar *mentions = NULL;
	g_autofree gchar *html = NULL;
	g_autofree gchar *expected_url = NULL;

	(void)data;

	principal_for(&editor, f->editor_id, VENTURE_USER_ROLE_EDITOR, editor_name);
	principal_for(&admin, f->admin_id, VENTURE_USER_ROLE_EDITOR, admin_name);

	/* The owner follows the company. */
	g_assert_true(venture_notify_watch(f->context, f->owner_id, "company",
		venture_entity_get_id(f->company), &error));

	comment = comment_as(f, &editor, f->company, 0,
		"@vic can you look? cc @otto and @nobody, but not `@ada`.");

	/* Only the person who may read the record is a mention. */
	g_object_get(comment, "mentions", &mentions, NULL);
	g_assert_cmpstr(mentions, ==, "vic");

	viewer_inbox = inbox_of(f, f->viewer_id);
	g_assert_cmpuint(count_kind(viewer_inbox, VENTURE_NOTIFICATION_KIND_MENTION), ==, 1);
	outsider_inbox = inbox_of(f, f->outsider_id);
	g_assert_cmpuint(outsider_inbox->len, ==, 0);
	admin_inbox = inbox_of(f, f->admin_id);
	g_assert_cmpuint(count_kind(admin_inbox, VENTURE_NOTIFICATION_KIND_MENTION), ==, 0);

	/* The watcher hears, and the line opens the comment. */
	owner_inbox = inbox_of(f, f->owner_id);
	g_assert_cmpuint(count_kind(owner_inbox, VENTURE_NOTIFICATION_KIND_WATCHED), ==, 1);
	as_json = venture_notify_to_json(g_ptr_array_index(owner_inbox, 0));
	expected_url = g_strdup_printf("/comments/%" G_GINT64_FORMAT,
	                               venture_entity_get_id(comment));
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(as_json), "url"),
	                ==, expected_url);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(as_json),
	                                              "target_type"), ==, "comment");

	/* Whoever comments follows the record. */
	g_assert_true(venture_notify_is_watching(f->context, f->editor_id, "company",
		venture_entity_get_id(f->company)));

	/* The mention renders as a chip; the others stay text. */
	html = venture_comment_render(f->context, f->company,
		"@vic and @otto", (const gchar *const[]){ "vic", NULL });
	g_assert_nonnull(strstr(html, "<span class=\"mention\" title=\"Vic Views\">@vic</span>"));
	g_assert_null(strstr(html, "@otto</span>"));

	/* A reply tells whoever wrote what it answers. */
	reply = comment_as(f, &admin, f->company, venture_entity_get_id(comment), "Looking.");
	editor_inbox = inbox_of(f, f->editor_id);
	g_assert_cmpuint(count_kind(editor_inbox, VENTURE_NOTIFICATION_KIND_REPLY), ==, 1);

	/* An edit that names somebody new tells them, and only them. */
	g_clear_pointer(&viewer_inbox, g_ptr_array_unref);
	g_clear_pointer(&admin_inbox, g_ptr_array_unref);
	edited = venture_comment_edit(f->context, &editor,
		venture_entity_get_id(comment),
		"@vic can you look? And @ada, you too.", &error);
	g_assert_no_error(error);
	admin_inbox = inbox_of(f, f->admin_id);
	g_assert_cmpuint(count_kind(admin_inbox, VENTURE_NOTIFICATION_KIND_MENTION), ==, 1);
	viewer_inbox = inbox_of(f, f->viewer_id);
	g_assert_cmpuint(count_kind(viewer_inbox, VENTURE_NOTIFICATION_KIND_MENTION), ==, 1);

	/* Somebody may only be offered in the @ menu if they may read it. */
	{
		g_autoptr(JsonNode) offered = venture_comment_mention_candidates(
			f->context, f->company, "", 50);
		g_autofree gchar *text = venture_json_to_string(offered, FALSE);

		g_assert_nonnull(strstr(text, "\"vic\""));
		g_assert_nonnull(strstr(text, "\"erin\""));
		g_assert_null(strstr(text, "\"otto\""));
	}
}

/*
 * A record's activity says when somebody commented, once, with the way to
 * the comment. If this regresses, a discussion is invisible in the history,
 * or listed twice -- once from the comment's own audit entry.
 */
static void
test_activity(Fixture *f, gconstpointer data)
{
	VentureAuthPrincipal editor;
	g_autoptr(VentureEntity) comment = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	g_autoptr(JsonNode) events = NULL;
	g_autofree gchar *anchor = NULL;
	JsonArray *list;
	guint i;
	guint comments = 0;

	(void)data;

	principal_for(&editor, f->editor_id, VENTURE_USER_ROLE_EDITOR, editor_name);
	comment = comment_as(f, &editor, f->company, 0, "**Paid** on the 15th.");
	anchor = venture_comment_anchor_url(comment);

	scope = venture_access_policy_enter(venture_database_get_access_policy(f->database),
	                                    &editor);
	events = venture_desk_activity(f->context, "company",
		venture_entity_get_id(f->company), 50, NULL);
	list = json_node_get_array(events);

	for (i = 0; i < json_array_get_length(list); i++)
	{
		JsonObject *event = json_array_get_object_element(list, i);

		if (0 != g_strcmp0(json_object_get_string_member(event, "kind"), "comment"))
			continue;

		comments++;
		g_assert_cmpstr(json_object_get_string_member(event, "actor"), ==, "Erin Edits");
		g_assert_cmpstr(json_object_get_string_member(event, "body"), ==, "Paid on the 15th.");
		g_assert_cmpstr(json_object_get_string_member(event, "url"), ==, anchor);
	}

	g_assert_cmpuint(comments, ==, 1);
}

/* --- The routes ------------------------------------------------------------ */

/*
 * The permalink lands on the record at the comment, for whoever may read
 * the record, and is not found for anybody else; the generic page of a
 * comment goes the same way. If this regresses, a link in an inbox lands
 * nowhere, or tells an outsider the comment exists.
 */
static void
test_permalink(Fixture *f, gconstpointer data)
{
	VentureAuthPrincipal editor;
	g_autoptr(VentureEntity) comment = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *generic = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *anchor = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *article = NULL;

	(void)data;

	principal_for(&editor, f->editor_id, VENTURE_USER_ROLE_EDITOR, editor_name);
	comment = comment_as(f, &editor, f->company, 0, "Permalinked.");
	path = venture_comment_permalink(venture_entity_get_id(comment));
	anchor = venture_comment_anchor_url(comment);

	g_assert_cmpuint(request(f, "GET", path, f->editor_token, NULL, NULL, FALSE,
	                         NULL, &location, NULL), ==, 302);
	g_assert_cmpstr(location, ==, anchor);
	g_assert_cmpuint(request(f, "GET", path, f->outsider_token, NULL, NULL, FALSE,
	                         NULL, NULL, NULL), ==, 404);
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, NULL, FALSE,
	                         NULL, NULL, NULL), ==, 302);

	generic = g_strdup_printf("/e/comment/%" G_GINT64_FORMAT,
	                          venture_entity_get_id(comment));
	g_assert_cmpuint(request(f, "GET", generic, f->editor_token, NULL, NULL,
	                         FALSE, NULL, &again, NULL), ==, 302);
	g_assert_cmpstr(again, ==, path);

	/* The anchor is on the page the permalink lands on. */
	g_assert_cmpuint(request(f, "GET", anchor, f->editor_token, NULL, NULL,
	                         FALSE, &page, NULL, NULL), ==, 200);
	article = g_strdup_printf("id=\"comment-%" G_GINT64_FORMAT "\" data-comment-id=",
	                          venture_entity_get_id(comment));
	g_assert_nonnull(strstr(page, article));
	g_assert_nonnull(strstr(page, "<article class=\"discussion-comment"));
}

/* Every button on the page has a name a screen reader can read out. */
static void
assert_buttons_named(const gchar *page)
{
	const gchar *cursor = page;

	while (NULL != (cursor = strstr(cursor, "<button")))
	{
		const gchar *open_end = strchr(cursor, '>');
		const gchar *close = strstr(cursor, "</button>");
		g_autofree gchar *tag = NULL;
		gboolean words = FALSE;
		gboolean in_tag = FALSE;
		const gchar *p;

		g_assert_nonnull(open_end);
		g_assert_nonnull(close);
		tag = g_strndup(cursor, open_end - cursor);

		for (p = open_end + 1; p < close; p++)
		{
			if ('<' == *p)
				in_tag = TRUE;
			else if ('>' == *p)
				in_tag = FALSE;
			else if (!in_tag && g_ascii_isalnum(*p))
				words = TRUE;
		}

		if (!words && (NULL == strstr(tag, "aria-label=\"")))
			g_error("a button with no name: %s>", tag);

		cursor = close;
	}
}

/*
 * The page works with no script at all: the composer is an ordinary form
 * whose post lands on the comment, a reply box and an edit box are
 * folds, the permalink is a link; and the script's own request gets the
 * panel back and the comment to land on. If this regresses, a person
 * without JavaScript cannot comment, or the in-place post loses its place.
 */
static void
test_page_and_form(Fixture *f, gconstpointer data)
{
	VentureAuthPrincipal viewer;
	g_autoptr(VentureEntity) seeded = NULL;
	g_autofree gchar *record = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *form = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *panel = NULL;
	g_autofree gchar *landed = NULL;
	g_autoptr(JsonNode) thread = NULL;
	g_autoptr(GError) error = NULL;

	(void)data;

	principal_for(&viewer, f->viewer_id, VENTURE_USER_ROLE_VIEWER, viewer_name);
	seeded = comment_as(f, &viewer, f->company, 0, "First, from **vic**.");

	record = g_strdup_printf("/e/company/%" G_GINT64_FORMAT,
	                         venture_entity_get_id(f->company));
	g_assert_cmpuint(request(f, "GET", record, f->editor_token, NULL, NULL, FALSE,
	                         &page, NULL, NULL), ==, 200);
	g_assert_nonnull(strstr(page, "<section class=\"card discussion\" id=\"discussion\""));
	g_assert_nonnull(strstr(page, "<form class=\"discussion-composer\" method=\"post\" action=\"/comments\""));
	g_assert_nonnull(strstr(page, "<strong>vic</strong>"));
	g_assert_nonnull(strstr(page, "<details class=\"discussion-reply\""));
	g_assert_nonnull(strstr(page, "data-discussion-copy>Copy link</a>"));
	/* The tabs and the toolbar wait for the script. */
	g_assert_nonnull(strstr(page, "data-discussion-bar hidden"));
	assert_buttons_named(page);

	/* With no script: a form post, a redirect to the comment. */
	form = g_strdup_printf("subject_type=company&subject_id=%" G_GINT64_FORMAT
	                       "&parent_id=0&body=Plain+post&author=olive",
	                       venture_entity_get_id(f->company));
	g_assert_cmpuint(request(f, "POST", "/comments", f->editor_token,
	                         "application/x-www-form-urlencoded", form, FALSE,
	                         NULL, &location, NULL), ==, 302);
	g_assert_true(g_str_has_prefix(location, record));
	g_assert_nonnull(strstr(location, "#comment-"));

	/* With the script: the panel again, and which comment to land on. */
	g_assert_cmpuint(request(f, "POST", "/comments", f->editor_token,
	                         "application/x-www-form-urlencoded", form, TRUE,
	                         &panel, NULL, &landed), ==, 200);
	g_assert_true(g_str_has_prefix(panel, "<section class=\"card discussion\""));
	g_assert_nonnull(landed);
	{
		g_autofree gchar *anchor = g_strdup_printf("id=\"comment-%s\"", landed);

		g_assert_nonnull(strstr(panel, anchor));
	}

	/* "author=olive" was not read. */
	thread = venture_comment_thread(f->context, NULL, "company",
		venture_entity_get_id(f->company), &error);
	g_assert_no_error(error);
	{
		g_autofree gchar *text = venture_json_to_string(thread, FALSE);

		g_assert_null(strstr(text, "\"author\":\"olive\""));
	}

	/* A ticket has its own conversation and no second one. */
	{
		g_autoptr(VentureEntity) ticket = g_object_new(VENTURE_TYPE_TICKET,
			"title", "A ticket", NULL);
		g_autofree gchar *ticket_page = NULL;
		g_autofree gchar *ticket_path = NULL;

		venture_entity_set_organization_id(ticket, f->org);
		save(f, ticket);
		ticket_path = g_strdup_printf("/e/ticket/%" G_GINT64_FORMAT,
		                              venture_entity_get_id(ticket));
		g_assert_cmpuint(request(f, "GET", ticket_path, f->admin_token, NULL,
		                         NULL, FALSE, &ticket_page, NULL, NULL), ==, 200);
		g_assert_null(strstr(ticket_page, "id=\"discussion\""));
	}
}

/*
 * The API: a tree to read, a body to post, an author never taken from the
 * request, edits only by the author, deletes by an administrator, a
 * preview that is exactly what posting renders, and the generic routes
 * shut. If this regresses, a script posts as somebody else, or reaches a
 * comment through /api/v1/comment that the comment routes would refuse.
 */
static void
test_api(Fixture *f, gconstpointer data)
{
	g_autofree gchar *create = NULL;
	g_autofree gchar *list = NULL;
	g_autofree gchar *one = NULL;
	g_autofree gchar *preview_body = NULL;
	g_autofree gchar *mentions = NULL;
	g_autoptr(JsonNode) made = NULL;
	g_autoptr(JsonNode) tree = NULL;
	g_autoptr(JsonNode) preview = NULL;
	g_autoptr(JsonNode) fetched = NULL;
	g_autoptr(JsonNode) offered = NULL;
	const gchar *text = "Hi @vic -- see [the list](/e/company) and #company/1.\n\n- one\n- two";
	JsonObject *object;
	gint64 id;

	(void)data;

	create = g_strdup_printf("{\"subject_type\":\"company\",\"subject_id\":%"
	                         G_GINT64_FORMAT ",\"body\":\"Hi @vic -- see [the list]"
	                         "(/e/company) and #company/1.\\n\\n- one\\n- two\","
	                         "\"author\":\"olive\",\"author_user_id\":1}",
	                         venture_entity_get_id(f->company));
	g_assert_cmpuint(api(f, "POST", "/api/v1/comments", f->editor_token, create,
	                     &made), ==, 201);
	object = json_node_get_object(made);
	id = json_object_get_int_member(object, "id");
	g_assert_cmpint(json_object_get_int_member(object, "author_user_id"), ==, f->editor_id);
	/* A token writes as its number, and its name never appears. */
	g_assert_true(g_str_has_prefix(json_object_get_string_member(object, "author"),
	                               "API token #"));
	g_assert_true(json_object_get_boolean_member(object, "via_token"));
	g_assert_cmpstr(json_object_get_string_member(object, "author_name"), ==, "Erin Edits");
	g_assert_true(json_object_get_boolean_member(object, "can_edit"));

	/* The preview is what posting renders, for the same reader. */
	preview_body = g_strdup_printf("{\"subject_type\":\"company\",\"subject_id\":%"
	                               G_GINT64_FORMAT ",\"body\":\"Hi @vic -- see [the list]"
	                               "(/e/company) and #company/1.\\n\\n- one\\n- two\"}",
	                               venture_entity_get_id(f->company));
	g_assert_cmpuint(api(f, "POST", "/api/v1/comments/preview", f->editor_token,
	                     preview_body, &preview), ==, 200);
	one = g_strdup_printf("/api/v1/comments/%" G_GINT64_FORMAT, id);
	g_assert_cmpuint(api(f, "GET", one, f->editor_token, NULL, &fetched), ==, 200);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(preview), "html"),
	                ==, json_object_get_string_member(json_node_get_object(fetched), "html"));
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(fetched), "body"),
	                ==, text);

	/* The tree, to a reader; not found to an outsider. */
	list = g_strdup_printf("/api/v1/comments?subject_type=company&subject_id=%"
	                       G_GINT64_FORMAT, venture_entity_get_id(f->company));
	g_assert_cmpuint(api(f, "GET", list, f->editor_token, NULL, &tree), ==, 200);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(tree), "count"), ==, 1);
	g_assert_cmpuint(api(f, "GET", list, f->outsider_token, NULL, NULL), ==, 404);
	g_assert_cmpuint(api(f, "GET", one, f->outsider_token, NULL, NULL), ==, 404);

	/* The @ menu, for this record: the viewer who owns it, not the outsider. */
	mentions = g_strdup_printf("/api/v1/comments/mentions?subject_type=company"
	                           "&subject_id=%" G_GINT64_FORMAT "&q=v",
	                           venture_entity_get_id(f->company));
	g_assert_cmpuint(api(f, "GET", mentions, f->editor_token, NULL, &offered), ==, 200);
	{
		g_autofree gchar *users = venture_json_to_string(offered, FALSE);

		g_assert_nonnull(strstr(users, "\"vic\""));
		g_assert_null(strstr(users, "\"otto\""));
	}

	/* Only the author edits; an administrator may delete. */
	g_assert_cmpuint(api(f, "PATCH", one, f->admin_token, "{\"body\":\"Mine now\"}",
	                     NULL), ==, 403);
	g_assert_cmpuint(api(f, "PATCH", one, f->editor_token, "{\"body\":\"Mine still\"}",
	                     NULL), ==, 200);
	g_assert_cmpuint(api(f, "DELETE", one, f->admin_token, NULL, NULL), ==, 200);

	/* The generic routes are shut: no create, and the list is the owner's. */
	g_assert_cmpuint(api(f, "POST", "/api/v1/comment", f->editor_token, create, NULL),
	                 ==, 403);
	g_assert_cmpuint(api(f, "GET", "/api/v1/comment", f->editor_token, NULL, NULL),
	                 ==, 403);
}

/*
 * Every record type that takes comments shows the Discussion card on its
 * page -- walked over the registry, so a plugin's type, or one added
 * tomorrow, is covered the day it registers -- and a type that opted out
 * shows none. If this regresses, some record pages quietly have nowhere
 * to talk.
 */
/*
 * Fills the fields a type cannot be saved without, where a plain value
 * will do, so the walk reaches more than the types with no requirements.
 * References are left alone: a made-up id is refused, as it should be.
 */
static void
fill_required(VentureEntity *record)
{
	g_autoptr(GPtrArray) specs = venture_entity_get_field_specs(record);
	guint i;

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);
		const gchar *value = NULL;

		if (0 == (venture_field_spec_get_flags(spec) & VENTURE_COLUMN_FLAG_NOT_NULL))
			continue;

		switch (venture_field_spec_get_kind(spec))
		{
		case VENTURE_FIELD_KIND_STRING:
		case VENTURE_FIELD_KIND_TEXT:
			value = "Example";
			break;
		case VENTURE_FIELD_KIND_DATE:
			value = "2026-01-15";
			break;
		case VENTURE_FIELD_KIND_DATETIME:
			value = "2026-01-15T10:00:00Z";
			break;
		case VENTURE_FIELD_KIND_MONEY:
			value = "1.00 USD";
			break;
		default:
			break;
		}

		if (NULL != value)
			venture_entity_set_field_from_string(record,
				venture_field_spec_get_name(spec), value, NULL);
	}
}

static void
ignore_warning(
	const gchar	*domain,
	GLogLevelFlags	 level,
	const gchar	*message,
	gpointer	 user_data
){
	(void)domain;
	(void)level;
	(void)message;
	(*(guint *)user_data)++;
}

static void
test_every_type(Fixture *f, gconstpointer data)
{
	g_autofree GType *types = NULL;
	GLogLevelFlags previous_fatal;
	guint n_types = 0;
	guint rendered = 0;
	guint refused = 0;
	guint ignored = 0;
	guint handler;
	guint i;

	(void)data;

	types = venture_entity_registry_list_types(
		venture_context_get_entity_registry(f->context), &n_types);

	/* Records made bare, as a person would never make them: some types'
	 * validators refuse one out loud, and that is not this test's
	 * business -- only pages that render are judged. */
	previous_fatal = g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
	handler = g_log_set_handler("Venture", G_LOG_LEVEL_WARNING, ignore_warning,
	                            &ignored);

	for (i = 0; i < n_types; i++)
	{
		g_autoptr(VentureEntity) record = NULL;
		g_autoptr(GError) error = NULL;
		g_autofree gchar *path = NULL;
		g_autofree gchar *page = NULL;
		guint status;

		if (G_TYPE_IS_ABSTRACT(types[i]))
			continue;

		record = g_object_new(types[i], NULL);
		venture_entity_set_organization_id(record, f->org);
		fill_required(record);

		if (!venture_database_save(f->database, record, NULL, &error))
			continue;

		path = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT,
		                       venture_entity_get_entity_name(record),
		                       venture_entity_get_id(record));
		status = request(f, "GET", path, NULL, NULL, NULL, FALSE, &page, NULL, NULL);

		if (200 != status)
			continue;

		if (venture_comment_subject_accepts(f->database, record))
		{
			if (NULL == strstr(page, "id=\"discussion\""))
				g_error("%s takes comments but its page has no discussion", path);

			assert_buttons_named(page);
			rendered++;
		}
		else
		{
			if (NULL != strstr(page, "id=\"discussion\""))
				g_error("%s takes no comments but its page has a discussion", path);

			refused++;
		}
	}

	g_log_remove_handler("Venture", handler);
	g_log_set_always_fatal(previous_fatal);

	/* A walk that rendered almost nothing walked nothing. */
	g_assert_cmpuint(rendered, >=, 80);
	g_test_message("%u record pages with a discussion, %u without, %u bare "
	               "records refused", rendered, refused, ignored);
}

/*
 * Both looks style the discussion. If this regresses, one look shows the
 * thread as an unstyled heap of links and paragraphs.
 */
static void
test_both_looks(Fixture *f, gconstpointer data)
{
	static const gchar *const classes[] = {
		".discussion-comment", ".discussion-avatar", ".discussion-head",
		".discussion-replies", ".discussion-composer", ".discussion-bar",
		".discussion-tab", ".discussion-tool", ".discussion-menu",
		".discussion-comment:target", ".discussion-empty", ".mention",
		".record-ref", ".markdown pre", ".markdown blockquote",
		".markdown table", ".timeline-link", NULL
	};
	g_autofree gchar *path = NULL;
	g_autofree gchar *industrial = NULL;
	g_autofree gchar *classic = NULL;
	gsize i;

	(void)data;

	path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT,
	                       venture_entity_get_id(f->company));
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, NULL, FALSE, &industrial,
	                         NULL, NULL), ==, 200);
	g_assert_nonnull(strstr(industrial, "instrument panel"));
	g_object_set(f->config, "ui-look", "classic", NULL);
	g_assert_cmpuint(request(f, "GET", path, NULL, NULL, NULL, FALSE, &classic,
	                         NULL, NULL), ==, 200);
	g_assert_nonnull(strstr(classic, "warm monochrome"));

	for (i = 0; NULL != classes[i]; i++)
	{
		if ((NULL == strstr(industrial, classes[i])) ||
		    (NULL == strstr(classic, classes[i])))
			g_error("%s is not styled in both looks", classes[i]);
	}

	/* The script hooks are attributes, so no class name lives in it. */
	g_assert_nonnull(strstr(industrial, "function wire_discussion"));
}

#define ADD(path, func) \
	g_test_add("/comments/" path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)
#define ADD_OPEN(path, func) \
	g_test_add("/comments/" path, Fixture, GINT_TO_POINTER(1), fixture_set_up, func, fixture_tear_down)

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/comments/types", test_types);
	ADD("permissions", test_permissions);
	ADD("unreadable", test_unreadable);
	ADD("validator", test_validator);
	ADD("thread-shape", test_thread_shape);
	ADD("mentions", test_mentions);
	ADD("activity", test_activity);
	ADD("permalink", test_permalink);
	ADD("page-and-form", test_page_and_form);
	ADD("api", test_api);
	ADD_OPEN("every-type", test_every_type);
	ADD_OPEN("both-looks", test_both_looks);

	return g_test_run();
}
