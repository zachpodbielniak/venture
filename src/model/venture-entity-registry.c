/*
 * venture-entity-registry.c - The catalogue of known record types
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

struct _VentureEntityRegistry
{
	GObject parent_instance;

	/* entity name -> GType, stored as a pointer-sized integer. */
	GHashTable *types;

	/* entity name -> a shared instance used purely for introspection.
	 * Every question about a type -- its table, its fields, how it names
	 * a record -- goes through a virtual method and therefore needs an
	 * instance; keeping one per type avoids constructing a throwaway
	 * object on every REST request. */
	GHashTable *prototypes;

	/*
	 * entity name -> the module that owns it. A type no module claims --
	 * a plugin's, usually -- is absent here and always visible.
	 */
	GHashTable *modules;

	/*
	 * entity name -> present when hidden. The type stays registered,
	 * because turning a module back on must show it again without
	 * re-registering anything, but every lookup and listing skips it.
	 */
	GHashTable *hidden;
};

enum
{
	SIGNAL_TYPE_REGISTERED,
	N_SIGNALS
};

static guint venture_entity_registry_signals[N_SIGNALS] = { 0 };

G_DEFINE_FINAL_TYPE(VentureEntityRegistry, venture_entity_registry, G_TYPE_OBJECT)

static void
venture_entity_registry_finalize(GObject *object)
{
	VentureEntityRegistry *self;

	self = VENTURE_ENTITY_REGISTRY(object);

	g_clear_pointer(&self->types, g_hash_table_unref);
	g_clear_pointer(&self->prototypes, g_hash_table_unref);
	g_clear_pointer(&self->modules, g_hash_table_unref);
	g_clear_pointer(&self->hidden, g_hash_table_unref);

	G_OBJECT_CLASS(venture_entity_registry_parent_class)->finalize(object);
}

static void
venture_entity_registry_class_init(VentureEntityRegistryClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = venture_entity_registry_finalize;

	/**
	 * VentureEntityRegistry::type-registered:
	 * @self: the registry
	 * @entity_name: the name of the newly registered type
	 *
	 * Emitted when a record type is registered. The schema builder, the
	 * REST router and the AI tool registry all listen for this, which is
	 * how a plugin loaded after startup still gets its table created and
	 * its routes and tools published.
	 */
	venture_entity_registry_signals[SIGNAL_TYPE_REGISTERED] =
		g_signal_new("type-registered", G_TYPE_FROM_CLASS(klass),
		             G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		             G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
venture_entity_registry_init(VentureEntityRegistry *self)
{
	self->types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->modules = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      g_free);
	self->hidden = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->prototypes = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                         g_free, g_object_unref);
}

VentureEntityRegistry *
venture_entity_registry_new(void)
{
	return g_object_new(VENTURE_TYPE_ENTITY_REGISTRY, NULL);
}

VentureEntityRegistry *
venture_entity_registry_get_default(void)
{
	static VentureEntityRegistry *instance = NULL;
	static gsize initialised = 0;

	if (g_once_init_enter(&initialised))
	{
		instance = venture_entity_registry_new();
		venture_entity_registry_register_builtins(instance);
		g_once_init_leave(&initialised, 1);
	}

	return instance;
}

/*
 * Finds a registered type by its singular or plural name, hidden or not.
 * @out_canonical receives the singular name as registered, which is the
 * key every other table here uses.
 *
 * Returns: %TRUE if the name is registered
 */
static gboolean
venture_entity_registry_find_any(
	VentureEntityRegistry	 *self,
	const gchar		 *entity_name,
	const gchar		**out_canonical,
	gpointer		 *out_value
){
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	if (g_hash_table_lookup_extended(self->types, entity_name, &key, &value))
	{
		if (NULL != out_canonical)
			*out_canonical = key;

		if (NULL != out_value)
			*out_value = value;

		return TRUE;
	}

	/* Accept the plural too. Callers arrive holding a REST path segment
	 * or a table name as often as a singular type name, and making each
	 * of them singularise first would just spread the same logic around. */
	g_hash_table_iter_init(&iter, self->types);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		g_autofree gchar *plural = NULL;

		plural = venture_pluralise(key);

		if (0 == g_strcmp0(plural, entity_name))
		{
			if (NULL != out_canonical)
				*out_canonical = key;

			if (NULL != out_value)
				*out_value = value;

			return TRUE;
		}
	}

	return FALSE;
}

/*
 * As venture_entity_registry_find_any(), but a hidden type is not found.
 */
static gboolean
venture_entity_registry_find(
	VentureEntityRegistry	 *self,
	const gchar		 *entity_name,
	gpointer		 *out_value
){
	const gchar *canonical;

	canonical = NULL;

	if (!venture_entity_registry_find_any(self, entity_name, &canonical,
	                                      out_value))
		return FALSE;

	return !g_hash_table_contains(self->hidden, canonical);
}

gboolean
venture_entity_registry_register(
	VentureEntityRegistry	 *self,
	GType			  entity_type,
	GError			**error
){
	g_autoptr(VentureEntity) prototype = NULL;
	const gchar *entity_name;

	g_return_val_if_fail(VENTURE_IS_ENTITY_REGISTRY(self), FALSE);

	if (!g_type_is_a(entity_type, VENTURE_TYPE_ENTITY))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%s does not derive from VentureEntity",
		            g_type_name(entity_type));
		return FALSE;
	}

	if (G_TYPE_IS_ABSTRACT(entity_type))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%s is abstract and cannot be registered",
		            g_type_name(entity_type));
		return FALSE;
	}

	/* The prototype is created first because the entity name is a virtual
	 * method: the type itself is the authority on what it is called. */
	prototype = g_object_new(entity_type, NULL);
	entity_name = venture_entity_get_entity_name(prototype);

	if (g_hash_table_contains(self->types, entity_name))
	{
		GType existing;

		existing = GPOINTER_TO_SIZE(g_hash_table_lookup(self->types,
		                                                entity_name));

		/* Re-registering the same type is harmless and happens when a
		 * plugin is reloaded. A different type claiming a taken name
		 * is not: half the system would route to one and half to the
		 * other, which is far worse than refusing. */
		if (existing == entity_type)
			return TRUE;

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "Entity name \"%s\" is already registered to %s; "
		            "%s cannot also claim it",
		            entity_name, g_type_name(existing),
		            g_type_name(entity_type));
		return FALSE;
	}

	g_hash_table_insert(self->types, g_strdup(entity_name),
	                    GSIZE_TO_POINTER(entity_type));
	g_hash_table_insert(self->prototypes, g_strdup(entity_name),
	                    g_steal_pointer(&prototype));

	g_signal_emit(self, venture_entity_registry_signals[SIGNAL_TYPE_REGISTERED],
	              0, entity_name);

	return TRUE;
}

GType
venture_entity_registry_lookup(
	VentureEntityRegistry	*self,
	const gchar		*entity_name
){
	gpointer value;

	g_return_val_if_fail(VENTURE_IS_ENTITY_REGISTRY(self), G_TYPE_INVALID);
	g_return_val_if_fail(NULL != entity_name, G_TYPE_INVALID);

	value = NULL;

	if (!venture_entity_registry_find(self, entity_name, &value))
		return G_TYPE_INVALID;

	return (GType)GPOINTER_TO_SIZE(value);
}

GType
venture_entity_registry_lookup_any(
	VentureEntityRegistry	*self,
	const gchar		*entity_name
){
	gpointer value;

	g_return_val_if_fail(VENTURE_IS_ENTITY_REGISTRY(self), G_TYPE_INVALID);
	g_return_val_if_fail(NULL != entity_name, G_TYPE_INVALID);

	value = NULL;

	if (!venture_entity_registry_find_any(self, entity_name, NULL, &value))
		return G_TYPE_INVALID;

	return (GType)GPOINTER_TO_SIZE(value);
}

gboolean
venture_entity_registry_is_type_enabled(
	VentureEntityRegistry	*self,
	const gchar		*entity_name
){
	const gchar *canonical;

	g_return_val_if_fail(VENTURE_IS_ENTITY_REGISTRY(self), FALSE);
	g_return_val_if_fail(NULL != entity_name, FALSE);

	canonical = NULL;

	if (!venture_entity_registry_find_any(self, entity_name, &canonical, NULL))
		return FALSE;

	return !g_hash_table_contains(self->hidden, canonical);
}

const gchar *
venture_entity_registry_get_type_module(
	VentureEntityRegistry	*self,
	const gchar		*entity_name
){
	const gchar *canonical;

	g_return_val_if_fail(VENTURE_IS_ENTITY_REGISTRY(self), NULL);
	g_return_val_if_fail(NULL != entity_name, NULL);

	canonical = NULL;

	if (!venture_entity_registry_find_any(self, entity_name, &canonical, NULL))
		return NULL;

	return g_hash_table_lookup(self->modules, canonical);
}

void
venture_entity_registry_set_type_module(
	VentureEntityRegistry	*self,
	const gchar		*entity_name,
	const gchar		*module_name,
	gboolean		 enabled
){
	const gchar *canonical;

	g_return_if_fail(VENTURE_IS_ENTITY_REGISTRY(self));
	g_return_if_fail(NULL != entity_name);

	canonical = NULL;

	/* A module may claim a type that was never registered -- a build
	 * without it, or a test registry -- and that is not an error, just
	 * nothing to mask. */
	if (!venture_entity_registry_find_any(self, entity_name, &canonical, NULL))
		return;

	if (NULL != module_name)
		g_hash_table_insert(self->modules, g_strdup(canonical),
		                    g_strdup(module_name));
	else
		g_hash_table_remove(self->modules, canonical);

	if (enabled)
		g_hash_table_remove(self->hidden, canonical);
	else
		g_hash_table_add(self->hidden, g_strdup(canonical));
}

void
venture_entity_registry_set_unknown_type_error(
	VentureEntityRegistry	 *self,
	const gchar		 *entity_name,
	GError			**error
){
	const gchar *module_name;

	g_return_if_fail(VENTURE_IS_ENTITY_REGISTRY(self));

	if (NULL == entity_name)
		entity_name = "";

	/*
	 * Registered but hidden is a different message from never
	 * registered: the first is a switch the operator can flip, the
	 * second is a typo or a plugin that is not loaded. Telling them
	 * apart is the difference between a two-second fix and a hunt.
	 */
	if ((G_TYPE_INVALID != venture_entity_registry_lookup_any(self, entity_name)) &&
	    !venture_entity_registry_is_type_enabled(self, entity_name))
	{
		module_name = venture_entity_registry_get_type_module(self,
		                                                      entity_name);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "The record type \"%s\" belongs to the %s module, "
		            "which is disabled (modules.%s.enabled)",
		            entity_name,
		            (NULL != module_name) ? module_name : "?",
		            (NULL != module_name) ? module_name : "?");
		return;
	}

	{
		g_auto(GStrv) known = NULL;
		g_autofree gchar *list = NULL;

		/* Listing what is available turns a dead end into a usable
		 * message -- which matters most for the AI, since this error
		 * is fed straight back to it as a tool result. */
		known = venture_entity_registry_list_names(self);
		list = g_strjoinv(", ", known);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no record type called \"%s\". Known types: %s",
		            entity_name, list);
	}
}

VentureEntity *
venture_entity_registry_create(
	VentureEntityRegistry	 *self,
	const gchar		 *entity_name,
	GError			**error
){
	GType entity_type;

	g_return_val_if_fail(VENTURE_IS_ENTITY_REGISTRY(self), NULL);

	entity_type = venture_entity_registry_lookup(self, entity_name);

	if (G_TYPE_INVALID == entity_type)
	{
		venture_entity_registry_set_unknown_type_error(self, entity_name,
		                                               error);
		return NULL;
	}

	return g_object_new(entity_type, NULL);
}

static gchar **
list_names(VentureEntityRegistry *self, gboolean include_hidden)
{
	g_autoptr(GPtrArray) names = NULL;
	g_autoptr(GList) keys = NULL;
	GList *iter;

	g_return_val_if_fail(VENTURE_IS_ENTITY_REGISTRY(self), NULL);

	keys = g_hash_table_get_keys(self->types);
	keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
	names = g_ptr_array_new();

	for (iter = keys; NULL != iter; iter = iter->next)
	{
		/* A hidden type is not offered anywhere: not as a REST
		 * resource, a schema entry, a form, an AI tool argument or a
		 * CLI subcommand. This one loop is what makes that true. */
		if (!include_hidden && g_hash_table_contains(self->hidden, iter->data))
			continue;

		g_ptr_array_add(names, g_strdup(iter->data));
	}

	g_ptr_array_add(names, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&names), FALSE);
}

gchar **
venture_entity_registry_list_names(VentureEntityRegistry *self)
{
	return list_names(self, FALSE);
}

gchar **
venture_entity_registry_list_all_names(VentureEntityRegistry *self)
{
	return list_names(self, TRUE);
}

GType *
venture_entity_registry_list_types(
	VentureEntityRegistry	*self,
	guint			*n_types
){
	g_auto(GStrv) names = NULL;
	GType *types;
	guint count;
	guint i;

	g_return_val_if_fail(VENTURE_IS_ENTITY_REGISTRY(self), NULL);
	g_return_val_if_fail(NULL != n_types, NULL);

	names = venture_entity_registry_list_names(self);
	count = g_strv_length(names);
	types = g_new0(GType, count + 1);

	for (i = 0; i < count; i++)
		types[i] = venture_entity_registry_lookup(self, names[i]);

	*n_types = count;

	return types;
}

VentureEntity *
venture_entity_registry_get_prototype(
	VentureEntityRegistry	*self,
	const gchar		*entity_name
){
	GType entity_type;
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	g_return_val_if_fail(VENTURE_IS_ENTITY_REGISTRY(self), NULL);
	g_return_val_if_fail(NULL != entity_name, NULL);

	entity_type = venture_entity_registry_lookup(self, entity_name);

	if (G_TYPE_INVALID == entity_type)
		return NULL;

	/* Resolve through the type rather than the name, so a plural name
	 * finds the same prototype the singular one does. */
	g_hash_table_iter_init(&iter, self->prototypes);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		if (G_OBJECT_TYPE(value) == entity_type)
			return value;
	}

	return NULL;
}

const gchar *
venture_entity_registry_get_table_name(
	VentureEntityRegistry	*self,
	const gchar		*entity_name
){
	VentureEntity *prototype;

	prototype = venture_entity_registry_get_prototype(self, entity_name);

	if (NULL == prototype)
		return NULL;

	return venture_entity_get_table_name(prototype);
}

JsonNode *
venture_entity_registry_describe(
	VentureEntityRegistry	*self,
	const gchar		*entity_name
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	VentureEntity *prototype;
	guint i;

	g_return_val_if_fail(VENTURE_IS_ENTITY_REGISTRY(self), NULL);

	prototype = venture_entity_registry_get_prototype(self, entity_name);

	if (NULL == prototype)
		return NULL;

	specs = venture_entity_get_field_specs(prototype);
	g_ptr_array_sort_values(specs, venture_field_spec_compare_display_order);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "name");
	json_builder_add_string_value(builder,
		venture_entity_get_entity_name(prototype));

	json_builder_set_member_name(builder, "plural");
	{
		g_autofree gchar *plural = NULL;

		plural = venture_pluralise(venture_entity_get_entity_name(prototype));
		json_builder_add_string_value(builder, plural);
	}

	json_builder_set_member_name(builder, "table");
	json_builder_add_string_value(builder,
		venture_entity_get_table_name(prototype));

	json_builder_set_member_name(builder, "federation_access");
	json_builder_add_int_value(builder, venture_entity_type_get_federation_access(G_OBJECT_TYPE(prototype)));

	json_builder_set_member_name(builder, "fields");
	json_builder_begin_array(builder);

	for (i = 0; i < specs->len; i++)
	{
		json_builder_add_value(builder,
			venture_field_spec_to_json(g_ptr_array_index(specs, i)));
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

JsonNode *
venture_entity_registry_describe_all(VentureEntityRegistry *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_auto(GStrv) names = NULL;
	gsize i;

	g_return_val_if_fail(VENTURE_IS_ENTITY_REGISTRY(self), NULL);

	names = venture_entity_registry_list_names(self);

	builder = json_builder_new();
	json_builder_begin_array(builder);

	for (i = 0; NULL != names[i]; i++)
	{
		g_autoptr(JsonNode) description = NULL;

		description = venture_entity_registry_describe(self, names[i]);

		if (NULL != description)
			json_builder_add_value(builder, g_steal_pointer(&description));
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}

void
venture_entity_registry_register_builtins(VentureEntityRegistry *self)
{
	/*
	 * The order matters only for readability; registration is by name and
	 * the schema builder resolves foreign keys after every type is known,
	 * so a type may reference one that has not been registered yet.
	 */
	static GType (*const builtins[]) (void) = {
		venture_organization_get_type,
		venture_venture_get_type,

		venture_product_get_type,
		venture_inventory_item_get_type,
		venture_inventory_txn_get_type,

		venture_sale_get_type,

		venture_expense_get_type,
		venture_account_get_type,
		venture_ledger_entry_get_type,
		venture_journal_get_type,
		venture_journal_line_get_type,
		venture_exchange_rate_get_type,
		venture_tax_category_get_type,
		venture_tax_code_get_type,

		venture_contact_get_type,
		venture_interaction_get_type,
		venture_deal_get_type,

		venture_campaign_get_type,
		venture_newsletter_get_type,
		venture_subscriber_get_type,
		venture_post_get_type,

		venture_idea_get_type,
		venture_research_note_get_type,

		venture_knowledge_base_get_type,
		venture_kb_article_get_type,
		venture_kb_chunk_get_type,
		venture_kb_link_get_type,

		venture_ticket_get_type,
		venture_ticket_comment_get_type,
		venture_ticket_relation_get_type,
		venture_company_get_type,
		venture_document_get_type,
		venture_record_link_get_type,

		venture_chat_thread_get_type,
		venture_chat_message_get_type,
		venture_ai_skill_get_type,

		venture_forge_get_type,
		venture_forge_repo_get_type,
		venture_forge_rule_get_type,
		venture_ticket_link_get_type,
		venture_agent_session_get_type,
		venture_agent_turn_get_type,
		venture_forge_run_get_type,

		venture_milestone_get_type,
		venture_release_get_type,
		venture_build_get_type,
		venture_environment_get_type,
		venture_deployment_get_type,
		venture_incident_get_type,

		venture_invoice_get_type,
		venture_invoice_line_get_type,
		venture_stripe_price_link_get_type,
		venture_stripe_customer_link_get_type,
		venture_stripe_checkout_get_type,
		venture_stripe_event_get_type,
		venture_processor_payout_get_type,
		venture_processor_payout_item_get_type,
		venture_processor_dispute_get_type,
		venture_processor_exception_get_type,
		venture_payment_get_type,
		venture_payment_allocation_get_type,
		venture_customer_credit_get_type,
		venture_refund_get_type,
		venture_invoice_event_get_type,

		venture_dashboard_get_type,
		venture_dashboard_widget_get_type,

		venture_saved_view_get_type,
		venture_watch_get_type,
		venture_notification_get_type,
		venture_sla_policy_get_type,
		venture_macro_get_type,
		venture_worklog_get_type,
		venture_sprint_get_type,
		venture_agent_budget_get_type,

		venture_federation_replica_get_type,
		venture_federation_peer_get_type,
		venture_federation_grant_get_type,
		venture_webhook_get_type,
		venture_webhook_delivery_get_type,
		venture_routing_rule_get_type,

		venture_plugin_config_get_type,

		venture_user_get_type,
		venture_api_token_get_type,
		venture_audit_entry_get_type
		, venture_fiscal_year_get_type
		, venture_fiscal_period_get_type
		, venture_report_snapshot_get_type
		, venture_fixed_asset_get_type
		, venture_depreciation_entry_get_type
		, venture_deferral_get_type
		, venture_deferral_entry_get_type
		, venture_organization_membership_get_type
		, venture_team_get_type
		, venture_team_membership_get_type
		, venture_plan_get_type
		, venture_plan_price_get_type
		, venture_customer_subscription_get_type
		, venture_subscription_event_get_type
		, venture_dunning_step_get_type
		, venture_billing_notice_get_type
		, venture_billing_request_get_type
		, venture_customer_payment_method_get_type
		, venture_client_project_get_type
		, venture_project_rate_get_type
		, venture_project_time_get_type
		, venture_project_cost_get_type
		, venture_project_billing_get_type
		, venture_mail_message_get_type
		, venture_mail_template_get_type

		, venture_price_list_get_type
		, venture_price_list_item_get_type
		, venture_quote_get_type
		, venture_quote_line_get_type
		, venture_quote_event_get_type
		, venture_quote_delivery_get_type
		, venture_quote_action_get_type
		, venture_lead_get_type
		, venture_lead_form_get_type
		, venture_lead_assignment_rule_get_type
		, venture_activity_get_type
		, venture_activity_type_get_type
		, venture_vendor_bill_get_type
		, venture_vendor_bill_line_get_type
		, venture_bill_payment_get_type
		, venture_bill_payment_allocation_get_type
		, venture_vendor_credit_get_type
		, venture_vendor_bill_event_get_type
		, venture_bill_refund_get_type
		, venture_bank_account_get_type
		, venture_bank_statement_get_type
		, venture_bank_transaction_get_type
		, venture_bank_match_get_type
		, venture_reconciliation_get_type
		, venture_bank_rule_get_type
		, venture_bank_transfer_get_type
		, venture_bank_connection_get_type

		, venture_sequence_get_type
		, venture_sequence_step_get_type
		, venture_sequence_enrollment_get_type
		, venture_sequence_delivery_get_type
		, venture_suppression_get_type
		, venture_posting_profile_get_type
		, venture_accounting_cutover_get_type
		, venture_accounting_cutover_row_get_type
		, venture_accounting_setup_get_type
		, venture_accounting_control_map_get_type
		, venture_progress_billing_get_type
		, venture_customer_retainer_get_type
		, venture_contract_retention_get_type
		, venture_customer_portal_access_get_type
		, venture_supplier_portal_access_get_type
		, venture_accounting_custom_field_get_type
		, venture_accounting_layout_get_type
		, venture_custom_field_value_get_type
		, venture_saved_report_get_type
		, venture_report_pack_get_type
		, venture_accounting_dimension_get_type
		, venture_accounting_approval_rule_get_type
		, venture_accounting_approval_get_type
		, venture_accounting_backup_get_type
		, venture_pipeline_get_type,
		venture_pipeline_stage_get_type,
		venture_deal_stage_entry_get_type,
		venture_loss_reason_get_type,
		venture_recurring_schedule_get_type,
		venture_recurring_occurrence_get_type,
		venture_collection_policy_get_type,
		venture_collection_step_get_type,
		venture_collection_case_get_type,
		venture_collection_notice_get_type,
		venture_financial_batch_get_type,
		venture_close_workspace_get_type,
		venture_close_task_get_type,
		venture_close_workpaper_get_type,
		venture_close_discrepancy_get_type,
		venture_close_signoff_get_type,
		venture_capture_item_get_type,
		venture_tax_filing_get_type,
		venture_contractor_tax_form_get_type,
		venture_contractor_tax_pack_get_type,
		venture_expense_claim_get_type,
		venture_expense_claim_line_get_type,
		venture_payroll_run_get_type,
		venture_payroll_line_get_type,
		venture_purchase_order_get_type,
		venture_purchase_order_line_get_type,
		venture_goods_receipt_get_type,
		venture_goods_receipt_line_get_type,
		venture_inventory_cost_layer_get_type,
		venture_sales_order_get_type,
		venture_sales_order_line_get_type,
		venture_fulfillment_get_type,
		venture_budget_get_type,
		venture_budget_line_get_type,
		venture_equity_transaction_get_type,
		venture_intercompany_link_get_type,
		venture_elimination_get_type,
		venture_tax_depreciation_entry_get_type,
	};
	gsize i;

	g_return_if_fail(VENTURE_IS_ENTITY_REGISTRY(self));

	venture_period_records_register_constraints();

	for (i = 0; i < G_N_ELEMENTS(builtins); i++)
	{
		g_autoptr(GError) local_error = NULL;

		if (!venture_entity_registry_register(self, builtins[i](),
		                                      &local_error))
		{
			/* A built-in failing to register is a programming error
			 * -- two types claiming one name -- and every later
			 * lookup would be subtly wrong, so fail loudly now. */
			g_error("Cannot register built-in record type: %s",
			        local_error->message);
		}
	}
}
