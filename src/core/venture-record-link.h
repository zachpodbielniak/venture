/*
 * venture-record-link.h - Linking any record to any other
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A #VentureRecordLink names both of its ends by type and id, so it can
 * join a release to the tickets it shipped, an incident to the deployment
 * that caused it, or a plugin's record to anything at all. That shape has
 * no database constraint behind it, which is why the checks live here and
 * run as a save validator: every writer goes through venture_database_save(),
 * so every writer -- the form, the API, an approved staged change, the AI
 * -- gets them.
 *
 * A link is stored once and read from either end. Standing on the source
 * it means its kind; standing on the target it means the inverse. The
 * functions that resolve a link therefore take the record you are standing
 * on, and answer in terms of "the other end".
 */

#ifndef VENTURE_RECORD_LINK_H
#define VENTURE_RECORD_LINK_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * venture_record_link_create:
 * @database: the database
 * @source_type: the registered entity name of the source, e.g. `release`
 * @source_id: the source's id
 * @kind: what the link means, read from the source
 * @target_type: the registered entity name of the target
 * @target_id: the target's id
 * @note: (nullable): why, in a few words
 * @error: (out) (optional): return location for a #GError
 *
 * Builds a link after checking it can mean something: both types must be
 * registered and on, both records must exist and not be deleted, a record
 * may not be linked to itself, and the same pair may not be linked twice
 * in either direction. Both labels are captured as they are now.
 *
 * The link inherits the source's organisation.
 *
 * Returns: (transfer full) (nullable): the unsaved link, or %NULL
 */
VentureRecordLink *
venture_record_link_create(
	VentureDatabase	 *database,
	const gchar	 *source_type,
	gint64		  source_id,
	VentureLinkKind	  kind,
	const gchar	 *target_type,
	gint64		  target_id,
	const gchar	 *note,
	GError		**error
);

/**
 * venture_record_link_install_validator:
 * @database: the database
 *
 * Registers the link checks as a save validator, so a link written by any
 * path -- including a generic `POST /api/v1/record_link` and an approved
 * staged change -- is held to the same rules as one built by
 * venture_record_link_create(). The context does this at construction.
 */
void
venture_record_link_install_validator(VentureDatabase *database);

/**
 * venture_record_link_find_for:
 * @database: the database
 * @entity_type: the registered entity name of the record
 * @entity_id: its id
 * @error: (out) (optional): return location for a #GError
 *
 * Finds every link touching a record, whichever end it is at, oldest
 * first.
 *
 * Returns: (transfer container) (element-type VentureRecordLink): the links
 */
GPtrArray *
venture_record_link_find_for(
	VentureDatabase	 *database,
	const gchar	 *entity_type,
	gint64		  entity_id,
	GError		**error
);

/**
 * venture_record_link_other_end:
 * @link: the link
 * @entity_type: the entity name of the record you are standing on
 * @entity_id: its id
 * @out_type: (out) (transfer full): the other end's entity name
 * @out_id: (out): the other end's id
 * @out_label: (out) (transfer full): the other end's stored label
 * @out_kind: (out): the kind as read from where you stand
 *
 * Reads a link from one of its ends. If the record given is the source,
 * the other end is the target and the kind is as stored; if it is the
 * target, the other end is the source and the kind is the inverse.
 *
 * Returns: %TRUE if the record is one end of the link
 */
gboolean
venture_record_link_other_end(
	VentureRecordLink	 *link,
	const gchar		 *entity_type,
	gint64			  entity_id,
	gchar			**out_type,
	gint64			 *out_id,
	gchar			**out_label,
	VentureLinkKind		 *out_kind
);

/**
 * venture_record_link_resolve:
 * @database: the database
 * @entity_type: an entity name
 * @entity_id: an id
 * @error: (out) (optional): return location for a #GError
 *
 * Loads the record at one end of a link. Returns %NULL with no error set
 * when the type is off or the record has gone, which is a normal state: the
 * link keeps its label so the page can still say what it pointed at.
 *
 * Returns: (transfer full) (nullable): the record, or %NULL
 */
VentureEntity *
venture_record_link_resolve(
	VentureDatabase	 *database,
	const gchar	 *entity_type,
	gint64		  entity_id,
	GError		**error
);

/**
 * venture_record_link_describe_for:
 * @database: the database
 * @entity_type: the entity name of the record you are standing on
 * @entity_id: its id
 * @error: (out) (optional): return location for a #GError
 *
 * Describes every link touching a record as seen from it: for each, the
 * link's id, the kind from here, the other end's type, id and current or
 * stored label, whether the other end still exists, and the note. This is
 * what `GET /api/v1/links/:type/:id`, the AI's link tool and venturectl
 * all return.
 *
 * Returns: (transfer full) (nullable): a JSON array, or %NULL on error
 */
JsonNode *
venture_record_link_describe_for(
	VentureDatabase	 *database,
	const gchar	 *entity_type,
	gint64		  entity_id,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_RECORD_LINK_H */
