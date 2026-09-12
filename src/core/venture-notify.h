/*
 * venture-notify.h - Watches, mentions and the inbox
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A person is told about what they follow, what is handed to them, and
 * where they are named. The three are one mechanism: a notification row
 * per person per event, written by the audit trail's own signal so that
 * every door -- form, API, CLI, automation, assistant -- tells the same
 * people the same things. Nothing here sends mail; the inbox is the
 * delivery, and an automation rule can forward it anywhere.
 */

#ifndef VENTURE_NOTIFY_H
#define VENTURE_NOTIFY_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * venture_notify_install:
 * @context: the wiring
 *
 * Connects the inbox to the database's audit signal, so that from now on
 * a change to a watched record, a comment naming somebody, and a ticket
 * handed over each put a row in the right inbox. Called once, by the
 * context, after the database is open.
 */
void
venture_notify_install(VentureContext *context);

/**
 * venture_notify_watch:
 * @context: the wiring
 * @user_id: who is watching
 * @target_type: the record type, e.g. ticket
 * @target_id: the record
 * @error: (out) (optional): return location for a #GError
 *
 * Starts following a record. Watching twice is not an error and leaves
 * one row. The record must exist and its type must be registered.
 *
 * Returns: %TRUE if the user is now watching
 */
gboolean
venture_notify_watch(
	VentureContext	 *context,
	gint64		  user_id,
	const gchar	 *target_type,
	gint64		  target_id,
	GError		**error
);

/**
 * venture_notify_unwatch:
 * @context: the wiring
 * @user_id: who
 * @target_type: the record type
 * @target_id: the record
 * @error: (out) (optional): return location for a #GError
 *
 * Stops following a record. Not watching is not an error.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_notify_unwatch(
	VentureContext	 *context,
	gint64		  user_id,
	const gchar	 *target_type,
	gint64		  target_id,
	GError		**error
);

/**
 * venture_notify_is_watching:
 * @context: the wiring
 * @user_id: who
 * @target_type: the record type
 * @target_id: the record
 *
 * Returns: %TRUE if the user follows the record
 */
gboolean
venture_notify_is_watching(
	VentureContext	*context,
	gint64		 user_id,
	const gchar	*target_type,
	gint64		 target_id
);

/**
 * venture_notify_list_watchers:
 * @context: the wiring
 * @target_type: the record type
 * @target_id: the record
 *
 * Everybody following a record.
 *
 * Returns: (transfer full) (element-type gint64): the user ids
 */
GArray *
venture_notify_list_watchers(
	VentureContext	*context,
	const gchar	*target_type,
	gint64		 target_id
);

/**
 * venture_notify_list_watched:
 * @context: the wiring
 * @user_id: who
 * @limit: at most this many, newest first; 0 for all
 *
 * Returns: (transfer container) (element-type VentureEntity) (nullable):
 *   the user's watches
 */
GPtrArray *
venture_notify_list_watched(
	VentureContext	*context,
	gint64		 user_id,
	guint		 limit
);

/**
 * venture_notify_send:
 * @context: the wiring
 * @user_id: who it is for
 * @kind: why
 * @title: the one line the inbox shows
 * @body: (nullable): the detail
 * @target_type: (nullable): what it is about
 * @target_id: the record, or 0
 * @target_label: (nullable): what the record is called
 * @actor: (nullable): who caused it
 * @error: (out) (optional): return location for a #GError
 *
 * Puts one notification in one inbox. A notification to user 0 is dropped
 * silently, because most callers resolve a username that may not be a
 * user and would otherwise have to check first.
 *
 * Returns: %TRUE if written or dropped, %FALSE on a database error
 */
gboolean
venture_notify_send(
	VentureContext		 *context,
	gint64			  user_id,
	VentureNotificationKind	  kind,
	const gchar		 *title,
	const gchar		 *body,
	const gchar		 *target_type,
	gint64			  target_id,
	const gchar		 *target_label,
	const gchar		 *actor,
	GError			**error
);

/**
 * venture_notify_broadcast:
 * @context: the wiring
 * @minimum_role: the least role that should hear it; owner and admin for
 *   a budget, everybody for an outage
 * @kind: why
 * @title: the one line
 * @body: (nullable): the detail
 * @target_type: (nullable): what it is about
 * @target_id: the record, or 0
 * @target_label: (nullable): what the record is called
 * @actor: (nullable): who caused it
 * @error: (out) (optional): return location for a #GError
 *
 * Tells every active user holding at least @minimum_role.
 *
 * Returns: how many inboxes it reached, or -1 on error
 */
gint
venture_notify_broadcast(
	VentureContext		 *context,
	VentureUserRole		  minimum_role,
	VentureNotificationKind	  kind,
	const gchar		 *title,
	const gchar		 *body,
	const gchar		 *target_type,
	gint64			  target_id,
	const gchar		 *target_label,
	const gchar		 *actor,
	GError			**error
);

/**
 * venture_notify_unread_count:
 * @context: the wiring
 * @user_id: whose inbox
 *
 * Returns: how many are unread
 */
gint64
venture_notify_unread_count(
	VentureContext	*context,
	gint64		 user_id
);

/**
 * venture_notify_list:
 * @context: the wiring
 * @user_id: whose inbox
 * @unread_only: leave out what has been read
 * @limit: at most this many, newest first; 0 for the default of 50
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer container) (element-type VentureEntity) (nullable):
 *   the notifications
 */
GPtrArray *
venture_notify_list(
	VentureContext	 *context,
	gint64		  user_id,
	gboolean	  unread_only,
	guint		  limit,
	GError		**error
);

/**
 * venture_notify_mark_read:
 * @context: the wiring
 * @user_id: whose inbox
 * @notification_id: one notification, or 0 for every unread one
 * @error: (out) (optional): return location for a #GError
 *
 * Marks read. A notification belonging to somebody else is not found,
 * never forbidden: the inbox is private, and "you may not read that" says
 * there is something to read.
 *
 * Returns: how many changed, or -1 on error
 */
gint
venture_notify_mark_read(
	VentureContext	 *context,
	gint64		  user_id,
	gint64		  notification_id,
	GError		**error
);

/**
 * venture_notify_extract_mentions:
 * @text: (nullable): a comment
 *
 * The usernames written as @name in a text, each once, in order of first
 * appearance. A name is letters, digits, dots, dashes and underscores; a
 * trailing dot is punctuation, not part of the name.
 *
 * Returns: (transfer full): the names, possibly empty
 */
GStrv
venture_notify_extract_mentions(const gchar *text);

/**
 * venture_notify_user_id_for_username:
 * @context: the wiring
 * @username: (nullable): a username, as a ticket's assignee holds one
 *
 * Returns: the user's id, or 0 if no active user has that name
 */
gint64
venture_notify_user_id_for_username(
	VentureContext	*context,
	const gchar	*username
);

/**
 * venture_notify_to_json:
 * @notification: a notification
 *
 * The notification as the API and the assistant see it: id, kind, title,
 * body, actor, target, whether read, when.
 *
 * Returns: (transfer full): a JSON object
 */
JsonNode *
venture_notify_to_json(VentureEntity *notification);

G_END_DECLS

#endif /* VENTURE_NOTIFY_H */
