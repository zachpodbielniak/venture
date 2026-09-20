/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl configuration_fields[] = {
	VENTURE_FIELD("purpose", "Purpose", "Independent chat, embedding or coding model and quota", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("title", "AI configuration", "Organization's explicit provider selection"),
	VENTURE_FIELD("configuration-key", "Configuration key", "One configuration per organization and purpose", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD("mode", "Source", "disabled, organization or platform; never a fallback", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("connection-id", "Own provider", "Organization-owned encrypted connection", "integration_connection", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("grant-id", "Platform grant", "Explicit grant to this organization", "ai_grant", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("monthly-requests", "Monthly requests", "Own connection request quota in a UTC calendar month", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("concurrency-limit", "Concurrent requests", "Own connection maximum unexpired reservations", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureAiConfiguration, venture_ai_configuration, configuration_fields)

static const VentureFieldDecl offer_fields[] = {
	VENTURE_FIELD("purpose", "Purpose", "Independent chat, embedding or coding model and quota", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("title", "Platform offer", "Safe service label, never credentials"),
	VENTURE_FIELD_REF("billing-organization-id", "Platform payer", "Explicit organization paying the provider bill", "organization", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("connection-id", "Platform provider", "Platform-managed encrypted connection", "integration_connection", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("enabled", "Enabled", "Disabling refuses every grant and pending result", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureAiPlatformOffer, venture_ai_platform_offer, offer_fields,
	g_type_set_qdata(G_TYPE_FROM_CLASS(klass), g_quark_from_static_string("venture-access-platform"), GINT_TO_POINTER(1));)

static const VentureFieldDecl grant_fields[] = {
	VENTURE_FIELD_NAME("title", "AI grant", "Explicit platform service eligibility"),
	VENTURE_FIELD("grant-key", "Grant key", "Stable organization/offer assignment", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD_REF("offer-id", "Platform offer", "Platform-owned service being granted", "ai_platform_offer", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("enabled", "Enabled", "Revocation refuses further requests and pending results", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("monthly-requests", "Monthly requests", "Request quota in a UTC calendar month", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("concurrency-limit", "Concurrent requests", "Maximum unexpired provider reservations", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureAiGrant, venture_ai_grant, grant_fields)

static const VentureFieldDecl period_fields[] = {
	VENTURE_FIELD_NAME("period-key", "Usage period", "Unique organization, purpose and UTC month; rotation never resets usage"),
	VENTURE_FIELD("unique-key", "Unique key", "Serialization key for durable quota reservations", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD("month", "UTC month", "Calendar month YYYY-MM", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("requests", "Requests reserved", "Includes failures and interrupted processes", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureAiUsagePeriod, venture_ai_usage_period, period_fields)

static const VentureFieldDecl usage_fields[] = {
	VENTURE_FIELD("purpose", "Purpose", "Independent chat, embedding or coding model and quota", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("title", "AI request", "Usage evidence without prompts or responses"),
	VENTURE_FIELD_REF("period-id", "Usage period", "Atomic monthly reservation", "ai_usage_period", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("connection-id", "Provider binding", "Exact organization or platform credential identity", "integration_connection", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("connection-version", "Provider revision", "Exact credentials used", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("grant-id", "Platform grant", "Zero for organization-owned service", "ai_grant", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("user-id", "Caller", "Authenticated local caller, or zero for trusted internal work", "user", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("payer-organization-id", "Payer", "Explicit organization responsible for provider cost", "organization", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("source", "Source", "organization or platform", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("state", "State", "reserved, completed, failed or revoked", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("lease-until", "Reservation expires", "Interrupted processes consume quota but release concurrency after this UTC time", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("input-tokens", "Input tokens", "Provider-reported usage; zero when unavailable", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("output-tokens", "Output tokens", "Provider-reported usage; zero when unavailable", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureAiUsage, venture_ai_usage, usage_fields)
