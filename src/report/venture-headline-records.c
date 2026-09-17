/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

/* One row per organisation: how the headline numbers are tuned and whether
 * the five cards are the home page. Absent means every default below. */
static const VentureFieldDecl headline_setting_fields[] = {
	VENTURE_FIELD("classic-home", "Classic home page",
		"Show the old dashboard at / instead of the five headline cards",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("hourly-rate", "Worklog hourly rate",
		"Cost of one logged support hour, attributed to the ticket's customer in gross margin"),
	VENTURE_FIELD("minimum-customer-months", "Minimum customer-months",
		"Projected LTV is withheld below this many customer-months of paid revenue; zero means 12",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("activity-days", "Activity churn days",
		"Days without paid revenue before a customer counts as churned; zero means 90",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureHeadlineSetting, venture_headline_setting, headline_setting_fields)
