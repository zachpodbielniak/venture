/*
 * venture-context.c - The wiring every subsystem is handed
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

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

static void
venture_context_class_init(VentureContextClass *klass)
{
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

	/* Who a new ticket goes to, and who outside hears that it changed.
	 * Routing is a validator so it runs before the row is written;
	 * webhooks listen to the audit trail, like the inbox. */
	venture_routing_install(self);
	venture_webhook_install(self);

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

	g_return_val_if_fail(VENTURE_IS_CONTEXT(self), 0);

	if (0 != self->default_organization_id)
		return self->default_organization_id;

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
	g_autoptr(VentureStripeService) provider = NULL;
	if (!venture_context_module_enabled(self, "stripe")) return TRUE;
	provider = venture_stripe_service_new(self->database,
		venture_context_get_default_organization_id(self), NULL, error);
	if (!provider) return FALSE;
	venture_context_set_stripe_service(self, provider);
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
		g_autoptr(VentureSmtpMailer) smtp = venture_smtp_mailer_new(self->config);
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
