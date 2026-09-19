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
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("health-touch-days", "Health: days since touch",
		"Days without an interaction, completed activity or inbound mail before a customer is quiet; zero means 30",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("health-overdue-days", "Health: overdue days",
		"Days past due before an overdue invoice counts against a customer's health; zero means 15",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("health-open-tickets", "Health: open tickets",
		"Open tickets at which the queue counts against a customer's health; zero means 3",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	/* The support rollup's two rates, whole units of the book currency.
	 * Integers rather than money so a rate is one number to type; the
	 * currency is the organisation's. */
	VENTURE_FIELD("support-hourly-rate", "Support hourly rate",
		"Cost of one agent hour logged on tickets, in whole units of the book currency; zero means the worklog hourly rate above",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("support-ticket-rate", "Support per-ticket rate",
		"Flat cost of a ticket with no logged minutes, in whole units of the book currency; zero means such tickets cost nothing",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureHeadlineSetting, venture_headline_setting, headline_setting_fields)
