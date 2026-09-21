/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ORGANIZATION_MAILER_H
#define VENTURE_ORGANIZATION_MAILER_H
#include "mail/venture-mailer.h"
G_BEGIN_DECLS
#define VENTURE_TYPE_ORGANIZATION_MAILER (venture_organization_mailer_get_type())
G_DECLARE_FINAL_TYPE(VentureOrganizationMailer, venture_organization_mailer, VENTURE, ORGANIZATION_MAILER, GObject)
/**
 * venture_organization_mailer_new:
 * @database: business repository, weakly held
 * @config: live operator policy, retained
 *
 * Selects encrypted SMTP settings using the message's business organization
 * on the construction thread. Never falls back to installation credentials.
 * Each preparation reads current settings; there is no credential cache.
 *
 * Returns: (transfer full): organization-aware selector
 */
VentureOrganizationMailer *venture_organization_mailer_new(VentureDatabase *database, VentureConfig *config);
/**
 * venture_organization_mailer_configure:
 * @self: organization selector
 * @organization_id: verified business organization
 * @values: write-only flat SMTP settings, copied into encrypted storage
 * @expected_version: zero for a new binding; current version for rotation
 * @expected_connection: zero for a new binding; exact displayed identity for rotation
 * @actor: (nullable): audit actor; authority comes from the access scope
 * @error: (out) (optional): redacted refusal
 *
 * Requires an operator-allowed endpoint and TLS. Identity includes the host,
 * port, username and sender, so changing any requires explicit disconnect.
 * Password rotation preserves identity. No test message is sent here.
 *
 * Returns: (transfer full) (nullable): safe binding record
 */
VentureIntegrationConnection *venture_organization_mailer_configure(VentureOrganizationMailer *self,
	gint64 organization_id, JsonObject *values, gint64 expected_version, gint64 expected_connection,
	const VentureActor *actor, GError **error);
/**
 * venture_organization_mailer_dup_schema:
 *
 * Describes the write-only settings form and scalar types accepted by the
 * adapter. Password fields have x-sensitive=true; no configured values are
 * present. Defaults and validation use this same declaration.
 *
 * Returns: (transfer full): JSON Schema object for SMTP settings
 */
JsonNode *venture_organization_mailer_dup_schema(void);
/**
 * venture_organization_mailer_status:
 * @self: organization selector
 * @organization_id: verified organization
 * @error: (out) (optional): repository or authorization failure
 *
 * Describes safe account identity, local readiness and the latest delivery
 * using the current configuration version. Does no network I/O and returns
 * no credential values. A sent result means relay acceptance, not inbox delivery.
 *
 * Returns: (transfer full) (nullable): safe JSON status object
 */
JsonNode *venture_organization_mailer_status(VentureOrganizationMailer *self,
	gint64 organization_id, GError **error);
G_END_DECLS
#endif
