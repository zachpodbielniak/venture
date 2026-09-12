/*
 * venture-forge-client.h - Talking to a git forge
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An interface rather than a single client because Forgejo is the first
 * forge here and not intended to be the last. Forgejo and Gitea share API v1
 * and are served by one implementation; GitHub and GitLab differ enough that
 * they would each be another, and the factory refuses them until one exists
 * rather than aiming a Forgejo-shaped request at a GitHub URL.
 *
 * These calls are synchronous, and that is a deliberate constraint rather
 * than an oversight: htmx-glib's handler interface returns a response
 * directly, so a route cannot yield. A forge call from a button press
 * therefore blocks the main loop for at most the configured timeout, which
 * is the same shape as the AI chat route and the invoice actions already in
 * this tree. What must not block is the minutes-long work -- the agent runs
 * and the git clones -- and that lives on the work service's own thread for
 * exactly this reason.
 *
 * Two members of the interface look like they belong in the webhook route
 * rather than here: verifying a signature and parsing an event. They are
 * here because both are the forge's format and not VENTURE's. Forgejo and
 * Gitea send a bare hex HMAC-SHA256 in X-Forgejo-Signature, GitHub sends
 * "sha256=..." in X-Hub-Signature-256, and GitLab sends a plain shared token
 * in X-Gitlab-Token. Keeping them behind the interface is what stops the
 * webhook route growing a switch on forge kind.
 */

#ifndef VENTURE_FORGE_CLIENT_H
#define VENTURE_FORGE_CLIENT_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_FORGE_CLIENT (venture_forge_client_get_type())

G_DECLARE_INTERFACE(VentureForgeClient, venture_forge_client,
                    VENTURE, FORGE_CLIENT, GObject)

/**
 * VentureForgeEvent:
 * @action: what happened -- opened, edited, closed, reopened, created
 * @repo_full_name: the repository, as `owner/repo`
 * @issue_number: the issue's number upstream
 * @title: the issue title
 * @body: the issue body
 * @state: "open" or "closed"
 * @url: the issue's web address
 * @sender: the login that caused the event
 * @delivery_id: the forge's delivery identifier, for retry suppression
 * @comment_body: the comment, on a comment event
 * @labels: (array zero-terminated=1): the issue's labels
 *
 * One inbound event, normalised out of whatever the forge sent.
 *
 * A stack struct cleared by venture_forge_event_clear() rather than a boxed
 * type, because it lives for the length of one handler and never escapes it.
 */
typedef struct
{
	gchar	*action;
	gchar	*repo_full_name;
	gint64	 issue_number;
	gchar	*title;
	gchar	*body;
	gchar	*state;
	gchar	*url;
	gchar	*sender;
	gchar	*delivery_id;
	gchar	*comment_body;
	GStrv	 labels;
} VentureForgeEvent;

/**
 * venture_forge_event_clear:
 * @event: the event to release
 *
 * Frees everything @event owns and zeroes it, so it is safe to clear twice.
 */
void
venture_forge_event_clear(VentureForgeEvent *event);

/**
 * VentureForgeWorkflowEvent:
 * @action: `requested`, `in_progress` or `completed`
 * @repo_full_name: `owner/repo`
 * @run_id: the forge's id for the run; what a build is matched on
 * @run_number: the run's number within the workflow
 * @workflow_name: the workflow's name
 * @title: the run's display title, usually the commit subject
 * @head_branch: the branch that was built
 * @head_sha: the commit that was built
 * @status: `queued`, `in_progress` or `completed`
 * @conclusion: `success`, `failure` or `cancelled` once completed
 * @url: the run's page on the forge
 * @started_at: (nullable): when it started, ISO 8601
 * @finished_at: (nullable): when it finished, ISO 8601
 * @sender: who caused the event
 * @delivery_id: the delivery's id, for the retry guard
 *
 * A CI workflow run, as a forge reports it. The factory turns these into
 * build records.
 */
typedef struct
{
	gchar	*action;
	gchar	*repo_full_name;
	gint64	 run_id;
	gint64	 run_number;
	gchar	*workflow_name;
	gchar	*title;
	gchar	*head_branch;
	gchar	*head_sha;
	gchar	*status;
	gchar	*conclusion;
	gchar	*url;
	gchar	*started_at;
	gchar	*finished_at;
	gchar	*sender;
	gchar	*delivery_id;
} VentureForgeWorkflowEvent;

/**
 * venture_forge_workflow_event_clear:
 * @event: the event to release
 *
 * Frees everything @event owns and zeroes it.
 */
void
venture_forge_workflow_event_clear(VentureForgeWorkflowEvent *event);

/**
 * VentureForgeReleaseEvent:
 * @action: `published`, `updated` or `deleted`
 * @repo_full_name: `owner/repo`
 * @release_id: the forge's id for the release
 * @tag: the tag the release is cut from
 * @name: the release's title
 * @body: the release notes
 * @url: the release's page on the forge
 * @published_at: (nullable): when it was published, ISO 8601
 * @draft: whether it is still a draft
 * @prerelease: whether it is marked a pre-release
 * @sender: who caused the event
 * @delivery_id: the delivery's id
 *
 * A release, as a forge reports it. The factory turns these into release
 * records.
 */
typedef struct
{
	gchar		*action;
	gchar		*repo_full_name;
	gint64		 release_id;
	gchar		*tag;
	gchar		*name;
	gchar		*body;
	gchar		*url;
	gchar		*published_at;
	gboolean	 draft;
	gboolean	 prerelease;
	gchar		*sender;
	gchar		*delivery_id;
} VentureForgeReleaseEvent;

/**
 * venture_forge_release_event_clear:
 * @event: the event to release
 *
 * Frees everything @event owns and zeroes it.
 */
void
venture_forge_release_event_clear(VentureForgeReleaseEvent *event);

/**
 * VentureForgeClientInterface:
 * @parent_iface: the parent interface
 * @whoami: the login the access token belongs to
 * @check_repository: whether a repository exists, and its default branch
 * @create_issue: files an issue
 * @update_issue: changes an issue's title, body or state
 * @comment_issue: adds a comment
 * @branch_exists: whether a branch is already there
 * @create_branch: creates a branch from another
 * @create_pull_request: opens a pull request
 * @verify_webhook: checks an inbound signature
 * @parse_issue_event: normalises an inbound payload
 *
 * What a forge has to be able to do for VENTURE to use it.
 */
struct _VentureForgeClientInterface
{
	GTypeInterface parent_iface;

	gchar    *(*whoami)             (VentureForgeClient  *self,
	                                 GError             **error);

	gboolean  (*check_repository)   (VentureForgeClient  *self,
	                                 const gchar         *repo_full_name,
	                                 gchar              **out_default_branch,
	                                 GError             **error);

	gboolean  (*create_issue)       (VentureForgeClient  *self,
	                                 const gchar         *repo_full_name,
	                                 const gchar         *title,
	                                 const gchar         *body,
	                                 const gchar *const  *labels,
	                                 gint64              *out_number,
	                                 gchar              **out_url,
	                                 GError             **error);

	/* Every optional argument is nullable and %NULL means "leave it
	 * alone". A PATCH that blanked a body because the caller only meant
	 * to close the issue is the obvious way to lose somebody's text. */
	gboolean  (*update_issue)       (VentureForgeClient  *self,
	                                 const gchar         *repo_full_name,
	                                 gint64               number,
	                                 const gchar         *title,
	                                 const gchar         *body,
	                                 const gchar         *state,
	                                 GError             **error);

	gboolean  (*comment_issue)      (VentureForgeClient  *self,
	                                 const gchar         *repo_full_name,
	                                 gint64               number,
	                                 const gchar         *body,
	                                 GError             **error);

	gboolean  (*branch_exists)      (VentureForgeClient  *self,
	                                 const gchar         *repo_full_name,
	                                 const gchar         *branch,
	                                 gboolean            *out_exists,
	                                 GError             **error);

	gboolean  (*create_branch)      (VentureForgeClient  *self,
	                                 const gchar         *repo_full_name,
	                                 const gchar         *branch,
	                                 const gchar         *from_branch,
	                                 GError             **error);

	gboolean  (*create_pull_request)(VentureForgeClient  *self,
	                                 const gchar         *repo_full_name,
	                                 const gchar         *title,
	                                 const gchar         *body,
	                                 const gchar         *head,
	                                 const gchar         *base,
	                                 gboolean             draft,
	                                 gint64              *out_number,
	                                 gchar              **out_url,
	                                 GError             **error);

	gboolean  (*verify_webhook)     (VentureForgeClient  *self,
	                                 SoupMessageHeaders  *headers,
	                                 GBytes              *body,
	                                 const gchar         *secret,
	                                 GError             **error);

	/* The factory's three, optional: a forge that cannot report builds
	 * or releases leaves them NULL and the dispatchers say so. */
	gboolean  (*parse_workflow_event) (VentureForgeClient        *self,
	                                   JsonNode                  *payload,
	                                   SoupMessageHeaders        *headers,
	                                   VentureForgeWorkflowEvent *out_event,
	                                   GError                   **error);

	gboolean  (*parse_release_event)  (VentureForgeClient        *self,
	                                   JsonNode                  *payload,
	                                   SoupMessageHeaders        *headers,
	                                   VentureForgeReleaseEvent  *out_event,
	                                   GError                   **error);

	gboolean  (*create_release)     (VentureForgeClient  *self,
	                                 const gchar         *repo_full_name,
	                                 const gchar         *tag,
	                                 const gchar         *target,
	                                 const gchar         *name,
	                                 const gchar         *body,
	                                 gboolean             draft,
	                                 gboolean             prerelease,
	                                 gint64              *out_id,
	                                 gchar              **out_url,
	                                 GError             **error);

	gboolean  (*parse_issue_event)  (VentureForgeClient  *self,
	                                 JsonNode            *payload,
	                                 SoupMessageHeaders  *headers,
	                                 VentureForgeEvent   *out_event,
	                                 GError             **error);
};

gchar *
venture_forge_client_whoami(
	VentureForgeClient	 *self,
	GError			**error
);

gboolean
venture_forge_client_check_repository(
	VentureForgeClient	 *self,
	const gchar		 *repo_full_name,
	gchar			**out_default_branch,
	GError			**error
);

gboolean
venture_forge_client_create_issue(
	VentureForgeClient	 *self,
	const gchar		 *repo_full_name,
	const gchar		 *title,
	const gchar		 *body,
	const gchar *const	 *labels,
	gint64			 *out_number,
	gchar			**out_url,
	GError			**error
);

gboolean
venture_forge_client_update_issue(
	VentureForgeClient	 *self,
	const gchar		 *repo_full_name,
	gint64			  number,
	const gchar		 *title,
	const gchar		 *body,
	const gchar		 *state,
	GError			**error
);

gboolean
venture_forge_client_comment_issue(
	VentureForgeClient	 *self,
	const gchar		 *repo_full_name,
	gint64			  number,
	const gchar		 *body,
	GError			**error
);

gboolean
venture_forge_client_branch_exists(
	VentureForgeClient	 *self,
	const gchar		 *repo_full_name,
	const gchar		 *branch,
	gboolean		 *out_exists,
	GError			**error
);

gboolean
venture_forge_client_create_branch(
	VentureForgeClient	 *self,
	const gchar		 *repo_full_name,
	const gchar		 *branch,
	const gchar		 *from_branch,
	GError			**error
);

gboolean
venture_forge_client_create_pull_request(
	VentureForgeClient	 *self,
	const gchar		 *repo_full_name,
	const gchar		 *title,
	const gchar		 *body,
	const gchar		 *head,
	const gchar		 *base,
	gboolean		  draft,
	gint64			 *out_number,
	gchar			**out_url,
	GError			**error
);

gboolean
venture_forge_client_verify_webhook(
	VentureForgeClient	 *self,
	SoupMessageHeaders	 *headers,
	GBytes			 *body,
	const gchar		 *secret,
	GError			**error
);

gboolean
venture_forge_client_parse_issue_event(
	VentureForgeClient	 *self,
	JsonNode		 *payload,
	SoupMessageHeaders	 *headers,
	VentureForgeEvent	 *out_event,
	GError			**error
);

/**
 * venture_forge_client_parse_workflow_event:
 * @self: a #VentureForgeClient
 * @payload: the parsed webhook body
 * @headers: the request headers, for the delivery id
 * @out_event: (out caller-allocates): the event, to clear with
 *   venture_forge_workflow_event_clear()
 * @error: (out) (optional): return location for a #GError
 *
 * Reads a `workflow_run` delivery.
 *
 * Returns: %TRUE if the payload described a workflow run
 */
gboolean
venture_forge_client_parse_workflow_event(
	VentureForgeClient		 *self,
	JsonNode			 *payload,
	SoupMessageHeaders		 *headers,
	VentureForgeWorkflowEvent	 *out_event,
	GError				**error
);

/**
 * venture_forge_client_parse_release_event:
 * @self: a #VentureForgeClient
 * @payload: the parsed webhook body
 * @headers: the request headers, for the delivery id
 * @out_event: (out caller-allocates): the event, to clear with
 *   venture_forge_release_event_clear()
 * @error: (out) (optional): return location for a #GError
 *
 * Reads a `release` delivery.
 *
 * Returns: %TRUE if the payload described a release
 */
gboolean
venture_forge_client_parse_release_event(
	VentureForgeClient		 *self,
	JsonNode			 *payload,
	SoupMessageHeaders		 *headers,
	VentureForgeReleaseEvent	 *out_event,
	GError				**error
);

/**
 * venture_forge_client_create_release:
 * @self: a #VentureForgeClient
 * @repo_full_name: `owner/repo`
 * @tag: the tag to release from, created at @target if it does not exist
 * @target: (nullable): the branch or commit to tag; the default branch
 *   when %NULL
 * @name: the release's title
 * @body: (nullable): the release notes
 * @draft: whether to leave it unpublished
 * @prerelease: whether to mark it a pre-release
 * @out_id: (out) (optional): the forge's id for the release
 * @out_url: (out) (optional) (transfer full): the release's page
 * @error: (out) (optional): return location for a #GError
 *
 * Publishes a release on the forge. This is what a release record's
 * Publish action does.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_forge_client_create_release(
	VentureForgeClient	 *self,
	const gchar		 *repo_full_name,
	const gchar		 *tag,
	const gchar		 *target,
	const gchar		 *name,
	const gchar		 *body,
	gboolean		  draft,
	gboolean		  prerelease,
	gint64			 *out_id,
	gchar			**out_url,
	GError			**error
);

/**
 * venture_forge_client_for_forge:
 * @forge: the forge record, loaded from the database
 * @timeout_seconds: how long a single call may take
 * @error: (out) (optional): return location for a #GError
 *
 * Builds the client that speaks to @forge.
 *
 * The access token is read off the record here and nowhere else. It is a
 * sensitive field, so it is absent from every serialisation -- a caller that
 * tried to pass it around would find it had nothing to pass.
 *
 * Returns %VENTURE_ERROR_NOT_SUPPORTED for a forge kind this build cannot
 * speak. That is a better answer than a 404 from a GitHub URL addressed as
 * though it were a Forgejo one.
 *
 * Returns: (transfer full) (nullable): a client, or %NULL
 */
VentureForgeClient *
venture_forge_client_for_forge(
	VentureForge	 *forge,
	gint		  timeout_seconds,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_FORGE_CLIENT_H */
