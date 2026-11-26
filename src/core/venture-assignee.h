/*
 * venture-assignee.h - Whom a record may be handed to
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A ticket's assignee is a username in a string, and a string takes a
 * typo as readily as a name: "oxpeker" saved, the inbox resolved it to
 * nobody, and whatever lists tickets by exact username never saw it. A
 * field flagged %VENTURE_COLUMN_FLAG_MEMBER_USERNAME is held to the
 * people who can actually take the work -- the active members of the
 * record's organization -- by one save validator, on every writer, and
 * the form offers those people instead of a text box.
 *
 * The wire format does not change: the value is still the username.
 */

#ifndef VENTURE_ASSIGNEE_H
#define VENTURE_ASSIGNEE_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * VENTURE_ASSIGNEE_PLACEHOLDER_ME:
 *
 * What a template field flagged %VENTURE_COLUMN_FLAG_MEMBER_PLACEHOLDER
 * may hold instead of a username: whoever applies the template.
 */
#define VENTURE_ASSIGNEE_PLACEHOLDER_ME "{me}"

/**
 * VENTURE_ASSIGNEE_MEMBERS_MAX:
 *
 * The most members one call to venture_assignee_list_members() returns.
 */
#define VENTURE_ASSIGNEE_MEMBERS_MAX (500)

/**
 * venture_assignee_is_member:
 * @database: the database
 * @organization_id: the organization the record belongs to
 * @username: (nullable): the name written
 *
 * Whether @username may be given work in @organization_id: an active,
 * undeleted user who holds an active membership of it. Two cases need no
 * membership row. With organization access off there are no memberships
 * at all and every active user reads every organization, so every active
 * user counts. Outside a hosted install an owner or administrator reads
 * every organization by role, so they count everywhere too.
 *
 * Read under a trusted scope: whether somebody is a member is a fact
 * about the organization, not about who is asking.
 *
 * Returns: %TRUE when @username may be assigned there
 */
gboolean
venture_assignee_is_member(
	VentureDatabase	*database,
	gint64		 organization_id,
	const gchar	*username
);

/**
 * venture_assignee_check:
 * @database: the database
 * @organization_id: the organization the record belongs to
 * @field: the field's wire name, for the refusal ("assignee")
 * @value: (nullable): what the field is being set to
 * @flags: the field's column flags; %VENTURE_COLUMN_FLAG_MEMBER_USERNAMES
 *   reads @value as a comma-separated list, and
 *   %VENTURE_COLUMN_FLAG_MEMBER_PLACEHOLDER lets "{me}" through
 * @error: (out) (optional): return location for a #GError
 *
 * The rule for one value. Empty (or, for a list, nothing but commas and
 * spaces) means nobody and is allowed. Anything else must name members
 * (venture_assignee_is_member()); the first one that does not is refused
 * with %VENTURE_ERROR_VALIDATION, as "@field: "name" is not an active
 * member of <organization>" -- the field and the value, which is what
 * the API's 422 body carries.
 *
 * Returns: %TRUE when @value may be written
 */
gboolean
venture_assignee_check(
	VentureDatabase		 *database,
	gint64			  organization_id,
	const gchar		 *field,
	const gchar		 *value,
	VentureColumnFlags	  flags,
	GError			**error
);

/**
 * venture_assignee_list_members:
 * @database: the database
 * @organization_id: the organization
 * @search: (nullable): text to match, case-insensitively, anywhere in the
 *   username or the display name; %NULL or empty matches everybody
 * @offset: how many matches to skip
 * @limit: the most to return; 0 or more than %VENTURE_ASSIGNEE_MEMBERS_MAX
 *   means %VENTURE_ASSIGNEE_MEMBERS_MAX
 * @out_total: (out) (optional): how many match in all
 * @error: (out) (optional): return location for a #GError
 *
 * The people venture_assignee_is_member() accepts for @organization_id,
 * ordered by username. This and the validator are one definition: the
 * picker never offers a name the save would refuse.
 *
 * Returns: (transfer full) (element-type VentureUser) (nullable): the
 *   users, or %NULL with @error set
 */
GPtrArray *
venture_assignee_list_members(
	VentureDatabase	 *database,
	gint64		  organization_id,
	const gchar	 *search,
	guint		  offset,
	guint		  limit,
	guint		 *out_total,
	GError		**error
);

/**
 * venture_assignee_install:
 * @context: the wiring
 *
 * Installs the save validator that holds every field flagged
 * %VENTURE_COLUMN_FLAG_MEMBER_USERNAME to venture_assignee_check(), on
 * every record type, whichever door wrote it. A field is judged only when
 * it is written -- on insert, or when its value or the record's
 * organization changed -- so a row whose stored name has gone stale (a
 * typo from before the rule, a member who left) still takes edits to its
 * other fields. Called once by the context; a second context over the
 * same database adds nothing.
 */
void
venture_assignee_install(VentureContext *context);

G_END_DECLS

#endif /* VENTURE_ASSIGNEE_H */
