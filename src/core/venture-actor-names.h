/*
 * venture-actor-names.h - Who an "API token #N" belongs to, for a reader
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A token acts as "API token #N" and that string is what the audit log,
 * the inbox, a confirmation and a webhook store and emit: it is a key
 * other programs match on, so it never changes. A person reading the
 * history wants to know whose token it was. This resolves the owner at
 * render time, for the reader in hand, and every surface that shows an
 * actor to a person asks here rather than looking the token up itself.
 */

#ifndef VENTURE_ACTOR_NAMES_H
#define VENTURE_ACTOR_NAMES_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VENTURE_ACTOR_NAMES_TOKEN_PREFIX:
 *
 * What every token actor string starts with; the token's number follows.
 */
#define VENTURE_ACTOR_NAMES_TOKEN_PREFIX "API token #"

/**
 * VentureActorNames:
 *
 * One page's worth of token-owner lookups, for one reader. The reader is
 * the principal of the access scope current when it was made; each
 * distinct token is resolved at most once, however many rows name it.
 */
typedef struct _VentureActorNames VentureActorNames;

/**
 * venture_actor_names_new:
 * @database: the repository
 *
 * Starts a cache for one render. Make it inside the reader's access scope
 * (any request handler is) and drop it when the page or response is
 * built: it holds what the reader was allowed to learn at that moment.
 * Nothing is read until a token actor is asked about.
 *
 * Returns: (transfer full): a new cache
 */
VentureActorNames *
venture_actor_names_new(VentureDatabase *database);

/**
 * venture_actor_names_free:
 * @self: (nullable): a cache
 *
 * Releases the cache.
 */
void
venture_actor_names_free(VentureActorNames *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureActorNames, venture_actor_names_free)

/**
 * venture_actor_names_parse_token:
 * @actor: (nullable): an actor string
 *
 * The token number in an actor string that is exactly "API token #N".
 * The prefix alone ("API token", what an ambiguous upgrade left) and a
 * name that merely contains it are not a token.
 *
 * Returns: the token's id, or 0 when @actor is not a token actor
 */
gint64
venture_actor_names_parse_token(const gchar *actor);

/**
 * venture_actor_names_lookup:
 * @self: a cache
 * @actor: (nullable): an actor string, as stored
 *
 * The username of the account that owns the token @actor names, when the
 * reader may learn it. %NULL for an actor that is not a token, a token
 * or owner that no longer exists, and a reader who shares no
 * organization with the owner.
 *
 * Returns: (transfer none) (nullable): the owner's username, owned by @self
 */
const gchar *
venture_actor_names_lookup(
	VentureActorNames	*self,
	const gchar		*actor
);

/**
 * venture_actor_names_format:
 * @actor: (nullable): an actor string, as stored
 * @username: (nullable): its owner's username, from
 *   venture_actor_names_lookup() or an API row's username field
 *
 * The one way an actor reads to a person: "erin (API token #4)" when the
 * owner is known, else @actor unchanged.
 *
 * Returns: (transfer full) (nullable): the text to show; %NULL for a
 *   %NULL @actor
 */
gchar *
venture_actor_names_format(
	const gchar	*actor,
	const gchar	*username
);

/**
 * venture_actor_names_display:
 * @self: a cache
 * @actor: (nullable): an actor string, as stored
 *
 * venture_actor_names_lookup() and venture_actor_names_format() together.
 *
 * Returns: (transfer full) (nullable): the text to show; %NULL for a
 *   %NULL @actor
 */
gchar *
venture_actor_names_display(
	VentureActorNames	*self,
	const gchar		*actor
);

/**
 * venture_actor_names_display_lead:
 * @self: a cache
 * @actor: (nullable): the actor string a sentence was written about
 * @text: (nullable): a sentence that may start with @actor, such as an
 *   inbox title ("API token #4 updated Bellhaven Books")
 *
 * @text with a leading @actor read as venture_actor_names_display()
 * reads it. Only the head is touched, so a record label that happens to
 * contain the same words is left alone.
 *
 * Returns: (transfer full) (nullable): the text to show; %NULL for a
 *   %NULL @text
 */
gchar *
venture_actor_names_display_lead(
	VentureActorNames	*self,
	const gchar		*actor,
	const gchar		*text
);

/**
 * venture_actor_names_may_see_user:
 * @self: a cache
 * @user_id: the account the reader would be shown
 *
 * Whether the reader may be shown who @user_id is -- their name, and so
 * their picture. The rule is the one that decides whether a token's owner
 * is named: a global owner or administrator, the account itself, anybody
 * signed in when organization access is off, or an active member of an
 * organization @user_id is also an active member of. An unauthenticated
 * reader and trusted internal work are told nothing.
 *
 * `GET /avatar/:id/:size` answers by this, so a picture is served to
 * exactly the readers a name is shown to.
 *
 * Returns: %TRUE when the reader may see @user_id
 */
gboolean
venture_actor_names_may_see_user(
	VentureActorNames	*self,
	gint64			 user_id
);

/**
 * venture_actor_names_avatar_version:
 * @self: a cache
 * @user_id: the account whose picture is wanted
 *
 * The version of @user_id's profile picture, when they have one and the
 * reader may see them (venture_actor_names_may_see_user()). Read once
 * per user per cache, however many rows name the same person.
 *
 * Returns: (transfer none) (nullable): the version, owned by @self; %NULL
 *   for no picture, an account that does not exist or was deleted, and a
 *   reader who may not see it
 */
const gchar *
venture_actor_names_avatar_version(
	VentureActorNames	*self,
	gint64			 user_id
);

G_END_DECLS

#endif /* VENTURE_ACTOR_NAMES_H */
