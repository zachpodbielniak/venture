/*
 * venture-factory.h - The software factory's operations
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The factory's records are ordinary types; what this file holds is the
 * handful of things done *with* them that a form cannot express: drafting
 * a release's changelog from its tickets, publishing a release to the
 * forge, reading a milestone's progress, an environment's running
 * release, and the whole loop at a glance. They live here rather than in
 * the web layer so that the page, the API, venturectl and the assistant
 * all call one implementation -- a changelog the assistant drafts is the
 * changelog the button drafts.
 */

#ifndef VENTURE_FACTORY_H
#define VENTURE_FACTORY_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * venture_factory_release_tickets:
 * @database: the database
 * @release_id: the release
 *
 * The tickets marked as fixed in a release, oldest first.
 *
 * Returns: (transfer container) (element-type VentureEntity) (nullable):
 *   the tickets
 */
GPtrArray *
venture_factory_release_tickets(
	VentureDatabase	*database,
	gint64		 release_id
);

/**
 * venture_factory_draft_changelog:
 * @context: the wiring
 * @release: the release
 *
 * Drafts a changelog from the release's tickets, grouped by issue type in
 * the enum's order so bugs and stories always land in the same place.
 * Markdown, because that is what a forge renders a release body as.
 * Nothing is written; the caller decides whether to keep it.
 *
 * Returns: (transfer full): the text
 */
gchar *
venture_factory_draft_changelog(
	VentureContext	*context,
	VentureEntity	*release
);

/**
 * venture_factory_apply_changelog:
 * @context: the wiring
 * @release: the release, updated in place
 * @replace: whether to overwrite a changelog somebody already wrote
 *
 * Sets the release's changelog to a fresh draft, unless one is there and
 * @replace is %FALSE: the draft is a starting point, not the truth. The
 * release is not saved; the caller saves or stages it.
 *
 * Returns: %TRUE if the changelog was changed
 */
gboolean
venture_factory_apply_changelog(
	VentureContext	*context,
	VentureEntity	*release,
	gboolean	 replace
);

/**
 * venture_factory_publish_release:
 * @context: the wiring
 * @release: the release, updated in place with what the forge answered
 * @prerelease: whether to mark it a pre-release on the forge
 * @actor: (nullable): who is responsible
 * @error: (out) (optional): return location for a #GError
 *
 * Cuts the release on the forge and records the answer. The tag comes
 * from the record, or "v" plus the version when none is set; the body is
 * the changelog. The forge creates the tag on the default branch if it
 * does not exist, and answers with the id and the page, which are written
 * to the record so the release is recognised when its own webhook
 * arrives a moment later. Refused when the release is already on the
 * forge, names no repository, or the forge module is off -- and a
 * refusal writes nothing.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_factory_publish_release(
	VentureContext		 *context,
	VentureEntity		 *release,
	gboolean		  prerelease,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_factory_milestone_progress:
 * @database: the database
 * @milestone_id: the milestone
 * @out_total: (out): how many tickets are planned into it
 * @out_done: (out): how many of those are done or cancelled
 *
 * Counts a milestone's tickets, and how many of them are finished.
 */
void
venture_factory_milestone_progress(
	VentureDatabase	*database,
	gint64		 milestone_id,
	gint64		*out_total,
	gint64		*out_done
);

/**
 * venture_factory_current_deployment:
 * @database: the database
 * @environment_id: the environment
 *
 * The latest deployment that succeeded into an environment: what it is
 * running now.
 *
 * Returns: (transfer full) (nullable): the deployment, or %NULL
 */
VentureEntity *
venture_factory_current_deployment(
	VentureDatabase	*database,
	gint64		 environment_id
);

/**
 * venture_factory_describe:
 * @context: the wiring
 * @organization_ids: (nullable) (array length=n_organizations): the
 *   entities to scope to, or %NULL for every entity
 * @n_organizations: how many
 * @error: (out) (optional): return location for a #GError
 *
 * The loop at a glance, as JSON: open milestones with their progress, the
 * newest releases, the latest builds, each environment with the release
 * it is running, and the incidents still open. This is what
 * `GET /api/v1/factory`, the assistant's factory tool and
 * `venturectl factory` all return, and it is the same five questions the
 * Factory page asks.
 *
 * Returns: (transfer full) (nullable): a JSON object, or %NULL on error
 */
JsonNode *
venture_factory_describe(
	VentureContext	 *context,
	const gint64	 *organization_ids,
	gsize		  n_organizations,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_FACTORY_H */
