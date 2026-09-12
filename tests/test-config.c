/*
 * test-config.c - Layered configuration
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The most important case here is that the YAML document compiled into the
 * binary actually parses and maps onto every property. If it did not, the
 * shipped defaults would silently be whatever the C table said instead, and
 * nobody would notice until a setting failed to take effect.
 */

#include <venture.h>

#include "venture-test-util.h"

static void
test_config_defaults(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autofree gchar *bind_address = NULL;
	g_autofree gchar *currency = NULL;
	gint64 port;
	gboolean require_auth;

	config = venture_config_new();

	g_object_get(config,
	             "server-bind-address", &bind_address,
	             "server-port", &port,
	             "security-require-auth", &require_auth,
	             "locale-default-currency", &currency,
	             NULL);

	g_assert_cmpstr(bind_address, ==, "127.0.0.1");
	g_assert_cmpint(port, ==, 8747);
	g_assert_true(require_auth);
	g_assert_cmpstr(currency, ==, "USD");
}

/*
 * ui.theme is a closed set because the web layer writes it into an inline
 * script. A theme name that is not one of the four is refused at
 * validation, not rendered.
 */
static void
test_config_theme_is_a_closed_set(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	config = venture_config_new();

	g_assert_true(venture_config_theme_is_valid("mocha"));
	g_assert_true(venture_config_theme_is_valid("system"));
	g_assert_false(venture_config_theme_is_valid("latte"));
	g_assert_false(venture_config_theme_is_valid(NULL));

	g_object_set(config, "ui-theme", "mocha", NULL);
	g_assert_true(venture_config_validate(config, &error));
	g_assert_no_error(error);

	g_object_set(config, "ui-theme", "');alert(1);//", NULL);
	g_assert_false(venture_config_validate(config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

/*
 * ui.look is a closed set for the same reason: the web layer picks a whole
 * embedded stylesheet from it.
 */
static void
test_config_look_is_a_closed_set(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	config = venture_config_new();

	g_assert_true(venture_config_look_is_valid("classic"));
	g_assert_true(venture_config_look_is_valid("industrial"));
	g_assert_false(venture_config_look_is_valid("brutalist"));
	g_assert_false(venture_config_look_is_valid(NULL));

	g_object_set(config, "ui-look", "classic", NULL);
	g_assert_true(venture_config_validate(config, &error));
	g_assert_no_error(error);

	g_object_set(config, "ui-look", "../venture.css", NULL);
	g_assert_false(venture_config_validate(config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

static void
test_config_ai_policy_defaults_to_confirm(void)
{
	g_autoptr(VentureConfig) config = NULL;

	config = venture_config_new();

	/* Not the enum's zero value, which is read-only. Staging writes for
	 * approval is the intended default and the whole security posture. */
	g_assert_cmpint(venture_config_get_ai_policy(config), ==,
	                VENTURE_AI_POLICY_CONFIRM_WRITES);
}

static void
test_config_embedded_yaml_parses(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *provider = NULL;
	g_auto(GStrv) auto_approve = NULL;
	gint64 confirmation_ttl;

	config = venture_config_new();

	/* The shipped document is applied on every startup, so a change that
	 * broke it would break the product. This test is that guarantee. */
	g_assert_true(venture_config_apply_yaml_string(
		config, venture_config_get_default_yaml(), &error));
	g_assert_no_error(error);

	g_object_get(config,
	             "ai-provider", &provider,
	             "ai-confirmation-ttl", &confirmation_ttl,
	             "ai-auto-approve-tools", &auto_approve,
	             NULL);

	g_assert_cmpstr(provider, ==, "claude");
	g_assert_cmpint(confirmation_ttl, ==, 3600);
	g_assert_nonnull(auto_approve);
	g_assert_true(g_strv_contains((const gchar * const *)auto_approve,
	                              "venture_query"));
}

static void
test_config_yaml_overrides(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *bind_address = NULL;
	gint64 port;

	config = venture_config_new();

	g_assert_true(venture_config_apply_yaml_string(config,
		"server:\n"
		"  port: 9000\n"
		"  bind_address: \"0.0.0.0\"\n", &error));
	g_assert_no_error(error);

	g_object_get(config,
	             "server-port", &port,
	             "server-bind-address", &bind_address,
	             NULL);

	g_assert_cmpint(port, ==, 9000);
	g_assert_cmpstr(bind_address, ==, "0.0.0.0");
}

static void
test_config_yaml_partial_leaves_rest(void)
{
	g_autoptr(VentureConfig) config = NULL;
	gint64 port;
	gboolean require_auth;

	config = venture_config_new();

	g_assert_true(venture_config_apply_yaml_string(config,
		"security:\n  require_auth: false\n", NULL));

	g_object_get(config, "server-port", &port,
	             "security-require-auth", &require_auth, NULL);

	/* Applying one section must not reset the others. */
	g_assert_cmpint(port, ==, 8747);
	g_assert_false(require_auth);
}

static void
test_config_yaml_empty_document(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	config = venture_config_new();

	/* An empty file is valid and changes nothing. */
	g_assert_true(venture_config_apply_yaml_string(config, "", &error));
	g_assert_no_error(error);
	g_assert_true(venture_config_apply_yaml_string(config, "# only a comment\n",
	                                               &error));
	g_assert_no_error(error);
}

static void
test_config_yaml_invalid_rejected(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	config = venture_config_new();

	/* A scalar at the top level is not a mapping of sections. */
	g_assert_false(venture_config_apply_yaml_string(config, "just a string\n",
	                                                &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

static void
test_config_unknown_keys_warn_but_continue(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	gint64 port;

	config = venture_config_new();

	g_test_expect_message("Venture", G_LOG_LEVEL_WARNING,
	                      "*unknown configuration settings*");

	/* An unknown key is usually a setting from another version. Refusing
	 * to start over one would be worse than carrying on without it. */
	g_assert_true(venture_config_apply_yaml_string(config,
		"server:\n"
		"  port: 9100\n"
		"  nonsense_key: 1\n", &error));
	g_assert_no_error(error);

	g_test_assert_expected_messages();

	g_object_get(config, "server-port", &port, NULL);
	g_assert_cmpint(port, ==, 9100);
}

static void
test_config_enum_from_yaml(void)
{
	g_autoptr(VentureConfig) config = NULL;

	config = venture_config_new();

	g_assert_true(venture_config_apply_yaml_string(config,
		"ai:\n  policy: \"autonomous\"\n", NULL));
	g_assert_cmpint(venture_config_get_ai_policy(config), ==,
	                VENTURE_AI_POLICY_AUTONOMOUS);

	g_assert_true(venture_config_apply_yaml_string(config,
		"ai:\n  policy: \"read_only\"\n", NULL));
	g_assert_cmpint(venture_config_get_ai_policy(config), ==,
	                VENTURE_AI_POLICY_READ_ONLY);
}

static void
test_config_environment_overrides(void)
{
	g_autoptr(VentureConfig) config = NULL;
	gint64 port;
	g_autofree gchar *model = NULL;

	config = venture_config_new();

	g_setenv("VENTURE_SERVER_PORT", "9999", TRUE);
	g_setenv("VENTURE_AI_MODEL", "claude-opus-5", TRUE);

	venture_config_apply_environment(config);

	g_object_get(config, "server-port", &port, "ai-model", &model, NULL);

	g_assert_cmpint(port, ==, 9999);
	g_assert_cmpstr(model, ==, "claude-opus-5");

	g_unsetenv("VENTURE_SERVER_PORT");
	g_unsetenv("VENTURE_AI_MODEL");
}

static void
test_config_environment_list_is_comma_separated(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_auto(GStrv) paths = NULL;

	config = venture_config_new();

	g_setenv("VENTURE_PLUGINS_PATHS", "/one,/two,/three", TRUE);
	venture_config_apply_environment(config);

	g_object_get(config, "plugins-paths", &paths, NULL);

	g_assert_nonnull(paths);
	g_assert_cmpuint(g_strv_length(paths), ==, 3);
	g_assert_cmpstr(paths[1], ==, "/two");

	g_unsetenv("VENTURE_PLUGINS_PATHS");
}

/* --- Validation ---------------------------------------------------------- */

static void
test_config_validate_accepts_defaults(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	config = venture_config_new();

	g_assert_true(venture_config_validate(config, &error));
	g_assert_no_error(error);
}

static void
test_config_validate_rejects_open_unauthenticated(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	config = venture_config_new();

	/*
	 * The single hard rule: this API reads and rewrites financial records
	 * and drives an AI that can do the same. Serving it to a network with
	 * authentication off is not a configuration choice.
	 */
	g_object_set(config,
	             "server-bind-address", "0.0.0.0",
	             "security-require-auth", FALSE,
	             NULL);

	g_assert_false(venture_config_validate(config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(g_strstr_len(error->message, -1, "require_auth"));
}

static void
test_config_validate_allows_loopback_unauthenticated(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	config = venture_config_new();

	g_object_set(config,
	             "server-bind-address", "127.0.0.1",
	             "security-require-auth", FALSE,
	             NULL);

	g_assert_true(venture_config_validate(config, &error));
	g_assert_no_error(error);
}

static void
test_config_validate_rejects_half_configured_tls(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	config = venture_config_new();
	g_object_set(config, "server-tls-certificate", "/etc/cert.pem", NULL);

	/* Half-configured TLS would silently serve plain HTTP, which is the
	 * opposite of what setting a certificate meant. */
	g_assert_false(venture_config_validate(config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

static void
test_config_validate_rejects_bad_port(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	config = venture_config_new();
	g_object_set(config, "server-port", (gint64)70000, NULL);

	g_assert_false(venture_config_validate(config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

static void
test_config_validate_rejects_unknown_database_scheme(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	config = venture_config_new();
	venture_config_set_database_uri(config, "mysql://localhost/venture");

	g_assert_false(venture_config_validate(config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

static void
test_config_validate_rejects_bad_currency(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	config = venture_config_new();
	g_object_set(config, "locale-default-currency", "DOLLARS", NULL);

	g_assert_false(venture_config_validate(config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

static void
test_config_validate_rejects_bad_fiscal_month(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(GError) error = NULL;

	config = venture_config_new();
	g_object_set(config, "locale-fiscal-year-start-month", (gint64)13, NULL);

	g_assert_false(venture_config_validate(config, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

/* --- Derived behaviour --------------------------------------------------- */

static void
test_config_database_backend_from_uri(void)
{
	g_autoptr(VentureConfig) config = NULL;

	config = venture_config_new();

	g_assert_cmpint(venture_config_get_database_backend(config), ==,
	                VENTURE_DATABASE_BACKEND_SQLITE);

	venture_config_set_database_uri(config, "postgres://localhost/venture");
	g_assert_cmpint(venture_config_get_database_backend(config), ==,
	                VENTURE_DATABASE_BACKEND_POSTGRES);

	venture_config_set_database_uri(config, "postgresql://localhost/venture");
	g_assert_cmpint(venture_config_get_database_backend(config), ==,
	                VENTURE_DATABASE_BACKEND_POSTGRES);
}

static void
test_config_secret_from_environment(void)
{
	g_autoptr(VentureConfig) config = NULL;

	config = venture_config_new();
	g_object_set(config, "ai-api-key-env", "VENTURE_TEST_SECRET", NULL);

	/* Unset variable and unset setting must be distinguishable from a
	 * configured-but-empty one. */
	g_assert_null(venture_config_get_secret(config, "ai-api-key-env"));

	g_setenv("VENTURE_TEST_SECRET", "sk-test", TRUE);
	g_assert_cmpstr(venture_config_get_secret(config, "ai-api-key-env"),
	                ==, "sk-test");
	g_unsetenv("VENTURE_TEST_SECRET");

	g_object_set(config, "ai-api-key-env", "", NULL);
	g_assert_null(venture_config_get_secret(config, "ai-api-key-env"));
}

static void
test_config_auto_approved_tools(void)
{
	g_autoptr(VentureConfig) config = NULL;

	config = venture_config_new();

	g_assert_true(venture_config_apply_yaml_string(config,
		venture_config_get_default_yaml(), NULL));

	/* Read-only tools run unattended; anything that writes does not. */
	g_assert_true(venture_config_is_tool_auto_approved(config, "venture_query"));
	g_assert_true(venture_config_is_tool_auto_approved(config, "venture_report"));
	g_assert_false(venture_config_is_tool_auto_approved(config, "venture_create"));
	g_assert_false(venture_config_is_tool_auto_approved(config, "venture_delete"));
}

static void
test_config_resolve_path(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autofree gchar *absolute = NULL;
	g_autofree gchar *relative = NULL;
	g_autofree gchar *state_dir = NULL;

	config = venture_config_new();

	/*
	 * A real temporary directory rather than a fixed name under
	 * g_get_tmp_dir(): setting state-dir creates it, so the fixed name
	 * was left behind by every run -- and being fixed, it was invisible
	 * to the litter check, whose prefixes come from g_dir_make_tmp()
	 * literals.
	 */
	state_dir = g_dir_make_tmp("venture-config-XXXXXX", NULL);
	g_assert_nonnull(state_dir);
	g_object_set(config, "state-dir", state_dir, NULL);

	absolute = venture_config_resolve_path(config, "/etc/venture/x.pod");
	g_assert_cmpstr(absolute, ==, "/etc/venture/x.pod");

	relative = venture_config_resolve_path(config, "automations.pod");
	g_assert_true(g_str_has_prefix(relative, state_dir));
	g_assert_true(g_str_has_suffix(relative, "automations.pod"));

	venture_test_remove_tree(state_dir);
}

static void
test_config_to_yaml_round_trips(void)
{
	g_autoptr(VentureConfig) original = NULL;
	g_autoptr(VentureConfig) restored = NULL;
	g_autofree gchar *yaml = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *model = NULL;
	gint64 port;

	original = venture_config_new();
	g_object_set(original,
	             "server-port", (gint64)9123,
	             "ai-model", "claude-opus-5",
	             NULL);

	yaml = venture_config_to_yaml(original, TRUE);
	g_assert_nonnull(yaml);

	restored = venture_config_new();
	g_assert_true(venture_config_apply_yaml_string(restored, yaml, &error));
	g_assert_no_error(error);

	g_object_get(restored, "server-port", &port, "ai-model", &model, NULL);
	g_assert_cmpint(port, ==, 9123);
	g_assert_cmpstr(model, ==, "claude-opus-5");
}

static void
test_config_to_yaml_omits_defaults(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autofree gchar *yaml = NULL;

	config = venture_config_new();
	g_object_set(config, "server-port", (gint64)9123, NULL);

	yaml = venture_config_to_yaml(config, FALSE);

	g_assert_nonnull(g_strstr_len(yaml, -1, "9123"));
	/* A setting still at its default is not worth writing out. */
	g_assert_null(g_strstr_len(yaml, -1, "chat_dock"));
}

/*
 * A URI reaches a human in three places -- a log line when a connection
 * fails, the settings page, and an API response. In none of them may it
 * carry a password. The connection failure is the one that matters most:
 * that message is what somebody pastes into a bug report.
 */
static void
test_config_redacts_a_uri_password(void)
{
	g_autofree gchar *redacted = NULL;

	redacted = venture_string_redact_uri(
		"postgres://venture:sup3rs3cret@db.internal:5432/venture");

	g_assert_null(g_strstr_len(redacted, -1, "sup3rs3cret"));

	/* Everything that is not the password survives, because a redacted
	 * URI you cannot recognise is no use for diagnosing anything. */
	g_assert_nonnull(g_strstr_len(redacted, -1, "postgres://venture:"));
	g_assert_nonnull(g_strstr_len(redacted, -1, "@db.internal:5432/venture"));
}

static void
test_config_redaction_leaves_other_uris_alone(void)
{
	g_autofree gchar *no_userinfo = NULL;
	g_autofree gchar *no_password = NULL;
	g_autofree gchar *sqlite = NULL;

	no_userinfo = venture_string_redact_uri("postgres://db.internal/venture");
	g_assert_cmpstr(no_userinfo, ==, "postgres://db.internal/venture");

	no_password = venture_string_redact_uri("postgres://venture@db/venture");
	g_assert_cmpstr(no_password, ==, "postgres://venture@db/venture");

	/* A SQLite path has no authority, and a path containing an @ or a
	 * colon must not be mangled into something that will not open. */
	sqlite = venture_string_redact_uri("sqlite:///srv/venture/venture.db");
	g_assert_cmpstr(sqlite, ==, "sqlite:///srv/venture/venture.db");

	g_assert_null(venture_string_redact_uri(NULL));
}

static void
test_config_describe_covers_every_setting(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(JsonNode) described = NULL;
	JsonArray *settings;
	guint i;

	config = venture_config_new();
	described = venture_config_describe(config);

	g_assert_nonnull(described);
	settings = json_node_get_array(described);

	/*
	 * Every setting is described, and each carries what the settings page
	 * renders: a section to group under, a key, the environment variable
	 * that overrides it, and a sentence saying what it does. A setting
	 * added later without a blurb would show up as a blank row.
	 */
	g_assert_cmpuint(json_array_get_length(settings), >, 40);

	for (i = 0; i < json_array_get_length(settings); i++)
	{
		JsonObject *setting;

		setting = json_array_get_object_element(settings, i);

		g_assert_true(json_object_has_member(setting, "name"));
		g_assert_true(json_object_has_member(setting, "section"));
		g_assert_true(json_object_has_member(setting, "key"));
		g_assert_true(json_object_has_member(setting, "type"));
		g_assert_true(json_object_has_member(setting, "value"));

		g_assert_cmpstr(json_object_get_string_member(setting, "help"), !=, "");

		g_assert_true(g_str_has_prefix(
			json_object_get_string_member(setting, "env"), "VENTURE_"));
	}
}

static void
test_config_describe_redacts_the_database_password(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(JsonNode) described = NULL;
	JsonArray *settings;
	guint i;
	gboolean seen;

	config = venture_config_new();
	g_object_set(config, "database-uri",
	             "postgres://venture:sup3rs3cret@db/venture", NULL);

	described = venture_config_describe(config);
	settings = json_node_get_array(described);
	seen = FALSE;

	for (i = 0; i < json_array_get_length(settings); i++)
	{
		JsonObject *setting;

		setting = json_array_get_object_element(settings, i);

		if (0 != g_strcmp0(json_object_get_string_member(setting, "name"),
		                   "database-uri"))
			continue;

		seen = TRUE;
		g_assert_null(g_strstr_len(
			json_object_get_string_member(setting, "value"), -1,
			"sup3rs3cret"));
	}

	g_assert_true(seen);
}

static void
test_config_describe_reports_secret_presence_not_value(void)
{
	g_autoptr(VentureConfig) config = NULL;
	g_autoptr(JsonNode) described = NULL;
	JsonArray *settings;
	guint i;
	gboolean checked;

	g_setenv("VENTURE_TEST_SECRET_PRESENT", "the-actual-secret", TRUE);

	config = venture_config_new();
	g_object_set(config, "security-session-secret-env",
	             "VENTURE_TEST_SECRET_PRESENT", NULL);

	described = venture_config_describe(config);
	settings = json_node_get_array(described);
	checked = FALSE;

	for (i = 0; i < json_array_get_length(settings); i++)
	{
		JsonObject *setting;

		setting = json_array_get_object_element(settings, i);

		if (0 != g_strcmp0(json_object_get_string_member(setting, "name"),
		                   "security-session-secret-env"))
			continue;

		checked = TRUE;

		/*
		 * "Is the secret present" is the useful question and a safe one
		 * to answer. The value itself must never appear -- the setting
		 * holds only the variable's name.
		 */
		g_assert_true(json_object_get_boolean_member(setting,
		                                             "secret_present"));
		g_assert_cmpstr(json_object_get_string_member(setting, "value"), ==,
		                "VENTURE_TEST_SECRET_PRESENT");
	}

	g_assert_true(checked);

	g_unsetenv("VENTURE_TEST_SECRET_PRESENT");
}


int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/config/defaults", test_config_defaults);
	g_test_add_func("/config/ai-policy-defaults-to-confirm",
	                test_config_ai_policy_defaults_to_confirm);
	g_test_add_func("/config/theme-is-a-closed-set",
	                test_config_theme_is_a_closed_set);
	g_test_add_func("/config/look-is-a-closed-set",
	                test_config_look_is_a_closed_set);
	g_test_add_func("/config/embedded-yaml-parses", test_config_embedded_yaml_parses);
	g_test_add_func("/config/yaml-overrides", test_config_yaml_overrides);
	g_test_add_func("/config/yaml-partial-leaves-rest",
	                test_config_yaml_partial_leaves_rest);
	g_test_add_func("/config/yaml-empty-document", test_config_yaml_empty_document);
	g_test_add_func("/config/yaml-invalid-rejected", test_config_yaml_invalid_rejected);
	g_test_add_func("/config/unknown-keys-warn-but-continue",
	                test_config_unknown_keys_warn_but_continue);
	g_test_add_func("/config/enum-from-yaml", test_config_enum_from_yaml);

	g_test_add_func("/config/environment-overrides", test_config_environment_overrides);
	g_test_add_func("/config/environment-list-is-comma-separated",
	                test_config_environment_list_is_comma_separated);

	g_test_add_func("/config/validate-accepts-defaults",
	                test_config_validate_accepts_defaults);
	g_test_add_func("/config/validate-rejects-open-unauthenticated",
	                test_config_validate_rejects_open_unauthenticated);
	g_test_add_func("/config/validate-allows-loopback-unauthenticated",
	                test_config_validate_allows_loopback_unauthenticated);
	g_test_add_func("/config/validate-rejects-half-configured-tls",
	                test_config_validate_rejects_half_configured_tls);
	g_test_add_func("/config/validate-rejects-bad-port",
	                test_config_validate_rejects_bad_port);
	g_test_add_func("/config/validate-rejects-unknown-database-scheme",
	                test_config_validate_rejects_unknown_database_scheme);
	g_test_add_func("/config/validate-rejects-bad-currency",
	                test_config_validate_rejects_bad_currency);
	g_test_add_func("/config/validate-rejects-bad-fiscal-month",
	                test_config_validate_rejects_bad_fiscal_month);

	g_test_add_func("/config/database-backend-from-uri",
	                test_config_database_backend_from_uri);
	g_test_add_func("/config/secret-from-environment",
	                test_config_secret_from_environment);
	g_test_add_func("/config/auto-approved-tools", test_config_auto_approved_tools);
	g_test_add_func("/config/resolve-path", test_config_resolve_path);
	g_test_add_func("/config/to-yaml-round-trips", test_config_to_yaml_round_trips);
	g_test_add_func("/config/to-yaml-omits-defaults", test_config_to_yaml_omits_defaults);

	g_test_add_func("/config/redacts-a-uri-password",
	                test_config_redacts_a_uri_password);
	g_test_add_func("/config/redaction-leaves-other-uris-alone",
	                test_config_redaction_leaves_other_uris_alone);
	g_test_add_func("/config/describe-covers-every-setting",
	                test_config_describe_covers_every_setting);
	g_test_add_func("/config/describe-redacts-the-database-password",
	                test_config_describe_redacts_the_database_password);
	g_test_add_func("/config/describe-reports-secret-presence-not-value",
	                test_config_describe_reports_secret_presence_not_value);

	return g_test_run();
}
