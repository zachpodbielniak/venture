/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-inbox-chat.c - Web chat: the public intake, visitor links and
 * the chat channel of the inbox
 *
 * A visitor gives a name and an email or a phone and writes; they are
 * matched to a contact the way lead capture compares addresses and
 * numbers, or become one, and get a link with a long random token that
 * shows their chat and our replies. The token is kept only as its hash.
 * Nothing here uses a cookie or a session: the link is the capability,
 * and every public call runs in the trusted scope bounded by what the
 * token names, never by whoever's browser cookie came along.
 */
#include "venture-inbox-private.h"
#include <string.h>

static gboolean
refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error_literal(error, VENTURE_ERROR, code, message);
	return FALSE;
}

static gint64
number(gpointer row, const gchar *field)
{
	gint64 value = 0;
	g_object_get(row, field, &value, NULL);
	return value;
}

static gchar *
string(gpointer row, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(row, field, &value, NULL);
	return value;
}

static VentureAccessScope *
trusted(VentureInboxService *self)
{
	return venture_access_policy_enter(venture_database_get_access_policy(self->database), NULL);
}

static gboolean
is_hex(const gchar *text, gsize least, gsize most)
{
	gsize length, i;
	if (text == NULL)
		return FALSE;
	length = strlen(text);
	if (length < least || length > most)
		return FALSE;
	for (i = 0; i < length; i++)
		if (!g_ascii_isxdigit(text[i]) || g_ascii_isupper(text[i]))
			return FALSE;
	return TRUE;
}

/* --- Bounds ------------------------------------------------------------------- */

static gboolean
no_controls(const gchar *text, gboolean lines)
{
	const gchar *p;
	for (p = text; *p != '\0'; p = g_utf8_next_char(p))
	{
		gunichar c = g_utf8_get_char(p);
		if ((c < 0x20 && !(lines && (c == '\n' || c == '\r' || c == '\t'))) || c == 0x7f || (c >= 0x80 && c < 0xa0))
			return FALSE;
	}
	return TRUE;
}

static gchar *
clean_body(const gchar *body, GError **error)
{
	g_autofree gchar *text = NULL;
	if (body == NULL || !g_utf8_validate(body, -1, NULL))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Write a message of up to %d characters", VENTURE_INBOX_CHAT_BODY_MAX);
		return NULL;
	}
	text = g_strstrip(g_strdup(body));
	if (*text == '\0' || g_utf8_strlen(text, -1) > VENTURE_INBOX_CHAT_BODY_MAX || !no_controls(text, TRUE))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Write a message of up to %d characters", VENTURE_INBOX_CHAT_BODY_MAX);
		return NULL;
	}
	return g_steal_pointer(&text);
}

static gchar *
clean_line(const gchar *value)
{
	if (value == NULL || !g_utf8_validate(value, -1, NULL))
		return NULL;
	return g_strstrip(g_strdup(value));
}

static gboolean
valid_email(const gchar *email)
{
	const gchar *at, *p;
	if (strlen(email) > VENTURE_INBOX_CHAT_EMAIL_MAX)
		return FALSE;
	for (p = email; *p != '\0'; p++)
		if ((guchar)*p <= 0x20 || (guchar)*p >= 0x7f || strchr("<>,;\"\\()[]", *p) != NULL)
			return FALSE;
	at = strchr(email, '@');
	return at != NULL && at != email && strchr(at + 1, '@') == NULL && strchr(at + 1, '.') != NULL &&
		at[1] != '.' && email[strlen(email) - 1] != '.';
}

static gboolean
valid_phone(const gchar *phone)
{
	const gchar *p;
	guint digits = 0;
	if (strlen(phone) > VENTURE_INBOX_CHAT_PHONE_MAX)
		return FALSE;
	for (p = phone; *p != '\0'; p++)
	{
		if (g_ascii_isdigit(*p))
			digits++;
		else if (strchr(" +-().", *p) == NULL)
			return FALSE;
	}
	return digits >= 7 && digits <= 15;
}

/* --- Matching a visitor to a contact -------------------------------------------- */

static VentureEntity *
match_email(VentureInboxService *self, gint64 org, const gchar *email, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTACT);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *wanted = venture_lead_normalize_email(email), *pattern = NULL;
	guint i;
	if (*wanted == '\0')
		return NULL;
	pattern = g_strdup_printf("%%%s%%", wanted);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 200);
	venture_query_add_filter_string(query, "email", VENTURE_FILTER_OP_ILIKE, pattern, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(self->database, query, error);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		g_autofree gchar *have = string(g_ptr_array_index(rows, i), "email");
		g_autofree gchar *normal = venture_lead_normalize_email(have);
		if (g_strcmp0(normal, wanted) == 0)
			return g_object_ref(g_ptr_array_index(rows, i));
	}
	return NULL;
}

/* Digits as lead capture keeps them; a number written with its country
 * code and one written without are the same number when the longer ends
 * with the shorter and the shorter is a whole national number. */
static gboolean
same_number(const gchar *a, const gchar *b)
{
	gsize la = strlen(a), lb = strlen(b);
	if (la == 0 || lb == 0)
		return FALSE;
	if (la == lb)
		return strcmp(a, b) == 0;
	if (MIN(la, lb) < 10)
		return FALSE;
	return la > lb ? strcmp(a + la - lb, b) == 0 : strcmp(b + lb - la, a) == 0;
}

static VentureEntity *
match_phone(VentureInboxService *self, gint64 org, const gchar *phone, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTACT);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *wanted = venture_lead_normalize_phone(phone), *pattern = NULL;
	gsize length = strlen(wanted);
	guint i;
	if (length < 7)
		return NULL;
	/* The last four digits are written together however the rest is. */
	pattern = g_strdup_printf("%%%s%%", wanted + length - 4);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 500);
	venture_query_add_filter_string(query, "phone", VENTURE_FILTER_OP_LIKE, pattern, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(self->database, query, error);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		g_autofree gchar *have = string(g_ptr_array_index(rows, i), "phone");
		g_autofree gchar *digits = venture_lead_normalize_phone(have);
		if (same_number(digits, wanted))
			return g_object_ref(g_ptr_array_index(rows, i));
	}
	return NULL;
}

/* --- Web chats -------------------------------------------------------------------- */

static gboolean
require_admin(VentureInboxService *self, gint64 org, GError **error)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureAccessPolicy *policy = venture_database_get_access_policy(self->database);
	const VentureAuthPrincipal *principal = venture_access_policy_get_actor(policy);
	if (principal != NULL && !venture_access_policy_has_organization_role(policy, principal, org, roles, G_N_ELEMENTS(roles)))
		return refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Web chat settings need an owner or administrator of the organization");
	return TRUE;
}

VentureEntity *
venture_inbox_chat_widget_create(VentureInboxService *self, gint64 organization_id, const gchar *name,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) organization = NULL, row = NULL;
	g_autofree gchar *token = NULL, *label = NULL;
	g_return_val_if_fail(VENTURE_IS_INBOX_SERVICE(self), NULL);
	if (!require_admin(self, organization_id, error))
		return NULL;
	organization = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, organization_id, NULL);
	if (organization == NULL || venture_entity_is_deleted(organization))
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "Organization is unavailable");
		return NULL;
	}
	label = clean_line(name);
	if (venture_string_is_empty(label) || g_utf8_strlen(label, -1) > 100 || !no_controls(label, FALSE))
	{
		g_free(label);
		label = g_strdup("Website chat");
	}
	token = venture_inbox_random_token(24, error);
	if (token == NULL)
		return NULL;
	row = g_object_new(VENTURE_TYPE_INBOX_CHAT_WIDGET, "organization-id", organization_id, "name", label,
		"token", token, "active", TRUE, "greeting", "Ask us anything. We answer here and by email or text.", NULL);
	if (!venture_inbox_service_write(self, row, actor, error))
		return NULL;
	return g_steal_pointer(&row);
}

static VentureEntity *
reread(VentureInboxService *self, VentureEntity *row, GType type, GError **error)
{
	VentureEntity *fresh = NULL;
	if (G_TYPE_CHECK_INSTANCE_TYPE(row, type) && venture_entity_is_persisted(row))
		fresh = venture_database_get(self->database, type, venture_entity_get_id(row), NULL);
	if (fresh == NULL || venture_entity_is_deleted(fresh))
	{
		g_clear_object(&fresh);
		refuse(error, VENTURE_ERROR_NOT_FOUND, "Not found");
	}
	return fresh;
}

gboolean
venture_inbox_chat_widget_update(VentureInboxService *self, VentureEntity *widget, const gchar *greeting,
	const gchar *allowed_origins, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autofree gchar *words = clean_line(greeting), *origins = clean_line(allowed_origins);
	g_return_val_if_fail(VENTURE_IS_INBOX_SERVICE(self), FALSE);
	row = reread(self, widget, VENTURE_TYPE_INBOX_CHAT_WIDGET, error);
	if (row == NULL || !require_admin(self, venture_entity_get_organization_id(row), error))
		return FALSE;
	if (words != NULL && g_utf8_strlen(words, -1) > 500)
		return refuse(error, VENTURE_ERROR_VALIDATION, "A greeting is at most 500 characters");
	if (origins != NULL)
	{
		g_auto(GStrv) lines = g_strsplit_set(origins, "\n\r\t ,", -1);
		guint i;
		for (i = 0; lines[i] != NULL; i++)
		{
			g_autoptr(GUri) uri = NULL;
			if (lines[i][0] == '\0')
				continue;
			uri = g_uri_parse(lines[i], G_URI_FLAGS_NONE, NULL);
			if (uri == NULL || (g_strcmp0(g_uri_get_scheme(uri), "https") != 0 && g_strcmp0(g_uri_get_scheme(uri), "http") != 0) ||
				g_uri_get_host(uri) == NULL || strpbrk(lines[i], ";'\"") != NULL ||
				(*g_uri_get_path(uri) != '\0' && g_strcmp0(g_uri_get_path(uri), "/") != 0))
				return refuse(error, VENTURE_ERROR_VALIDATION, "Write each site as its origin, for example https://www.example.com");
		}
	}
	g_object_set(row, "greeting", words, "allowed-origins", origins, NULL);
	return venture_inbox_service_write(self, row, actor, error);
}

gboolean
venture_inbox_chat_widget_set_active(VentureInboxService *self, VentureEntity *widget, gboolean active,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	g_return_val_if_fail(VENTURE_IS_INBOX_SERVICE(self), FALSE);
	row = reread(self, widget, VENTURE_TYPE_INBOX_CHAT_WIDGET, error);
	if (row == NULL || !require_admin(self, venture_entity_get_organization_id(row), error))
		return FALSE;
	g_object_set(row, "active", active, NULL);
	return venture_inbox_service_write(self, row, actor, error);
}

VentureEntity *
venture_inbox_chat_widget_lookup(VentureInboxService *self, const gchar *token)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) row = NULL;
	gboolean active = FALSE;
	g_return_val_if_fail(VENTURE_IS_INBOX_SERVICE(self), NULL);
	if (self->database == NULL || !is_hex(token, 32, 128))
		return NULL;
	internal = trusted(self);
	query = venture_query_new(VENTURE_TYPE_INBOX_CHAT_WIDGET);
	venture_query_add_filter_string(query, "token", VENTURE_FILTER_OP_EQ, token, NULL);
	row = venture_database_find_one(self->database, query, NULL);
	if (row == NULL)
		return NULL;
	g_object_get(row, "active", &active, NULL);
	return active ? g_steal_pointer(&row) : NULL;
}

/* --- A visitor's chat ------------------------------------------------------------------ */

static const VentureActor *
visitor(void)
{
	return venture_inbox_service_actor("Web chat visitor");
}

static gboolean
record_message(VentureInboxService *self, VentureEntity *session, const gchar *body, gboolean inbound,
	const gchar *author, GDateTime *now, const VentureActor *actor, VentureEntity **out, GError **error)
{
	g_autoptr(VentureEntity) message = g_object_new(VENTURE_TYPE_INBOX_CHAT_MESSAGE,
		"organization-id", venture_entity_get_organization_id(session), "session-id", venture_entity_get_id(session),
		"contact-id", number(session, "contact-id"), "inbound", inbound, "body", body, "author", author, "sent-at", now, NULL);
	if (!venture_inbox_service_write(self, message, actor, error))
		return FALSE;
	g_object_set(session, "last-message-at", now, NULL);
	if (!venture_inbox_service_write(self, session, actor, error))
		return FALSE;
	if (out != NULL)
		*out = g_steal_pointer(&message);
	return TRUE;
}

static gboolean
rate_limited(GError **error)
{
	g_set_error_literal(error, VENTURE_INBOX_ERROR, VENTURE_INBOX_ERROR_RATE_LIMITED,
		"That is a lot of messages at once; please wait a few minutes and try again");
	return FALSE;
}

VentureEntity *
venture_inbox_chat_start(VentureInboxService *self, const gchar *widget_token, const VentureInboxChatIntake *intake,
	GDateTime *now, gchar **visitor_token, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) widget = NULL, contact = NULL, session = NULL;
	g_autofree gchar *name = NULL, *email = NULL, *phone = NULL, *body = NULL, *token = NULL, *hash = NULL, *key = NULL;
	gint64 org;
	g_return_val_if_fail(VENTURE_IS_INBOX_SERVICE(self), NULL);
	g_return_val_if_fail(intake != NULL && now != NULL, NULL);
	if (visitor_token != NULL)
		*visitor_token = NULL;
	widget = venture_inbox_chat_widget_lookup(self, widget_token);
	if (widget == NULL)
	{
		refuse(error, VENTURE_ERROR_NOT_FOUND, "There is no such chat");
		return NULL;
	}
	/* A person never fills the hidden field; a form robot does. It is
	 * thanked and forgotten, so it learns nothing to adjust. */
	if (!venture_string_is_empty(intake->trap))
		return NULL;
	name = clean_line(intake->name);
	email = clean_line(intake->email);
	phone = clean_line(intake->phone);
	if (venture_string_is_empty(name) || g_utf8_strlen(name, -1) > VENTURE_INBOX_CHAT_NAME_MAX || !no_controls(name, FALSE))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Give your name, up to %d characters", VENTURE_INBOX_CHAT_NAME_MAX);
		return NULL;
	}
	if (venture_string_is_empty(email) && venture_string_is_empty(phone))
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "Give an email address or a phone number, so we can answer you");
		return NULL;
	}
	if ((intake->email != NULL && email == NULL) || (!venture_string_is_empty(email) && !valid_email(email)))
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "That email address does not look right");
		return NULL;
	}
	if ((intake->phone != NULL && phone == NULL) || (!venture_string_is_empty(phone) && !valid_phone(phone)))
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "That phone number does not look right");
		return NULL;
	}
	body = clean_body(intake->body, error);
	if (body == NULL)
		return NULL;
	key = g_strdup_printf("start:%" G_GINT64_FORMAT ":%s", venture_entity_get_id(widget),
		intake->remote != NULL ? intake->remote : "unknown");
	if (!venture_inbox_service_allow(self, key, VENTURE_INBOX_CHAT_STARTS_PER_WINDOW, now))
	{
		rate_limited(error);
		return NULL;
	}
	org = venture_entity_get_organization_id(widget);
	internal = trusted(self);
	if (!venture_database_begin(self->database, error))
		return NULL;
	if (!venture_string_is_empty(email))
		contact = match_email(self, org, email, error);
	if (contact == NULL && (error == NULL || *error == NULL) && !venture_string_is_empty(phone))
		contact = match_phone(self, org, phone, error);
	if (error != NULL && *error != NULL)
		goto fail;
	if (contact == NULL)
	{
		contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", org, "name", name,
			"email", venture_string_is_empty(email) ? NULL : email, "phone", venture_string_is_empty(phone) ? NULL : phone,
			"source", "Web chat", NULL);
		if (!venture_database_save(self->database, contact, visitor(), error))
			goto fail;
	}
	token = venture_inbox_random_token(32, error);
	if (token == NULL)
		goto fail;
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, token, -1);
	session = g_object_new(VENTURE_TYPE_INBOX_CHAT_SESSION, "organization-id", org,
		"widget-id", venture_entity_get_id(widget), "contact-id", venture_entity_get_id(contact),
		"visitor-name", name, "visitor-email", email, "visitor-phone", phone, "token-hash", hash, NULL);
	if (!venture_inbox_service_write(self, session, visitor(), error))
		goto fail;
	/* The opening message counts toward the chat's own limit. */
	g_free(key);
	key = g_strdup_printf("post:%" G_GINT64_FORMAT, venture_entity_get_id(session));
	venture_inbox_service_allow(self, key, VENTURE_INBOX_CHAT_MESSAGES_PER_WINDOW, now);
	if (!record_message(self, session, body, TRUE, NULL, now, visitor(), NULL, error))
		goto fail;
	if (!venture_database_commit(self->database, error))
		return NULL;
	if (visitor_token != NULL)
		*visitor_token = g_steal_pointer(&token);
	return g_steal_pointer(&session);
fail:
	venture_database_rollback(self->database);
	return NULL;
}

VentureEntity *
venture_inbox_chat_session_lookup(VentureInboxService *self, const gchar *visitor_token)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autofree gchar *hash = NULL;
	g_return_val_if_fail(VENTURE_IS_INBOX_SERVICE(self), NULL);
	if (self->database == NULL || !is_hex(visitor_token, 64, 64))
		return NULL;
	internal = trusted(self);
	hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, visitor_token, -1);
	query = venture_query_new(VENTURE_TYPE_INBOX_CHAT_SESSION);
	venture_query_add_filter_string(query, "token-hash", VENTURE_FILTER_OP_EQ, hash, NULL);
	return venture_database_find_one(self->database, query, NULL);
}

VentureEntity *
venture_inbox_chat_post(VentureInboxService *self, VentureEntity *session, const gchar *body, const gchar *remote,
	GDateTime *now, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) row = NULL, message = NULL;
	g_autofree gchar *text = NULL, *key = NULL;
	g_return_val_if_fail(VENTURE_IS_INBOX_SERVICE(self), NULL);
	(void)remote;
	internal = trusted(self);
	row = reread(self, session, VENTURE_TYPE_INBOX_CHAT_SESSION, error);
	if (row == NULL)
		return NULL;
	text = clean_body(body, error);
	if (text == NULL)
		return NULL;
	key = g_strdup_printf("post:%" G_GINT64_FORMAT, venture_entity_get_id(row));
	if (!venture_inbox_service_allow(self, key, VENTURE_INBOX_CHAT_MESSAGES_PER_WINDOW, now))
	{
		rate_limited(error);
		return NULL;
	}
	if (!record_message(self, row, text, TRUE, NULL, now, visitor(), &message, error))
		return NULL;
	return g_steal_pointer(&message);
}

VentureEntity *
venture_inbox_chat_answer(VentureInboxService *self, VentureEntity *session, const gchar *body, const gchar *author,
	GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL, message = NULL;
	g_autofree gchar *text = NULL;
	g_return_val_if_fail(VENTURE_IS_INBOX_SERVICE(self), NULL);
	row = reread(self, session, VENTURE_TYPE_INBOX_CHAT_SESSION, error);
	if (row == NULL)
		return NULL;
	text = clean_body(body, error);
	if (text == NULL)
		return NULL;
	if (!record_message(self, row, text, FALSE, author, now, actor, &message, error))
		return NULL;
	return g_steal_pointer(&message);
}

GPtrArray *
venture_inbox_chat_messages(VentureInboxService *self, VentureEntity *session, gint64 after_id, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_return_val_if_fail(VENTURE_IS_INBOX_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_INBOX_CHAT_SESSION(session), NULL);
	internal = trusted(self);
	query = venture_query_new(VENTURE_TYPE_INBOX_CHAT_MESSAGE);
	venture_query_set_organization(query, venture_entity_get_organization_id(session));
	venture_query_add_filter_int(query, "session-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(session), NULL);
	if (after_id > 0)
		venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_GT, after_id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 500);
	return venture_database_find(self->database, query, error);
}

/* --- The chat channel of the inbox ------------------------------------------------------ */

static gboolean
chat_list(VentureContext *context, VentureEntity *contact, GDateTime *since, guint limit, GPtrArray *items, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INBOX_CHAT_MESSAGE);
	g_autoptr(GPtrArray) rows = NULL;
	const VentureInboxSource *self = venture_inbox_source_lookup("chat");
	guint i;
	(void)since;
	venture_query_set_organization(query, venture_entity_get_organization_id(contact));
	venture_query_add_filter_int(query, "contact-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(contact), NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, limit);
	rows = venture_database_find(database, query, error);
	if (rows == NULL)
		return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) when = NULL;
		gboolean inbound = FALSE;
		g_object_get(row, "sent-at", &when, "inbound", &inbound, NULL);
		g_ptr_array_add(items, venture_inbox_item_new(self, row, when, inbound));
	}
	return TRUE;
}

static gboolean
chat_observe(VentureEntity *record, gint64 *contact_id, GDateTime **at, gboolean *inbound)
{
	if (!VENTURE_IS_INBOX_CHAT_MESSAGE(record))
		return FALSE;
	g_object_get(record, "contact-id", contact_id, "sent-at", at, "inbound", inbound, NULL);
	return TRUE;
}

static void
chat_render(VentureContext *context, const VentureInboxItem *item, GString *html)
{
	g_autofree gchar *body = string(item->record, "body"), *author = string(item->record, "author");
	(void)context;
	g_string_append(html, "<div class=\"inbox-text\">");
	venture_html_escape_append(html, body);
	g_string_append(html, "</div>");
	if (!item->inbound && !venture_string_is_empty(author))
	{
		g_string_append(html, "<span class=\"inbox-by\">");
		venture_html_escape_append(html, author);
		g_string_append(html, "</span>");
	}
}

static gchar *
chat_summary(VentureContext *context, const VentureInboxItem *item)
{
	(void)context;
	return string(item->record, "body");
}

static gchar *
chat_reply(VentureContext *context, VentureEntity *contact, const VentureInboxItem *answering, const gchar *body,
	GDateTime *now, const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	VentureInboxService *self = venture_inbox_service_get(database);
	g_autoptr(VentureEntity) session = NULL, message = NULL;
	(void)contact;
	if (answering == NULL)
	{
		refuse(error, VENTURE_ERROR_VALIDATION, "They have not written by web chat, so there is no chat to answer in");
		return NULL;
	}
	session = venture_database_get(database, VENTURE_TYPE_INBOX_CHAT_SESSION, number(answering->record, "session-id"), error);
	if (session == NULL)
	{
		if (error != NULL && *error == NULL)
			refuse(error, VENTURE_ERROR_NOT_FOUND, "That chat is gone");
		return NULL;
	}
	message = venture_inbox_chat_answer(self, session, body, actor != NULL ? actor->name : NULL, now, actor, error);
	if (message == NULL)
		return NULL;
	return g_strdup("Sent to their chat; they see it on their chat page");
}

static const VentureInboxSource chat_source = {
	"chat", "Web chat", "inbox", chat_list, chat_observe, chat_render, chat_summary, chat_reply, NULL
};
VENTURE_INBOX_SOURCE(chat_source)
