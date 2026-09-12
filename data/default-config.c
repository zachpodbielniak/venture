/*
 * config.c - Compiled configuration for VENTURE
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is optional. If it exists next to your config.yaml, VENTURE
 * compiles it with crispy at startup (caching the result, so it costs
 * nothing on subsequent runs) and calls venture_configure() after the YAML
 * has been loaded. Anything you set here wins.
 *
 * The point of it is that some configuration is a decision, not a value:
 *
 *   - use the production database on the server and a scratch one on the
 *     laptop, decided by hostname
 *   - raise the AI's authority only when a particular environment variable
 *     is present, so an automated run is more autonomous than an
 *     interactive one
 *   - register a venture type, a report or an AI tool in a few lines
 *     without building a plugin
 *
 * Everything the umbrella header exposes is available here. Return FALSE
 * with @error set to abort startup.
 *
 * Regenerate this template with:  venture --generate-config --format=c
 */

#include <venture/venture.h>

gboolean
venture_configure(
	VentureConfig	 *config,
	GError		**error
){
	const gchar *hostname;

	(void)error;

	hostname = g_get_host_name();

	/* Point at the shared PostgreSQL instance when running on the box
	 * that has it, and stay on a local SQLite file everywhere else. This
	 * keeps one configuration working on every machine. */
	if (0 == g_strcmp0(hostname, "CHANGE-ME-server-hostname"))
	{
		venture_config_set_database_uri(config,
			"postgres://venture@localhost/venture");
	}

	/* An unattended run -- a cron job, a container entrypoint -- has no
	 * one to approve a staged write, so let it act on its own. An
	 * interactive run keeps the default, which stages every mutation for
	 * approval. */
	if (NULL != g_getenv("VENTURE_UNATTENDED"))
		venture_config_set_ai_policy(config, VENTURE_AI_POLICY_AUTONOMOUS);

	/* Modules are switched here the same way the YAML switches them, so a
	 * machine can run a different shape of the product: the laptop keeps
	 * the books and nothing else, the server runs the whole factory. */
	if (0 == g_strcmp0(hostname, "CHANGE-ME-laptop-hostname"))
	{
		venture_config_set_module_enabled(config, "forge", FALSE);
		venture_config_set_module_enabled(config, "factory", FALSE);
		venture_config_set_module_enabled(config, "kb", FALSE);
	}

	/* House rules for the AI. This text is appended to the system prompt,
	 * so it is the cheapest place to teach it your conventions. */
	venture_config_set_ai_system_prompt_extra(config,
		"Amounts are US dollars unless stated otherwise. "
		"When asked about \"the business\", assume the default "
		"organization. Never modify an expense that has already been "
		"included in a filed tax report.");

	return TRUE;
}
