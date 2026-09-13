/*
 * venture-module.c - Modules: the switchable pieces VENTURE is made of
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

/* --- VentureModuleOrigin -------------------------------------------------- */

GType
venture_module_origin_get_type(void)
{
	static gsize type_id = 0;

	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_MODULE_ORIGIN_BUILTIN, "VENTURE_MODULE_ORIGIN_BUILTIN",
			  "builtin" },
			{ VENTURE_MODULE_ORIGIN_PLUGIN, "VENTURE_MODULE_ORIGIN_PLUGIN",
			  "plugin" },
			{ 0, NULL, NULL }
		};
		GType id;

		id = g_enum_register_static("VentureModuleOrigin", values);
		g_once_init_leave(&type_id, id);
	}

	return type_id;
}

/* --- VentureModule -------------------------------------------------------- */

struct _VentureModule
{
	GObject parent_instance;

	const VentureModuleInfo	*info;
	VentureModuleOrigin	 origin;

	/* Resolved from the types at registration, so the table never
	 * spells a name the type already knows. */
	GStrv			 entity_names;

	/* Empty, never NULL, so callers can iterate without checking. */
	const gchar *const	*requires;
	const gchar *const	*suggests;
	const gchar *const	*reports;

	gboolean		 enabled;
	gchar			*disabled_reason;
};

G_DEFINE_FINAL_TYPE(VentureModule, venture_module, G_TYPE_OBJECT)

static const gchar *const venture_module_no_names[] = { NULL };

static void
venture_module_finalize(GObject *object)
{
	VentureModule *self;

	self = VENTURE_MODULE(object);

	g_clear_pointer(&self->entity_names, g_strfreev);
	g_clear_pointer(&self->disabled_reason, g_free);

	G_OBJECT_CLASS(venture_module_parent_class)->finalize(object);
}

static void
venture_module_class_init(VentureModuleClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_module_finalize;
}

static void
venture_module_init(VentureModule *self)
{
	self->enabled = TRUE;
	self->requires = venture_module_no_names;
	self->suggests = venture_module_no_names;
	self->reports = venture_module_no_names;
}

/*
 * Builds a module from its description. The entity names are resolved
 * here, once: each type is instantiated for its name, which is a virtual
 * method and therefore needs an instance.
 */
static VentureModule *
venture_module_new(
	const VentureModuleInfo	*info,
	VentureModuleOrigin	 origin
){
	VentureModule *self;
	g_autoptr(GPtrArray) names = NULL;
	gsize i;

	self = g_object_new(VENTURE_TYPE_MODULE, NULL);
	self->info = info;
	self->origin = origin;

	if (NULL != info->requires)
		self->requires = info->requires;

	if (NULL != info->suggests)
		self->suggests = info->suggests;

	if (NULL != info->reports)
		self->reports = info->reports;

	names = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; (NULL != info->entity_types) &&
	            (NULL != info->entity_types[i]); i++)
	{
		g_autoptr(VentureEntity) prototype = NULL;

		prototype = g_object_new(info->entity_types[i](), NULL);
		g_ptr_array_add(names,
		                g_strdup(venture_entity_get_entity_name(prototype)));
	}

	g_ptr_array_add(names, NULL);
	self->entity_names = (GStrv)g_ptr_array_free(g_steal_pointer(&names),
	                                             FALSE);

	return self;
}

const gchar *
venture_module_get_name(VentureModule *self)
{
	g_return_val_if_fail(VENTURE_IS_MODULE(self), NULL);

	return self->info->name;
}

const gchar *
venture_module_get_label(VentureModule *self)
{
	g_return_val_if_fail(VENTURE_IS_MODULE(self), NULL);

	return (NULL != self->info->label) ? self->info->label : self->info->name;
}

const gchar *
venture_module_get_description(VentureModule *self)
{
	g_return_val_if_fail(VENTURE_IS_MODULE(self), NULL);

	return (NULL != self->info->description) ? self->info->description : "";
}

const gchar *const *
venture_module_get_requires(VentureModule *self)
{
	g_return_val_if_fail(VENTURE_IS_MODULE(self), venture_module_no_names);

	return self->requires;
}

const gchar *const *
venture_module_get_suggests(VentureModule *self)
{
	g_return_val_if_fail(VENTURE_IS_MODULE(self), venture_module_no_names);

	return self->suggests;
}

const gchar *const *
venture_module_get_entity_names(VentureModule *self)
{
	g_return_val_if_fail(VENTURE_IS_MODULE(self), venture_module_no_names);

	return (const gchar *const *)self->entity_names;
}

const gchar *const *
venture_module_get_reports(VentureModule *self)
{
	g_return_val_if_fail(VENTURE_IS_MODULE(self), venture_module_no_names);

	return self->reports;
}

const gchar *
venture_module_get_legacy_switch(VentureModule *self)
{
	g_return_val_if_fail(VENTURE_IS_MODULE(self), NULL);

	return self->info->legacy_switch;
}

gboolean
venture_module_is_locked(VentureModule *self)
{
	g_return_val_if_fail(VENTURE_IS_MODULE(self), FALSE);

	return self->info->locked;
}

gboolean
venture_module_is_enabled(VentureModule *self)
{
	g_return_val_if_fail(VENTURE_IS_MODULE(self), FALSE);

	return self->enabled;
}

const gchar *
venture_module_get_disabled_reason(VentureModule *self)
{
	g_return_val_if_fail(VENTURE_IS_MODULE(self), NULL);

	return self->enabled ? NULL : self->disabled_reason;
}

VentureModuleOrigin
venture_module_get_origin(VentureModule *self)
{
	g_return_val_if_fail(VENTURE_IS_MODULE(self), VENTURE_MODULE_ORIGIN_BUILTIN);

	return self->origin;
}

static void
venture_module_set_state(
	VentureModule	*self,
	gboolean	 enabled,
	const gchar	*reason
){
	self->enabled = enabled;
	g_clear_pointer(&self->disabled_reason, g_free);

	if (!enabled)
		self->disabled_reason = g_strdup((NULL != reason) ? reason : "disabled");
}

static void
venture_module_json_add_strv(
	JsonBuilder		*builder,
	const gchar		*member,
	const gchar *const	*values
){
	gsize i;

	json_builder_set_member_name(builder, member);
	json_builder_begin_array(builder);

	for (i = 0; (NULL != values) && (NULL != values[i]); i++)
		json_builder_add_string_value(builder, values[i]);

	json_builder_end_array(builder);
}

JsonNode *
venture_module_to_json(VentureModule *self)
{
	g_autoptr(JsonBuilder) builder = NULL;

	g_return_val_if_fail(VENTURE_IS_MODULE(self), NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder, self->info->name);
	json_builder_set_member_name(builder, "label");
	json_builder_add_string_value(builder, venture_module_get_label(self));
	json_builder_set_member_name(builder, "description");
	json_builder_add_string_value(builder,
	                              venture_module_get_description(self));
	json_builder_set_member_name(builder, "origin");
	json_builder_add_string_value(builder,
		venture_enum_to_nick(VENTURE_TYPE_MODULE_ORIGIN, (gint)self->origin));
	json_builder_set_member_name(builder, "locked");
	json_builder_add_boolean_value(builder, self->info->locked);
	json_builder_set_member_name(builder, "enabled");
	json_builder_add_boolean_value(builder, self->enabled);

	json_builder_set_member_name(builder, "disabled_reason");

	if (self->enabled)
		json_builder_add_null_value(builder);
	else
		json_builder_add_string_value(builder, self->disabled_reason);

	json_builder_set_member_name(builder, "legacy_switch");

	if (NULL == self->info->legacy_switch)
		json_builder_add_null_value(builder);
	else
		json_builder_add_string_value(builder, self->info->legacy_switch);

	venture_module_json_add_strv(builder, "requires", self->requires);
	venture_module_json_add_strv(builder, "suggests", self->suggests);
	venture_module_json_add_strv(builder, "entity_types",
		(const gchar *const *)self->entity_names);
	venture_module_json_add_strv(builder, "reports", self->reports);

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* --- The built-in table --------------------------------------------------- */

/*
 * Written bottom-up: a module may only require what precedes it. The
 * graph, read downwards:
 *
 *   core
 *   +-- sales ------ finance ------ invoicing (also needs crm)
 *   +-- crm
 *   +-- outreach (needs sales)
 *   +-- ideas
 *   +-- kb
 *   +-- tickets ---- forge -------- factory
 *   +-- ai --------- chat
 *   +-- automation
 *   +-- plugins
 *   +-- dashboards
 *
 * finance requires sales because a profit-and-loss without revenue is not
 * a report anybody wants; outreach requires sales because a campaign's
 * return is measured in the sales attributed to it. Both could have been
 * softer, and were not: a module that is on but whose reports all fail is
 * worse than one the configuration refuses.
 */

#define VENTURE_MODULE_NAMES(...) \
	((const gchar *const []){ __VA_ARGS__, NULL })

static GType (*const venture_module_core_types[]) (void) = {
	venture_organization_get_type,
	venture_venture_get_type,
	venture_document_get_type,
	venture_record_link_get_type,
	venture_plugin_config_get_type,
	venture_user_get_type,
	venture_api_token_get_type,
	venture_audit_entry_get_type,
	/* Saved views, watches and the inbox belong to everybody, so they
	 * belong to the module that is always on. */
	venture_saved_view_get_type,
	venture_watch_get_type,
	venture_notification_get_type,
	NULL
};

static GType (*const venture_module_sales_types[]) (void) = {
	venture_product_get_type,
	venture_inventory_item_get_type,
	venture_inventory_txn_get_type,
	venture_sale_get_type,
	NULL
};

static GType (*const venture_module_finance_types[]) (void) = {
	venture_expense_get_type,
	venture_account_get_type,
	venture_ledger_entry_get_type,
	venture_tax_category_get_type,
	NULL
};

static GType (*const venture_module_crm_types[]) (void) = {
	venture_company_get_type,
	venture_contact_get_type,
	venture_interaction_get_type,
	venture_deal_get_type,
	NULL
};

static GType (*const venture_module_invoicing_types[]) (void) = {
	venture_invoice_get_type,
	venture_invoice_line_get_type,
	venture_invoice_event_get_type,
	NULL
};

static GType (*const venture_module_outreach_types[]) (void) = {
	venture_campaign_get_type,
	venture_newsletter_get_type,
	venture_subscriber_get_type,
	venture_post_get_type,
	NULL
};

static GType (*const venture_module_ideas_types[]) (void) = {
	venture_idea_get_type,
	venture_research_note_get_type,
	NULL
};

static GType (*const venture_module_kb_types[]) (void) = {
	venture_knowledge_base_get_type,
	venture_kb_article_get_type,
	venture_kb_chunk_get_type,
	venture_kb_link_get_type,
	NULL
};

static GType (*const venture_module_tickets_types[]) (void) = {
	venture_ticket_get_type,
	venture_ticket_comment_get_type,
	venture_ticket_relation_get_type,
	venture_sla_policy_get_type,
	venture_macro_get_type,
	venture_worklog_get_type,
	venture_sprint_get_type,
	venture_routing_rule_get_type,
	NULL
};

static GType (*const venture_module_federation_types[]) (void) = {
	venture_federation_replica_get_type,
	venture_federation_peer_get_type,
	venture_federation_grant_get_type,
	NULL
};

static GType (*const venture_module_webhooks_types[]) (void) = {
	venture_webhook_get_type,
	venture_webhook_delivery_get_type,
	NULL
};

static GType (*const venture_module_chat_types[]) (void) = {
	venture_chat_thread_get_type,
	venture_chat_message_get_type,
	venture_ai_skill_get_type,
	NULL
};

static GType (*const venture_module_forge_types[]) (void) = {
	venture_forge_get_type,
	venture_forge_repo_get_type,
	venture_forge_rule_get_type,
	venture_ticket_link_get_type,
	venture_forge_run_get_type,
	venture_agent_session_get_type,
	venture_agent_turn_get_type,
	venture_agent_budget_get_type,
	NULL
};

static GType (*const venture_module_factory_types[]) (void) = {
	venture_milestone_get_type,
	venture_release_get_type,
	venture_build_get_type,
	venture_environment_get_type,
	venture_deployment_get_type,
	venture_incident_get_type,
	NULL
};

static GType (*const venture_module_dashboards_types[]) (void) = {
	venture_dashboard_get_type,
	venture_dashboard_widget_get_type,
	NULL
};

static const gchar *const venture_module_requires_core[] = { "core", NULL };
static const gchar *const venture_module_requires_sales[] = { "sales", NULL };
static const gchar *const venture_module_requires_invoicing[] = {
	"finance", "crm", "ledger", NULL
};
static const gchar *const venture_module_requires_tickets[] = { "tickets", NULL };
static const gchar *const venture_module_requires_ai[] = { "ai", NULL };
static const gchar *const venture_module_requires_forge[] = { "forge", NULL };

static const gchar *const venture_module_suggests_crm[] = { "crm", NULL };
static const gchar *const venture_module_suggests_crm_sales[] = {
	"crm", "sales", NULL
};
static const gchar *const venture_module_suggests_tickets[] = {
	"crm", "ideas", "forge", NULL
};
static const gchar *const venture_module_suggests_forge[] = {
	"sales", "ai", NULL
};
static const gchar *const venture_module_suggests_ideas[] = {
	"kb", NULL
};
static const gchar *const venture_module_suggests_factory[] = {
	"sales", "automation", NULL
};

static const gchar *const venture_module_reports_sales[] = {
	"categories", "inventory", NULL
};
static const gchar *const venture_module_reports_finance[] = {
	"pnl", "ventures", "monthly", "tax", NULL
};
static const gchar *const venture_module_reports_crm[] = { "pipeline", NULL };
static const gchar *const venture_module_reports_receivables[] = {
	"receivables", "customer_statement", NULL
};
static const gchar *const venture_module_requires_receivables[] = {
	"finance", "invoicing", NULL
};
static GType (*const venture_module_receivables_types[]) (void) = {
	venture_payment_get_type, venture_payment_allocation_get_type,
	venture_customer_credit_get_type, venture_refund_get_type, NULL
};
static const gchar *const venture_module_reports_outreach[] = {
	"campaigns", NULL
};
static const gchar *const venture_module_reports_ideas[] = { "ideas", NULL };
static const gchar *const venture_module_reports_tickets[] = {
	"support", NULL
};
static const gchar *const venture_module_reports_factory[] = {
	"releases", "lead_time", "incidents", "delivery", NULL
};

static GType (*const venture_module_ledger_types[]) (void) = {
	venture_journal_get_type, venture_journal_line_get_type, NULL
};
static const gchar *const venture_module_requires_finance[] = { "finance", NULL };
static const gchar *const venture_module_reports_ledger[] = { "trial_balance", NULL };
static GType (*const venture_module_periods_types[]) (void) = {
	venture_fiscal_year_get_type, venture_fiscal_period_get_type,
	venture_report_snapshot_get_type, NULL
};
static const gchar *const venture_module_reports_periods[] = { "snapshot_vs_live", NULL };

static GType (*const venture_module_mail_types[]) (void) = {
	venture_mail_message_get_type, venture_mail_template_get_type, NULL
};
static GType (*const venture_module_leads_types[]) (void) = {
	venture_lead_get_type, venture_lead_form_get_type,
	venture_lead_assignment_rule_get_type, NULL
};
static const gchar *const venture_module_reports_leads[] = { "lead_sources", "lead_response_time", "leads_recycled_due", NULL };
static const gchar *const venture_module_requires_leads[] = { "crm", NULL };
static GType (*const venture_module_activities_types[]) (void) = {
	venture_activity_get_type, venture_activity_type_get_type, NULL
};
static const gchar *const venture_module_activities_requires[] = { "crm", NULL };
static const gchar *const venture_module_activities_reports[] = { "worklist", NULL };
static GType (*const venture_module_payables_types[]) (void) = {
	venture_vendor_bill_get_type, venture_vendor_bill_line_get_type,
	venture_bill_payment_get_type, venture_bill_payment_allocation_get_type,
	venture_vendor_credit_get_type, venture_vendor_bill_event_get_type,
	venture_bill_refund_get_type, NULL
};
static const gchar *const venture_module_requires_payables[] = { "finance", "ledger", "crm", NULL };
static const gchar *const venture_module_reports_payables[] = { "payables", "vendor_statement", NULL };

static const gchar *const banking_reports[] = { "bank_reconciliation", NULL };
static const gchar *const banking_requires[] = { "ledger", NULL };
static GType (*const banking_types[]) (void) = {
	venture_bank_account_get_type, venture_bank_statement_get_type,
	venture_bank_transaction_get_type, venture_bank_match_get_type,
	venture_reconciliation_get_type, NULL
};
static GType (*const venture_module_pipelines_types[]) (void) = {
	venture_pipeline_get_type, venture_pipeline_stage_get_type,
	venture_deal_stage_entry_get_type, venture_loss_reason_get_type, NULL
};
static const gchar *const venture_module_requires_crm[] = { "crm", NULL };
static const gchar *const venture_module_reports_pipelines[] = {
	"stage_duration", "funnel", "forecast", "loss_reasons", "overdue_deals", NULL
};

static GType (*const sequence_types[]) (void) = {
	venture_sequence_get_type, venture_sequence_step_get_type,
	venture_sequence_enrollment_get_type, venture_sequence_delivery_get_type,
	venture_suppression_get_type, NULL
};
static const gchar *const sequence_requires[] = { "crm", NULL };
static const gchar *const sequence_reports[] = { "sequence_performance", "sequence_failures", NULL };
static GType (*const autojournal_types[]) (void) = { venture_posting_profile_get_type, NULL };
static const gchar *const autojournal_requires[] = { "ledger", NULL };
static const gchar *const autojournal_reports[] = { "unposted", NULL };
static const gchar *const venture_module_requires_statements[] = { "ledger", "periods", NULL };
static const gchar *const venture_module_reports_statements[] = {
	"balance_sheet", "income_statement", "cash_flow", "general_ledger", "account_balances", "pnl_reconciliation", NULL
};

static const VentureModuleInfo venture_module_builtins[] = {
	{
		"core", "Core",
		"Entities, ventures, documents, accounts, the audit log, and the "
		"links between records. Always on.",
		NULL, NULL, venture_module_core_types, NULL, NULL, TRUE
	},
	{
		"sales", "Sales",
		"Products, inventory and sales: what you sell and what it sold for.",
		venture_module_requires_core, venture_module_suggests_crm,
		venture_module_sales_types, venture_module_reports_sales, NULL, FALSE
	},
	{
		"finance", "Finance",
		"Expenses, accounts, the ledger, tax categories, and the profit-and-"
		"loss reports built on them.",
		venture_module_requires_sales, NULL,
		venture_module_finance_types, venture_module_reports_finance, NULL,
		FALSE
	},
	{
		"ledger", "General journal", "Immutable double-entry journals and account balances.",
		venture_module_requires_finance, NULL, venture_module_ledger_types,
		venture_module_reports_ledger, NULL, FALSE
	},
	{
		"crm", "CRM",
		"Companies, contacts, interactions and deals.",
		venture_module_requires_core, NULL,
		venture_module_crm_types, venture_module_reports_crm, NULL, FALSE
	},
	{
		"invoicing", "Invoicing",
		"Invoices and their lines, billed to a company and recorded as a "
		"sale when paid.",
		venture_module_requires_invoicing, NULL,
		venture_module_invoicing_types, NULL, NULL,
		FALSE
	},
	{
		"receivables", "Receivables",
		"Customer receipts, allocations, credits, refunds and historical balances.",
		venture_module_requires_receivables, NULL,
		venture_module_receivables_types, venture_module_reports_receivables, NULL,
		FALSE
	},
	{
		"outreach", "Outreach",
		"Campaigns, newsletters, subscribers and posts, with revenue "
		"attributed back to the campaign that earned it.",
		venture_module_requires_sales, venture_module_suggests_crm_sales,
		venture_module_outreach_types, venture_module_reports_outreach, NULL,
		FALSE
	},
	{
		"ideas", "Ideas",
		"Ideas and research notes: what might become a venture, and what "
		"you found out about it.",
		venture_module_requires_core, venture_module_suggests_ideas,
		venture_module_ideas_types, venture_module_reports_ideas, NULL, FALSE
	},
	{
		"kb", "Knowledge bases",
		"Documents the assistant can read, indexed by meaning.",
		venture_module_requires_core, NULL,
		venture_module_kb_types, NULL, "kb-enabled", FALSE
	},
	{
		"tickets", "Tickets",
		"Internal tasks and external support requests, one shape, on a "
		"kanban board.",
		venture_module_requires_core, venture_module_suggests_tickets,
		venture_module_tickets_types, venture_module_reports_tickets, NULL,
		FALSE
	},
	{
		"federation", "Federation",
		"Explicit object sharing between authenticated servers.",
		venture_module_requires_core, NULL,
		venture_module_federation_types, NULL, "federation-enabled", FALSE
	},
	{
		"webhooks", "Webhooks",
		"Telling something outside that a record changed: a URL, the "
		"events it wants, a signing secret, and the log of what went out.",
		venture_module_requires_core, NULL,
		venture_module_webhooks_types, NULL, NULL, FALSE
	},
	{
		"ai", "AI",
		"The in-process assistant, its tools and the write-confirmation "
		"policy.",
		venture_module_requires_core, NULL, NULL, NULL, "ai-enabled", FALSE
	},
	{
		"chat", "Chat",
		"Persistent, per-user conversations with the assistant.",
		venture_module_requires_ai, NULL,
		venture_module_chat_types, NULL, NULL, FALSE
	},
	{
		"automation", "Automation",
		"Scheduled and event-driven rules, run by the embedded podomation "
		"engine.",
		venture_module_requires_core, NULL, NULL, NULL, "automation-enabled",
		FALSE
	},
	{
		"plugins", "Plugins",
		"Native and crispy plugins, and declarative venture types.",
		venture_module_requires_core, NULL, NULL, NULL, "plugins-enabled",
		FALSE
	},
	{
		"forge", "Git forges",
		"Repositories, issues, branches, pull requests, and AI coding runs "
		"against a Forgejo or Gitea instance.",
		venture_module_requires_tickets, venture_module_suggests_forge,
		venture_module_forge_types, NULL, "forge-enabled", FALSE
	},
	{
		"factory", "Software factory",
		"Milestones, releases, builds, environments, deployments and "
		"incidents: the loop from a ticket to a running release and back.",
		venture_module_requires_forge, venture_module_suggests_factory,
		venture_module_factory_types, venture_module_reports_factory, NULL,
		FALSE
	},
	{
		"dashboards", "Dashboards",
		"Custom pages of widgets: any report, any record type, any count "
		"or queue, arranged as you like, and more than one of them.",
		venture_module_requires_core, NULL,
		venture_module_dashboards_types, NULL, NULL, FALSE
	},

	{
		"periods", "Fiscal periods", "Fiscal calendars, closing controls and historical reports.",
		venture_module_requires_finance, NULL,
		venture_module_periods_types, venture_module_reports_periods, NULL, FALSE
	},
	{
		"mail", "Transactional mail", "Durable outbound messages and templates.",
		venture_module_requires_core, NULL, venture_module_mail_types, NULL, NULL, FALSE
	},
	{
		"leads", "Leads", "Capture, qualify, assign and convert inquiries.",
		venture_module_requires_leads, NULL, venture_module_leads_types,
		venture_module_reports_leads, NULL, FALSE
	},
	{
		"activities", "Planned activities", "Tasks, calls, meetings and the daily worklist.",
		venture_module_activities_requires, NULL, venture_module_activities_types,
		venture_module_activities_reports, NULL, FALSE
	},
	{
		"payables", "Payables", "Supplier bills, payments and dated vendor balances.",
		venture_module_requires_payables, NULL, venture_module_payables_types,
		venture_module_reports_payables, NULL, FALSE
	},

	{
		"banking", "Banking", "Statement import, matching and reconciliation.",
		banking_requires, NULL, banking_types, banking_reports, NULL, FALSE
	},
	{
		"pipelines", "Sales pipelines", "Configurable stages, history and forecasts.",
		venture_module_requires_crm, NULL, venture_module_pipelines_types,
		venture_module_reports_pipelines, NULL, FALSE
	},
	{
		"sequences", "Follow-up sequences", "Durable timed follow-ups and suppression.",
		sequence_requires, NULL, sequence_types, sequence_reports, NULL, FALSE
	},
	{ "autojournal", "Automatic journals", "Configurable source accounting.",
		autojournal_requires, NULL, autojournal_types, autojournal_reports, NULL, FALSE
	},
	{
		"statements", "Statements", "Financial statements from posted ledger evidence.",
		venture_module_requires_statements, NULL, NULL,
		venture_module_reports_statements, NULL, FALSE
	}

};

const VentureModuleInfo *
venture_module_registry_get_builtin_infos(gsize *n_infos)
{
	g_return_val_if_fail(NULL != n_infos, NULL);

	*n_infos = G_N_ELEMENTS(venture_module_builtins);

	return venture_module_builtins;
}

/* --- VentureModuleRegistry ------------------------------------------------ */

struct _VentureModuleRegistry
{
	GObject parent_instance;

	/* Registration order, which is dependency order by construction. */
	GPtrArray	*modules;

	/* name -> VentureModule, borrowed from @modules. */
	GHashTable	*by_name;

	/* The configuration the registry was last resolved against, so a
	 * module added afterwards -- by a plugin -- resolves the same way. */
	VentureConfig	*config;
};

enum
{
	SIGNAL_CHANGED,
	N_REGISTRY_SIGNALS
};

static guint venture_module_registry_signals[N_REGISTRY_SIGNALS] = { 0 };

G_DEFINE_FINAL_TYPE(VentureModuleRegistry, venture_module_registry, G_TYPE_OBJECT)

static void
venture_module_registry_on_config_changed(
	VentureModuleRegistry	*self
);

static void
venture_module_registry_finalize(GObject *object)
{
	VentureModuleRegistry *self;

	self = VENTURE_MODULE_REGISTRY(object);

	if (NULL != self->config)
	{
		g_signal_handlers_disconnect_by_data(self->config, self);
		g_clear_object(&self->config);
	}

	g_clear_pointer(&self->by_name, g_hash_table_unref);
	g_clear_pointer(&self->modules, g_ptr_array_unref);

	G_OBJECT_CLASS(venture_module_registry_parent_class)->finalize(object);
}

static void
venture_module_registry_class_init(VentureModuleRegistryClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_module_registry_finalize;

	/**
	 * VentureModuleRegistry::changed:
	 * @self: the registry
	 *
	 * Emitted after the resolved states change -- because the
	 * configuration the registry was configured against changed a
	 * switch, or because a module was added. The context listens and
	 * re-applies the states to the entity and report registries.
	 */
	venture_module_registry_signals[SIGNAL_CHANGED] =
		g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
venture_module_registry_init(VentureModuleRegistry *self)
{
	self->modules = g_ptr_array_new_with_free_func(g_object_unref);
	self->by_name = g_hash_table_new(g_str_hash, g_str_equal);
}

VentureModuleRegistry *
venture_module_registry_new(void)
{
	return g_object_new(VENTURE_TYPE_MODULE_REGISTRY, NULL);
}

/*
 * A module name is a configuration key and a URL segment, so it is held
 * to the same alphabet an entity name is.
 */
static gboolean
venture_module_name_is_valid(const gchar *name)
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
 * Resolves one module against the configuration it was given, in the
 * order that decides precedence: the `modules` section, then the legacy
 * switch, then its requirements. Earlier modules are already resolved,
 * which is what makes the requirement check a lookup rather than a
 * search.
 *
 * Returns: %NULL if the module has what it needs, otherwise a message
 *   naming the requirement that is off
 */
static gchar *
venture_module_registry_resolve_one(
	VentureModuleRegistry	*self,
	VentureModule		*module
){
	const gchar *const *requires;
	gboolean enabled;
	gsize i;

	enabled = TRUE;
	venture_module_set_state(module, TRUE, NULL);

	if (module->info->locked)
		return NULL;

	if (NULL != self->config)
	{
		gboolean configured;

		if (venture_config_get_module_switch(self->config, module->info->name,
		                                     &configured) && !configured)
		{
			g_autofree gchar *reason = NULL;

			reason = g_strdup_printf("modules.%s.enabled is false",
			                         module->info->name);
			venture_module_set_state(module, FALSE, reason);
			enabled = FALSE;
		}

		if (enabled && (NULL != module->info->legacy_switch))
		{
			GParamSpec *pspec;
			gboolean legacy;

			pspec = g_object_class_find_property(
				G_OBJECT_GET_CLASS(self->config),
				module->info->legacy_switch);

			if ((NULL != pspec) && (G_TYPE_BOOLEAN == pspec->value_type))
			{
				g_object_get(self->config, module->info->legacy_switch,
				             &legacy, NULL);

				if (!legacy)
				{
					g_autofree gchar *reason = NULL;
					g_autofree gchar *key = NULL;

					/* Spelled the way it is written in the file,
					 * because that is where the operator will go
					 * to change it. */
					key = g_strdup(module->info->legacy_switch);
					g_strdelimit(key, "-", '.');
					reason = g_strdup_printf("%s is false", key);
					venture_module_set_state(module, FALSE, reason);
					enabled = FALSE;
				}
			}
		}
	}

	if (!enabled)
		return NULL;

	requires = module->requires;

	for (i = 0; NULL != requires[i]; i++)
	{
		VentureModule *dependency;

		dependency = g_hash_table_lookup(self->by_name, requires[i]);

		if ((NULL != dependency) && dependency->enabled)
			continue;

		{
			g_autofree gchar *reason = NULL;

			reason = g_strdup_printf("requires %s, which is disabled (%s)",
			                         requires[i],
			                         (NULL != dependency)
			                                 ? dependency->disabled_reason
			                                 : "not registered");
			venture_module_set_state(module, FALSE, reason);
		}

		return g_strdup_printf("Module \"%s\" requires \"%s\", which is "
		                       "disabled. Enable %s, or disable %s as well.",
		                       module->info->name, requires[i], requires[i],
		                       module->info->name);
	}

	return NULL;
}

gboolean
venture_module_registry_add(
	VentureModuleRegistry		 *self,
	const VentureModuleInfo		 *info,
	VentureModuleOrigin		  origin,
	GError				**error
){
	VentureModule *module;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_MODULE_REGISTRY(self), FALSE);
	g_return_val_if_fail(NULL != info, FALSE);

	if (!venture_module_name_is_valid(info->name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a module name: use lowercase letters, "
		            "digits, - and _",
		            (NULL != info->name) ? info->name : "");
		return FALSE;
	}

	if (g_hash_table_contains(self->by_name, info->name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "A module named \"%s\" is already registered",
		            info->name);
		return FALSE;
	}

	/*
	 * Requirements must precede the module. This is the rule that makes
	 * a cycle impossible: nothing can require a module that has not been
	 * added yet, so nothing can require something that requires it.
	 */
	for (i = 0; (NULL != info->requires) && (NULL != info->requires[i]); i++)
	{
		if (0 == g_strcmp0(info->requires[i], info->name))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "Module \"%s\" cannot require itself", info->name);
			return FALSE;
		}

		if (!g_hash_table_contains(self->by_name, info->requires[i]))
		{
			g_auto(GStrv) known = NULL;
			g_autofree gchar *list = NULL;

			known = venture_module_registry_list_names(self);
			list = g_strjoinv(", ", known);

			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Module \"%s\" requires \"%s\", which is not "
			            "registered. Dependencies must be registered "
			            "first. Known modules: %s",
			            info->name, info->requires[i],
			            (NULL != list) ? list : "");
			return FALSE;
		}
	}

	for (i = 0; (NULL != info->entity_types) &&
	            (NULL != info->entity_types[i]); i++)
	{
		if (!g_type_is_a(info->entity_types[i](), VENTURE_TYPE_ENTITY) ||
		    G_TYPE_IS_ABSTRACT(info->entity_types[i]()))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "Module \"%s\" claims %s, which is not a concrete "
			            "record type",
			            info->name, g_type_name(info->entity_types[i]()));
			return FALSE;
		}
	}

	module = venture_module_new(info, origin);

	/* A type can belong to one module only, or two switches would fight
	 * over it. */
	for (i = 0; NULL != module->entity_names[i]; i++)
	{
		VentureModule *owner;

		owner = venture_module_registry_get_module_for_type(
			self, module->entity_names[i]);

		if (NULL != owner)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
			            "Module \"%s\" claims the record type \"%s\", "
			            "which module \"%s\" already owns",
			            info->name, module->entity_names[i],
			            owner->info->name);
			g_object_unref(module);
			return FALSE;
		}
	}

	g_ptr_array_add(self->modules, module);
	g_hash_table_insert(self->by_name, (gpointer)info->name, module);

	/* Registered after the registry was configured -- a plugin's module
	 * -- so it resolves now, against the same configuration. */
	if (NULL != self->config)
	{
		g_autofree gchar *problem = NULL;

		problem = venture_module_registry_resolve_one(self, module);
		g_signal_emit(self, venture_module_registry_signals[SIGNAL_CHANGED],
		              0);

		if (NULL != problem)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			                    problem);
			return FALSE;
		}
	}

	return TRUE;
}

void
venture_module_registry_register_builtins(VentureModuleRegistry *self)
{
	gsize i;

	g_return_if_fail(VENTURE_IS_MODULE_REGISTRY(self));

	for (i = 0; i < G_N_ELEMENTS(venture_module_builtins); i++)
	{
		g_autoptr(GError) local_error = NULL;

		if (g_hash_table_contains(self->by_name,
		                          venture_module_builtins[i].name))
			continue;

		if (!venture_module_registry_add(self, &venture_module_builtins[i],
		                                 VENTURE_MODULE_ORIGIN_BUILTIN,
		                                 &local_error))
		{
			/* The built-in table is written by hand and checked here
			 * on every start; a mistake in it is a programming error,
			 * and a registry missing a core module would fail in a
			 * hundred quieter ways later. */
			g_error("Cannot register built-in module: %s",
			        local_error->message);
		}
	}
}

/*
 * Resolves every module against the held configuration, collecting the
 * conflicts. Shared by the explicit configure and the live re-resolution.
 */
static gchar *
venture_module_registry_resolve_all(VentureModuleRegistry *self)
{
	g_autoptr(GPtrArray) problems = NULL;
	guint i;

	problems = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < self->modules->len; i++)
	{
		gchar *problem;

		problem = venture_module_registry_resolve_one(
			self, g_ptr_array_index(self->modules, i));

		if (NULL != problem)
			g_ptr_array_add(problems, problem);
	}

	if (0 == problems->len)
		return NULL;

	g_ptr_array_add(problems, NULL);

	return g_strjoinv(" ", (gchar **)problems->pdata);
}

/*
 * A switch or a legacy setting changed after the registry was configured.
 * Re-resolve and say so; a conflict is a debug line here rather than an
 * error, because there is nobody to hand the error to -- and the state is
 * still consistent, with the dependent off.
 */
static void
venture_module_registry_on_config_changed(VentureModuleRegistry *self)
{
	g_autofree gchar *problem = NULL;

	problem = venture_module_registry_resolve_all(self);

	if (NULL != problem)
		g_debug("Modules after a configuration change: %s", problem);

	g_signal_emit(self, venture_module_registry_signals[SIGNAL_CHANGED], 0);
}

static void
venture_module_registry_on_notify(
	GObject		*config,
	GParamSpec	*pspec,
	gpointer	 user_data
){
	(void)config;
	(void)pspec;

	venture_module_registry_on_config_changed(user_data);
}

static void
venture_module_registry_on_switch(
	VentureConfig	*config,
	const gchar	*module_name,
	gpointer	 user_data
){
	(void)config;
	(void)module_name;

	venture_module_registry_on_config_changed(user_data);
}

gboolean
venture_module_registry_configure(
	VentureModuleRegistry	 *self,
	VentureConfig		 *config,
	GError			**error
){
	g_autofree gchar *problem = NULL;

	g_return_val_if_fail(VENTURE_IS_MODULE_REGISTRY(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_CONFIG(config), FALSE);

	if (config != self->config)
	{
		if (NULL != self->config)
		{
			g_signal_handlers_disconnect_by_data(self->config, self);
			g_clear_object(&self->config);
		}

		self->config = g_object_ref(config);

		/*
		 * Kept live. A switch flipped after startup -- by a compiled
		 * configuration, or a test -- would otherwise leave the
		 * registry describing a configuration that no longer exists.
		 */
		g_signal_connect(config, "notify",
		                 G_CALLBACK(venture_module_registry_on_notify), self);
		g_signal_connect(config, "module-switch-changed",
		                 G_CALLBACK(venture_module_registry_on_switch), self);
	}

	problem = venture_module_registry_resolve_all(self);
	g_signal_emit(self, venture_module_registry_signals[SIGNAL_CHANGED], 0);

	if (NULL != problem)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    problem);
		return FALSE;
	}

	return TRUE;
}

gboolean
venture_module_registry_check_configured(
	VentureModuleRegistry	 *self,
	VentureConfig		 *config,
	GError			**error
){
	g_auto(GStrv) configured = NULL;
	g_autoptr(GPtrArray) unknown = NULL;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_MODULE_REGISTRY(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_CONFIG(config), FALSE);

	configured = venture_config_list_module_switches(config);
	unknown = g_ptr_array_new();

	for (i = 0; (NULL != configured) && (NULL != configured[i]); i++)
	{
		if (!g_hash_table_contains(self->by_name, configured[i]))
			g_ptr_array_add(unknown, configured[i]);
	}

	if (unknown->len > 0)
	{
		g_auto(GStrv) known = NULL;
		g_autofree gchar *known_list = NULL;
		g_autofree gchar *unknown_list = NULL;

		g_ptr_array_add(unknown, NULL);
		unknown_list = g_strjoinv(", ", (gchar **)unknown->pdata);
		known = venture_module_registry_list_names(self);
		known_list = g_strjoinv(", ", known);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		            "The modules section names %s, which %s not a module. "
		            "Known modules: %s",
		            unknown_list,
		            (unknown->len > 2) ? "are" : "is", known_list);
		return FALSE;
	}

	return TRUE;
}

void
venture_module_registry_apply(
	VentureModuleRegistry	*self,
	VentureEntityRegistry	*entities
){
	guint i;

	g_return_if_fail(VENTURE_IS_MODULE_REGISTRY(self));
	g_return_if_fail(VENTURE_IS_ENTITY_REGISTRY(entities));

	for (i = 0; i < self->modules->len; i++)
	{
		VentureModule *module;
		gsize j;

		module = g_ptr_array_index(self->modules, i);

		for (j = 0; NULL != module->entity_names[j]; j++)
		{
			venture_entity_registry_set_type_module(
				entities, module->entity_names[j], module->info->name,
				module->enabled);
		}
	}
}

VentureModule *
venture_module_registry_lookup(
	VentureModuleRegistry	*self,
	const gchar		*name
){
	g_return_val_if_fail(VENTURE_IS_MODULE_REGISTRY(self), NULL);

	if (NULL == name)
		return NULL;

	return g_hash_table_lookup(self->by_name, name);
}

gboolean
venture_module_registry_is_enabled(
	VentureModuleRegistry	*self,
	const gchar		*name
){
	VentureModule *module;

	g_return_val_if_fail(VENTURE_IS_MODULE_REGISTRY(self), FALSE);

	module = venture_module_registry_lookup(self, name);

	return (NULL != module) && module->enabled;
}

GPtrArray *
venture_module_registry_list(VentureModuleRegistry *self)
{
	GPtrArray *list;
	guint i;

	g_return_val_if_fail(VENTURE_IS_MODULE_REGISTRY(self), NULL);

	list = g_ptr_array_new();

	for (i = 0; i < self->modules->len; i++)
		g_ptr_array_add(list, g_ptr_array_index(self->modules, i));

	return list;
}

gchar **
venture_module_registry_list_names(VentureModuleRegistry *self)
{
	g_autoptr(GPtrArray) names = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_MODULE_REGISTRY(self), NULL);

	names = g_ptr_array_new();

	for (i = 0; i < self->modules->len; i++)
	{
		VentureModule *module;

		module = g_ptr_array_index(self->modules, i);
		g_ptr_array_add(names, g_strdup(module->info->name));
	}

	g_ptr_array_add(names, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&names), FALSE);
}

gchar **
venture_module_registry_get_dependents(
	VentureModuleRegistry	*self,
	const gchar		*name
){
	g_autoptr(GPtrArray) names = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_MODULE_REGISTRY(self), NULL);
	g_return_val_if_fail(NULL != name, NULL);

	names = g_ptr_array_new();

	for (i = 0; i < self->modules->len; i++)
	{
		VentureModule *module;

		module = g_ptr_array_index(self->modules, i);

		if (g_strv_contains(module->requires, name))
			g_ptr_array_add(names, g_strdup(module->info->name));
	}

	g_ptr_array_add(names, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&names), FALSE);
}

VentureModule *
venture_module_registry_get_module_for_type(
	VentureModuleRegistry	*self,
	const gchar		*entity_name
){
	guint i;

	g_return_val_if_fail(VENTURE_IS_MODULE_REGISTRY(self), NULL);

	if (NULL == entity_name)
		return NULL;

	for (i = 0; i < self->modules->len; i++)
	{
		VentureModule *module;

		module = g_ptr_array_index(self->modules, i);

		if (g_strv_contains((const gchar *const *)module->entity_names,
		                    entity_name))
			return module;
	}

	return NULL;
}

JsonNode *
venture_module_registry_describe(VentureModuleRegistry *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_MODULE_REGISTRY(self), NULL);

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; i < self->modules->len; i++)
	{
		g_autoptr(JsonNode) node = NULL;
		g_auto(GStrv) dependents = NULL;
		VentureModule *module;
		JsonObject *object;
		JsonArray *array;
		gsize j;

		module = g_ptr_array_index(self->modules, i);
		node = venture_module_to_json(module);

		/* Who needs it, which the module itself cannot know. */
		dependents = venture_module_registry_get_dependents(
			self, module->info->name);
		object = json_node_get_object(node);
		array = json_array_new();

		for (j = 0; NULL != dependents[j]; j++)
			json_array_add_string_element(array, dependents[j]);

		json_object_set_array_member(object, "required_by", array);

		json_builder_add_value(builder, g_steal_pointer(&node));
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}
