/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * test-venture-settings.c - One settings API (docs/settings.org)
 *
 * The registry, its validation and defaults, permission, the one audit
 * entry per change, reset, each feature's registrations, organization
 * isolation, and the four ways in -- the settings screen, REST, venturectl
 * and the assistant's settings tools -- arriving at the same record
 * change and the same audit entry. The assistant is a canned provider
 * defined here, so nothing leaves the machine.
 */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

static gchar *client_path;

/* --- A canned AI provider that can call a tool ------------------------------ */

#define TYPE_SCRIPTED_PROVIDER (scripted_provider_get_type())
G_DECLARE_FINAL_TYPE(ScriptedProvider, scripted_provider, SCRIPTED, PROVIDER, GObject)
struct _ScriptedProvider {
	GObject parent_instance;
	GQueue *answers;
	guint calls;
};
static void scripted_iface(AiProviderInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(ScriptedProvider, scripted_provider, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER, scripted_iface))
static void scripted_finalize(GObject *object)
{
	ScriptedProvider *self = SCRIPTED_PROVIDER(object);
	g_queue_free_full(self->answers, g_free);
	G_OBJECT_CLASS(scripted_provider_parent_class)->finalize(object);
}
static void scripted_provider_class_init(ScriptedProviderClass *klass) { G_OBJECT_CLASS(klass)->finalize = scripted_finalize; }
static void scripted_provider_init(ScriptedProvider *self) { self->answers = g_queue_new(); }
/* An answer "TOOL name {json}" is a tool call; anything else is words. */
static void scripted_chat(AiProvider *provider, GList *messages, const gchar *system_prompt, gint max_tokens,
	GList *offered_tools, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
	ScriptedProvider *self = SCRIPTED_PROVIDER(provider);
	g_autoptr(GTask) task = g_task_new(provider, cancellable, callback, data);
	g_autofree gchar *answer = g_queue_pop_head(self->answers);
	AiResponse *response;
	(void)messages; (void)system_prompt; (void)max_tokens; (void)offered_tools;
	self->calls++;
	if (answer == NULL)
	{
		g_task_return_new_error(task, VENTURE_ERROR, VENTURE_ERROR_AI, "The scripted provider had no answer queued");
		return;
	}
	response = ai_response_new("fixture", "fixture");
	if (g_str_has_prefix(answer, "TOOL "))
	{
		g_auto(GStrv) parts = g_strsplit(answer + 5, " ", 2);
		g_autofree gchar *id = g_strdup_printf("call-%u", self->calls);
		AiToolUse *use = ai_tool_use_new_from_json_string(id, parts[0], parts[1]);
		ai_response_add_content_block(response, AI_CONTENT_BLOCK(use));
		ai_response_set_stop_reason(response, AI_STOP_REASON_TOOL_USE);
	}
	else
	{
		AiTextContent *text = ai_text_content_new(answer);
		ai_response_add_content_block(response, AI_CONTENT_BLOCK(text));
	}
	g_task_return_pointer(task, response, g_object_unref);
}
static AiResponse *scripted_finish(AiProvider *provider, GAsyncResult *result, GError **error)
{
	(void)provider;
	return g_task_propagate_pointer(G_TASK(result), error);
}
static const gchar *scripted_name(AiProvider *provider) { (void)provider; return "fixture"; }
static AiProviderType scripted_type(AiProvider *provider) { (void)provider; return AI_PROVIDER_OPENAI; }
static void scripted_iface(AiProviderInterface *iface)
{
	iface->chat_async = scripted_chat;
	iface->chat_finish = scripted_finish;
	iface->get_name = scripted_name;
	iface->get_default_model = scripted_name;
	iface->get_provider_type = scripted_type;
}

/* --- The fixture ----------------------------------------------------------- */

typedef struct {
	gchar *root;
	VentureConfig *config;
	VentureDatabase *db;
	VentureContext *context;
	gint64 org, other;
	VentureEntity *owner, *editor, *stranger;
	ScriptedProvider *provider;
	VentureAiService *ai;
} Fixture;

static void
persist(Fixture *f, gpointer row)
{
	g_autoptr(GError) error = NULL;
	gboolean saved = venture_database_save(f->db, VENTURE_ENTITY(row), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(saved);
}

static gchar *
text_of(gpointer row, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(row, field, &value, NULL);
	return value;
}

static VentureActor
person(const gchar *name)
{
	VentureActor actor;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = name;
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	return actor;
}

/* A user with an organization role in @org (none when @org is 0). */
static VentureEntity *
user_in(Fixture *f, const gchar *username, VentureUserRole role, gint64 org, gint org_role)
{
	VentureEntity *user = g_object_new(VENTURE_TYPE_USER, "username", username, "role", role, "active", TRUE, NULL);
	persist(f, user);
	if (org > 0)
	{
		g_autoptr(VentureEntity) membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", org,
			"user-id", venture_entity_get_id(user), "role", org_role, "active", TRUE, NULL);
		persist(f, membership);
	}
	return user;
}

static VentureAuthPrincipal
principal_of(VentureEntity *user)
{
	VentureAuthPrincipal principal;
	gint role = 0;
	g_object_get(user, "role", &role, NULL);
	principal.user_id = venture_entity_get_id(user);
	principal.token_id = 0;
	principal.role = (VentureUserRole)role;
	principal.name = text_of(user, "username");
	principal.authenticated = TRUE;
	principal.provider_token = FALSE;
	return principal;
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) second = NULL;
	(void)data;
	f->root = g_dir_make_tmp("venture-settings-XXXXXX", &error);
	g_assert_no_error(error);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->root, "server-base-url", "https://venture.example.com", NULL);
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->org = venture_context_get_default_organization_id(f->context);
	second = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Second business", "slug", "settings-second", "active", TRUE, NULL);
	persist(f, second);
	f->other = venture_entity_get_id(second);
	f->owner = user_in(f, "settings-owner", VENTURE_USER_ROLE_EDITOR, f->org, VENTURE_ORGANIZATION_ROLE_OWNER);
	f->editor = user_in(f, "settings-editor", VENTURE_USER_ROLE_EDITOR, f->org, VENTURE_ORGANIZATION_ROLE_EDITOR);
	f->stranger = user_in(f, "settings-stranger", VENTURE_USER_ROLE_EDITOR, f->other, VENTURE_ORGANIZATION_ROLE_OWNER);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	if (f->ai != NULL)
		venture_context_set_ai_service(f->context, NULL);
	g_clear_object(&f->ai);
	g_clear_object(&f->provider);
	g_clear_object(&f->owner);
	g_clear_object(&f->editor);
	g_clear_object(&f->stranger);
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	venture_test_remove_tree(f->root);
	g_clear_pointer(&f->root, g_free);
}

/* --- Audit helpers ------------------------------------------------------------ */

static gint64
audit_count(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_autoptr(GError) error = NULL;
	gint64 n = venture_database_count(f->db, query, &error);
	g_assert_no_error(error);
	return n;
}

/* The audit entries written after @since, oldest first. */
static GPtrArray *
audit_since(Fixture *f, gint64 since)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_autoptr(GError) error = NULL;
	GPtrArray *rows;
	venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_GT, since, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, &error);
	g_assert_no_error(error);
	return rows;
}

static gint64
last_audit_id(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_autoptr(VentureEntity) row = NULL;
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	row = venture_database_find_one(f->db, query, NULL);
	return row != NULL ? venture_entity_get_id(row) : 0;
}

/* What an audit entry says about the change, leaving out who and how:
 * action, target and the diff. Two channels making one change must agree
 * on exactly this. */
static gchar *
audit_change(VentureEntity *entry)
{
	g_autofree gchar *type = text_of(entry, "target-type"), *label = text_of(entry, "target-label");
	g_autofree gchar *diff = NULL;
	gint action = 0;
	gint64 target = 0;
	g_object_get(entry, "action", &action, "target-id", &target, "diff", &diff, NULL);
	if (diff == NULL)
		diff = g_strdup("");
	return g_strdup_printf("%d %s %" G_GINT64_FORMAT " %s %s", action, type, target, label, diff);
}

static VentureOrganization *
org_row(Fixture *f, gint64 id)
{
	VentureEntity *row = venture_database_get(f->db, VENTURE_TYPE_ORGANIZATION, id, NULL);
	g_assert_nonnull(row);
	return VENTURE_ORGANIZATION(row);
}

/* --- The registry ------------------------------------------------------------- */

/* Every workflow setting this epic added is registered by its feature,
 * with a label, help, a place it is stored and a default its own
 * validation accepts. A missing one is a setting nobody can find. */
static void
test_registrations(Fixture *f, gconstpointer data)
{
	static const gchar *const keys[] = {
		"mail.preview-link-check",
		"email-brand.primary-color", "email-brand.accent-color", "email-brand.text-color",
		"email-brand.background-color", "email-brand.font",
		"marketing.warm-up", "marketing.local-hour", "marketing.ab-wait-hours", "marketing.ab-metric",
		"marketing.resend-days",
		"leads.ai-scoring",
		"telephony.quiet-hours", "telephony.recording-links", "telephony.country",
		"telephony.stop-reply", "telephony.start-reply", "telephony.help-reply",
		"inbox.webchat",
		"ai-messages.off", "ai-messages.business-hours", "ai-messages.auto-send",
		NULL
	};
	g_autoptr(GPtrArray) all = venture_settings_registered();
	guint i, j;
	(void)f; (void)data;
	g_assert_nonnull(all);
	for (i = 0; keys[i]; i++)
	{
		const VentureSettingInfo *info = venture_settings_lookup(keys[i], NULL);
		gboolean found = FALSE;
		if (info == NULL)
			g_error("setting %s is not registered", keys[i]);
		g_assert_cmpstr(info->key, ==, keys[i]);
		g_assert_false(venture_string_is_empty(info->workflow));
		g_assert_false(venture_string_is_empty(info->label));
		g_assert_false(venture_string_is_empty(info->help));
		g_assert_false(venture_string_is_empty(info->stored_in));
		g_assert_false(venture_string_is_empty(info->field));
		g_assert_nonnull(info->default_value);
		if (info->type != VENTURE_SETTING_TEXT || info->default_value[0] != '\0')
		{
			g_autoptr(GError) error = NULL;
			g_assert_true(venture_settings_validate(info, info->default_value, NULL, &error));
			g_assert_no_error(error);
		}
		for (j = 0; j < all->len; j++)
			found = found || g_ptr_array_index(all, j) == info;
		g_assert_true(found);
	}
	/* The families are families. */
	g_assert_nonnull(venture_settings_lookup("ai-messages.auto-send", NULL)->members);
	g_assert_nonnull(venture_settings_lookup("inbox.webchat", NULL)->members);
	g_assert_null(venture_settings_lookup("telephony.quiet-hours", NULL)->members);
	g_assert_null(venture_settings_lookup("no.such-setting", NULL));
	/* Registering again adds nothing. */
	venture_settings_register(venture_settings_lookup("telephony.quiet-hours", NULL));
	{
		g_autoptr(GPtrArray) again = venture_settings_registered();
		g_assert_cmpuint(again->len, ==, all->len);
	}
}

/* A fresh organization reads every default, documented in docs/settings.org. */
static void
test_defaults(Fixture *f, gconstpointer data)
{
	static const struct { const gchar *key, *value; } expected[] = {
		{ "mail.preview-link-check", "false" },
		{ "email-brand.primary-color", "#1f5fbf" },
		{ "email-brand.font", "sans" },
		{ "marketing.warm-up", "1:50,3:100,5:250,7:500,10:1000,14:2500,21:5000,30:0" },
		{ "marketing.local-hour", "9" },
		{ "marketing.ab-wait-hours", "4" },
		{ "marketing.ab-metric", "opens" },
		{ "marketing.resend-days", "3" },
		{ "leads.ai-scoring", "false" },
		{ "telephony.quiet-hours", "21:00-08:00" },
		{ "telephony.recording-links", "false" },
		{ "telephony.country", "US" },
		{ "ai-messages.off", "false" },
		{ "ai-messages.business-hours", "" },
	};
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) listed = NULL;
	gsize i;
	(void)data;
	for (i = 0; i < G_N_ELEMENTS(expected); i++)
	{
		g_autoptr(VentureSettingValue) value = venture_settings_get(f->context, f->org, expected[i].key, &error);
		g_assert_no_error(error);
		g_assert_nonnull(value);
		g_assert_cmpstr(value->value, ==, expected[i].value);
		g_assert_true(value->is_default);
	}
	{
		g_autoptr(VentureSettingValue) stop = venture_settings_get(f->context, f->org, "telephony.stop-reply", &error);
		g_autoptr(VentureSettingValue) help = venture_settings_get(f->context, f->org, "telephony.help-reply", &error);
		g_assert_no_error(error);
		g_assert_nonnull(strstr(stop->value, "START"));
		g_assert_nonnull(strstr(help->value, "STOP"));
	}
	listed = venture_settings_list(f->context, f->org, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(listed->len, >=, 20);
	{
		g_autoptr(VentureSettingValue) missing = venture_settings_get(f->context, f->org, "no.such-setting", &error);
		g_assert_null(missing);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	}
}

/* Wrong types, values past the bounds and malformed text are refused
 * in words, and refusing writes nothing. */
static void
test_validation(Fixture *f, gconstpointer data)
{
	static const struct { const gchar *key, *value; } refused[] = {
		{ "telephony.recording-links", "maybe" },
		{ "marketing.ab-wait-hours", "four" },
		{ "marketing.ab-wait-hours", "0" },
		{ "marketing.ab-wait-hours", "169" },
		{ "marketing.local-hour", "24" },
		{ "marketing.local-hour", "-1" },
		{ "marketing.resend-days", "366" },
		{ "marketing.ab-metric", "sometimes" },
		{ "email-brand.font", "comic" },
		{ "email-brand.primary-color", "red" },
		{ "telephony.quiet-hours", "25:00-08:00" },
		{ "telephony.quiet-hours", "nine to five" },
		{ "telephony.country", "XX" },
		{ "telephony.help-reply", "Call us." },
		{ "ai-messages.business-hours", "Someday 09:00-17:00" },
		{ "marketing.warm-up", "3:50" },
	};
	gint64 before = audit_count(f);
	VentureActor owner = person("settings-owner");
	gsize i;
	(void)data;
	for (i = 0; i < G_N_ELEMENTS(refused); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureSettingValue) value = venture_settings_set(f->context, f->org, refused[i].key, refused[i].value, &owner, &error);
		if (value != NULL)
			g_error("%s=%s was accepted", refused[i].key, refused[i].value);
		g_assert_nonnull(error);
		g_assert_false(venture_string_is_empty(error->message));
	}
	g_assert_cmpint(audit_count(f), ==, before);
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureSettingValue) value = venture_settings_set(f->context, f->org, "no.such-setting", "1", &owner, &error);
		g_assert_null(value);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	}
	/* What is accepted is normalised: a switch is true or false. */
	{
		gchar *normal = NULL;
		g_assert_true(venture_settings_validate(venture_settings_lookup("telephony.recording-links", NULL), " On ", &normal, NULL));
		g_assert_cmpstr(normal, ==, "true");
		g_free(normal);
		g_assert_true(venture_settings_validate(venture_settings_lookup("marketing.local-hour", NULL), " 07 ", &normal, NULL));
		g_assert_cmpstr(normal, ==, "7");
		g_free(normal);
		g_assert_true(venture_settings_validate(venture_settings_lookup("telephony.quiet-hours", NULL), "off", &normal, NULL));
		g_free(normal);
	}
}

/* One change is one audit entry, saying who and old to new, on the
 * record that already held the setting; the same value again writes
 * nothing; reset is one more entry and reads the default again. */
static void
test_set_and_reset(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureSettingValue) set = NULL, again = NULL, reset = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(VentureOrganization) org = NULL;
	g_autofree gchar *stored = NULL, *actor = NULL, *diff = NULL, *type = NULL;
	VentureActor owner = person("settings-owner");
	gint64 since = last_audit_id(f);
	(void)data;
	set = venture_settings_set(f->context, f->org, "telephony.quiet-hours", "20:00-07:00", &owner, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(set->value, ==, "20:00-07:00");
	g_assert_false(set->is_default);
	org = org_row(f, f->org);
	stored = text_of(org, "telephony-quiet-hours");
	g_assert_cmpstr(stored, ==, "20:00-07:00");
	entries = audit_since(f, since);
	g_assert_cmpuint(entries->len, ==, 1);
	actor = text_of(g_ptr_array_index(entries, 0), "actor");
	type = text_of(g_ptr_array_index(entries, 0), "target-type");
	g_assert_cmpstr(actor, ==, "settings-owner");
	g_assert_cmpstr(type, ==, "organization");
	g_object_get(g_ptr_array_index(entries, 0), "diff", &diff, NULL);
	g_assert_nonnull(strstr(diff, "telephony_quiet_hours"));
	g_assert_nonnull(strstr(diff, "20:00-07:00"));

	/* The one send path reads it: quiet now means 20:00 to 07:00. */
	{
		g_autoptr(VentureEntity) contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", f->org, "name", "Ada Example",
			"timezone", "UTC", NULL);
		g_autoptr(GDateTime) evening = g_date_time_new_from_iso8601("2026-09-15T20:30:00Z", NULL);
		g_autoptr(GDateTime) until = NULL;
		persist(f, contact);
		until = venture_telephony_service_quiet_until(venture_telephony_service_get(f->db), contact, evening, NULL);
		g_assert_nonnull(until);
		g_assert_cmpint(g_date_time_get_hour(until), ==, 7);
	}

	since = last_audit_id(f);
	again = venture_settings_set(f->context, f->org, "telephony.quiet-hours", "20:00-07:00", &owner, &error);
	g_assert_no_error(error);
	g_assert_nonnull(again);
	g_assert_cmpint(last_audit_id(f), ==, since);

	reset = venture_settings_reset(f->context, f->org, "telephony.quiet-hours", &owner, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(reset->value, ==, "21:00-08:00");
	g_assert_true(reset->is_default);
	g_clear_pointer(&entries, g_ptr_array_unref);
	entries = audit_since(f, since);
	g_assert_cmpuint(entries->len, ==, 1);
	g_clear_object(&org);
	g_clear_pointer(&stored, g_free);
	org = org_row(f, f->org);
	stored = text_of(org, "telephony-quiet-hours");
	g_assert_true(venture_string_is_empty(stored));
}

/* Only an owner or administrator of that organization changes it: an
 * editor of it, or an owner of another, is refused and nothing is written. */
static void
test_permission(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	VentureAccessPolicy *policy = venture_database_get_access_policy(f->db);
	VentureActor editor = person("settings-editor"), stranger = person("settings-stranger"), owner = person("settings-owner");
	VentureAuthPrincipal as_editor = principal_of(f->editor), as_stranger = principal_of(f->stranger), as_owner = principal_of(f->owner);
	gint64 before = audit_count(f);
	(void)data;
	{
		g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(policy, &as_editor);
		g_autoptr(VentureSettingValue) value = venture_settings_set(f->context, f->org, "ai-messages.off", "true", &editor, &error);
		g_assert_null(value);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
		g_clear_error(&error);
	}
	{
		g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(policy, &as_stranger);
		g_autoptr(VentureSettingValue) value = venture_settings_set(f->context, f->org, "ai-messages.off", "true", &stranger, &error);
		g_assert_null(value);
		g_assert_nonnull(error);
		g_clear_error(&error);
	}
	g_assert_cmpint(audit_count(f), ==, before);
	{
		g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(policy, &as_owner);
		g_autoptr(VentureSettingValue) value = venture_settings_set(f->context, f->org, "ai-messages.off", "true", &owner, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(value->value, ==, "true");
	}
	g_free(as_editor.name);
	g_free(as_stranger.name);
	g_free(as_owner.name);
}

/* A setting in one organization is that organization's alone. */
static void
test_isolation(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureSettingValue) set = NULL, there = NULL;
	VentureActor owner = person("settings-owner");
	(void)data;
	set = venture_settings_set(f->context, f->org, "marketing.ab-wait-hours", "12", &owner, &error);
	g_assert_no_error(error);
	there = venture_settings_get(f->context, f->other, "marketing.ab-wait-hours", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(there->value, ==, "4");
	g_assert_true(there->is_default);
	/* A rule in one organization is not a member of the other's family. */
	{
		g_autoptr(VentureEntity) rule = g_object_new(VENTURE_TYPE_AI_MESSAGE_RULE, "organization-id", f->org,
			"name", "After hours", "template", "We are closed.", "active", TRUE, NULL);
		g_autoptr(GPtrArray) theirs = NULL;
		g_autofree gchar *key = NULL;
		guint i;
		g_assert_true(venture_ai_messages_save_rule(venture_ai_messages_get(f->db), rule, NULL, NULL));
		key = g_strdup_printf("ai-messages.auto-send.%" G_GINT64_FORMAT, venture_entity_get_id(rule));
		theirs = venture_settings_list(f->context, f->other, &error);
		g_assert_no_error(error);
		for (i = 0; i < theirs->len; i++)
			g_assert_cmpstr(((VentureSettingValue *)g_ptr_array_index(theirs, i))->key, !=, key);
		{
			g_autoptr(VentureSettingValue) foreign = venture_settings_set(f->context, f->other, key, "true", &owner, &error);
			g_assert_null(foreign);
			g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
		}
	}
}

/* Families: each auto-reply rule and each web chat is a member, and a
 * member's setting writes that record. */
static void
test_families(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) rule = NULL, widget = NULL, fresh = NULL;
	g_autoptr(GPtrArray) listed = NULL;
	g_autofree gchar *rule_key = NULL, *chat_key = NULL;
	VentureActor owner = person("settings-owner");
	gboolean seen_rule = FALSE, seen_chat = FALSE, auto_send = FALSE, active = TRUE;
	guint i;
	(void)data;
	rule = g_object_new(VENTURE_TYPE_AI_MESSAGE_RULE, "organization-id", f->org, "name", "Missed call",
		"template", "Sorry we missed you.", "active", TRUE, NULL);
	g_assert_true(venture_ai_messages_save_rule(venture_ai_messages_get(f->db), rule, NULL, NULL));
	widget = venture_inbox_chat_widget_create(venture_inbox_service_get(f->db), f->org, "Website chat", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(widget);
	rule_key = g_strdup_printf("ai-messages.auto-send.%" G_GINT64_FORMAT, venture_entity_get_id(rule));
	chat_key = g_strdup_printf("inbox.webchat.%" G_GINT64_FORMAT, venture_entity_get_id(widget));
	listed = venture_settings_list(f->context, f->org, &error);
	g_assert_no_error(error);
	for (i = 0; i < listed->len; i++)
	{
		VentureSettingValue *value = g_ptr_array_index(listed, i);
		if (g_strcmp0(value->key, rule_key) == 0)
		{
			seen_rule = TRUE;
			g_assert_cmpstr(value->value, ==, "false");
			g_assert_cmpstr(value->member_label, ==, "Missed call");
		}
		if (g_strcmp0(value->key, chat_key) == 0)
		{
			seen_chat = TRUE;
			g_assert_cmpstr(value->value, ==, "true");
		}
	}
	g_assert_true(seen_rule);
	g_assert_true(seen_chat);
	{
		g_autoptr(VentureSettingValue) a = venture_settings_set(f->context, f->org, rule_key, "true", &owner, &error);
		g_autoptr(VentureSettingValue) b = NULL;
		g_assert_no_error(error);
		b = venture_settings_set(f->context, f->org, chat_key, "false", &owner, &error);
		g_assert_no_error(error);
	}
	fresh = venture_database_get(f->db, VENTURE_TYPE_AI_MESSAGE_RULE, venture_entity_get_id(rule), NULL);
	g_object_get(fresh, "auto-send", &auto_send, NULL);
	g_assert_true(auto_send);
	g_clear_object(&fresh);
	fresh = venture_database_get(f->db, VENTURE_TYPE_INBOX_CHAT_WIDGET, venture_entity_get_id(widget), NULL);
	g_object_get(fresh, "active", &active, NULL);
	g_assert_false(active);
	{
		g_autoptr(VentureSettingValue) none = venture_settings_get(f->context, f->org, "ai-messages.auto-send.999999", &error);
		g_assert_null(none);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	}
}

/* Settings stored on records other than the organization: the warm-up on
 * the first sending domain (none yet: refused with the way forward), the
 * brand made on first use (one entry). */
static void
test_storage(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) domain = NULL, fresh = NULL;
	g_autofree gchar *schedule = NULL;
	VentureActor owner = person("settings-owner");
	gint64 since;
	(void)data;
	{
		g_autoptr(VentureSettingValue) none = venture_settings_set(f->context, f->org, "marketing.warm-up", "off", &owner, &error);
		g_assert_null(none);
		g_assert_nonnull(error);
		g_assert_nonnull(strstr(error->message, "sending domain"));
		g_clear_error(&error);
	}
	domain = g_object_new(VENTURE_TYPE_MARKETING_SENDING_DOMAIN, "organization-id", f->org, "domain", "example.com", NULL);
	persist(f, domain);
	{
		g_autoptr(VentureSettingValue) set = venture_settings_set(f->context, f->org, "marketing.warm-up", "1:20,4:100,15:0", &owner, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(set->value, ==, "1:20,4:100,15:0");
	}
	fresh = venture_database_get(f->db, VENTURE_TYPE_MARKETING_SENDING_DOMAIN, venture_entity_get_id(domain), NULL);
	schedule = text_of(fresh, "ramp-schedule");
	g_assert_cmpstr(schedule, ==, "1:20,4:100,15:0");

	since = last_audit_id(f);
	{
		g_autoptr(VentureSettingValue) set = venture_settings_set(f->context, f->org, "email-brand.primary-color", "#c2410c", &owner, &error);
		g_autoptr(GPtrArray) entries = NULL;
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_EMAIL_BRAND);
		g_autoptr(VentureEntity) brand = NULL;
		g_autofree gchar *colour = NULL;
		g_assert_no_error(error);
		g_assert_cmpstr(set->value, ==, "#c2410c");
		entries = audit_since(f, since);
		g_assert_cmpuint(entries->len, ==, 1);
		venture_query_set_organization(query, f->org);
		brand = venture_database_find_one(f->db, query, NULL);
		g_assert_nonnull(brand);
		colour = text_of(brand, "primary-color");
		g_assert_cmpstr(colour, ==, "#c2410c");
	}
}

/* The defaults reach the features: a new local-time send takes the
 * organization's hour, an A/B test its wait and metric, a resend its
 * days; the STOP answer is the organization's words. */
static void
test_feature_effects(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) list = NULL, send = NULL;
	VentureActor owner = person("settings-owner");
	gint64 hour = 0;
	gint metric = 0;
	(void)data;
	g_assert_nonnull(venture_settings_set(f->context, f->org, "marketing.local-hour", "10", &owner, &error));
	g_assert_no_error(error);
	g_assert_nonnull(venture_settings_set(f->context, f->org, "marketing.ab-metric", "clicks", &owner, &error));
	g_assert_no_error(error);
	list = g_object_new(VENTURE_TYPE_MARKETING_LIST, "organization-id", f->org, "name", "Customers", NULL);
	persist(f, list);
	send = g_object_new(VENTURE_TYPE_MARKETING_SEND, "organization-id", f->org, "list-id", venture_entity_get_id(list),
		"name", "Hello", "subject", "Hello", "text-body", "Body", "local-time", TRUE, NULL);
	persist(f, send);
	g_object_get(send, "local-hour", &hour, "winner-metric", &metric, NULL);
	g_assert_cmpint(hour, ==, 10);
	g_assert_cmpint(metric, ==, 1);
	g_assert_cmpint(venture_marketing_service_default_resend_days(venture_marketing_service_get(f->db), f->org), ==, 3);
	g_assert_nonnull(venture_settings_set(f->context, f->org, "marketing.resend-days", "5", &owner, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_marketing_service_default_resend_days(venture_marketing_service_get(f->db), f->org), ==, 5);
	g_assert_nonnull(venture_settings_set(f->context, f->org, "telephony.stop-reply",
		"{business}: you will hear no more from us. Reply START to come back.", &owner, &error));
	g_assert_no_error(error);
	{
		g_autofree gchar *reply = venture_telephony_service_keyword_reply(venture_telephony_service_get(f->db), f->org, "stop");
		g_assert_nonnull(reply);
		g_assert_nonnull(strstr(reply, "you will hear no more from us"));
		g_assert_null(strstr(reply, "{business}"));
	}
}

/* A module switched off takes its settings with it. */
static void
test_module_off(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) listed = NULL;
	guint i;
	(void)data;
	venture_config_set_module_enabled(f->config, "ai_messages", FALSE);
	venture_config_set_module_enabled(f->config, "inbox", FALSE);
	venture_config_set_module_enabled(f->config, "telephony", FALSE);
	listed = venture_settings_list(f->context, f->org, &error);
	g_assert_no_error(error);
	for (i = 0; i < listed->len; i++)
		g_assert_false(g_str_has_prefix(((VentureSettingValue *)g_ptr_array_index(listed, i))->key, "telephony."));
	{
		g_autoptr(VentureSettingValue) value = venture_settings_get(f->context, f->org, "telephony.quiet-hours", &error);
		g_assert_null(value);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	}
}

/* --- The four ways in ------------------------------------------------------------- */

typedef struct { gboolean done; GBytes *body; GError *error; } WebCall;

static void
web_done(GObject *source, GAsyncResult *result, gpointer data)
{
	WebCall *call = data;
	call->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &call->error);
	call->done = TRUE;
}

static guint
web_request(VentureWebServer *server, const gchar *method, const gchar *path, const gchar *token, const gchar *type,
	const gchar *body, gchar **out)
{
	g_autoptr(SoupSession) session = soup_session_new();
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", venture_web_server_get_port(server), path);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	WebCall call = { FALSE, NULL, NULL };
	guint status;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (token != NULL)
	{
		g_autofree gchar *bearer = g_strconcat("Bearer ", token, NULL);
		soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", bearer);
	}
	if (body != NULL)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, type != NULL ? type : "application/x-www-form-urlencoded", bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, web_done, &call);
	while (!call.done)
		g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(call.error);
	if (out != NULL)
		*out = g_strndup(g_bytes_get_data(call.body, NULL), g_bytes_get_size(call.body));
	status = soup_message_get_status(message);
	g_bytes_unref(call.body);
	return status;
}

static VentureWebServer *
web_start(Fixture *f, gboolean auth)
{
	g_autoptr(GError) error = NULL;
	VentureWebServer *server;
	gboolean held = g_getenv("VENTURE_TEST_SETTINGS_HOLD") != NULL;
	/* Held open for screenshots, it listens where a local instance would
	 * and draws the theme asked for. */
	g_object_set(f->config, "server-bind-address", "127.0.0.1", "server-port", (gint64)(held ? 8748 : 0),
		"security-require-auth", auth, NULL);
	if (g_getenv("VENTURE_TEST_SETTINGS_THEME") != NULL)
		g_object_set(f->config, "ui-theme", g_getenv("VENTURE_TEST_SETTINGS_THEME"), NULL);
	server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(server, &error));
	g_assert_no_error(error);
	return server;
}

static void
hold(VentureWebServer *server, const gchar *what)
{
	/* VENTURE_TEST_SETTINGS_HOLD=SECONDS keeps the server up that long. */
	if (g_getenv("VENTURE_TEST_SETTINGS_HOLD") != NULL)
	{
		gint64 until = g_get_monotonic_time() + g_ascii_strtoll(g_getenv("VENTURE_TEST_SETTINGS_HOLD"), NULL, 10) * G_USEC_PER_SEC;
		g_printerr("settings pages on http://127.0.0.1:%u %s\n", venture_web_server_get_port(server), what);
		while (g_get_monotonic_time() < until)
			g_main_context_iteration(NULL, FALSE), g_usleep(2000);
	}
}

/* An API token for @user, returned as the secret. */
static gchar *
token_for(Fixture *f, VentureEntity *user, const gchar *name)
{
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	gchar *secret;
	g_object_set(token, "name", name, "user-id", venture_entity_get_id(user), "role", VENTURE_USER_ROLE_EDITOR, NULL);
	secret = venture_api_token_generate(token);
	persist(f, token);
	return secret;
}

typedef struct { gboolean done; gchar *out, *err; GError *error; } CliCall;

static void
cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	CliCall *call = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &call->out, &call->err, &call->error);
	call->done = TRUE;
}

/* venturectl against the test's own server, the main loop turning so the
 * server can answer. */
static gint
run_cli(VentureWebServer *server, const gchar *token, const gchar *const *args, gchar **out)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	CliCall call = { FALSE, NULL, NULL, NULL };
	gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
	guint i;
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", token, TRUE);
	g_ptr_array_add(argv, g_strdup(client_path));
	g_ptr_array_add(argv, g_strdup("--server"));
	g_ptr_array_add(argv, g_strdup_printf("http://127.0.0.1:%u", venture_web_server_get_port(server)));
	g_ptr_array_add(argv, g_strdup("--format"));
	g_ptr_array_add(argv, g_strdup("json"));
	for (i = 0; args[i]; i++)
		g_ptr_array_add(argv, g_strdup(args[i]));
	g_ptr_array_add(argv, NULL);
	child = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, cli_done, &call);
	while (!call.done && g_get_monotonic_time() < deadline)
		g_main_context_iteration(NULL, FALSE), g_usleep(1000);
	g_assert_true(call.done);
	g_assert_no_error(call.error);
	if (out != NULL)
		*out = g_steal_pointer(&call.out);
	g_free(call.out);
	if (!g_subprocess_get_successful(child))
		g_printerr("venturectl: %s\n", call.err != NULL ? call.err : "");
	g_free(call.err);
	return g_subprocess_get_exit_status(child);
}

/* The one change the channels make, and what each one's audit entry says
 * about it once its own actor is left out. */
static gchar *
channel_change(Fixture *f, gint64 since)
{
	g_autoptr(GPtrArray) all = audit_since(f, since);
	g_autoptr(GPtrArray) entries = g_ptr_array_new();
	guint i;
	/* A token's first use stamps it (its own entry); that is the
	 * credential's bookkeeping, not the change. */
	for (i = 0; i < all->len; i++) {
		g_autofree gchar *type = text_of(g_ptr_array_index(all, i), "target-type");
		if (g_strcmp0(type, "api_token") != 0)
			g_ptr_array_add(entries, g_ptr_array_index(all, i));
	}
	for (i = 0; entries->len != 1 && i < entries->len; i++) {
		g_autofree gchar *what = audit_change(g_ptr_array_index(entries, i));
		g_printerr("unexpected audit entry: %s\n", what);
	}
	g_assert_cmpuint(entries->len, ==, 1);
	return audit_change(g_ptr_array_index(entries, 0));
}

static void
back_to_default(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureSettingValue) value = venture_settings_reset(f->context, f->org, "telephony.quiet-hours", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(value->is_default);
}

/* The screen, REST, venturectl and the assistant (staged, then approved)
 * each change the quiet hours from the default to 20:00-07:00: four audit
 * entries identical in what changed, differing only in who and how. */
static void
test_channels(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *screen_token = NULL, *rest_token = NULL, *cli_token = NULL;
	g_autofree gchar *screen = NULL, *rest = NULL, *cli = NULL, *chat = NULL, *page = NULL, *api = NULL, *path = NULL, *out = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	gint64 since;
	(void)data;
	screen_token = token_for(f, f->owner, "screen");
	rest_token = token_for(f, f->owner, "rest");
	cli_token = token_for(f, f->owner, "cli");
	server = web_start(f, TRUE);

	/* The screen draws every registered setting with its default. */
	path = g_strdup_printf("/organizations/%" G_GINT64_FORMAT "/settings/workflows", f->org);
	g_assert_cmpuint(web_request(server, "GET", path, screen_token, NULL, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Marketing, sales, phone and AI"));
	g_assert_nonnull(strstr(page, "Text quiet hours"));
	g_assert_nonnull(strstr(page, "21:00-08:00"));
	g_assert_nonnull(strstr(page, "Phone and text"));
	g_assert_nonnull(strstr(page, "Reset to default"));

	/* Screen. */
	since = last_audit_id(f);
	g_assert_cmpuint(web_request(server, "POST", path, screen_token, NULL,
		"action=set&key=telephony.quiet-hours&value=20%3A00-07%3A00", NULL), ==, 303);
	screen = channel_change(f, since);
	back_to_default(f);

	/* REST. */
	{
		g_autofree gchar *one = g_strdup_printf("/api/v1/workflow-settings/telephony.quiet-hours?organization_id=%" G_GINT64_FORMAT, f->org);
		g_autofree gchar *all = g_strdup_printf("/api/v1/workflow-settings?organization_id=%" G_GINT64_FORMAT, f->org);
		g_autofree gchar *listed = NULL, *got = NULL;
		g_assert_cmpuint(web_request(server, "GET", all, rest_token, NULL, NULL, &listed), ==, 200);
		g_assert_nonnull(strstr(listed, "\"telephony.quiet-hours\""));
		g_assert_cmpuint(web_request(server, "GET", one, rest_token, NULL, NULL, &got), ==, 200);
		{
			g_autoptr(JsonNode) parsed = venture_json_parse(got, NULL);
			g_assert_nonnull(parsed);
			g_assert_true(json_object_get_boolean_member(json_node_get_object(parsed), "is_default"));
			g_assert_cmpstr(json_object_get_string_member(json_node_get_object(parsed), "value"), ==, "21:00-08:00");
		}
		since = last_audit_id(f);
		g_assert_cmpuint(web_request(server, "PUT", one, rest_token, "application/json", "{\"value\":\"20:00-07:00\"}", &api), ==, 200);
		g_assert_nonnull(strstr(api, "20:00-07:00"));
		rest = channel_change(f, since);
		back_to_default(f);
		/* Refusals over REST are words with the right status. */
		g_clear_pointer(&api, g_free);
		g_assert_cmpuint(web_request(server, "PUT", one, rest_token, "application/json", "{\"value\":\"25:00-07:00\"}", &api), ==, 422);
		g_clear_pointer(&api, g_free);
		g_assert_cmpuint(web_request(server, "GET", "/api/v1/workflow-settings/no.such", rest_token, NULL, NULL, &api), ==, 404);
	}

	/* venturectl. */
	{
		g_autofree gchar *org = g_strdup_printf("organization_id=%" G_GINT64_FORMAT, f->org);
		const gchar *const set[] = { "settings", "set", "telephony.quiet-hours", "20:00-07:00", org, NULL };
		const gchar *const get[] = { "settings", "get", "telephony.quiet-hours", org, NULL };
		const gchar *const list[] = { "settings", "list", org, NULL };
		const gchar *const reset[] = { "settings", "reset", "telephony.quiet-hours", org, NULL };
		since = last_audit_id(f);
		g_assert_cmpint(run_cli(server, cli_token, set, NULL), ==, 0);
		cli = channel_change(f, since);
		g_assert_cmpint(run_cli(server, cli_token, get, &out), ==, 0);
		g_assert_nonnull(strstr(out, "20:00-07:00"));
		g_clear_pointer(&out, g_free);
		g_assert_cmpint(run_cli(server, cli_token, list, &out), ==, 0);
		g_assert_nonnull(strstr(out, "marketing.ab-wait-hours"));
		g_clear_pointer(&out, g_free);
		g_assert_cmpint(run_cli(server, cli_token, reset, NULL), ==, 0);
		{
			g_autoptr(VentureSettingValue) value = venture_settings_get(f->context, f->org, "telephony.quiet-hours", &error);
			g_assert_no_error(error);
			g_assert_true(value->is_default);
		}
	}

	/* The assistant: the model calls settings_set, which stages; nothing
	 * changes until a person approves; approving is the one entry. */
	{
		g_autoptr(GPtrArray) pending = NULL;
		g_autofree gchar *reply = NULL, *confirmation = NULL, *stored = NULL, *via = NULL;
		g_autoptr(VentureOrganization) org = NULL;
		VentureAuthPrincipal asking = principal_of(f->owner);
		f->provider = g_object_new(TYPE_SCRIPTED_PROVIDER, NULL);
		f->ai = venture_ai_service_new_with_provider(f->context, AI_PROVIDER(f->provider), &error);
		g_assert_no_error(error);
		venture_context_set_ai_service(f->context, f->ai);
		g_queue_push_tail(f->provider->answers, g_strdup("TOOL settings_get {\"key\":\"telephony.quiet-hours\"}"));
		g_queue_push_tail(f->provider->answers, g_strdup_printf("TOOL settings_set {\"key\":\"telephony.quiet-hours\","
			"\"value\":\"20:00-07:00\",\"organization_id\":%" G_GINT64_FORMAT "}", f->org));
		g_queue_push_tail(f->provider->answers, g_strdup("I have staged quiet hours of 20:00 to 07:00; approve it to apply."));
		since = last_audit_id(f);
		reply = venture_ai_service_answer(f->ai, "Make the text quiet hours 8pm to 7am", &asking, &error);
		g_assert_no_error(error);
		g_assert_nonnull(reply);
		/* Staged, not written. */
		org = org_row(f, f->org);
		stored = text_of(org, "telephony-quiet-hours");
		g_assert_true(venture_string_is_empty(stored));
		g_assert_cmpint(last_audit_id(f), ==, since);
		pending = venture_confirmation_store_list_pending(venture_context_get_confirmations(f->context));
		g_assert_cmpuint(pending->len, ==, 1);
		confirmation = g_strdup(venture_confirmation_get_id(g_ptr_array_index(pending, 0)));
		g_assert_true(venture_confirmation_get_entity_type(g_ptr_array_index(pending, 0)) == VENTURE_TYPE_ORGANIZATION);
		/* An editor cannot approve a setting change. */
		{
			VentureAuthPrincipal as_editor = principal_of(f->editor);
			g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &as_editor);
			g_assert_false(venture_confirmation_store_approve(venture_context_get_confirmations(f->context), confirmation,
				"settings-editor", &error));
			g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
			g_clear_error(&error);
			g_free(as_editor.name);
		}
		g_assert_cmpint(last_audit_id(f), ==, since);
		/* The owner approves over the API. */
		{
			g_autofree gchar *approve = g_strdup_printf("/api/v1/confirmations/%s/approve", confirmation);
			g_autofree gchar *body = NULL;
			g_assert_cmpuint(web_request(server, "POST", approve, rest_token, "application/json", "{}", &body), ==, 200);
		}
		chat = channel_change(f, since);
		entries = audit_since(f, since);
		{
			VentureEntity *entry = g_ptr_array_index(entries, 0);
			g_autofree gchar *approved = text_of(entry, "approved-by"), *request = text_of(entry, "request-id");
			gint kind = 0;
			g_object_get(entry, "actor-kind", &kind, NULL);
			g_assert_cmpint(kind, ==, VENTURE_ACTOR_KIND_AI);
			g_assert_false(venture_string_is_empty(approved));
			g_assert_cmpstr(request, ==, confirmation);
		}
		g_free(asking.name);
	}

	g_assert_cmpstr(screen, ==, rest);
	g_assert_cmpstr(rest, ==, cli);
	g_assert_cmpstr(cli, ==, chat);
	hold(server, "(settings and the chat exchange)");
}

/* Held open with no sign-in, for the screenshots in docs/settings.org:
 * a few settings changed so default and current differ. */
static void
test_screen(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureWebServer) server = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL, *page = NULL, *problem = NULL;
	VentureActor owner = person("settings-owner");
	(void)data;
	g_assert_nonnull(venture_settings_set(f->context, f->org, "telephony.quiet-hours", "20:00-07:00", &owner, &error));
	g_assert_nonnull(venture_settings_set(f->context, f->org, "marketing.ab-wait-hours", "6", &owner, &error));
	g_assert_no_error(error);
	server = web_start(f, FALSE);
	path = g_strdup_printf("/organizations/%" G_GINT64_FORMAT "/settings/workflows", f->org);
	g_assert_cmpuint(web_request(server, "GET", path, NULL, NULL, NULL, &page), ==, 200);
	/* Grouped by workflow, the changed ones say so beside their default. */
	g_assert_nonnull(strstr(page, "Campaigns"));
	g_assert_nonnull(strstr(page, "AI on messages"));
	g_assert_nonnull(strstr(page, "Default: <code>4</code>"));
	g_assert_nonnull(strstr(page, "name=\"key\" value=\"marketing.ab-wait-hours\""));
	/* A refusal comes back to the page in words. */
	g_assert_cmpuint(web_request(server, "POST", path, NULL, NULL,
		"action=set&key=marketing.ab-wait-hours&value=999", NULL), ==, 303);
	g_assert_cmpstr(venture_settings_get(f->context, f->org, "marketing.ab-wait-hours", NULL)->value, ==, "6");
	/* Reset from the screen. */
	g_assert_cmpuint(web_request(server, "POST", path, NULL, NULL, "action=reset&key=marketing.ab-wait-hours", NULL), ==, 303);
	{
		g_autoptr(VentureSettingValue) value = venture_settings_get(f->context, f->org, "marketing.ab-wait-hours", &error);
		g_assert_no_error(error);
		g_assert_true(value->is_default);
	}
	g_assert_nonnull(venture_settings_set(f->context, f->org, "marketing.ab-wait-hours", "6", &owner, &error));
	/* VENTURE_TEST_SETTINGS_CHAT_HTML=FILE also saves the settings screen
	 * with the assistant's panel open on a chat that changes the quiet
	 * hours, its staged change waiting: the screenshot in docs/settings.org. */
	if (g_getenv("VENTURE_TEST_SETTINGS_CHAT_HTML") != NULL)
	{
		g_autofree gchar *exchange = NULL, *screen = NULL, *saved = NULL;
		g_autoptr(GRegex) scripts = g_regex_new("<script\\b.*?</script>", G_REGEX_DOTALL, 0, NULL);
		const gchar *log;
		g_assert_true(venture_settings_reset(f->context, f->org, "telephony.quiet-hours", &owner, &error) != NULL);
		f->provider = g_object_new(TYPE_SCRIPTED_PROVIDER, NULL);
		f->ai = venture_ai_service_new_with_provider(f->context, AI_PROVIDER(f->provider), &error);
		g_assert_no_error(error);
		venture_context_set_ai_service(f->context, f->ai);
		g_queue_push_tail(f->provider->answers, g_strdup("TOOL settings_get {\"key\":\"telephony.quiet-hours\"}"));
		g_queue_push_tail(f->provider->answers, g_strdup("TOOL settings_set {\"key\":\"telephony.quiet-hours\",\"value\":\"20:00-07:00\"}"));
		g_queue_push_tail(f->provider->answers, g_strdup("Texts are quiet from 21:00 to 08:00 now, in each person's own time. "
			"I have staged 20:00 to 07:00; nothing changes until you press Apply on the card below."));
		{
			g_autofree gchar *form = g_strdup_printf("organization_id=%" G_GINT64_FORMAT "&message=Make+the+text+quiet+hours+8pm+to+7am", f->org);
			if (web_request(server, "POST", "/ui/chat", NULL, NULL, form, &exchange) != 200)
				g_error("the chat refused: %s", exchange);
		}
		g_object_set(f->config, "ui-chat-dock-expanded", TRUE, NULL);
		g_assert_cmpuint(web_request(server, "GET", path, NULL, NULL, NULL, &screen), ==, 200);
		log = strstr(screen, "id=\"chat-log\">");
		g_assert_nonnull(log);
		log += strlen("id=\"chat-log\">");
		saved = g_strdup_printf("%.*s%s%s", (gint)(log - screen), screen, exchange, log);
		{
			g_autofree gchar *still = g_regex_replace(scripts, saved, -1, 0, "", 0, NULL);
			g_assert_true(g_file_set_contents(g_getenv("VENTURE_TEST_SETTINGS_CHAT_HTML"), still, -1, NULL));
		}
	}
	hold(server, path);
}

int
main(int argc, char **argv)
{
	g_autofree gchar *program = g_canonicalize_filename(argv[0], NULL);
	g_autofree gchar *directory = g_path_get_dirname(program);
	g_autofree gchar *parent = g_path_get_dirname(directory);
	gint result;
	g_test_init(&argc, &argv, NULL);
	client_path = g_build_filename(parent, "venturectl", NULL);
	g_test_add("/settings/registrations", Fixture, NULL, setup, test_registrations, teardown);
	g_test_add("/settings/defaults", Fixture, NULL, setup, test_defaults, teardown);
	g_test_add("/settings/validation", Fixture, NULL, setup, test_validation, teardown);
	g_test_add("/settings/set-and-reset", Fixture, NULL, setup, test_set_and_reset, teardown);
	g_test_add("/settings/permission", Fixture, NULL, setup, test_permission, teardown);
	g_test_add("/settings/isolation", Fixture, NULL, setup, test_isolation, teardown);
	g_test_add("/settings/families", Fixture, NULL, setup, test_families, teardown);
	g_test_add("/settings/storage", Fixture, NULL, setup, test_storage, teardown);
	g_test_add("/settings/feature-effects", Fixture, NULL, setup, test_feature_effects, teardown);
	g_test_add("/settings/module-off", Fixture, NULL, setup, test_module_off, teardown);
	g_test_add("/settings/channels", Fixture, NULL, setup, test_channels, teardown);
	g_test_add("/settings/screen", Fixture, NULL, setup, test_screen, teardown);
	result = g_test_run();
	g_free(client_path);
	return result;
}
