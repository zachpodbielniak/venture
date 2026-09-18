/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_LEAD_SERVICE_H
#define VENTURE_LEAD_SERVICE_H
G_BEGIN_DECLS
typedef struct _VentureReportRegistry VentureReportRegistry;
typedef struct _VentureConfirmation VentureConfirmation;
typedef struct _VentureConfirmationStore VentureConfirmationStore;
#define VENTURE_TYPE_LEAD_SERVICE (venture_lead_service_get_type())
G_DECLARE_FINAL_TYPE(VentureLeadService, venture_lead_service, VENTURE, LEAD_SERVICE, GObject)
/**
 * venture_database_get_lead_service:
 * @database: the owning repository
 * Returns: (transfer none): its canonical lead service
 */
VentureLeadService *venture_database_get_lead_service(VentureDatabase *database);
/**
 * venture_lead_service_save_hook:
 * @self: the canonical service
 * @entity: the proposed record
 * @actor: (nullable): audit actor
 * @handled: (out): whether the service handled the save
 * @error: (out) (optional): error
 * Returns: whether the write may proceed or succeeded
 */
gboolean venture_lead_service_save_hook(VentureLeadService *self, VentureEntity *entity,
	const VentureActor *actor, gboolean *handled, GError **error);
/**
 * venture_lead_service_capture:
 * @self: the canonical service
 * @token: public form token
 * @fields: submitted fields in wire spelling
 * @redirect_url: (out) (optional) (nullable) (transfer full): configured redirect after a real capture
 * @error: (out) (optional): error
 * Returns: whether the submission was accepted (including a honeypot)
 */
gboolean venture_lead_service_capture(VentureLeadService *self, const gchar *token, JsonObject *fields, gchar **redirect_url, GError **error);
/**
 * venture_lead_service_convert:
 * @self: the canonical service
 * @lead: saved lead, carrying the expected version
 * @options: (nullable): deal boolean, company_id and contact_id links
 * @actor: (nullable): audit actor
 * @error: (out) (optional): error
 * Returns: (transfer full) (nullable): converted lead; inputs remain untouched
 */
VentureEntity *venture_lead_service_convert(VentureLeadService *self, VentureEntity *lead,
	JsonObject *options, const VentureActor *actor, GError **error);
/**
 * venture_lead_service_reassign:
 * @self: the canonical service
 * @lead: saved lead
 * @owner: (nullable): explicit owner, or NULL to run assignment rules
 * @actor: (nullable): audit actor
 * @error: (out) (optional): error
 * Returns: whether reassignment succeeded
 */
gboolean venture_lead_service_reassign(VentureLeadService *self, VentureEntity *lead,
	const gchar *owner, const VentureActor *actor, GError **error);
/**
 * venture_leads_register_reports:
 * @registry: the report registry
 * Registers source conversion, response time and recycled-due reports.
 */
void venture_leads_register_reports(VentureReportRegistry *registry);
/**
 * venture_lead_service_stage_convert:
 * @self: the canonical service
 * @store: confirmation queue
 * @lead: saved lead with expected version
 * @options: (nullable): conversion options
 * @actor: audit origin
 * @error: (out) (optional): error
 * Returns: (transfer none) (nullable): pending conversion, with no persisted changes
 */
VentureConfirmation *venture_lead_service_stage_convert(VentureLeadService *self,
	VentureConfirmationStore *store, VentureEntity *lead, JsonObject *options,
	const VentureActor *actor, GError **error);
/**
 * venture_lead_service_apply_staged:
 * @self: the canonical service
 * @staged: the proposal created by stage_convert
 * @actor: approving actor
 * @error: (out) (optional): error
 * Returns: whether the atomic conversion succeeded
 */
gboolean venture_lead_service_apply_staged(VentureLeadService *self, VentureEntity *staged,
	const VentureActor *actor, GError **error);
/**
 * venture_lead_normalize_email:
 * @value: (nullable): an address as typed
 *
 * The leads module's duplicate-detection normalisation for email: lower
 * case, trimmed, with a +tag removed. Exposed so inbound mail matches the
 * way lead deduplication does rather than reimplementing it.
 * Returns: (transfer full): the normalised address, possibly empty
 */
gchar *venture_lead_normalize_email(const gchar *value);
/**
 * venture_lead_normalize_website:
 * @value: (nullable): a website or bare domain as typed
 *
 * The leads module's duplicate-detection normalisation for a website:
 * the lower-case host without a leading www., or empty when the text is
 * not a host. Exposed so a CRM migration matches companies by domain the
 * way lead deduplication does rather than reimplementing it.
 * Returns: (transfer full): the normalised host, possibly empty
 */
gchar *venture_lead_normalize_website(const gchar *value);
G_END_DECLS
#endif
