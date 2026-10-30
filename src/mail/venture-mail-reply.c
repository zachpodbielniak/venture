/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-mail-reply.c - Replying to a synced message from the timeline
 *
 * The message answered is the mail_inbound row the sync filed for the
 * interaction; its Message-ID and thread root become In-Reply-To and
 * References. Everything after that is the outbox's: it sends, retries,
 * and once the relay has the reply records it on the contact's timeline
 * as outbound (venture_mail_sync_record_outbound()). Nothing here sends.
 */
#include <venture.h>
#include <string.h>

static gboolean
reply_refuse(GError **error, const gchar *message)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, message);
	return FALSE;
}

VentureEntity *
venture_mail_reply_source(VentureDatabase *database, VentureEntity *interaction)
{
	g_autoptr(VentureQuery) query = NULL;
	gint kind = 0;
	gboolean outbound = FALSE;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	if (!VENTURE_IS_INTERACTION(interaction) || !venture_entity_is_persisted(interaction) ||
		!venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_inbound"))
		return NULL;
	g_object_get(interaction, "kind", &kind, "outbound", &outbound, NULL);
	if (kind != VENTURE_INTERACTION_KIND_EMAIL || outbound)
		return NULL;
	query = venture_query_new(VENTURE_TYPE_MAIL_INBOUND);
	venture_query_set_organization(query, venture_entity_get_organization_id(interaction));
	venture_query_add_filter_int(query, "interaction-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(interaction), NULL);
	venture_query_add_filter_int(query, "duplicate-of-id", VENTURE_FILTER_OP_EQ, 0, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 1);
	return venture_database_find_one(database, query, NULL);
}

/* "<id>" from a Message-ID stored with or without its brackets, or NULL
 * when it holds anything that could end the header or add another. */
static gchar *
reply_angle(const gchar *id)
{
	g_autofree gchar *bare = NULL;
	gsize length;
	if (venture_string_is_empty(id))
		return NULL;
	bare = g_strstrip(g_strdup(id));
	length = strlen(bare);
	if (length >= 2 && bare[0] == '<' && bare[length - 1] == '>')
	{
		memmove(bare, bare + 1, length - 2);
		bare[length - 2] = '\0';
	}
	if (*bare == '\0' || strpbrk(bare, "<> \t\r\n,") != NULL || strchr(bare, '@') == NULL || strlen(bare) > 900)
		return NULL;
	return g_strdup_printf("<%s>", bare);
}

static gchar *
reply_line(const gchar *text, gsize limit)
{
	g_autofree gchar *valid = g_utf8_make_valid(text != NULL ? text : "", -1);
	gchar *p;
	for (p = valid; *p; p++)
		if (*p == '\r' || *p == '\n' || *p == '\t')
			*p = ' ';
	g_strstrip(valid);
	if (strlen(valid) > limit)
		valid[limit] = '\0';
	return g_utf8_make_valid(valid, -1);
}

VentureMailMessage *
venture_mail_reply_enqueue(VentureContext *context, VentureEntity *interaction,
	const gchar *body, const VentureActor *actor, GError **error)
{
	VentureDatabase *database;
	VentureMailOutbox *outbox;
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureEntity) account = NULL;
	g_autoptr(VentureMailMessage) message = NULL;
	g_autofree gchar *trimmed = NULL, *subject = NULL, *clean_subject = NULL, *reply_subject = NULL;
	g_autofree gchar *message_id = NULL, *thread_id = NULL, *from = NULL, *in_reply_to = NULL, *root = NULL;
	g_autofree gchar *references = NULL, *key = NULL, *uuid = NULL;
	g_autofree gchar *recipient = NULL, *prefix = NULL, *prefix_key = NULL;
	gint64 account_id = 0;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(interaction), NULL);
	database = venture_context_get_database(context);
	trimmed = g_strstrip(g_strdup(body != NULL ? body : ""));
	if (*trimmed == '\0')
	{
		reply_refuse(error, "Write the reply before sending it");
		return NULL;
	}
	if (strlen(trimmed) > VENTURE_MAIL_REPLY_MAX_BODY || !g_utf8_validate(trimmed, -1, NULL))
	{
		reply_refuse(error, "A reply is at most 64 KiB of text");
		return NULL;
	}
	source = venture_mail_reply_source(database, interaction);
	if (source == NULL)
	{
		reply_refuse(error, "Only a message that arrived by mail can be answered from the timeline");
		return NULL;
	}
	g_object_get(source, "message-id", &message_id, "thread-id", &thread_id, "from-address", &from,
		"subject", &subject, "account-id", &account_id, "recipient-address", &recipient, NULL);
	in_reply_to = reply_angle(message_id);
	if (in_reply_to == NULL || venture_string_is_empty(from) || strpbrk(from, "\r\n,;<> ") != NULL || strchr(from, '@') == NULL)
	{
		reply_refuse(error, "This message's sender or Message-ID cannot be answered safely");
		return NULL;
	}
	root = reply_angle(thread_id);
	references = root != NULL && g_strcmp0(root, in_reply_to) != 0 ? g_strdup_printf("%s %s", root, in_reply_to) : g_strdup(in_reply_to);
	clean_subject = reply_line(subject, 900);
	reply_subject = g_ascii_strncasecmp(clean_subject, "re:", 3) == 0 ? g_strdup(clean_subject) : g_strdup_printf("Re: %s", clean_subject);
	outbox = venture_database_get_mail_outbox(database);
	if (outbox == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Mail is off, so nothing can be sent");
		return NULL;
	}
	if (recipient) {
		if (account_id > 0) account = venture_database_get(database, VENTURE_TYPE_MAIL_ACCOUNT, account_id, NULL);
		if (account && venture_entity_get_organization_id(account) == venture_entity_get_organization_id(source))
			g_object_get(account, "reply-alias-prefix", &prefix, NULL);
		/* Sync stores the address in lower case. A prefix typed in mixed
		 * case must still match, or the reply drops Reply-To. */
		prefix_key = prefix != NULL ? g_ascii_strdown(prefix, -1) : NULL;
		if (!prefix_key || !g_str_has_prefix(recipient, prefix_key) || !g_str_has_suffix(recipient, "@lightsite.app") ||
			strpbrk(recipient, "\r\n\t ,;<>")) g_clear_pointer(&recipient, g_free);
	}
	/* Each reply is its own message; the key only stops a resubmitted
	 * request from queueing the same reply twice within one call. */
	uuid = g_uuid_string_random();
	key = g_strdup_printf("reply:%s:%s", venture_entity_get_uuid(interaction), uuid);
	message = venture_mail_message_new();
	g_object_set(message, "organization-id", venture_entity_get_organization_id(interaction), "to", from,
		"subject", reply_subject, "text-body", trimmed, "in-reply-to", in_reply_to, "reply-references", references,
		"reply-to", recipient,
		"related-type", "interaction", "related-id", venture_entity_get_id(interaction), "idempotency-key", key, NULL);
	return venture_mail_outbox_enqueue(outbox, message, actor, error);
}
