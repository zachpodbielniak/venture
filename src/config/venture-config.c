/*
 * venture-config.c - Layered configuration
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The properties are installed from a table rather than written out, for the
 * same reason the record types are: one declaration drives the property, the
 * YAML mapping, the environment variable name and the generated
 * documentation, so they cannot drift apart.
 */

#include "venture.h"

#include <yaml-glib.h>

#include <string.h>

#ifdef VENTURE_SERVER_BUILD
#include "plugin/venture-crispy-host.h"
#endif

#include "venture-default-config.h"

/*
 * One configuration setting.
 *
 * @section and @key together give the YAML path; @name is the GObject
 * property, always "<section>-<key>" with underscores turned into hyphens.
 * Keeping the two forms in one row is what stops a rename in the YAML from
 * quietly losing a setting.
 */
typedef struct
{
	const gchar	*name;
	const gchar	*section;
	const gchar	*key;
	GType		 value_type;
	GType		(*enum_type_func) (void);
	const gchar	*default_string;
	gint64		 default_int;
	gboolean	 default_bool;
	const gchar	*blurb;
} VentureConfigSetting;

#define VC_STR(name, section, key, fallback, blurb) \
	{ name, section, key, G_TYPE_STRING, NULL, fallback, 0, FALSE, blurb }
#define VC_INT(name, section, key, fallback, blurb) \
	{ name, section, key, G_TYPE_INT64, NULL, NULL, fallback, FALSE, blurb }
#define VC_BOOL(name, section, key, fallback, blurb) \
	{ name, section, key, G_TYPE_BOOLEAN, NULL, NULL, 0, fallback, blurb }
#define VC_ENUM(name, section, key, type_func, blurb) \
	{ name, section, key, G_TYPE_INVALID, type_func, NULL, 0, FALSE, blurb }
/*
 * G_TYPE_STRV is a function call rather than a constant, so it cannot appear
 * in a static initialiser. G_TYPE_BOXED is a constant and no setting is a
 * plain boxed value, which makes it an unambiguous marker for "string list";
 * the real G_TYPE_STRV is used when the property is actually installed.
 */
#define VC_STRV(name, section, key, blurb) \
	{ name, section, key, G_TYPE_BOXED, NULL, NULL, 0, FALSE, blurb }

static const VentureConfigSetting venture_config_settings[] = {
	VC_BOOL("ocr-enabled", "ocr", "enabled", FALSE, "Enable bounded local OCR explicitly"),
	VC_STR("ocr-executable", "ocr", "executable", "tesseract", "Local OCR executable, chosen by the operator"),
	VC_STR("ocr-language", "ocr", "language", "eng", "Installed OCR languages joined with +"),
	VC_BOOL("federation-enabled", "federation", "enabled", FALSE,
	        "Opt in to federation; records remain private without grants"),
	VC_INT("federation-sync-interval", "federation", "sync_interval", 60,
	       "Seconds between automatic replica sync attempts; zero disables automatic sync"),
	VC_STR("federation-mode", "federation", "mode", "allowlist",
	       "allowlist or global; global grants never permit remote writes"),
	VC_STR("federation-origin", "federation", "origin", "",
	       "Canonical public HTTPS origin without a trailing slash"),
	VC_STR("federation-ca-file", "federation", "ca_file", "",
	       "Optional PEM trust anchors for a private federation CA; empty uses system trust"),
	VC_STR("federation-key-file", "federation", "key_file", "",
	       "Owner-only Ed25519 PEM private key file"),

	VC_STR ("server-bind-address", "server", "bind_address", "127.0.0.1",
	        "Address to listen on"),
	VC_INT ("server-port", "server", "port", 8747, "Port to listen on"),
	VC_STR ("server-base-url", "server", "base_url", "",
	        "Externally visible base URL"),
	VC_STR ("server-tls-certificate", "server", "tls_certificate", "",
	        "PEM certificate; enables HTTPS when set with a key"),
	VC_STR ("server-tls-private-key", "server", "tls_private_key", "",
	        "PEM private key"),
	VC_INT ("server-max-request-size-mb", "server", "max_request_size_mb", 32,
	        "Largest request body accepted, in megabytes"),
	VC_INT ("server-request-timeout", "server", "request_timeout", 60,
	        "Seconds a request may run"),

	VC_STR ("database-uri", "database", "uri", "sqlite://venture.db",
	        "Connection URI"),
	VC_STR ("database-password-env", "database", "password_env",
	        "VENTURE_DB_PASSWORD", "Environment variable holding the password"),
	VC_INT ("database-pool-size", "database", "pool_size", 4,
	        "Connections held open"),
	VC_BOOL("database-auto-migrate", "database", "auto_migrate", TRUE,
	        "Apply pending migrations at startup"),
	VC_INT ("database-busy-timeout", "database", "busy_timeout", 5,
	        "Seconds to wait for a lock"),

	VC_BOOL("hosted-enabled", "hosted", "enabled", FALSE, "Pin this database to one hosted workspace"),
	VC_STR("hosted-workspace-id", "hosted", "workspace_id", "", "Immutable hosted workspace UUID"),
	VC_STR("hosted-origin", "hosted", "origin", "", "Immutable public HTTPS tenant origin"),
	VC_INT("hosted-http-requests-per-minute", "hosted", "http_requests_per_minute", 600,
	       "Workspace dynamic HTTP requests per minute, 1 through 1000000"),
	VC_INT("hosted-http-burst", "hosted", "http_burst", 120,
	       "Workspace dynamic HTTP burst capacity, 1 through 1000000"),
	VC_INT("hosted-http-concurrency", "hosted", "http_concurrency", 8,
	       "Maximum nested dynamic HTTP handlers, 1 through 256"),

	VC_BOOL("security-require-auth", "security", "require_auth", TRUE,
	        "Require authentication for every request"),
	VC_STR ("security-session-secret-env", "security", "session_secret_env",
	        "VENTURE_SESSION_SECRET",
	        "Environment variable holding the session signing secret"),
	VC_INT ("security-session-lifetime", "security", "session_lifetime",
	        1209600, "Seconds a browser session stays valid"),
	VC_BOOL("security-cookie-secure", "security", "cookie_secure", FALSE,
	        "Mark the session cookie Secure"),
	VC_INT ("security-password-iterations", "security", "password_iterations",
	        210000, "PBKDF2 iterations"),
	VC_INT ("security-password-min-length", "security", "password_min_length",
	        8, "Shortest password accepted when one is set or changed"),
	VC_INT ("security-login-rate-limit", "security", "login_rate_limit", 10,
	        "Sign-in attempts allowed per address per minute; 0 disables"),

	/* Preserve legacy embedding settings for migration; explicit encrypted
	 * organization bindings authorize every actual embedding request. */
	VC_BOOL("kb-enabled", "kb", "enabled", TRUE,
	        "Whether knowledge bases and retrieval are available"),
	VC_STR ("kb-embedding-provider", "kb", "embedding_provider", "ollama",
	        "Legacy migration hint; configure the organization embedding binding"),
	VC_STR ("kb-embedding-url", "kb", "embedding_url",
	        "http://127.0.0.1:11434",
	        "Legacy embedding URL; not used for provider requests"),
	VC_STR ("kb-embedding-model", "kb", "embedding_model",
	        "nomic-embed-text:v1.5", "Legacy embedding model; select it explicitly in organization settings"),
	VC_STR ("kb-embedding-key-env", "kb", "embedding_key_env", "",
	        "Legacy embedding key reference; ambient credentials are not used"),
	/*
	 * Chunk size is in characters rather than tokens because the tokeniser
	 * is the model's and we do not have it. 1200 is roughly 300 tokens,
	 * comfortably inside every embedding context worth using, and short
	 * enough that a hit points at a paragraph rather than a chapter.
	 */
	VC_INT ("kb-chunk-chars", "kb", "chunk_chars", 1200,
	        "Target passage length, in characters"),
	/*
	 * Overlap exists so a sentence spanning a boundary is still findable.
	 * Without it the one paragraph that answers the question is the one
	 * split down the middle.
	 */
	VC_INT ("kb-chunk-overlap", "kb", "chunk_overlap", 200,
	        "Characters each passage repeats from the previous one"),
	VC_INT ("kb-search-limit", "kb", "search_limit", 8,
	        "Passages returned by a search, and given to the assistant"),
	/*
	 * Percent rather than a fraction: the config vocabulary has no double,
	 * and a threshold nobody can express is a threshold nobody sets.
	 */
	VC_INT ("kb-crossref-min-score", "kb", "crossref_min_score", 60,
	        "Cosine similarity, as a percentage, below which a "
	        "cross-reference is not recorded"),
	VC_INT ("kb-crossref-max-links", "kb", "crossref_max_links", 5,
	        "Most cross-references recorded for one record"),
	VC_INT ("kb-max-upload-mb", "kb", "max_upload_mb", 64,
	        "Largest file accepted by an import"),
	VC_INT ("kb-max-archive-entries", "kb", "max_archive_entries", 2000,
	        "Most files taken from a single uploaded archive"),

	VC_STR ("locale-default-currency", "locale", "default_currency", "USD",
	        "Currency assumed when an amount does not name one"),
	VC_STR ("locale-timezone", "locale", "timezone", "America/New_York",
	        "IANA timezone used for dates and report buckets"),
	VC_INT ("locale-fiscal-year-start-month", "locale",
	        "fiscal_year_start_month", 1, "Month the fiscal year begins"),

	VC_BOOL("ai-enabled", "ai", "enabled", TRUE, "Enable AI features"),
	VC_STRV("ai-allowed-base-urls", "ai", "allowed_base_urls", "Exact platform-approved alternate provider base URLs; HTTPS unless explicit loopback testing is enabled"),
	VC_BOOL("ai-allow-loopback", "ai", "allow_loopback", FALSE, "Permit literal HTTP loopback endpoints from the exact AI base URL allowlist for isolated tests"),
	VC_INT ("ai-provider-deadline-seconds", "ai", "provider_deadline_seconds", 60, "Total provider deadline, 1 through 60 seconds, below the durable reservation lease"),
	VC_STR ("ai-provider", "ai", "provider", "claude", "Legacy provider hint; configure each organization explicitly"),
	VC_STR ("ai-model", "ai", "model", "claude-sonnet-5", "Legacy model hint; organization bindings choose the model"),
	VC_STR ("ai-api-key-env", "ai", "api_key_env", "",
	        "Legacy key reference; ambient credentials are not used"),
	VC_ENUM("ai-policy", "ai", "policy", venture_ai_policy_get_type,
	        "How much authority AI tool calls have"),
	VC_STRV("ai-auto-approve-tools", "ai", "auto_approve_tools",
	        "Tools that may run without confirmation"),
	VC_INT ("ai-confirmation-ttl", "ai", "confirmation_ttl", 3600,
	        "Seconds a staged change waits for a decision before it is "
	        "dropped"),
	VC_INT ("ai-confirmation-limit", "ai", "confirmation_limit", 200,
	        "Most staged changes that may wait at once; further staging is "
	        "refused"),
	VC_INT ("ai-max-tokens", "ai", "max_tokens", 4096, "Response token limit"),
	VC_INT ("ai-tool-timeout", "ai", "tool_timeout", 30,
	        "Seconds a single tool call may run"),
	VC_INT ("ai-max-turns", "ai", "max_turns", 12,
	        "Turns the tool-use loop may take"),
	VC_STR ("ai-system-prompt-extra", "ai", "system_prompt_extra", "",
	        "Appended to the built-in system prompt"),
	VC_BOOL("ai-log-conversations", "ai", "log_conversations", TRUE,
	        "Record every AI request and response"),

	VC_BOOL("automation-enabled", "automation", "enabled", TRUE,
	        "Enable scheduled automations"),
	VC_STR ("automation-pods-file", "automation", "pods_file",
	        "automations.pod", "The podomation DSL file"),
	VC_STRV("automation-module-paths", "automation", "module_paths",
	        "Extra podomation module directories"),
	VC_INT ("automation-graceful-shutdown-timeout", "automation",
	        "graceful_shutdown_timeout", 10,
	        "Seconds to let automations finish during shutdown"),
	VC_BOOL("automation-persist-state", "automation", "persist_state", TRUE,
	        "Persist engine state across restarts"),

	VC_BOOL("stripe-enabled", "stripe", "enabled", FALSE,
	        "Enable hosted Stripe payments; requires deployment credentials"),
	VC_BOOL("oidc-enabled", "oidc", "enabled", FALSE,
	        "Enable explicitly linked organization OpenID Connect sign-in"),
	VC_BOOL("oidc-allow-loopback", "oidc", "allow_loopback", FALSE,
	        "Permit HTTP loopback issuers only for isolated test deployments"),
	VC_STRV("oidc-allowed-issuers", "oidc", "allowed_issuers",
	        "Exact platform-approved issuer URLs; empty refuses all providers"),
	VC_BOOL("payroll-enabled", "payroll", "enabled", FALSE,
	        "Enable imported payroll runs; native tax calculation is out of scope"),
	VC_BOOL("bankfeed-enabled", "bankfeed", "enabled", FALSE,
	        "Enable pluggable bank feeds; requires VENTURE_BANKFEED_TELLER_KEY"),
	VC_BOOL("commerce-enabled", "commerce", "enabled", FALSE,
	        "Enable commerce connectors with explicit organization account settings"),
	VC_BOOL("group-enabled", "group", "enabled", FALSE,
	        "Enable intercompany links, eliminations and consolidated statements"),

	VC_BOOL("forge-enabled", "forge", "enabled", TRUE,
	        "Enable git forge integration"),
	VC_INT ("forge-request-timeout", "forge", "request_timeout", 30,
	        "Seconds an outbound forge call may take"),
	VC_BOOL("forge-webhooks-enabled", "forge", "webhooks_enabled", TRUE,
	        "Accept inbound webhooks at /hooks/forge/:id"),
	VC_BOOL("forge-runs-enabled", "forge", "runs_enabled", FALSE,
	        "Let rules start AI coding runs"),
	VC_INT ("forge-run-concurrency", "forge", "run_concurrency", 1,
	        "Coding runs in flight at once"),
	VC_INT ("forge-run-timeout", "forge", "run_timeout", 1800,
	        "Seconds a coding run may take before it is abandoned"),
	VC_INT ("forge-run-max-turns", "forge", "run_max_turns", 40,
	        "Turns a coding run may take before it is cut off"),
	VC_STR ("forge-workspace-dir", "forge", "workspace_dir", "forge",
	        "Directory under the state directory holding checkouts"),
	VC_INT ("forge-workspace-retention-hours", "forge",
	        "workspace_retention_hours", 168,
	        "Hours a finished run's checkout is kept"),
	VC_BOOL("forge-keep-failed-workspaces", "forge", "keep_failed_workspaces",
	        TRUE, "Keep the checkout of a run that failed, to look at"),
	VC_STR ("forge-git-path", "forge", "git_path", "git",
	        "The git executable"),
	VC_INT ("forge-git-timeout", "forge", "git_timeout", 600,
	        "Seconds a single git invocation may take"),
	VC_STR ("forge-git-author-name", "forge", "git_author_name", "VENTURE",
	        "Author recorded on commits a run makes"),
	VC_STR ("forge-git-author-email", "forge", "git_author_email",
	        "venture@localhost", "Author email recorded on those commits"),
	VC_STRV("forge-cli-allowed-commands", "forge", "cli_allowed_commands",
	        "Commands the CLI runner may spawn; anything else is refused"),
	/*
	 * Directories an agent-harness session may be pointed at.
	 *
	 * Empty -- the default -- means a session may only work in a
	 * checkout it cloned itself, under the workspace directory. A path
	 * listed here lets a session work directly in a tree on this
	 * machine, which is what somebody running VENTURE beside their own
	 * code wants and is also write access for a coding agent, so it is
	 * something an operator turns on deliberately rather than a default.
	 */
	VC_STRV("forge-workspace-roots", "forge", "workspace_roots",
	        "Directories a harness session may work in directly; empty "
	        "means cloned checkouts only"),
	VC_INT ("forge-poll-interval", "forge", "poll_interval", 3,
	        "Seconds between run-status refreshes in the browser"),

	VC_BOOL("plugins-enabled", "plugins", "enabled", TRUE, "Enable plugins"),
	VC_STRV("plugins-paths", "plugins", "paths", "Native plugin directories"),
	VC_BOOL("plugins-allow-crispy", "plugins", "allow_crispy", TRUE,
	        "Compile and load .c plugins on demand"),
	VC_STRV("plugins-venture-type-paths", "plugins", "venture_type_paths",
	        "Directories of declarative venture-type definitions"),
	VC_STRV("plugins-required", "plugins", "required",
	        "Plugins that must load or startup fails"),

	VC_STR ("ui-theme", "ui", "theme", "system",
	        "system, light, dark or mocha"),
	VC_STR ("ui-look", "ui", "look", "industrial",
	        "classic or industrial"),
	VC_STR ("ui-accent", "ui", "accent", "",
	        "Accent colour; empty means the look's own"),
	VC_INT ("ui-page-size", "ui", "page_size", 50, "Rows per page"),
	VC_BOOL("ui-chat-dock", "ui", "chat_dock", TRUE, "Show the AI chat dock"),
	VC_BOOL("ui-chat-dock-expanded", "ui", "chat_dock_expanded", FALSE,
	        "Start the chat dock expanded"),
	VC_STR ("ui-title", "ui", "title", "VENTURE", "Title shown in the header"),

	VC_STR ("logging-level", "logging", "level", "info",
	        "error, warning, info or debug"),
	VC_STR ("logging-file", "logging", "file", "",
	        "Log file; empty means stderr"),
	VC_STR ("logging-format", "logging", "format", "text", "text or json"),
	VC_BOOL("logging-access-log", "logging", "access_log", TRUE,
	        "Log every request line"),

	/* Not part of the YAML document: set from --state-dir or derived. */
	VC_STR ("state-dir", NULL, NULL, "", "Directory holding runtime state"),
	VC_STR("imap-allowed-endpoints", "imap", "allowed_endpoints", "", "Operator-allowed IMAP host:port pairs, comma-separated; empty denies all"),
	VC_STR("calendar-allowed-origins", "calendar", "allowed_origins", "", "Operator-allowed CalDAV HTTPS origins, comma-separated; empty denies all"),
	VC_BOOL("connectors-allow-plaintext-loopback", "connectors", "allow_plaintext_loopback", FALSE, "Allow explicitly configured loopback IMAP fixtures without TLS; never permits remote plaintext"),
	VC_STR("mail-allowed-endpoints", "mail", "allowed_endpoints", "", "Operator-allowed organization SMTP host:port pairs, comma-separated; empty denies all"),
	VC_STR("mail-tls-ca-file", "mail", "tls_ca_file", "", "Operator-owned SMTP CA bundle; empty uses system trust"),
	VC_STR("mail-host", "mail", "host", "", "SMTP relay host"),
	VC_INT("mail-port", "mail", "port", 587, "SMTP relay port"),
	VC_STR("mail-security", "mail", "security", "starttls", "starttls, tls or none"),
	VC_STR("mail-username", "mail", "username", "", "SMTP username; empty disables authentication"),
	VC_STR("mail-password-env", "mail", "password_env", "VENTURE_SMTP_PASSWORD", "Environment variable NAME holding the SMTP password"),
	VC_STR("mail-from-address", "mail", "from_address", "", "Sender address"),
	VC_STR("mail-from-name", "mail", "from_name", "Venture", "Sender display name"),
	VC_STR("mail-reply-to", "mail", "reply_to", "", "Default reply address"),
	VC_STR("backup-directory", "backup", "directory", "backups", "Where scheduled backups are written when a schedule names no destination; relative to the state directory"),
	VC_INT("backup-retention", "backup", "retention", 7, "Successful backups kept per schedule when the schedule says zero"),
	VC_STR("security-mfa-key-env", "security", "mfa_key_env", "VENTURE_MFA_KEY",
	        "Environment variable holding the key that encrypts stored second-factor secrets; falls back to session_secret_env"),

	/*
	 * Where /docs reads the rendered documentation from. `make install`
	 * puts the site here; a tree run sets VENTURE_DOCS_SITE_DIR to its
	 * build/docs-site instead.
	 */
	VC_STR("docs-site-dir", "docs", "site_dir", VENTURE_DATADIR "/docs-site",
	       "Directory holding the rendered documentation site served at /docs"),
};

#define VENTURE_CONFIG_N_SETTINGS G_N_ELEMENTS(venture_config_settings)
#define VENTURE_CONFIG_PROP_BASE (1)

struct _VentureConfig
{
	GObject parent_instance;

	/* property name -> GValue, in the same generic style the record
	 * types use. */
	GHashTable	*values;

	gchar		*resolved_state_dir;
	gchar		*loaded_from;

	/*
	 * module name -> GINT_TO_POINTER(enabled). The `modules` section is
	 * not a flat setting like the rest: its keys are module names, which
	 * a plugin can add to, so they cannot be enumerated in the static
	 * table above. Held apart and resolved by #VentureModuleRegistry.
	 */
	GHashTable	*module_switches;
};

static GParamSpec *venture_config_properties[VENTURE_CONFIG_N_SETTINGS + 1];

enum
{
	SIGNAL_MODULE_SWITCH_CHANGED,
	N_SIGNALS
};

static guint venture_config_signals[N_SIGNALS] = { 0 };

G_DEFINE_FINAL_TYPE(VentureConfig, venture_config, G_TYPE_OBJECT)

/* --- Property plumbing --------------------------------------------------- */

static void
venture_config_free_value(gpointer data)
{
	GValue *value;

	value = data;

	g_value_unset(value);
	g_free(value);
}

static void
venture_config_get_property(
	GObject		*object,
	guint		 prop_id,
	GValue		*value,
	GParamSpec	*pspec
){
	VentureConfig *self;
	const GValue *stored;

	self = VENTURE_CONFIG(object);
	stored = g_hash_table_lookup(self->values, pspec->name);

	if (NULL != stored)
	{
		g_value_copy(stored, value);
		return;
	}

	(void)prop_id;
}

static void
venture_config_set_property(
	GObject		*object,
	guint		 prop_id,
	const GValue	*value,
	GParamSpec	*pspec
){
	VentureConfig *self;
	GValue *stored;

	self = VENTURE_CONFIG(object);

	stored = g_new0(GValue, 1);
	g_value_init(stored, pspec->value_type);
	g_value_copy(value, stored);

	g_hash_table_insert(self->values, g_strdup(pspec->name), stored);

	(void)prop_id;
}

static void
venture_config_finalize(GObject *object)
{
	VentureConfig *self;

	self = VENTURE_CONFIG(object);

	g_clear_pointer(&self->values, g_hash_table_unref);
	g_clear_pointer(&self->module_switches, g_hash_table_unref);
	g_clear_pointer(&self->resolved_state_dir, g_free);
	g_clear_pointer(&self->loaded_from, g_free);

	G_OBJECT_CLASS(venture_config_parent_class)->finalize(object);
}

static void
venture_config_class_init(VentureConfigClass *klass)
{
	GObjectClass *object_class;
	gsize i;

	object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = venture_config_get_property;
	object_class->set_property = venture_config_set_property;
	object_class->finalize = venture_config_finalize;

	/**
	 * VentureConfig::module-switch-changed:
	 * @self: the configuration
	 * @module_name: the module whose switch changed
	 *
	 * Emitted when a module switch is set or cleared. The module
	 * registry listens, alongside the ordinary property notifications,
	 * so a switch flipped after the registry was configured is still
	 * honoured.
	 */
	venture_config_signals[SIGNAL_MODULE_SWITCH_CHANGED] =
		g_signal_new("module-switch-changed", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
		             G_TYPE_STRING);

	for (i = 0; i < VENTURE_CONFIG_N_SETTINGS; i++)
	{
		const VentureConfigSetting *setting;
		GParamSpec *pspec;

		setting = &venture_config_settings[i];

		if (NULL != setting->enum_type_func)
		{
			pspec = g_param_spec_enum(setting->name, setting->name,
			                          setting->blurb,
			                          setting->enum_type_func(), 0,
			                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
		}
		else if (G_TYPE_STRING == setting->value_type)
		{
			pspec = g_param_spec_string(setting->name, setting->name,
			                            setting->blurb,
			                            setting->default_string,
			                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
		}
		else if (G_TYPE_INT64 == setting->value_type)
		{
			pspec = g_param_spec_int64(setting->name, setting->name,
			                           setting->blurb,
			                           G_MININT64, G_MAXINT64,
			                           setting->default_int,
			                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
		}
		else if (G_TYPE_BOOLEAN == setting->value_type)
		{
			pspec = g_param_spec_boolean(setting->name, setting->name,
			                             setting->blurb,
			                             setting->default_bool,
			                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
		}
		else
		{
			pspec = g_param_spec_boxed(setting->name, setting->name,
			                           setting->blurb, G_TYPE_STRV,
			                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
		}

		venture_config_properties[i] = pspec;
		g_object_class_install_property(object_class,
		                                (guint)(VENTURE_CONFIG_PROP_BASE + i),
		                                pspec);
	}
}

static void
venture_config_init(VentureConfig *self)
{
	gsize i;

	self->module_switches = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                              g_free, NULL);
	self->values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                     venture_config_free_value);

	/* Seed every setting with its declared default, so a getter never has
	 * to distinguish "unset" from "default" and the YAML writer can emit a
	 * complete document. */
	for (i = 0; i < VENTURE_CONFIG_N_SETTINGS; i++)
	{
		const VentureConfigSetting *setting;
		GValue value = G_VALUE_INIT;

		setting = &venture_config_settings[i];

		if (NULL != setting->enum_type_func)
		{
			g_value_init(&value, setting->enum_type_func());
			/* The zero value of every configuration enum is its
			 * intended default, which for the AI policy is the
			 * most restrictive one. */
			g_value_set_enum(&value, 0);
		}
		else if (G_TYPE_STRING == setting->value_type)
		{
			g_value_init(&value, G_TYPE_STRING);
			g_value_set_string(&value, setting->default_string);
		}
		else if (G_TYPE_INT64 == setting->value_type)
		{
			g_value_init(&value, G_TYPE_INT64);
			g_value_set_int64(&value, setting->default_int);
		}
		else if (G_TYPE_BOOLEAN == setting->value_type)
		{
			g_value_init(&value, G_TYPE_BOOLEAN);
			g_value_set_boolean(&value, setting->default_bool);
		}
		else
		{
			g_value_init(&value, G_TYPE_STRV);
			g_value_set_boxed(&value, NULL);
		}

		g_object_set_property(G_OBJECT(self), setting->name, &value);
		g_value_unset(&value);
	}

	/* The AI policy defaults to staging writes for approval rather than
	 * to the zero value, which would be read-only and make the assistant
	 * useless for anything but questions. */
	g_object_set(self, "ai-policy", VENTURE_AI_POLICY_CONFIRM_WRITES, NULL);
}

VentureConfig *
venture_config_new(void)
{
	return g_object_new(VENTURE_TYPE_CONFIG, NULL);
}

/* --- YAML --------------------------------------------------------------- */

static const VentureConfigSetting *
venture_config_find_setting(
	const gchar	*section,
	const gchar	*key
){
	gsize i;

	for (i = 0; i < VENTURE_CONFIG_N_SETTINGS; i++)
	{
		const VentureConfigSetting *setting;

		setting = &venture_config_settings[i];

		if (NULL == setting->section)
			continue;

		if ((0 == g_strcmp0(setting->section, section)) &&
		    (0 == g_strcmp0(setting->key, key)))
			return setting;
	}

	return NULL;
}

/*
 * Applies one section of the parsed document. Unknown keys are collected
 * rather than ignored: a setting that silently does nothing because of a
 * typo is the most confusing configuration failure there is, and the whole
 * point of enumerating settings is to be able to catch it.
 */
static void
venture_config_apply_section(
	VentureConfig	*self,
	const gchar	*section,
	JsonObject	*object,
	GPtrArray	*unknown
){
	g_autoptr(GList) members = NULL;
	GList *iter;

	members = json_object_get_members(object);

	for (iter = members; NULL != iter; iter = iter->next)
	{
		const VentureConfigSetting *setting;
		g_auto(GValue) value = G_VALUE_INIT;
		g_autoptr(GError) local_error = NULL;
		GParamSpec *pspec;
		JsonNode *node;

		setting = venture_config_find_setting(section, iter->data);

		if (NULL == setting)
		{
			g_ptr_array_add(unknown,
			                g_strdup_printf("%s.%s", section,
			                                (const gchar *)iter->data));
			continue;
		}

		pspec = g_object_class_find_property(
			G_OBJECT_GET_CLASS(self), setting->name);
		node = json_object_get_member(object, iter->data);

		if (!venture_json_value_from_node(node, pspec->value_type,
		                                  &value, &local_error))
		{
			g_warning("Ignoring %s.%s: %s", section,
			          (const gchar *)iter->data, local_error->message);
			continue;
		}

		g_object_set_property(G_OBJECT(self), setting->name, &value);
	}
}

/*
 * Whether a module name may appear as a configuration key. The same
 * alphabet venture_module_registry_add() enforces, checked here too so a
 * malformed key is reported where it was written.
 */
static gboolean
venture_config_module_name_is_valid(const gchar *name)
{
	const gchar *cursor;

	if (venture_string_is_empty(name))
		return FALSE;

	for (cursor = name; '\0' != *cursor; cursor++)
	{
		if (!g_ascii_islower(*cursor) && !g_ascii_isdigit(*cursor) &&
		    ('_' != *cursor) && ('-' != *cursor))
			return FALSE;
	}

	return TRUE;
}

/*
 * Applies the `modules` section. Each key is a module name and its value
 * is either a bare boolean or a mapping with an `enabled` key:
 *
 *   modules:
 *     crm: false
 *     factory:
 *       enabled: true
 *
 * Both spellings are accepted because the short one is what a person
 * writes and the long one is what leaves room for per-module settings
 * later. Anything else under a module is reported as unknown, the same
 * way a misspelt setting anywhere else is.
 *
 * Whether a name is a real module is not known here -- a plugin can add
 * one -- so that check lives in venture_module_registry_check_configured()
 * and runs once plugins have loaded.
 */
static void
venture_config_apply_modules_section(
	VentureConfig	*self,
	JsonObject	*object,
	GPtrArray	*unknown
){
	g_autoptr(GList) members = NULL;
	GList *iter;

	members = json_object_get_members(object);

	for (iter = members; NULL != iter; iter = iter->next)
	{
		const gchar *name;
		JsonNode *node;

		name = iter->data;
		node = json_object_get_member(object, name);

		if (!venture_config_module_name_is_valid(name))
		{
			g_warning("Ignoring modules.%s: not a module name (use "
			          "lowercase letters, digits, - and _)", name);
			continue;
		}

		if (JSON_NODE_HOLDS_VALUE(node) &&
		    (G_TYPE_BOOLEAN == json_node_get_value_type(node)))
		{
			venture_config_set_module_enabled(self, name,
			                                  json_node_get_boolean(node));
			continue;
		}

		if (JSON_NODE_HOLDS_OBJECT(node))
		{
			g_autoptr(GList) keys = NULL;
			JsonObject *settings;
			JsonNode *enabled;
			GList *key;

			settings = json_node_get_object(node);
			enabled = json_object_get_member(settings, "enabled");

			if ((NULL != enabled) && JSON_NODE_HOLDS_VALUE(enabled) &&
			    (G_TYPE_BOOLEAN == json_node_get_value_type(enabled)))
			{
				venture_config_set_module_enabled(
					self, name, json_node_get_boolean(enabled));
			}
			else if (NULL != enabled)
			{
				g_warning("Ignoring modules.%s.enabled: expected "
				          "true or false", name);
			}

			keys = json_object_get_members(settings);

			for (key = keys; NULL != key; key = key->next)
			{
				if (0 != g_strcmp0(key->data, "enabled"))
				{
					g_ptr_array_add(unknown,
					                g_strdup_printf("modules.%s.%s", name,
					                                (const gchar *)key->data));
				}
			}

			continue;
		}

		g_warning("Ignoring modules.%s: expected true, false, or a "
		          "mapping with an enabled key", name);
	}
}

void
venture_config_set_module_enabled(
	VentureConfig	*self,
	const gchar	*module_name,
	gboolean	 enabled
){
	g_return_if_fail(VENTURE_IS_CONFIG(self));
	g_return_if_fail(NULL != module_name);

	g_hash_table_insert(self->module_switches, g_strdup(module_name),
	                    GINT_TO_POINTER(enabled ? 1 : 0));

	g_signal_emit(self, venture_config_signals[SIGNAL_MODULE_SWITCH_CHANGED],
	              0, module_name);
}

void
venture_config_clear_module_switch(
	VentureConfig	*self,
	const gchar	*module_name
){
	g_return_if_fail(VENTURE_IS_CONFIG(self));
	g_return_if_fail(NULL != module_name);

	g_hash_table_remove(self->module_switches, module_name);

	g_signal_emit(self, venture_config_signals[SIGNAL_MODULE_SWITCH_CHANGED],
	              0, module_name);
}

gboolean
venture_config_get_module_switch(
	VentureConfig	*self,
	const gchar	*module_name,
	gboolean	*out_enabled
){
	gpointer value;

	g_return_val_if_fail(VENTURE_IS_CONFIG(self), FALSE);
	g_return_val_if_fail(NULL != module_name, FALSE);

	if (!g_hash_table_lookup_extended(self->module_switches, module_name,
	                                  NULL, &value))
		return FALSE;

	if (NULL != out_enabled)
		*out_enabled = (0 != GPOINTER_TO_INT(value));

	return TRUE;
}

gchar **
venture_config_list_module_switches(VentureConfig *self)
{
	g_autoptr(GPtrArray) names = NULL;
	g_autoptr(GList) keys = NULL;
	GList *iter;

	g_return_val_if_fail(VENTURE_IS_CONFIG(self), NULL);

	keys = g_hash_table_get_keys(self->module_switches);
	keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
	names = g_ptr_array_new();

	for (iter = keys; NULL != iter; iter = iter->next)
		g_ptr_array_add(names, g_strdup(iter->data));

	g_ptr_array_add(names, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&names), FALSE);
}

gboolean
venture_config_theme_is_valid(const gchar *theme)
{
	static const gchar *const themes[] = {
		"system", "light", "dark", "mocha", NULL
	};

	return (NULL != theme) && g_strv_contains(themes, theme);
}

gboolean
venture_config_look_is_valid(const gchar *look)
{
	static const gchar *const looks[] = { "classic", "industrial", NULL };

	return (NULL != look) && g_strv_contains(looks, look);
}

gboolean
venture_config_apply_yaml_string(
	VentureConfig	 *self,
	const gchar	 *yaml,
	GError		**error
){
	g_autoptr(YamlParser) parser = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autoptr(GPtrArray) unknown = NULL;
	g_autoptr(GList) sections = NULL;
	YamlNode *yaml_root;
	JsonObject *object;
	GList *iter;

	g_return_val_if_fail(VENTURE_IS_CONFIG(self), FALSE);
	g_return_val_if_fail(NULL != yaml, FALSE);

	parser = yaml_parser_new();

	if (!yaml_parser_load_from_data(parser, yaml, -1, &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "Invalid YAML: %s", local_error->message);
		return FALSE;
	}

	yaml_root = yaml_parser_get_root(parser);

	/* An empty document is valid and simply changes nothing. */
	if ((NULL == yaml_root) || yaml_node_is_null(yaml_root))
		return TRUE;

	/* Converting to JSON lets the same tolerant value decoder handle both
	 * formats, so "8747" and 8747 mean the same thing in either. */
	root = yaml_node_to_json_node(yaml_root);

	if ((NULL == root) || !JSON_NODE_HOLDS_OBJECT(root))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "The configuration must be a mapping of sections");
		return FALSE;
	}

	object = json_node_get_object(root);
	sections = json_object_get_members(object);
	unknown = g_ptr_array_new_with_free_func(g_free);

	for (iter = sections; NULL != iter; iter = iter->next)
	{
		JsonNode *section_node;

		section_node = json_object_get_member(object, iter->data);

		if (!JSON_NODE_HOLDS_OBJECT(section_node))
		{
			g_ptr_array_add(unknown, g_strdup(iter->data));
			continue;
		}

		if (0 == g_strcmp0(iter->data, "modules"))
		{
			venture_config_apply_modules_section(
				self, json_node_get_object(section_node), unknown);
			continue;
		}

		venture_config_apply_section(self, iter->data,
		                             json_node_get_object(section_node),
		                             unknown);
	}

	if (unknown->len > 0)
	{
		g_autofree gchar *list = NULL;

		g_ptr_array_add(unknown, NULL);
		list = g_strjoinv(", ", (gchar **)unknown->pdata);

		/* A warning rather than an error: an unknown key is usually a
		 * setting from a newer or older version, and refusing to start
		 * over one would be worse than carrying on without it. */
		g_warning("Ignoring unknown configuration settings: %s", list);
	}

	return TRUE;
}

gboolean
venture_config_apply_yaml_file(
	VentureConfig	 *self,
	const gchar	 *path,
	GError		**error
){
	g_autofree gchar *contents = NULL;
	g_autoptr(GError) local_error = NULL;

	g_return_val_if_fail(VENTURE_IS_CONFIG(self), FALSE);
	g_return_val_if_fail(NULL != path, FALSE);

	if (!g_file_get_contents(path, &contents, NULL, &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "Cannot read %s: %s", path, local_error->message);
		return FALSE;
	}

	if (!venture_config_apply_yaml_string(self, contents, error))
	{
		g_prefix_error(error, "%s: ", path);
		return FALSE;
	}

	g_free(self->loaded_from);
	self->loaded_from = g_strdup(path);

	return TRUE;
}

/* --- Environment --------------------------------------------------------- */

void
venture_config_apply_environment(VentureConfig *self)
{
	gsize i;

	g_return_if_fail(VENTURE_IS_CONFIG(self));

	for (i = 0; i < VENTURE_CONFIG_N_SETTINGS; i++)
	{
		const VentureConfigSetting *setting;
		g_autofree gchar *variable = NULL;
		g_autofree gchar *upper = NULL;
		g_auto(GValue) value = G_VALUE_INIT;
		g_autoptr(GError) local_error = NULL;
		g_autoptr(JsonNode) node = NULL;
		GParamSpec *pspec;
		const gchar *text;

		setting = &venture_config_settings[i];

		upper = g_ascii_strup(setting->name, -1);
		g_strdelimit(upper, "-", '_');
		variable = g_strdup_printf("VENTURE_%s", upper);

		text = g_getenv(variable);

		if (NULL == text)
			continue;

		pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(self),
		                                     setting->name);

		/* A list-valued setting takes a comma-separated environment
		 * value, which is the only sane encoding for one variable. */
		if (G_TYPE_STRV == pspec->value_type)
		{
			g_auto(GStrv) parts = NULL;

			parts = g_strsplit(text, ",", -1);
			g_object_set(self, setting->name, parts, NULL);
			continue;
		}

		node = json_node_init_string(json_node_alloc(), text);

		if (!venture_json_value_from_node(node, pspec->value_type,
		                                  &value, &local_error))
		{
			g_warning("Ignoring %s: %s", variable, local_error->message);
			continue;
		}

		g_object_set_property(G_OBJECT(self), setting->name, &value);
	}

	/*
	 * VENTURE_MODULE_<NAME>=true|false switches a module, the way
	 * VENTURE_<SECTION>_<KEY> sets a setting. The whole environment is
	 * scanned rather than a known list looked up, because the module
	 * names are not a known list: a plugin's module is switched the same
	 * way. The name is lowercased back, so VENTURE_MODULE_CRM is
	 * modules.crm.
	 */
	{
		g_auto(GStrv) names = NULL;
		gsize n;

		names = g_listenv();

		for (n = 0; (NULL != names) && (NULL != names[n]); n++)
		{
			g_autofree gchar *module_name = NULL;
			g_autoptr(JsonNode) node = NULL;
			g_auto(GValue) value = G_VALUE_INIT;
			g_autoptr(GError) local_error = NULL;
			const gchar *text;

			if (!g_str_has_prefix(names[n], "VENTURE_MODULE_"))
				continue;

			module_name = g_ascii_strdown(
				names[n] + strlen("VENTURE_MODULE_"), -1);

			if (!venture_config_module_name_is_valid(module_name))
			{
				g_warning("Ignoring %s: not a module name", names[n]);
				continue;
			}

			text = g_getenv(names[n]);

			if (NULL == text)
				continue;

			node = json_node_init_string(json_node_alloc(), text);

			if (!venture_json_value_from_node(node, G_TYPE_BOOLEAN, &value,
			                                  &local_error))
			{
				g_warning("Ignoring %s: %s", names[n],
				          local_error->message);
				continue;
			}

			venture_config_set_module_enabled(self, module_name,
			                                  g_value_get_boolean(&value));
		}
	}
}

/* --- Compiled C configuration -------------------------------------------- */

gboolean
venture_config_apply_c_config(
	VentureConfig	 *self,
	const gchar	 *path,
	GError		**error
){
#ifdef VENTURE_SERVER_BUILD
	g_autoptr(VentureCrispyHost) host = NULL;
	g_autofree gchar *cache_dir = NULL;
	VentureConfigureFunc configure = NULL;
	gpointer symbol = NULL;

	g_return_val_if_fail(VENTURE_IS_CONFIG(self), FALSE);
	g_return_val_if_fail(NULL != path, FALSE);

	if (!g_file_test(path, G_FILE_TEST_EXISTS))
		return TRUE;

	cache_dir = g_build_filename(venture_config_get_state_dir(self),
	                             "crispy-cache", NULL);
	host = venture_crispy_host_new(cache_dir, error);

	if (NULL == host)
		return FALSE;

	if (!venture_crispy_host_lookup(host, path, "venture_configure",
	                                &symbol, NULL, error))
		return FALSE;

	configure = (VentureConfigureFunc)symbol;

	if (!configure(self, error))
	{
		if ((NULL != error) && (NULL == *error))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			            "%s: venture_configure() refused to continue "
			            "without saying why", path);
		}

		return FALSE;
	}

	return TRUE;
#else
	g_return_val_if_fail(VENTURE_IS_CONFIG(self), FALSE);

	/* libventure-core.a has no compiler dependency; a CLI-only build
	 * simply has no compiled configuration. */
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
	                    "This build has no support for compiled C configuration");
	(void)path;

	return FALSE;
#endif
}

/* --- Loading ------------------------------------------------------------- */

VentureConfig *
venture_config_load(
	const gchar	 *explicit_path,
	GError		**error
){
	g_autoptr(VentureConfig) self = NULL;
	g_autofree gchar *user_path = NULL;
	g_autofree gchar *user_c_path = NULL;
	const gchar *system_path = VENTURE_SYSCONFDIR "/venture/config.yaml";

	self = venture_config_new();

	/* The compiled defaults are applied as YAML rather than assumed, so
	 * the shipped document is exercised on every single startup and
	 * cannot rot into something that no longer parses. */
	if (!venture_config_apply_yaml_string(self, venture_default_yaml_config,
	                                      error))
	{
		g_prefix_error(error, "built-in defaults: ");
		return NULL;
	}

	if (g_file_test(system_path, G_FILE_TEST_EXISTS))
	{
		if (!venture_config_apply_yaml_file(self, system_path, error))
			return NULL;
	}

	user_path = g_build_filename(g_get_user_config_dir(), "venture",
	                             "config.yaml", NULL);

	if (g_file_test(user_path, G_FILE_TEST_EXISTS))
	{
		if (!venture_config_apply_yaml_file(self, user_path, error))
			return NULL;
	}

	if (NULL != explicit_path)
	{
		/* A file named explicitly must exist. Silently carrying on with
		 * defaults because of a mistyped path is exactly the failure
		 * that wastes an afternoon. */
		if (!venture_config_apply_yaml_file(self, explicit_path, error))
			return NULL;
	}

	/* The compiled configuration sits beside whichever YAML file was
	 * loaded last, so the pair travels together. */
	{
		g_autofree gchar *directory = NULL;

		directory = (NULL != self->loaded_from)
			? g_path_get_dirname(self->loaded_from)
			: g_build_filename(g_get_user_config_dir(), "venture", NULL);

		user_c_path = g_build_filename(directory, "config.c", NULL);
	}

	if (g_file_test(user_c_path, G_FILE_TEST_EXISTS))
	{
		if (!venture_config_apply_c_config(self, user_c_path, error))
			return NULL;
	}

	venture_config_apply_environment(self);

	/* Apply the currency now so that every VentureMoney created from here
	 * on defaults correctly, including during migration and seeding. */
	{
		g_autofree gchar *currency = NULL;

		g_object_get(self, "locale-default-currency", &currency, NULL);

		if (venture_currency_is_valid(currency))
			venture_money_set_default_currency(currency);
	}

	return g_steal_pointer(&self);
}

/* --- Validation ---------------------------------------------------------- */

gboolean
venture_config_validate(
	VentureConfig	 *self,
	GError		**error
){
	g_autofree gchar *bind_address = NULL;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *certificate = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *currency = NULL;
	gint64 port;
	gint64 fiscal_month;
	gboolean require_auth;

	g_return_val_if_fail(VENTURE_IS_CONFIG(self), FALSE);

	g_object_get(self,
	             "server-bind-address", &bind_address,
	             "server-port", &port,
	             "server-tls-certificate", &certificate,
	             "server-tls-private-key", &key,
	             "database-uri", &uri,
	             "security-require-auth", &require_auth,
	             "locale-default-currency", &currency,
	             "locale-fiscal-year-start-month", &fiscal_month,
	             NULL);

	{
		g_autofree gchar *theme = NULL;

		g_object_get(self, "ui-theme", &theme, NULL);

		if (!venture_config_theme_is_valid(theme))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			            "ui.theme must be system, light, dark or mocha, "
			            "not \"%s\"", (NULL != theme) ? theme : "");
			return FALSE;
		}
	}

	{
		g_autofree gchar *look = NULL;

		g_object_get(self, "ui-look", &look, NULL);

		if (!venture_config_look_is_valid(look))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			            "ui.look must be classic or industrial, not \"%s\"",
			            (NULL != look) ? look : "");
			return FALSE;
		}
	}

	if ((port < 1) || (port > 65535))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "server.port must be between 1 and 65535, not %"
		            G_GINT64_FORMAT, port);
		return FALSE;
	}

	/*
	 * Refusing to serve unauthenticated on a non-loopback address is the
	 * one hard rule here. This API can read and rewrite financial records
	 * and drive an AI that can do the same; exposing it to a network with
	 * no authentication is not a configuration choice, it is an accident.
	 */
	if (!require_auth &&
	    (0 != g_strcmp0(bind_address, "127.0.0.1")) &&
	    (0 != g_strcmp0(bind_address, "::1")) &&
	    (0 != g_strcmp0(bind_address, "localhost")))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "security.require_auth is off while server.bind_address "
		            "is \"%s\". Anything that can reach that address could "
		            "read and change your records. Either bind to 127.0.0.1 "
		            "or turn authentication on.",
		            bind_address);
		return FALSE;
	}

	/* Half-configured TLS would silently fall back to plain HTTP, which
	 * is the opposite of what someone setting one of these two wanted. */
	if (venture_string_is_empty(certificate) != venture_string_is_empty(key))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "server.tls_certificate and server.tls_private_key "
		                    "must be set together");
		return FALSE;
	}

	if (venture_string_is_empty(uri))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "database.uri is required");
		return FALSE;
	}

	if (!g_str_has_prefix(uri, "sqlite://") &&
	    !g_str_has_prefix(uri, "postgres://") &&
	    !g_str_has_prefix(uri, "postgresql://"))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "database.uri must begin with sqlite:// or postgres://, "
		            "not \"%s\"", uri);
		return FALSE;
	}

#ifndef VENTURE_HAVE_POSTGRES
	if (g_str_has_prefix(uri, "postgres"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
		                    "This build has no PostgreSQL support. Rebuild "
		                    "with libpq installed, or use a sqlite:// URI.");
		return FALSE;
	}
#endif

	if (!venture_currency_is_valid(currency))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "locale.default_currency must be a three-letter ISO 4217 "
		            "code, not \"%s\"", currency);
		return FALSE;
	}

	if ((fiscal_month < 1) || (fiscal_month > 12))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "locale.fiscal_year_start_month must be 1 to 12, not %"
		            G_GINT64_FORMAT, fiscal_month);
		return FALSE;
	}

	/*
	 * Modules last, once every plain setting has passed: the built-in
	 * modules and the legacy switches are known here, so "invoicing is
	 * on but the CRM is off" is caught before a database is opened. A
	 * plugin's module is checked again once plugins have loaded, in
	 * venture_module_registry_check_configured().
	 */
	{
		g_autoptr(VentureModuleRegistry) modules = NULL;
		g_autoptr(GError) local_error = NULL;

		modules = venture_module_registry_new();
		venture_module_registry_register_builtins(modules);

		if (!venture_module_registry_configure(modules, self, &local_error))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			            "modules: %s", local_error->message);
			return FALSE;
		}
#ifdef VENTURE_SERVER_BUILD
		if (venture_module_registry_is_enabled(modules, "ocr")) {
			g_autofree gchar *executable = NULL;
			g_object_get(self, "ocr-executable", &executable, NULL);
			if (!venture_ocr_local_check(executable, error)) return FALSE;
		}
#endif

	}

	return TRUE;
}

/* --- Serialisation ------------------------------------------------------- */

gchar *
venture_config_to_yaml(
	VentureConfig	*self,
	gboolean	 include_defaults
){
	g_autoptr(GString) yaml = NULL;
	const gchar *current_section = NULL;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_CONFIG(self), NULL);

	yaml = g_string_new("# VENTURE configuration\n");

	for (i = 0; i < VENTURE_CONFIG_N_SETTINGS; i++)
	{
		const VentureConfigSetting *setting;
		g_auto(GValue) value = G_VALUE_INIT;
		GParamSpec *pspec;

		setting = &venture_config_settings[i];

		if (NULL == setting->section)
			continue;

		pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(self),
		                                     setting->name);

		g_value_init(&value, pspec->value_type);
		g_object_get_property(G_OBJECT(self), setting->name, &value);

		if (!include_defaults &&
		    g_param_value_defaults(pspec, &value))
			continue;

		if (0 != g_strcmp0(current_section, setting->section))
		{
			g_string_append_printf(yaml, "\n%s:\n", setting->section);
			current_section = setting->section;
		}

		if (G_TYPE_STRV == pspec->value_type)
		{
			const gchar * const *items;
			gsize item;

			items = g_value_get_boxed(&value);

			if ((NULL == items) || (NULL == items[0]))
			{
				g_string_append_printf(yaml, "  %s: []\n", setting->key);
				continue;
			}

			g_string_append_printf(yaml, "  %s:\n", setting->key);

			for (item = 0; NULL != items[item]; item++)
			{
				g_string_append_printf(yaml, "    - \"%s\"\n",
				                       items[item]);
			}

			continue;
		}

		if (G_TYPE_BOOLEAN == pspec->value_type)
		{
			g_string_append_printf(yaml, "  %s: %s\n", setting->key,
			                       g_value_get_boolean(&value)
			                               ? "true" : "false");
			continue;
		}

		if (G_TYPE_INT64 == pspec->value_type)
		{
			g_string_append_printf(yaml, "  %s: %" G_GINT64_FORMAT "\n",
			                       setting->key, g_value_get_int64(&value));
			continue;
		}

		if (G_TYPE_IS_ENUM(pspec->value_type))
		{
			g_string_append_printf(yaml, "  %s: \"%s\"\n", setting->key,
			                       venture_enum_to_nick(pspec->value_type,
			                                            g_value_get_enum(&value)));
			continue;
		}

		g_string_append_printf(yaml, "  %s: \"%s\"\n", setting->key,
		                       (NULL != g_value_get_string(&value))
		                               ? g_value_get_string(&value) : "");
	}

	/* The module switches, only those actually set: an absent module is
	 * on by default, and writing every module out would turn a document
	 * that says what was decided into one that says everything. */
	{
		g_auto(GStrv) names = NULL;
		gsize n;

		names = venture_config_list_module_switches(self);

		if ((NULL != names) && (NULL != names[0]))
		{
			g_string_append(yaml, "\nmodules:\n");

			for (n = 0; NULL != names[n]; n++)
			{
				gboolean enabled = TRUE;

				venture_config_get_module_switch(self, names[n], &enabled);
				g_string_append_printf(yaml, "  %s:\n    enabled: %s\n",
				                       names[n], enabled ? "true" : "false");
			}
		}
	}

	return g_string_free(g_steal_pointer(&yaml), FALSE);
}

JsonNode *
venture_config_describe(VentureConfig *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_CONFIG(self), NULL);

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < VENTURE_CONFIG_N_SETTINGS; i++)
	{
		const VentureConfigSetting *setting;
		g_auto(GValue) value = G_VALUE_INIT;
		GParamSpec *pspec;

		setting = &venture_config_settings[i];

		pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(self),
		                                     setting->name);

		if (NULL == pspec)
			continue;

		json_builder_begin_object(builder);

		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, setting->name);

		json_builder_set_member_name(builder, "section");
		json_builder_add_string_value(builder,
			(NULL != setting->section) ? setting->section : "");

		json_builder_set_member_name(builder, "key");
		json_builder_add_string_value(builder,
			(NULL != setting->key) ? setting->key : setting->name);

		json_builder_set_member_name(builder, "help");
		json_builder_add_string_value(builder,
			(NULL != setting->blurb) ? setting->blurb : "");

		/*
		 * The environment variable that would override this one. Shown
		 * because the commonest configuration question on a running
		 * server is "which knob do I turn from a compose file".
		 */
		{
			g_autofree gchar *upper = NULL;
			g_autofree gchar *variable = NULL;

			upper = g_ascii_strup(setting->name, -1);
			g_strdelimit(upper, "-", '_');
			variable = g_strdup_printf("VENTURE_%s", upper);

			json_builder_set_member_name(builder, "env");
			json_builder_add_string_value(builder, variable);
		}

		g_value_init(&value, pspec->value_type);
		g_object_get_property(G_OBJECT(self), setting->name, &value);

		json_builder_set_member_name(builder, "value");

		if (G_TYPE_STRING == pspec->value_type)
		{
			const gchar *text;

			text = g_value_get_string(&value);

			if (0 == g_strcmp0(setting->name, "database-uri"))
			{
				g_autofree gchar *redacted = NULL;

				redacted = venture_string_redact_uri(text);
				json_builder_add_string_value(builder, redacted);
			}
			else
			{
				json_builder_add_string_value(builder,
					(NULL != text) ? text : "");
			}

			json_builder_set_member_name(builder, "type");
			json_builder_add_string_value(builder, "string");
		}
		else if (G_TYPE_INT64 == pspec->value_type)
		{
			json_builder_add_int_value(builder, g_value_get_int64(&value));
			json_builder_set_member_name(builder, "type");
			json_builder_add_string_value(builder, "integer");
		}
		else if (G_TYPE_BOOLEAN == pspec->value_type)
		{
			json_builder_add_boolean_value(builder,
			                               g_value_get_boolean(&value));
			json_builder_set_member_name(builder, "type");
			json_builder_add_string_value(builder, "boolean");
		}
		else if (G_TYPE_STRV == pspec->value_type)
		{
			g_auto(GStrv) items = NULL;
			gsize j;

			items = g_value_dup_boxed(&value);

			json_builder_begin_array(builder);

			for (j = 0; (NULL != items) && (NULL != items[j]); j++)
				json_builder_add_string_value(builder, items[j]);

			json_builder_end_array(builder);

			json_builder_set_member_name(builder, "type");
			json_builder_add_string_value(builder, "list");
		}
		else if (G_TYPE_IS_ENUM(pspec->value_type))
		{
			json_builder_add_string_value(builder,
				venture_enum_to_nick(pspec->value_type,
				                     g_value_get_enum(&value)));

			json_builder_set_member_name(builder, "type");
			json_builder_add_string_value(builder, "enum");
		}
		else
		{
			json_builder_add_null_value(builder);
			json_builder_set_member_name(builder, "type");
			json_builder_add_string_value(builder, "unknown");
		}

		/*
		 * A setting whose name ends in _env holds the NAME of an
		 * environment variable. Reporting whether that variable is
		 * actually set -- never its value -- answers "is the secret
		 * present" without disclosing anything.
		 */
		if (g_str_has_suffix(setting->name, "-env") &&
		    (G_TYPE_STRING == pspec->value_type))
		{
			const gchar *variable;

			variable = g_value_get_string(&value);

			json_builder_set_member_name(builder, "secret_present");
			json_builder_add_boolean_value(builder,
				(!venture_string_is_empty(variable)) &&
				(NULL != g_getenv(variable)));
		}

		json_builder_end_object(builder);
	}

	/* The module switches, in the same shape, so a client reading the
	 * settings sees them beside everything else. */
	{
		g_auto(GStrv) names = NULL;
		gsize n;

		names = venture_config_list_module_switches(self);

		for (n = 0; (NULL != names) && (NULL != names[n]); n++)
		{
			g_autofree gchar *upper = NULL;
			g_autofree gchar *variable = NULL;
			g_autofree gchar *key = NULL;
			g_autofree gchar *name = NULL;
			gboolean enabled = TRUE;

			venture_config_get_module_switch(self, names[n], &enabled);

			upper = g_ascii_strup(names[n], -1);
			g_strdelimit(upper, "-", '_');
			variable = g_strdup_printf("VENTURE_MODULE_%s", upper);
			key = g_strdup_printf("%s.enabled", names[n]);
			name = g_strdup_printf("modules-%s-enabled", names[n]);

			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "name");
			json_builder_add_string_value(builder, name);
			json_builder_set_member_name(builder, "section");
			json_builder_add_string_value(builder, "modules");
			json_builder_set_member_name(builder, "key");
			json_builder_add_string_value(builder, key);
			json_builder_set_member_name(builder, "description");
			json_builder_add_string_value(builder,
				"Whether the module is enabled");
			json_builder_set_member_name(builder, "env");
			json_builder_add_string_value(builder, variable);
			json_builder_set_member_name(builder, "value");
			json_builder_add_boolean_value(builder, enabled);
			json_builder_set_member_name(builder, "type");
			json_builder_add_string_value(builder, "boolean");
			json_builder_end_object(builder);
		}
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}

const gchar *
venture_config_get_loaded_from(VentureConfig *self)
{
	g_return_val_if_fail(VENTURE_IS_CONFIG(self), NULL);

	return self->loaded_from;
}

const gchar *
venture_config_get_default_yaml(void)
{
	return venture_default_yaml_config;
}

const gchar *
venture_config_get_default_c_config(void)
{
	return venture_default_c_config;
}

/* --- Accessors ----------------------------------------------------------- */

const gchar *
venture_config_get_secret(
	VentureConfig	*self,
	const gchar	*env_property
){
	g_autofree gchar *variable = NULL;

	g_return_val_if_fail(VENTURE_IS_CONFIG(self), NULL);
	g_return_val_if_fail(NULL != env_property, NULL);

	g_object_get(self, env_property, &variable, NULL);

	if (venture_string_is_empty(variable))
		return NULL;

	return g_getenv(variable);
}

const gchar *
venture_config_get_state_dir(VentureConfig *self)
{
	g_return_val_if_fail(VENTURE_IS_CONFIG(self), NULL);

	if (NULL != self->resolved_state_dir)
		return self->resolved_state_dir;

	{
		g_autofree gchar *configured = NULL;

		g_object_get(self, "state-dir", &configured, NULL);

		self->resolved_state_dir = venture_string_is_empty(configured)
			? g_build_filename(g_get_user_data_dir(), "venture", NULL)
			: g_strdup(configured);
	}

	/* Created on first use rather than at startup, so a command that
	 * never touches state -- `venture --version` -- leaves no trace. */
	if (0 != g_mkdir_with_parents(self->resolved_state_dir, 0700))
	{
		g_warning("Cannot create the state directory %s",
		          self->resolved_state_dir);
	}

	return self->resolved_state_dir;
}

gchar *
venture_config_resolve_path(
	VentureConfig	*self,
	const gchar	*path
){
	g_return_val_if_fail(VENTURE_IS_CONFIG(self), NULL);
	g_return_val_if_fail(NULL != path, NULL);

	if (g_path_is_absolute(path))
		return g_strdup(path);

	return g_build_filename(venture_config_get_state_dir(self), path, NULL);
}

const gchar *
venture_config_get_database_uri(VentureConfig *self)
{
	const GValue *stored;

	g_return_val_if_fail(VENTURE_IS_CONFIG(self), NULL);

	/* Returned without copying, which is safe because the value lives in
	 * the configuration's own table for as long as the object does. */
	stored = g_hash_table_lookup(self->values, "database-uri");

	return (NULL != stored) ? g_value_get_string(stored) : NULL;
}

void
venture_config_set_database_uri(
	VentureConfig	*self,
	const gchar	*uri
){
	g_return_if_fail(VENTURE_IS_CONFIG(self));

	g_object_set(self, "database-uri", uri, NULL);
}

VentureDatabaseBackend
venture_config_get_database_backend(VentureConfig *self)
{
	const gchar *uri;

	g_return_val_if_fail(VENTURE_IS_CONFIG(self),
	                     VENTURE_DATABASE_BACKEND_SQLITE);

	uri = venture_config_get_database_uri(self);

	if ((NULL != uri) && g_str_has_prefix(uri, "postgres"))
		return VENTURE_DATABASE_BACKEND_POSTGRES;

	return VENTURE_DATABASE_BACKEND_SQLITE;
}

VentureAiPolicy
venture_config_get_ai_policy(VentureConfig *self)
{
	VentureAiPolicy policy;

	g_return_val_if_fail(VENTURE_IS_CONFIG(self),
	                     VENTURE_AI_POLICY_READ_ONLY);

	g_object_get(self, "ai-policy", &policy, NULL);

	return policy;
}

void
venture_config_set_ai_policy(
	VentureConfig	*self,
	VentureAiPolicy	 policy
){
	g_return_if_fail(VENTURE_IS_CONFIG(self));

	g_object_set(self, "ai-policy", policy, NULL);
}

void
venture_config_set_ai_system_prompt_extra(
	VentureConfig	*self,
	const gchar	*text
){
	g_return_if_fail(VENTURE_IS_CONFIG(self));

	g_object_set(self, "ai-system-prompt-extra", text, NULL);
}

gboolean
venture_config_is_tool_auto_approved(
	VentureConfig	*self,
	const gchar	*tool_name
){
	g_auto(GStrv) tools = NULL;

	g_return_val_if_fail(VENTURE_IS_CONFIG(self), FALSE);
	g_return_val_if_fail(NULL != tool_name, FALSE);

	g_object_get(self, "ai-auto-approve-tools", &tools, NULL);

	if (NULL == tools)
		return FALSE;

	return g_strv_contains((const gchar * const *)tools, tool_name);
}

GTimeZone *
venture_config_get_timezone(VentureConfig *self)
{
	g_autofree gchar *name = NULL;

	g_return_val_if_fail(VENTURE_IS_CONFIG(self), NULL);

	g_object_get(self, "locale-timezone", &name, NULL);

	return venture_time_get_timezone(name);
}
