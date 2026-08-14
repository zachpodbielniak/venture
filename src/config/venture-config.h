/*
 * venture-config.h - Layered configuration
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Configuration is a plain GObject whose properties mirror the YAML
 * document, one property per setting, named `section-key`. Making them real
 * properties rather than a hash table means the CLI can bind options to them
 * generically, the web UI can render a settings form from introspection, and
 * a typo in a YAML file is caught at load rather than at first use.
 *
 * Sources are applied in order, each overriding the last field by field:
 *
 *   1. the defaults compiled into the binary
 *   2. /etc/venture/config.yaml
 *   3. $XDG_CONFIG_HOME/venture/config.yaml
 *   4. the file named by --config
 *   5. the compiled C configuration, which runs last among the files and
 *      can compute values rather than state them
 *   6. VENTURE_* environment variables
 *   7. command-line options
 *
 * Secrets never live in the configuration itself. Every secret setting is a
 * *_env naming an environment variable, so a config file can be committed to
 * a dotfiles repository without thinking about it.
 */

#ifndef VENTURE_CONFIG_H
#define VENTURE_CONFIG_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_CONFIG (venture_config_get_type())

G_DECLARE_FINAL_TYPE(VentureConfig, venture_config, VENTURE, CONFIG, GObject)

/**
 * VentureConfigureFunc:
 * @config: the configuration to adjust
 * @error: (out) (optional): return location for a #GError
 *
 * The signature a compiled C configuration must export as
 * `venture_configure`. Return %FALSE with @error set to abort startup.
 *
 * Returns: %TRUE to continue
 */
typedef gboolean (*VentureConfigureFunc) (
	VentureConfig	 *config,
	GError		**error
);

/**
 * venture_config_new:
 *
 * Creates a configuration holding the compiled-in defaults.
 *
 * Returns: (transfer full): a new #VentureConfig
 */
VentureConfig *
venture_config_new(void);

/**
 * venture_config_load:
 * @explicit_path: (nullable): a configuration file named on the command line
 * @error: (out) (optional): return location for a #GError
 *
 * Builds a configuration by applying every source in order. A missing file
 * at any of the standard locations is not an error; a malformed one is.
 *
 * Returns: (transfer full) (nullable): the configuration, or %NULL on error
 */
VentureConfig *
venture_config_load(
	const gchar	 *explicit_path,
	GError		**error
);

/**
 * venture_config_apply_yaml_file:
 * @self: a #VentureConfig
 * @path: the file to read
 * @error: (out) (optional): return location for a #GError
 *
 * Applies one YAML file over the current values. Keys that do not
 * correspond to a property are reported rather than ignored, because a
 * silently-dropped setting is the single most confusing configuration
 * failure there is.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_config_apply_yaml_file(
	VentureConfig	 *self,
	const gchar	 *path,
	GError		**error
);

/**
 * venture_config_apply_yaml_string:
 * @self: a #VentureConfig
 * @yaml: the YAML text
 * @error: (out) (optional): return location for a #GError
 *
 * As venture_config_apply_yaml_file(), for text already in memory.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_config_apply_yaml_string(
	VentureConfig	 *self,
	const gchar	 *yaml,
	GError		**error
);

/**
 * venture_config_apply_environment:
 * @self: a #VentureConfig
 *
 * Applies VENTURE_* environment variables. The variable name is the
 * property name uppercased with hyphens as underscores, so
 * `VENTURE_SERVER_PORT` sets `server-port`.
 */
void
venture_config_apply_environment(VentureConfig *self);

/**
 * venture_config_apply_c_config:
 * @self: a #VentureConfig
 * @path: the C source file to compile and run
 * @error: (out) (optional): return location for a #GError
 *
 * Compiles @path with crispy, resolves its `venture_configure` symbol and
 * calls it. The result is cached by content hash, so this costs a compile
 * only after an edit.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_config_apply_c_config(
	VentureConfig	 *self,
	const gchar	 *path,
	GError		**error
);

/**
 * venture_config_validate:
 * @self: a #VentureConfig
 * @error: (out) (optional): return location for a #GError
 *
 * Checks the configuration for combinations that would fail later or, worse,
 * work insecurely: a non-loopback bind address with authentication off, TLS
 * half configured, a database URI with no recognised scheme.
 *
 * Returns: %TRUE if the configuration is usable
 */
gboolean
venture_config_validate(
	VentureConfig	 *self,
	GError		**error
);

/**
 * venture_config_to_yaml:
 * @self: a #VentureConfig
 * @include_defaults: whether to emit settings still at their default
 *
 * Serialises the configuration back to YAML, which is what
 * `venture --generate-config` and the settings screen's export both use.
 *
 * Returns: (transfer full): the YAML text
 */
gchar *
venture_config_to_yaml(
	VentureConfig	*self,
	gboolean	 include_defaults
);

/**
 * venture_config_describe:
 * @self: a #VentureConfig
 *
 * Describes every setting: its section, key, resolved value, type and the
 * one-line explanation of what it does. This is what the settings page and
 * `/api/v1/settings` render, so that the configuration a running server is
 * actually using can be read without guessing which file won.
 *
 * Credentials are redacted. No secret is ever stored in a setting -- the
 * *_env settings hold a variable name, not its value -- but a database URI
 * can carry a password if someone spelled one out, and that one is masked.
 *
 * Returns: (transfer full): a JSON array of setting objects
 */
JsonNode *
venture_config_describe(VentureConfig *self);

/**
 * venture_config_get_loaded_from:
 * @self: a #VentureConfig
 *
 * Retrieves the path of the last configuration file applied, which is the
 * file whose values won wherever they were set.
 *
 * Returns: (transfer none) (nullable): the path, or %NULL if the
 *   configuration is the compiled-in default
 */
const gchar *
venture_config_get_loaded_from(VentureConfig *self);

/**
 * venture_config_get_default_yaml:
 *
 * Retrieves the commented default configuration compiled into the binary.
 *
 * Returns: (transfer none): the YAML text
 */
const gchar *
venture_config_get_default_yaml(void);

/**
 * venture_config_get_default_c_config:
 *
 * Retrieves the compiled C configuration template.
 *
 * Returns: (transfer none): the C source text
 */
const gchar *
venture_config_get_default_c_config(void);

/**
 * venture_config_get_secret:
 * @self: a #VentureConfig
 * @env_property: the name of the property naming an environment variable,
 *   e.g. "ai-api-key-env"
 *
 * Reads the secret a *_env setting points at. Returns %NULL when either the
 * setting or the variable is unset, so a caller can tell "not configured"
 * from "configured but empty".
 *
 * Returns: (transfer none) (nullable): the secret
 */
const gchar *
venture_config_get_secret(
	VentureConfig	*self,
	const gchar	*env_property
);

/**
 * venture_config_get_state_dir:
 * @self: a #VentureConfig
 *
 * Retrieves the directory holding runtime state -- the SQLite database, the
 * automation state file, the crispy cache -- creating it if needed.
 *
 * Returns: (transfer none): the directory path
 */
const gchar *
venture_config_get_state_dir(VentureConfig *self);

/**
 * venture_config_resolve_path:
 * @self: a #VentureConfig
 * @path: a possibly-relative path
 *
 * Resolves @path against the state directory when it is relative, so a
 * configuration file can name "automations.pod" without knowing where the
 * server will run.
 *
 * Returns: (transfer full): an absolute path
 */
gchar *
venture_config_resolve_path(
	VentureConfig	*self,
	const gchar	*path
);

/* --- Convenience accessors ----------------------------------------------- */
/*
 * Every setting is reachable with g_object_get(). These exist for the ones
 * read from many places, and for the ones a compiled C configuration is
 * most likely to want to set.
 */

const gchar *
venture_config_get_database_uri(VentureConfig *self);

void
venture_config_set_database_uri(
	VentureConfig	*self,
	const gchar	*uri
);

VentureDatabaseBackend
venture_config_get_database_backend(VentureConfig *self);

VentureAiPolicy
venture_config_get_ai_policy(VentureConfig *self);

void
venture_config_set_ai_policy(
	VentureConfig	*self,
	VentureAiPolicy	 policy
);

void
venture_config_set_ai_system_prompt_extra(
	VentureConfig	*self,
	const gchar	*text
);

/**
 * venture_config_is_tool_auto_approved:
 * @self: a #VentureConfig
 * @tool_name: the AI tool name
 *
 * Determines whether a tool may run without confirmation under the
 * confirm-writes policy.
 *
 * Returns: %TRUE if @tool_name is auto-approved
 */
gboolean
venture_config_is_tool_auto_approved(
	VentureConfig	*self,
	const gchar	*tool_name
);

/**
 * venture_config_get_timezone:
 * @self: a #VentureConfig
 *
 * Returns: (transfer full): the configured timezone
 */
GTimeZone *
venture_config_get_timezone(VentureConfig *self);

G_END_DECLS

#endif /* VENTURE_CONFIG_H */
