/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl territory_fields[] = {
	VENTURE_FIELD_NAME("name", "Territory", "Organization-owned sales territory"),
	VENTURE_FIELD_TEXT("description", "Description", "Routing conditions belong to the linked lead routing rules"),
	VENTURE_FIELD_REF("team-id", "Owning team", "Team for future assignments; retained assignments do not change", "team", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("active", "Active", "Inactive territories are skipped by routing", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureSalesTerritory, venture_sales_territory, territory_fields)
static const VentureFieldDecl quota_fields[] = {
	VENTURE_FIELD_NAME("name", "Quota", "Independent period target; overlapping windows are not summed"),
	VENTURE_FIELD("quota-key", "Quota key", "Unique recipient, metric, currency and complete period", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE),
	VENTURE_FIELD_REF("owner-user-id", "Representative", "Choose exactly one representative or team", "user", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("team-id", "Team", "Choose exactly one representative or team", "team", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("metric", "Metric", "booked_revenue: captured won sales, not ledger revenue or cash", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("target", "Target", "Positive exact amount and explicit currency"),
	VENTURE_FIELD("starts-at", "Starts", "Inclusive midnight UTC", VENTURE_FIELD_KIND_DATE, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("ends-at", "Ends", "Exclusive midnight UTC", VENTURE_FIELD_KIND_DATE, VENTURE_COLUMN_FLAG_NOT_NULL)
};
VENTURE_DEFINE_ENTITY(VentureSalesQuota, venture_sales_quota, quota_fields)
/* Snapshot identities are integers rather than live foreign references:
 * deactivating or removing a representative must not erase historical credit. */
static const VentureFieldDecl credit_fields[] = {
	VENTURE_FIELD_NAME("name", "Credit", "Immutable booking or dated cancellation reversal"),
	VENTURE_FIELD("kind", "Kind", "booking or reversal", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("rep-name", "Credited representative", "Username captured when won", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("value", "Credited amount", "Signed amount; absent means unvalued, never an inferred currency"),
	VENTURE_FIELD("credited-at", "Credited at", "Booking close or cancellation time; periods are half-open UTC", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("team-name", "Credited team", "Name captured when won", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("territory-name", "Territory", "Name captured when won", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("deal-id", "Deal", "Business record whose transition created this credit", "deal", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("metric", "Metric", "booked_revenue", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("owner-user-id", "Credited representative ID", "Identity captured when won", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("team-id", "Credited team ID", "Identity captured when won", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("territory-id", "Territory ID", "Identity captured when won", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reverses-id", "Reversed credit ID", "Original positive booking; zero for a booking", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("credit-key", "Event identity", "Exactly one event per deal revision and kind", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE)
};
VENTURE_DEFINE_ENTITY(VentureSalesCredit, venture_sales_credit, credit_fields)
static const VentureFieldDecl assignment_fields[] = {
	VENTURE_FIELD_NAME("name", "Assignment", "Immutable ownership transition"),
	VENTURE_FIELD("owner", "New owner", "Retained username", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("previous-owner", "Previous owner", "Retained username", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("assigned-at", "Assigned at", "Observed transition time", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("lead-id", "Lead", "Exactly one lead or deal source", "lead", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("deal-id", "Deal", "Exactly one lead or deal source", "deal", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("team-id", "New team ID", "Retained identity", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("previous-team-id", "Previous team ID", "Retained identity", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("territory-id", "New territory ID", "Retained identity", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("previous-territory-id", "Previous territory ID", "Retained identity", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("owner-user-id", "New owner ID", "Retained identity", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("previous-owner-user-id", "Previous owner ID", "Retained identity", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("assignment-key", "Event identity", "Exactly one assignment per source revision", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE)
};
VENTURE_DEFINE_ENTITY(VentureSalesAssignment, venture_sales_assignment, assignment_fields)
