/*
 * venture-forge-rules.h - Which rule governs a ticket, and what to call its branch
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two questions with exact answers, kept away from the network so they can
 * be tested without one.
 *
 * Rule resolution decides whether anything runs at all, which makes "no rule
 * matched" a legitimate answer rather than a failure -- a repository nobody
 * enrolled must not acquire an AI because a rule elsewhere was written
 * generously.
 *
 * Branch naming has to produce something git will actually accept from a
 * title a person typed, which is a narrower target than it looks: a refname
 * may not contain a space, `..`, a control character or any of `~^:?*[\`,
 * may not begin with `-` or end in `.lock`, and a name beginning with `-`
 * would additionally be read as a flag by the git command line.
 */

#ifndef VENTURE_FORGE_RULES_H
#define VENTURE_FORGE_RULES_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * venture_forge_rule_resolve:
 * @database: the database
 * @repo_id: the repository the work belongs in, or 0 for none
 * @issue_type: the ticket's issue type
 * @error: (out) (optional): return location for a #GError
 *
 * Finds the one rule that governs a ticket, most specific first:
 *
 * 1. the repository's rule for this issue type
 * 2. the repository's catch-all rule
 * 3. the forge-wide rule for this issue type
 * 4. the forge-wide catch-all rule
 *
 * A rule that is not enabled is skipped and the search continues, so
 * switching one off falls through to whatever broader rule exists rather
 * than stopping the search. Suppressing work for one repository is done with
 * a rule that is enabled and has AI turned off.
 *
 * Returns %NULL with no error set when nothing matches. That is the answer
 * "nothing runs", not a failure.
 *
 * Returns: (transfer full) (nullable): the governing rule, or %NULL
 */
VentureForgeRule *
venture_forge_rule_resolve(
	VentureDatabase	 *database,
	gint64		  repo_id,
	VentureIssueType  issue_type,
	GError		**error
);

/**
 * venture_forge_branch_name:
 * @template_text: (nullable): the rule's template, or %NULL for the default
 * @prefix: (nullable): the repository's branch prefix
 * @issue_type: the ticket's issue type
 * @ticket_id: the ticket's id
 * @issue_number: the upstream issue number, or 0 if there is none
 * @title: (nullable): the ticket's title
 *
 * Expands a branch template into a name git will accept.
 *
 * Recognised placeholders are `{type}`, `{id}`, `{issue}` and `{slug}`. The
 * slug comes from venture_slugify(), which already yields lowercase
 * alphanumerics joined by single hyphens with no leading or trailing one --
 * so it cannot introduce a character git refuses, cannot begin with `-`, and
 * cannot be empty. It is truncated on a hyphen boundary rather than
 * mid-word, because a branch ending in half a word reads as a typo.
 *
 * The ticket id is always present in the default template. Two tickets with
 * the same title must not collide on one branch, and the id is the only
 * thing guaranteed to differ.
 *
 * Returns: (transfer full): the branch name
 */
gchar *
venture_forge_branch_name(
	const gchar	 *template_text,
	const gchar	 *prefix,
	VentureIssueType  issue_type,
	gint64		  ticket_id,
	gint64		  issue_number,
	const gchar	 *title
);

/**
 * venture_forge_refname_is_valid:
 * @name: a candidate branch name
 *
 * Whether @name is a legal git branch name and safe as a command-line
 * argument.
 *
 * Public because it is the check standing between a webhook payload and a
 * git invocation, and therefore deserves a test that does not depend on
 * git's willingness to reject it.
 *
 * Returns: %TRUE if the name may be used
 */
gboolean
venture_forge_refname_is_valid(const gchar *name);

/**
 * venture_forge_clone_url:
 * @repo_clone_url: (nullable): the repository's own clone URL, if it has one
 * @forge_clone_base: (nullable): the forge's clone base
 * @forge_base_url: (nullable): the forge's API base URL, used as a fallback
 * @full_name: the repository as `owner/repo`
 *
 * Composes the URL git should clone from.
 *
 * Most specific wins: the repository's own clone URL if it has one, then the
 * forge's clone base, then the forge's API base URL. That last fallback is
 * only right when one host serves both, which is common enough to be worth
 * defaulting to and wrong often enough that the clone base exists.
 *
 * Two shapes are produced, chosen by what the base looks like:
 *
 * - `git@host` (an `@`, no scheme) yields the scp-like
 *   `git@host:owner/repo.git`. The separator is a colon, not a slash --
 *   `git@host/owner/repo.git` is not a thing git understands.
 * - anything with a scheme yields `<base>/owner/repo.git`.
 *
 * Returns: (transfer full) (nullable): the clone URL, or %NULL if
 *   @full_name is malformed or there is no base to work from
 */
gchar *
venture_forge_clone_url(
	const gchar	*repo_clone_url,
	const gchar	*forge_clone_base,
	const gchar	*forge_base_url,
	const gchar	*full_name
);

/**
 * venture_forge_repo_split:
 * @full_name: an `owner/repo` string
 * @out_owner: (out) (transfer full): return location for the owner
 * @out_repo: (out) (transfer full): return location for the repository
 *
 * Splits a repository's full name on its single slash.
 *
 * Neither half may contain a slash upstream, so this is exact rather than a
 * best guess. A name with no slash, an empty half, or any path-traversal
 * segment is refused -- these two values become path segments in an API URL,
 * and a value containing a slash or a `..` is how a caller-supplied name
 * walks out of the path it was meant to sit in.
 *
 * Returns: %TRUE if @full_name was well formed
 */
gboolean
venture_forge_repo_split(
	const gchar	 *full_name,
	gchar		**out_owner,
	gchar		**out_repo
);

G_END_DECLS

#endif /* VENTURE_FORGE_RULES_H */
