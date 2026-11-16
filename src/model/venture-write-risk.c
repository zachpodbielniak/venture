/*
 * venture-write-risk.c - Which writes may skip a person's approval
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

/*
 * People, access, credentials, wiring and the outward actions -- the high
 * risk that no field reveals. By record-type name, as /api/v1/:type and
 * the schema spell them. Money is not listed: it is derived (below).
 */
static const gchar *const high_risk_names[] = {
	/* people and access */
	"user", "organization", "organization_membership", "team",
	"team_membership", "api_token", "session", "oidc_identity",
	"user_mfa", "mfa_policy", "mfa_recovery_code",
	"customer_portal_access", "supplier_portal_access",
	"tenant_identity", "tenant_invitation", "tenant_membership",
	"tenant_signup", "tenant_support_grant", "tenant_workspace",
	"household_share",
	/* credentials and wiring */
	"forge", "forge_rule", "webhook", "integration_connection",
	"mail_account", "mail_oauth_provider", "calendar_account",
	"telephony_account", "bank_connection", "federation_grant",
	"federation_peer", "federation_replica", "ai_grant",
	"ai_configuration", "plugin_config", "backup_schedule",
	"accounting_backup", "accounting_setup", "posting_profile",
	"headline_setting", "routing_rule",
	/* outward and irreversible */
	"mail_message", "text_message", "quote_delivery", "marketing_send",
	"sequence_enrollment", "release", "deployment",
	NULL
};

/* Whether the field table holds an amount of money. */
static gboolean
has_money_field(GType type)
{
	g_autoptr(GTypeClass) klass = g_type_class_ref(type);
	g_autofree GParamSpec **properties = NULL;
	guint n = 0;
	guint i;

	properties = venture_entity_class_list_persistent_properties(
		VENTURE_ENTITY_CLASS(klass), &n);

	for (i = 0; i < n; i++)
	{
		if (VENTURE_TYPE_MONEY == properties[i]->value_type)
			return TRUE;
	}

	return FALSE;
}

VentureWriteRisk
venture_write_risk_for_type(GType type)
{
	g_autoptr(VentureEntity) prototype = NULL;
	const gchar *name;

	g_return_val_if_fail(g_type_is_a(type, VENTURE_TYPE_ENTITY),
	                     VENTURE_WRITE_RISK_HIGH);

	/* The access policy's own mark: finance and what requires it. */
	if (NULL != g_type_get_qdata(type,
	        g_quark_from_static_string("venture-access-financial")))
		return VENTURE_WRITE_RISK_HIGH;

	if (has_money_field(type))
		return VENTURE_WRITE_RISK_HIGH;

	prototype = g_object_new(type, NULL);
	name = venture_entity_get_entity_name(prototype);

	if (NULL != name && g_strv_contains(high_risk_names, name))
		return VENTURE_WRITE_RISK_HIGH;

	return VENTURE_WRITE_RISK_LOW;
}

VentureWriteRisk
venture_write_risk_for_write(GType type, const gchar *method)
{
	if (0 == g_ascii_strcasecmp(method != NULL ? method : "", "DELETE"))
		return VENTURE_WRITE_RISK_HIGH;

	return venture_write_risk_for_type(type);
}

const gchar *
venture_write_risk_nick(VentureWriteRisk risk)
{
	return (VENTURE_WRITE_RISK_LOW == risk) ? "low" : "high";
}
