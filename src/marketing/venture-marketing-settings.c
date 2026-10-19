/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-marketing-settings.c - Campaign settings (docs/marketing.org,
 * docs/deliverability.org, docs/settings.org)
 *
 * The warm-up stays on the organization's first sending domain, where the
 * deliverability page has always kept it. The send defaults are fields on
 * the organization that a new send starts from; each send keeps its own
 * values once made.
 */
#include "venture.h"

static VentureEntity *first_domain(VentureContext *context, gint64 organization_id, gint64 member, gboolean for_write,
	GError **error)
{
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_SENDING_DOMAIN);
	VentureEntity *domain;
	(void)member;
	venture_query_set_organization(query, organization_id);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	domain = venture_database_find_one(database, query, error);
	if (domain == NULL && for_write && (error == NULL || *error == NULL))
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"The warm-up is kept on your sending domain: add it on the Deliverability page first. Until then the default schedule applies.");
	return domain;
}

static gboolean schedule(const gchar *value, GError **error)
{
	return venture_deliverability_ramp_limit(value, 1, error) >= 0;
}

static const VentureSettingInfo campaigns[] = {
	{ "marketing.warm-up", "Campaigns", "marketing", "Warm-up daily limits",
	  "How many campaign messages may go each day while a new sending domain builds trust: day:limit pairs from day 1, "
	  "0 for no limit, for example 1:50,3:100,30:0; or off for no warm-up.",
	  VENTURE_SETTING_TEXT, VENTURE_DELIVERABILITY_DEFAULT_RAMP, 0, 0, NULL, VENTURE_SETTING_PERMISSION_ADMIN,
	  "marketing_sending_domain (the first): ramp-schedule", "ramp-schedule", "", first_domain, NULL, schedule },
	{ "marketing.local-hour", "Campaigns", "marketing", "Local-time hour for new sends",
	  "The hour (0 to 23, in each recipient's own time) a new local-time send starts at. Change one send's hour on the send.",
	  VENTURE_SETTING_INTEGER, "9", 0, 23, NULL, VENTURE_SETTING_PERMISSION_ADMIN,
	  "organization: marketing-local-hour", "marketing-local-hour", "", NULL, NULL, NULL },
	{ "marketing.ab-wait-hours", "Campaigns", "marketing", "A/B test wait",
	  "Hours after the test messages go before the winner is picked and sent to everyone else, for a send that sets none.",
	  VENTURE_SETTING_INTEGER, "4", 1, 168, NULL, VENTURE_SETTING_PERMISSION_ADMIN,
	  "organization: marketing-ab-wait-hours", "marketing-ab-wait-hours", "0", NULL, NULL, NULL },
	{ "marketing.ab-metric", "Campaigns", "marketing", "A/B test winner by",
	  "What a new A/B test is won on: opens or clicks, by people, not machines.",
	  VENTURE_SETTING_CHOICE, "opens", 0, 0, "opens,clicks", VENTURE_SETTING_PERMISSION_ADMIN,
	  "organization: marketing-ab-metric", "marketing-ab-metric", "", NULL, NULL, NULL },
	{ "marketing.resend-days", "Campaigns", "marketing", "Resend to non-openers after",
	  "Days after the first message a resend to non-openers waits when it names none (0 to 365).",
	  VENTURE_SETTING_INTEGER, "3", 0, 365, NULL, VENTURE_SETTING_PERMISSION_ADMIN,
	  "organization: marketing-resend-days", "marketing-resend-days", "", NULL, NULL, NULL }
};

void venture_marketing_settings_register(void)
{
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(campaigns); i++)
		venture_settings_register(&campaigns[i]);
}
