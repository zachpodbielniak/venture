/*
 * venture-comment.h - A discussion on any record
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every record type takes comments unless it opted out
 * (venture_entity_class_set_commentable()). A comment names its record by
 * type and id, replies one level deep, is written in markdown, and may
 * name people with @username -- who are told in their inbox, but only if
 * they may read the record, because an inbox line about a record is a
 * piece of that record.
 *
 * Who may do what is the record's own rule, applied by the access policy:
 * anybody who may read a record may read and write its comments, the
 * author alone may edit one, and the author or an organization owner or
 * administrator may delete one. The save validator installed here holds
 * every writer -- this service, the API, an approved change, a plugin --
 * to the subject existing, the organization being the subject's and the
 * author being whoever the write is made as.
 */

#ifndef VENTURE_COMMENT_H
#define VENTURE_COMMENT_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_COMMENT_MAX_LENGTH:
 *
 * The longest comment body, in bytes. A comment is a message, not a
 * document; a knowledge-base article is where a document goes.
 */
#define VENTURE_COMMENT_MAX_LENGTH (20000)

/**
 * venture_comment_install:
 * @context: the wiring
 *
 * Registers the save validator that holds every comment to its record.
 * Called once by the context; a second context over the same database adds
 * nothing.
 */
void
venture_comment_install(VentureContext *context);

/**
 * venture_comment_type_accepts:
 * @type: a record type
 * @error: (out) (optional): why not
 *
 * Whether records of @type take comments: a registered, commentable type
 * that is not workspace administration.
 *
 * Returns: %TRUE if a comment may be made on one
 */
gboolean
venture_comment_type_accepts(
	GType		  type,
	GError		**error
);

/**
 * venture_comment_subject_accepts:
 * @database: the database
 * @subject: a record
 *
 * Whether this record, rather than its type, takes comments: its type
 * does, it is not deleted, and it is not somebody's private record --
 * a comment's inbox lines and webhooks are shared with the organization,
 * and a private record's must not be.
 *
 * Returns: %TRUE if the panel and the composer belong on its page
 */
gboolean
venture_comment_subject_accepts(
	VentureDatabase	*database,
	VentureEntity	*subject
);

/**
 * venture_comment_create:
 * @context: the wiring
 * @principal: who is writing; the author is taken from here, never from
 *   the request
 * @subject_type: the record type, e.g. "invoice"
 * @subject_id: the record
 * @parent_id: the comment this answers, or 0 for a new thread
 * @body: the markdown
 * @error: (out) (optional): return location for a #GError
 *
 * Writes a comment. A record the principal may not read is not found,
 * exactly as its page would be.
 *
 * Returns: (transfer full) (nullable): the saved comment
 */
VentureEntity *
venture_comment_create(
	VentureContext			 *context,
	const VentureAuthPrincipal	 *principal,
	const gchar			 *subject_type,
	gint64				  subject_id,
	gint64				  parent_id,
	const gchar			 *body,
	GError				**error
);

/**
 * venture_comment_edit:
 * @context: the wiring
 * @principal: who is editing; only the author may
 * @comment_id: the comment
 * @body: the new markdown
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the saved comment
 */
VentureEntity *
venture_comment_edit(
	VentureContext			 *context,
	const VentureAuthPrincipal	 *principal,
	gint64				  comment_id,
	const gchar			 *body,
	GError				**error
);

/**
 * venture_comment_delete:
 * @context: the wiring
 * @principal: the author, or an owner or administrator
 * @comment_id: the comment
 * @error: (out) (optional): return location for a #GError
 *
 * Deletes a comment the way every record is deleted: softly, so it can be
 * restored. A deleted comment that has replies stays in its thread as a
 * placeholder, because the replies answer something.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_comment_delete(
	VentureContext			 *context,
	const VentureAuthPrincipal	 *principal,
	gint64				  comment_id,
	GError				**error
);

/**
 * venture_comment_get:
 * @context: the wiring
 * @principal: (nullable): who is asking; %NULL is trusted internal work
 * @comment_id: the comment
 * @error: (out) (optional): return location for a #GError
 *
 * A comment the principal may read, deleted or not.
 *
 * Returns: (transfer full) (nullable): the comment
 */
VentureEntity *
venture_comment_get(
	VentureContext			 *context,
	const VentureAuthPrincipal	 *principal,
	gint64				  comment_id,
	GError				**error
);

/**
 * venture_comment_get_subject:
 * @context: the wiring
 * @comment: a comment
 * @error: (out) (optional): return location for a #GError
 *
 * The record a comment is on, read under whatever scope is current.
 *
 * Returns: (transfer full) (nullable): the record
 */
VentureEntity *
venture_comment_get_subject(
	VentureContext	 *context,
	VentureEntity	 *comment,
	GError		**error
);

/**
 * venture_comment_thread:
 * @context: the wiring
 * @principal: (nullable): who is reading; %NULL is trusted internal work
 * @subject_type: the record type
 * @subject_id: the record
 * @error: (out) (optional): return location for a #GError
 *
 * A record's discussion as JSON: an array of top-level comments, oldest
 * first, each with its "replies" in order. Every comment is the shape of
 * venture_comment_to_json(). A deleted comment with replies is kept as a
 * placeholder with no body; one without is left out.
 *
 * Returns: (transfer full) (nullable): the thread, or %NULL when the
 *   record cannot be read
 */
JsonNode *
venture_comment_thread(
	VentureContext			 *context,
	const VentureAuthPrincipal	 *principal,
	const gchar			 *subject_type,
	gint64				  subject_id,
	GError				**error
);

/**
 * venture_comment_count:
 * @context: the wiring
 * @subject_type: the record type
 * @subject_id: the record
 *
 * Returns: how many live comments the record has, replies included
 */
gint64
venture_comment_count(
	VentureContext	*context,
	const gchar	*subject_type,
	gint64		 subject_id
);

/**
 * venture_comment_to_json:
 * @context: the wiring
 * @principal: (nullable): who is reading, for can_edit and can_delete
 * @comment: a comment
 *
 * One comment for the API: who wrote it and when, its markdown and its
 * rendered HTML (both %NULL once deleted), what it is on, its permalink
 * and whether the reader may edit or delete it.
 *
 * Returns: (transfer full): the object
 */
JsonNode *
venture_comment_to_json(
	VentureContext			*context,
	const VentureAuthPrincipal	*principal,
	VentureEntity			*comment
);

/**
 * venture_comment_render:
 * @context: the wiring
 * @subject: the record the text is about
 * @body: (nullable): markdown
 * @mentions: (nullable) (array zero-terminated=1): the usernames that are
 *   chips; %NULL to resolve them now, as a preview does
 *
 * The HTML of a comment body. @mentions become chips; #type/id references
 * become links only to records the current scope may read, so the same
 * text renders a link for one reader and plain text for another.
 *
 * Returns: (transfer full): the HTML
 */
gchar *
venture_comment_render(
	VentureContext	*context,
	VentureEntity	*subject,
	const gchar	*body,
	const gchar *const *mentions
);

/**
 * venture_comment_resolve_mentions:
 * @context: the wiring
 * @subject: the record the text is about
 * @body: (nullable): markdown
 *
 * The @usernames in @body -- outside code -- that name an active user who
 * may read @subject, each once, in order. A name that is nobody, or
 * somebody who may not read the record, is left out: it renders as text
 * and tells nobody anything.
 *
 * Returns: (transfer full): the usernames, possibly empty
 */
GStrv
venture_comment_resolve_mentions(
	VentureContext	*context,
	VentureEntity	*subject,
	const gchar	*body
);

/**
 * venture_comment_user_can_read:
 * @context: the wiring
 * @user_id: an account
 * @subject: a record
 *
 * Whether an account other than the caller's may read a record, under the
 * same policy its own requests would meet. Inactive and deleted accounts
 * may read nothing.
 *
 * Returns: %TRUE if it may
 */
gboolean
venture_comment_user_can_read(
	VentureContext	*context,
	gint64		 user_id,
	VentureEntity	*subject
);

/**
 * venture_comment_mention_candidates:
 * @context: the wiring
 * @subject: the record being discussed
 * @prefix: (nullable): the start of a username or display name
 * @limit: at most this many
 *
 * Who the composer offers after an @: active users who may read @subject,
 * matching @prefix, by username. Nobody who may not read the record is
 * offered, so the list says nothing about anybody outside it.
 *
 * Returns: (transfer full): a JSON array of {username, name, initials}
 */
JsonNode *
venture_comment_mention_candidates(
	VentureContext	*context,
	VentureEntity	*subject,
	const gchar	*prefix,
	guint		 limit
);

/**
 * venture_comment_author_name:
 * @context: the wiring
 * @comment: a comment
 *
 * Who wrote it, as a person reads it: the account's display name, else its
 * username, else the actor string it was written under.
 *
 * Returns: (transfer full): the name
 */
gchar *
venture_comment_author_name(
	VentureContext	*context,
	VentureEntity	*comment
);

/**
 * venture_comment_permalink:
 * @comment_id: a comment
 *
 * The comment's stable address, "/comments/ID", which answers with the
 * record's page at the comment's anchor for whoever may read the record.
 *
 * Returns: (transfer full): the path
 */
gchar *
venture_comment_permalink(gint64 comment_id);

/**
 * venture_comment_anchor_url:
 * @comment: a comment
 *
 * "/e/TYPE/ID#comment-N": the record's page, at the comment.
 *
 * Returns: (transfer full): the path
 */
gchar *
venture_comment_anchor_url(VentureEntity *comment);

/**
 * venture_comment_excerpt:
 * @comment: a comment
 * @max_chars: at most this many characters
 *
 * The comment's words on one line, its markdown taken away: what an inbox
 * line and an activity entry show.
 *
 * Returns: (transfer full): the text
 */
gchar *
venture_comment_excerpt(
	VentureEntity	*comment,
	gsize		 max_chars
);

G_END_DECLS

#endif /* VENTURE_COMMENT_H */
