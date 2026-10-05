/*
 * venture-context.c - The wiring every subsystem is handed
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"
#include <string.h>
#include <errno.h>

struct _VentureContext
{
	GObject parent_instance;

	VentureConfig		*config;
	VentureDatabase		*database;
	VentureEntityRegistry	*entities;
	VentureReportRegistry	*reports;
	VentureVentureTypeRegistry *venture_types;
	VentureConfirmationStore *confirmations;
	VentureAiService	*ai;
	VentureAiHarness	*harness;
	VentureAutomation	*automation;
	VenturePluginManager	*plugins;
	VenturePluginProvidesRegistry	*plugin_provides;
	VentureAutomationHandlerRegistry	*automation_handlers;
	GPtrArray		*web_extensions;	/* VentureWebExtension */
	VentureWorkService	*work;
	VentureKbService	*kb;
	VentureStripeService *stripe;
	VentureBankFeedService *bankfeed;
	VentureCommerceService *commerce;
	VentureModuleRegistry	*modules;
	VentureMailerRegistry *mailers;
	VentureMailOutbox *mail_outbox;

	GTimeZone		*timezone;
	gint64			 default_organization_id;
	VentureReconciliationRegistry *reconciliation_registry;
	VentureReconciliationService *reconciliation_service;
};

G_DEFINE_FINAL_TYPE(VentureContext, venture_context, G_TYPE_OBJECT)

static void
venture_context_on_modules_changed(VentureContext *self)
{
	venture_context_apply_modules(self);
}

static void
venture_context_finalize(GObject *object)
{
	VentureContext *self;

	self = VENTURE_CONTEXT(object);

	g_clear_object(&self->mail_outbox);
	g_clear_object(&self->mailers);
	g_clear_object(&self->config);
	g_clear_object(&self->database);
	g_clear_object(&self->reports);
	g_clear_object(&self->venture_types);
	g_clear_object(&self->confirmations);
	g_clear_object(&self->ai);
	g_clear_object(&self->harness);
	g_clear_object(&self->automation);
	g_clear_object(&self->plugins);
	g_clear_object(&self->plugin_provides);
	g_clear_object(&self->automation_handlers);
	g_clear_pointer(&self->web_extensions, g_ptr_array_unref);
	g_clear_object(&self->work);
	g_clear_object(&self->kb);
	g_clear_object(&self->stripe);
	g_clear_object(&self->bankfeed);
	g_clear_object(&self->commerce);
	g_clear_object(&self->modules);
	g_clear_pointer(&self->timezone, g_time_zone_unref);
	g_clear_object(&self->reconciliation_registry);
	g_clear_object(&self->reconciliation_service);

	/* The entity registry is the process-wide default and is not owned. */

	G_OBJECT_CLASS(venture_context_parent_class)->finalize(object);
}

/*
 * The feeds worker is joined here, while everything it was handed is
 * still alive, rather than in finalize when the database may already be
 * gone: a thread must never outlive the context that started it.
 */
static void
venture_context_dispose(GObject *object)
{
#ifdef VENTURE_HAVE_SQLITE
	venture_feeds_shutdown(VENTURE_CONTEXT(object));
#endif

	G_OBJECT_CLASS(venture_context_parent_class)->dispose(object);
}

static void
venture_context_class_init(VentureContextClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = venture_context_dispose;
	G_OBJECT_CLASS(klass)->finalize = venture_context_finalize;
}

static void
venture_context_init(VentureContext *self)
{
}

VentureContext *
venture_context_new(
	VentureConfig	*config,
	VentureDatabase	*database
){
	VentureContext *self;

	g_return_val_if_fail(VENTURE_IS_CONFIG(config), NULL);
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	self = g_object_new(VENTURE_TYPE_CONTEXT, NULL);
	self->config = g_object_ref(config);
	self->database = g_object_ref(database);
	self->entities = venture_entity_registry_get_default();
	self->reports = venture_report_registry_new();
	self->venture_types = venture_venture_type_registry_new();
	self->timezone = venture_config_get_timezone(config);
	self->mailers = venture_mailer_registry_new();
	self->mail_outbox = venture_mail_outbox_new(database, NULL);
	{
		g_autofree gchar *state = NULL, *root = NULL;
		g_object_get(config, "state-dir", &state, NULL);
		if (!state || !*state) { g_free(state); state = g_build_filename(g_get_user_data_dir(), "venture", NULL); }
		root = g_build_filename(state, "attachments", NULL);
		g_object_set(self->mail_outbox, "attachment-root", root, NULL);
	}

	/* A payment answered with a receipt, once its transaction commits. */
	venture_financial_documents_install_receipts(self);

	/* The cross-row checks a polymorphic link needs, on every writer. */
	venture_record_link_install_validator(database);
	venture_federation_install_validators(database);
	/* Scheduled packs must validate through generic writers too. */
	venture_report_pack_service_get(database);

	/* And the ones a dashboard needs: a widget kind that exists, a
	 * report that exists, one home page at a time. */
	venture_dashboard_install_validators(self);

	/* The workdesk: the inbox listens to the audit trail, a ticket gets
	 * its service-level clocks on its first save, and a worklog keeps
	 * its ticket's total in step. All on every writer. */
	venture_notify_install(self);
	venture_sla_install(self);
	venture_desk_install(self);

	/* A comment stays on its record, in its record's organization, under
	 * the author it was written by -- whichever door wrote it. */
	venture_comment_install(self);

	/* The factory's timestamps follow its statuses, so the reports that
	 * measure the loop see every record whoever wrote it. */
	venture_factory_install(self);

	/* Who a new ticket goes to, and who outside hears that it changed.
	 * Routing is a validator so it runs before the row is written;
	 * webhooks listen to the audit trail, like the inbox. */
	venture_routing_install(self);
	venture_webhook_install(self);

	/* The currencies this install defined, into the registry every
	 * formatter reads, and kept there as the table changes. */
	venture_currency_install(self);
	/* A purchase order's and a vendor bill's lines are in their
	 * document's currency. */
	venture_purchasing_install_validators(self->database);

	/* Category and location trees: no loops, no parent in another
	 * organization, and a category used only by the records it groups. */
	venture_category_install(self);

	/* A venture held to its declared type: the YAML's required, choices,
	 * min and max, on every writer. */
	venture_venture_type_check_install(self);

	/* Listings and price observations: quantities that agree with the
	 * outcome, a closing time that follows it, one currency per listing. */
	venture_market_install(self);

	/* Venues and instruments with one derived reference each, an
	 * instrument tree with no loop, and watchlists that name an
	 * instrument once. */
	venture_marketdata_install(self);

	/* Recipes that make a product in their own organization and never
	 * take it, and the craft action that turns stock into other stock. */
	venture_production_install(self);

	/* Sessions whose length follows their times, yields that are goods
	 * or money and never both, and the post action that puts goods into
	 * stock once. */
	venture_sessions_install(self);

	/* Holdings: memo movements only the ledger derives, and no holding
	 * spent below zero unless its account allows it; the transfer action
	 * between two locations. */
	venture_holdings_install(self);

	/* Goals whose target differs from their start, sub-goals that do not
	 * loop, and achieved and done times that follow the status. */
	venture_goals_install(self);

	/* Arbitrage trades and legs: an executed leg posts on save through the
	 * ledger's source registry, stock legs are executed by an action that
	 * moves the units once, and a closed trade's executed legs are frozen. */
	venture_arbitrage_install(self);

	/* Forms whose tokens are unique capabilities, questions whose keys
	 * never move, and responses only the public door creates. */
	venture_forms_install(self);
	venture_booking_service_install(self->database);

	/*
	 * The confirmation queue exists whether or not AI does. It began as
	 * the assistant's, but a change proposed by an outside agent holding
	 * an API token has to land somewhere a person can answer it, and one
	 * queue answered from one page is the whole point. An install with
	 * `ai.enabled: false` still stages REST writes.
	 */
	{
		gint64 ttl;
		gint64 limit;

		g_object_get(config,
		             "ai-confirmation-ttl", &ttl,
		             "ai-confirmation-limit", &limit,
		             NULL);

		self->confirmations = venture_confirmation_store_new(database, ttl,
		                                                     limit);
	}

	venture_report_registry_register_builtins(self->reports);
	/* Registered collection actions need the same reports as explicit callers. */
	venture_collection_service_set_context(venture_collection_service_get(self->database), self);
	/* Bound rather than copied: a base URL corrected on a running install
	 * must change the next reminder's pay link, not the one after a restart. */
	g_object_bind_property(config, "server-base-url", venture_dunning_service_get(self->database), "base-url",
		G_BINDING_SYNC_CREATE);
	/* Scheduled backups read their destination and retention defaults here. */
	venture_backup_schedule_service_set_config(venture_backup_schedule_service_get(self->database), config);
	venture_oidc_service_set_config(venture_oidc_service_get(self->database), config);
	venture_ai_provider_service_set_config(venture_ai_provider_service_get(self->database), config);
	g_object_bind_property(config, "server-base-url", venture_sequence_service_get(self->database), "base-url",
		G_BINDING_SYNC_CREATE);
	/* Bound, like the base URL: turning price-change mail off on a running
	 * install must stop the next one, not the one after a restart. */
	g_object_bind_property(config, "billing-trial-reminder-days", venture_billing_service_get(self->database),
		"trial-reminder-days", G_BINDING_SYNC_CREATE);
	g_object_bind_property(config, "billing-price-change-notices", venture_billing_service_get(self->database),
		"price-change-notices", G_BINDING_SYNC_CREATE);
	g_object_bind_property(config, "locale-timezone", venture_billing_service_get(self->database), "timezone",
		G_BINDING_SYNC_CREATE);
	/* Generated invoice dates and same-day receipts follow the business
	 * calendar, not the zone the process runs in. */
	g_object_bind_property(config, "locale-timezone", venture_settlement_service_get(self->database), "timezone",
		G_BINDING_SYNC_CREATE);
	venture_mfa_service_configure(venture_mfa_service_get(self->database), config);
	venture_ocr_service_configure(venture_ocr_service_get(self->database), config);
	venture_stripe_actions_set_context(self->database, self);
	venture_marketing_service_configure(venture_marketing_service_get(self->database), self);

	/*
	 * Modules, resolved against this configuration and applied to the
	 * registries above. The configuration was validated before the
	 * database was opened, so a dependency conflict cannot reach here
	 * from the server; a test that skips validation gets the resolved
	 * state -- every module that cannot work is off -- and a warning.
	 */
	self->modules = venture_module_registry_new();
	venture_module_registry_register_builtins(self->modules);

	{
		g_autoptr(GError) local_error = NULL;

		if (!venture_module_registry_configure(self->modules, config,
		                                       &local_error))
			g_warning("Modules: %s", local_error->message);
	}

	venture_context_apply_modules(self);

	/* The registry re-resolves when the configuration changes under it;
	 * the registries this context masks have to follow. */
	g_signal_connect_object(self->modules, "changed",
	                        G_CALLBACK(venture_context_on_modules_changed),
	                        self, G_CONNECT_SWAPPED);
	venture_period_service_install(self);
	venture_close_service_install(self);

#ifdef VENTURE_HAVE_SQLITE
	/* Market data sources: their settings checked against their provider,
	 * the sync, test and purge actions, and the listeners that keep the
	 * feeds worker in step with the records. Last, because it listens to
	 * the module registry made above. */
	venture_feeds_install(self);
#endif

	/* Alert rules and hits: their validators and the evaluate action in
	 * any build, and with SQLite the feeds hook that evaluates rules after
	 * every run. After the feeds module, whose hook it adds. */
	venture_marketdata_alerts_install(self);

	/* The operator's accounts as locations and positions as listings: the
	 * location and settings validators in any build, and with SQLite the
	 * feeds hook that mirrors after every run. After the feeds module. */
	venture_marketdata_mirror_install(self);

	/* An external ledger in the books: the books settings and posting
	 * validators, the post_ledger and record_flips actions, and with
	 * SQLite the hook that posts after a run. After the mirror, whose
	 * hook makes a run's accounts into places first. */
	venture_arbitrage_books_install(self);

	return self;
}

VentureModuleRegistry *
venture_context_get_modules(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->modules;
}

gboolean
venture_context_module_enabled(
	VentureContext	*self,
	const gchar	*module_name
){
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), FALSE);

	return venture_module_registry_is_enabled(self->modules, module_name);
}

gboolean
venture_context_register_module(
	VentureContext			 *self,
	const VentureModuleInfo		 *info,
	GError				**error
){
	gsize i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), FALSE);
	g_return_val_if_fail(NULL != info, FALSE);

	/* The types first, so the module can be applied to them at once. A
	 * type already registered -- a plugin reloaded -- is accepted. */
	for (i = 0; (NULL != info->entity_types) &&
	            (NULL != info->entity_types[i]); i++)
	{
		if (!venture_entity_registry_register(self->entities,
		                                      info->entity_types[i](), error))
			return FALSE;
	}

	if (!venture_module_registry_add(self->modules, info,
	                                 VENTURE_MODULE_ORIGIN_PLUGIN, error))
		return FALSE;

	venture_context_apply_modules(self);

	return TRUE;
}

void
venture_context_apply_modules(VentureContext *self)
{
	g_autoptr(GPtrArray) modules = NULL;
	guint i;

	g_return_if_fail(VENTURE_IS_CONTEXT(self));

	venture_module_registry_apply(self->modules, self->entities);

	/*
	 * A disabled module's reports are hidden rather than left to fail: a
	 * report over a table its module never created is a SQL error
	 * dressed as a report, and the reports page, the AI's report tool
	 * and venturectl all list what the registry offers.
	 */
	modules = venture_module_registry_list(self->modules);

	for (i = 0; i < modules->len; i++)
	{
		VentureModule *module;
		const gchar *const *reports;
		gsize j;

		module = g_ptr_array_index(modules, i);
		reports = venture_module_get_reports(module);

		for (j = 0; NULL != reports[j]; j++)
			venture_report_registry_set_enabled(
				self->reports, reports[j],
				venture_module_is_enabled(module));
	}
	if (venture_context_module_enabled(self, "autojournal")) venture_database_get_autojournal_service(self->database);
}

VentureConfirmationStore *
venture_context_get_confirmations(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->confirmations;
}

gchar *
venture_context_get_plugin_cache_dir(
	VentureContext	 *self,
	const gchar	 *plugin,
	GError		**error
){
	g_autofree gchar *path = NULL;
	const gchar *p;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	for (p = plugin; (NULL != p) && ('\0' != *p); p++)
		if (!(g_ascii_islower(*p) || g_ascii_isdigit(*p) || ('-' == *p) || ('_' == *p)))
			break;

	if ((NULL == plugin) || ('\0' == *plugin) || ('\0' != *p) || (strlen(plugin) > 64))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a plugin name a cache directory can be named for",
		            (NULL != plugin) ? plugin : "");
		return NULL;
	}

	path = g_build_filename(venture_config_get_state_dir(venture_context_get_config(self)),
	                        "plugin-cache", plugin, NULL);

	if (0 != g_mkdir_with_parents(path, 0700))
	{
		gint saved = errno;

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		            "Cannot create %s: %s", path, g_strerror(saved));
		return NULL;
	}

	return g_steal_pointer(&path);
}

VentureConfig *
venture_context_get_config(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->config;
}

VentureDatabase *
venture_context_get_database(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->database;
}

VentureEntityRegistry *
venture_context_get_entity_registry(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->entities;
}

VentureReportRegistry *
venture_context_get_report_registry(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->reports;
}

GTimeZone *
venture_context_get_timezone(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->timezone;
}

gint
venture_context_get_fiscal_year_start_month(VentureContext *self)
{
	gint64 month;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), 1);

	g_object_get(self->config, "locale-fiscal-year-start-month", &month, NULL);

	return (gint)month;
}

VentureDateRange *
venture_context_parse_period(
	VentureContext	 *self,
	const gchar	 *text,
	GError		**error
){
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	/* Defaulting to the current month rather than to all time matters:
	 * an unqualified "how are sales doing" should answer about now, and
	 * an all-time default would quietly scan the whole table. */
	if (venture_string_is_empty(text))
		text = "this_month";

	return venture_date_range_parse(text, self->timezone,
		venture_context_get_fiscal_year_start_month(self), error);
}

gint64
venture_context_get_default_organization_id(VentureContext *self)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), 0);

	if (0 != self->default_organization_id)
		return self->default_organization_id;

	/* The workspace's answer, kept for every later caller, so it is looked up
	 * with trusted internal authority, never through the access of whichever
	 * request happens to ask first. */
	internal = venture_access_policy_enter(
		venture_database_get_access_policy(self->database), NULL);

	/* Prefer the entity explicitly flagged as default. */
	query = venture_query_new(VENTURE_TYPE_ORGANIZATION);

	if (venture_query_add_filter_string(query, "is-default",
	                                    VENTURE_FILTER_OP_EQ, "true", NULL))
	{
		organization = venture_database_find_one(self->database, query, NULL);
	}

	if (NULL == organization)
	{
		g_autoptr(VentureQuery) any = NULL;

		/* Otherwise the first one, which on a fresh install is the
		 * organisation the migration seeded. */
		any = venture_query_new(VENTURE_TYPE_ORGANIZATION);
		organization = venture_database_find_one(self->database, any, NULL);
	}

	if (NULL == organization)
		return 0;

	self->default_organization_id = venture_entity_get_id(organization);

	return self->default_organization_id;
}

VentureVentureTypeRegistry *
venture_context_get_venture_types(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->venture_types;
}

void
venture_context_set_ai_service(
	VentureContext		*self,
	VentureAiService	*service
){
	g_return_if_fail(VENTURE_IS_CONTEXT(self));

	g_set_object(&self->ai, service);
	if (self->reconciliation_registry != NULL)
	{
		venture_reconciliation_registry_remove(self->reconciliation_registry, "ai");
		if (service != NULL)
			venture_reconciliation_registry_add(self->reconciliation_registry, VENTURE_RECONCILIATION_MATCHER(venture_ai_matcher_new(service)));
	}
}

void
venture_context_set_kb_service(
	VentureContext		*self,
	VentureKbService	*service
){
	g_return_if_fail(VENTURE_IS_CONTEXT(self));

	g_set_object(&self->kb, service);
}

VentureKbService *
venture_context_get_kb_service(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->kb;
}

VentureAiService *
venture_context_get_ai_service(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->ai;
}

/*
 * The harness is built on first use rather than at construction.
 *
 * Building it scans the resource directories, which is a handful of
 * stats against the home directory. An install nobody opens the assistant
 * on should not pay for that at startup, and a command written after the
 * server came up is found because the first request is what goes looking.
 */
VentureAiHarness *
venture_context_get_ai_harness(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	if (NULL == self->harness)
		self->harness = venture_ai_harness_new(self);

	return self->harness;
}

void
venture_context_set_automation(
	VentureContext		*self,
	VentureAutomation	*automation
){
	g_return_if_fail(VENTURE_IS_CONTEXT(self));

	g_set_object(&self->automation, automation);
}

VentureAutomation *
venture_context_get_automation(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->automation;
}

void
venture_context_set_work_service(
	VentureContext		*self,
	VentureWorkService	*service
){
	g_return_if_fail(VENTURE_IS_CONTEXT(self));

	g_set_object(&self->work, service);
}

VentureWorkService *
venture_context_get_work_service(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->work;
}

void
venture_context_set_plugin_manager(
	VentureContext		*self,
	VenturePluginManager	*manager
){
	g_return_if_fail(VENTURE_IS_CONTEXT(self));

	g_set_object(&self->plugins, manager);
}

VenturePluginManager *
venture_context_get_plugin_manager(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	return self->plugins;
}

VenturePluginProvidesRegistry *
venture_context_get_plugin_provides(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	/*
	 * On the context, not the plugin manager: the subsystem that
	 * understands a kind registers it when it is built, which is before
	 * main() makes a manager -- and a test may make several managers over
	 * one context.
	 */
	if (NULL == self->plugin_provides)
	{
		self->plugin_provides = venture_plugin_provides_registry_new();

		/* The kinds core understands, before any plugin can name one. */
		venture_automation_register_provides(self->plugin_provides);
#ifdef VENTURE_HAVE_SQLITE
		venture_feeds_register_provides(self->plugin_provides);
#endif
	}

	return self->plugin_provides;
}

VentureAutomationHandlerRegistry *
venture_context_get_automation_handlers(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), NULL);

	/* Built-ins first, so no plugin can take one of their names. */
	if (NULL == self->automation_handlers)
	{
		self->automation_handlers = venture_automation_handler_registry_new();
		venture_automation_register_builtins(self->automation_handlers);
#ifdef VENTURE_HAVE_SQLITE
		venture_feeds_register_automation(self->automation_handlers);
#endif
	}

	return self->automation_handlers;
}

typedef struct
{
	VentureWebExtensionFunc	 func;
	gpointer		 user_data;
	GDestroyNotify		 destroy;
} VentureWebExtension;

static void
venture_web_extension_free(gpointer data)
{
	VentureWebExtension *extension;

	extension = data;

	if (NULL != extension->destroy)
		extension->destroy(extension->user_data);

	g_free(extension);
}

guint
venture_context_count_web_extensions(VentureContext *self)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), 0);

	return (NULL != self->web_extensions) ? self->web_extensions->len : 0;
}

void
venture_context_remove_web_extension(
	VentureContext	*self,
	guint		 index
){
	g_return_if_fail(VENTURE_IS_CONTEXT(self));

	if ((NULL != self->web_extensions) && (index < self->web_extensions->len))
		g_ptr_array_remove_index(self->web_extensions, index);
}

void
venture_context_add_web_extension(
	VentureContext		*self,
	VentureWebExtensionFunc	 func,
	gpointer		 user_data,
	GDestroyNotify		 destroy
){
	VentureWebExtension *extension;

	g_return_if_fail(VENTURE_IS_CONTEXT(self));
	g_return_if_fail(NULL != func);

	if (NULL == self->web_extensions)
		self->web_extensions = g_ptr_array_new_with_free_func(
			venture_web_extension_free);

	extension = g_new0(VentureWebExtension, 1);
	extension->func = func;
	extension->user_data = user_data;
	extension->destroy = destroy;

	g_ptr_array_add(self->web_extensions, extension);
}

guint
venture_context_run_web_extensions(
	VentureContext		*self,
	VentureWebServer	*server
){
	guint succeeded;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), 0);

	succeeded = 0;

	for (i = 0; (NULL != self->web_extensions) &&
	            (i < self->web_extensions->len); i++)
	{
		g_autoptr(GError) error = NULL;
		VentureWebExtension *extension;

		extension = g_ptr_array_index(self->web_extensions, i);

		/*
		 * One broken extension must not cost the operator the rest of
		 * the server -- the same rule a broken plugin follows at load.
		 * The routes it added before failing cannot be taken back out
		 * of the router, so the warning says so.
		 */
		if (extension->func(server, extension->user_data, &error))
		{
			succeeded++;
			continue;
		}

		g_warning("Skipping the rest of a web extension: %s",
		          (NULL != error) ? error->message : "it gave no reason");
	}

	return succeeded;
}

VentureReconciliationRegistry *
venture_context_get_reconciliation_registry(VentureContext *self)
{
	if (!venture_context_module_enabled(self, "reconciliation")) return NULL;
	if (self->reconciliation_registry == NULL)
	{
		self->reconciliation_registry = venture_reconciliation_registry_new();
		venture_reconciliation_registry_add(self->reconciliation_registry, VENTURE_RECONCILIATION_MATCHER(venture_exact_matcher_new()));
		if (self->ai != NULL)
			venture_reconciliation_registry_add(self->reconciliation_registry, VENTURE_RECONCILIATION_MATCHER(venture_ai_matcher_new(self->ai)));
	}
	return self->reconciliation_registry;
}
VentureReconciliationService *
venture_context_get_reconciliation_service(VentureContext *self)
{
	if (!venture_context_module_enabled(self, "reconciliation")) return NULL;
	if (self->reconciliation_service == NULL)
		self->reconciliation_service = venture_reconciliation_service_new(self);
	return self->reconciliation_service;
}

VentureStripeService *
venture_context_get_stripe_service(VentureContext *self)
{
	return venture_context_module_enabled(self, "stripe") ? self->stripe : NULL;
}

void
venture_context_set_stripe_service(VentureContext *self, VentureStripeService *service)
{
	g_set_object(&self->stripe, service);
}

gboolean
venture_context_start_stripe(VentureContext *self, GError **error)
{
	/* Each verified invoice selects its own organization binding at use time. */
	(void)self;
	(void)error;
	return TRUE;
}

VentureBankFeedService *
venture_context_get_bankfeed_service(VentureContext *self)
{
	return venture_context_module_enabled(self, "bankfeed") ? self->bankfeed : NULL;
}
void
venture_context_set_bankfeed_service(VentureContext *self, VentureBankFeedService *service)
{
	g_set_object(&self->bankfeed, service);
}
gboolean
venture_context_start_feeds(VentureContext *self, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), FALSE);

	(void)error;

#ifdef VENTURE_HAVE_SQLITE
	/* Made now so a source that runs on its own starts on its own; with
	 * the module off this is nothing at all. */
	(void)venture_context_get_feeds_service(self);
#endif

	return TRUE;
}

gboolean
venture_context_start_bankfeed(VentureContext *self, GError **error)
{
	g_autoptr(VentureBankFeedService) provider = NULL;
	if (!venture_context_module_enabled(self, "bankfeed")) return TRUE;
	provider = venture_bankfeed_service_new(self->database,
		venture_context_get_default_organization_id(self), NULL, error);
	if (!provider) return FALSE;
	venture_context_set_bankfeed_service(self, provider);
	return TRUE;
}
VentureCommerceService *
venture_context_get_commerce_service(VentureContext *self)
{
	return venture_context_module_enabled(self, "commerce") ? self->commerce : NULL;
}
void
venture_context_set_commerce_service(VentureContext *self, VentureCommerceService *service)
{
	g_set_object(&self->commerce, service);
}
gboolean
venture_context_start_commerce(VentureContext *self, GError **error)
{
	g_autoptr(VentureCommerceService) provider = NULL;
	if (!venture_context_module_enabled(self, "commerce")) return TRUE;
	provider = venture_commerce_service_new(self->database,
		venture_context_get_default_organization_id(self), NULL, error);
	if (!provider) return FALSE;
	venture_context_set_commerce_service(self, provider);
	return TRUE;
}
VentureMailer *venture_context_get_mailer(VentureContext *self)
{
	if (!venture_context_module_enabled(self, "mail")) return NULL;
	if (!venture_mailer_registry_lookup(self->mailers, "smtp")) {
		g_autoptr(VentureOrganizationMailer) smtp = venture_organization_mailer_new(self->database, self->config);
		venture_mailer_registry_add(self->mailers, "smtp", VENTURE_MAILER(smtp));
	}
	return venture_mailer_registry_lookup(self->mailers, "smtp");
}
VentureMailOutbox *venture_context_get_mail_outbox(VentureContext *self)
{
	VentureMailer *mailer = venture_context_get_mailer(self);
	if (!mailer) return NULL;
	g_object_set(self->mail_outbox, "mailer", mailer, NULL);
	return self->mail_outbox;
}

void venture_context_set_mailer(VentureContext *self, VentureMailer *mailer)
{
	venture_mailer_registry_add(self->mailers, "smtp", mailer);
}
VentureMailerRegistry *venture_context_get_mailer_registry(VentureContext *self)
{
	return self->mailers;
}
