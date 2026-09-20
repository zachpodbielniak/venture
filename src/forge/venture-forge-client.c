/*
 * venture-forge-client.c - The forge interface and its factory
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

G_DEFINE_INTERFACE(VentureForgeClient, venture_forge_client, G_TYPE_OBJECT)

static void
venture_forge_client_default_init(VentureForgeClientInterface *iface)
{
	(void)iface;
}

void
venture_forge_event_clear(VentureForgeEvent *event)
{
	if (NULL == event)
		return;

	g_clear_pointer(&event->action, g_free);
	g_clear_pointer(&event->repo_full_name, g_free);
	g_clear_pointer(&event->title, g_free);
	g_clear_pointer(&event->body, g_free);
	g_clear_pointer(&event->state, g_free);
	g_clear_pointer(&event->url, g_free);
	g_clear_pointer(&event->sender, g_free);
	g_clear_pointer(&event->delivery_id, g_free);
	g_clear_pointer(&event->comment_body, g_free);
	g_clear_pointer(&event->labels, g_strfreev);

	event->issue_number = 0;
}

void
venture_forge_workflow_event_clear(VentureForgeWorkflowEvent *event)
{
	if (NULL == event)
		return;

	g_clear_pointer(&event->action, g_free);
	g_clear_pointer(&event->repo_full_name, g_free);
	g_clear_pointer(&event->workflow_name, g_free);
	g_clear_pointer(&event->title, g_free);
	g_clear_pointer(&event->head_branch, g_free);
	g_clear_pointer(&event->head_sha, g_free);
	g_clear_pointer(&event->status, g_free);
	g_clear_pointer(&event->conclusion, g_free);
	g_clear_pointer(&event->url, g_free);
	g_clear_pointer(&event->started_at, g_free);
	g_clear_pointer(&event->finished_at, g_free);
	g_clear_pointer(&event->sender, g_free);
	g_clear_pointer(&event->delivery_id, g_free);

	event->run_id = 0;
	event->run_number = 0;
}

void
venture_forge_release_event_clear(VentureForgeReleaseEvent *event)
{
	if (NULL == event)
		return;

	g_clear_pointer(&event->action, g_free);
	g_clear_pointer(&event->repo_full_name, g_free);
	g_clear_pointer(&event->tag, g_free);
	g_clear_pointer(&event->name, g_free);
	g_clear_pointer(&event->body, g_free);
	g_clear_pointer(&event->url, g_free);
	g_clear_pointer(&event->published_at, g_free);
	g_clear_pointer(&event->sender, g_free);
	g_clear_pointer(&event->delivery_id, g_free);

	event->release_id = 0;
	event->draft = FALSE;
	event->prerelease = FALSE;
}

/*
 * The dispatchers.
 *
 * Each checks that the implementation actually provides the method rather
 * than calling through a NULL pointer, because a partially implemented forge
 * is a normal state while a new one is being written -- GitLab will arrive
 * with three of these before it has all ten.
 */

#define VENTURE_FORGE_DISPATCH(method, fail)                                  \
	VentureForgeClientInterface *iface;                                   \
                                                                              \
	g_return_val_if_fail(VENTURE_IS_FORGE_CLIENT(self), fail);            \
                                                                              \
	if (!venture_forge_client_check_credentials(self, error)) return fail; \
	iface = VENTURE_FORGE_CLIENT_GET_IFACE(self);                         \
                                                                              \
	if (NULL == iface->method)                                            \
	{                                                                     \
		g_set_error_literal(error, VENTURE_ERROR,                     \
		                    VENTURE_ERROR_UNSUPPORTED,                \
		                    "This forge cannot do that yet");         \
		return fail;                                                  \
	}

gchar *
venture_forge_client_whoami(
	VentureForgeClient	 *self,
	GError			**error
){
	VENTURE_FORGE_DISPATCH(whoami, NULL)

	return iface->whoami(self, error);
}

gboolean
venture_forge_client_check_repository(
	VentureForgeClient	 *self,
	const gchar		 *repo_full_name,
	gchar			**out_default_branch,
	GError			**error
){
	VENTURE_FORGE_DISPATCH(check_repository, FALSE)

	return iface->check_repository(self, repo_full_name, out_default_branch,
	                               error);
}

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
){
	VENTURE_FORGE_DISPATCH(create_issue, FALSE)

	return iface->create_issue(self, repo_full_name, title, body, labels,
	                           out_number, out_url, error);
}

gboolean
venture_forge_client_update_issue(
	VentureForgeClient	 *self,
	const gchar		 *repo_full_name,
	gint64			  number,
	const gchar		 *title,
	const gchar		 *body,
	const gchar		 *state,
	GError			**error
){
	VENTURE_FORGE_DISPATCH(update_issue, FALSE)

	return iface->update_issue(self, repo_full_name, number, title, body,
	                           state, error);
}

gboolean
venture_forge_client_comment_issue(
	VentureForgeClient	 *self,
	const gchar		 *repo_full_name,
	gint64			  number,
	const gchar		 *body,
	GError			**error
){
	VENTURE_FORGE_DISPATCH(comment_issue, FALSE)

	return iface->comment_issue(self, repo_full_name, number, body, error);
}

gboolean
venture_forge_client_branch_exists(
	VentureForgeClient	 *self,
	const gchar		 *repo_full_name,
	const gchar		 *branch,
	gboolean		 *out_exists,
	GError			**error
){
	VENTURE_FORGE_DISPATCH(branch_exists, FALSE)

	return iface->branch_exists(self, repo_full_name, branch, out_exists,
	                            error);
}

gboolean
venture_forge_client_create_branch(
	VentureForgeClient	 *self,
	const gchar		 *repo_full_name,
	const gchar		 *branch,
	const gchar		 *from_branch,
	GError			**error
){
	VENTURE_FORGE_DISPATCH(create_branch, FALSE)

	return iface->create_branch(self, repo_full_name, branch, from_branch,
	                            error);
}

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
){
	VENTURE_FORGE_DISPATCH(create_pull_request, FALSE)

	return iface->create_pull_request(self, repo_full_name, title, body, head,
	                                  base, draft, out_number, out_url, error);
}

gboolean
venture_forge_client_verify_webhook(
	VentureForgeClient	 *self,
	SoupMessageHeaders	 *headers,
	GBytes			 *body,
	const gchar		 *secret,
	GError			**error
){
	VENTURE_FORGE_DISPATCH(verify_webhook, FALSE)

	return iface->verify_webhook(self, headers, body,
		venture_forge_client_get_credentials(self) ? venture_forge_credentials_get_webhook_secret(venture_forge_client_get_credentials(self)) : secret, error);
}

gboolean
venture_forge_client_parse_issue_event(
	VentureForgeClient	 *self,
	JsonNode		 *payload,
	SoupMessageHeaders	 *headers,
	VentureForgeEvent	 *out_event,
	GError			**error
){
	VENTURE_FORGE_DISPATCH(parse_issue_event, FALSE)

	return iface->parse_issue_event(self, payload, headers, out_event, error);
}

gboolean
venture_forge_client_parse_workflow_event(
	VentureForgeClient		 *self,
	JsonNode			 *payload,
	SoupMessageHeaders		 *headers,
	VentureForgeWorkflowEvent	 *out_event,
	GError				**error
){
	VENTURE_FORGE_DISPATCH(parse_workflow_event, FALSE)

	return iface->parse_workflow_event(self, payload, headers, out_event,
	                                   error);
}

gboolean
venture_forge_client_parse_release_event(
	VentureForgeClient		 *self,
	JsonNode			 *payload,
	SoupMessageHeaders		 *headers,
	VentureForgeReleaseEvent	 *out_event,
	GError				**error
){
	VENTURE_FORGE_DISPATCH(parse_release_event, FALSE)

	return iface->parse_release_event(self, payload, headers, out_event,
	                                  error);
}

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
){
	VENTURE_FORGE_DISPATCH(create_release, FALSE)

	return iface->create_release(self, repo_full_name, tag, target, name,
	                             body, draft, prerelease, out_id, out_url,
	                             error);
}

#undef VENTURE_FORGE_DISPATCH

VentureForgeClient *
venture_forge_client_for_forge(
	VentureForge	 *forge,
	gint		  timeout_seconds,
	GError		**error
){
	g_return_val_if_fail(VENTURE_IS_FORGE(forge), NULL);
	(void)timeout_seconds;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		"Use venture_forge_client_for_database with an explicitly configured encrypted binding");
	return NULL;
}
