/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PERIOD_RECORDS_H
#define VENTURE_PERIOD_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

/**
 * VenturePeriodState:
 * @VENTURE_PERIOD_OPEN: financial writes are permitted
 * @VENTURE_PERIOD_CLOSED: writes require an authorized reopening first
 * @VENTURE_PERIOD_LOCKED: final; reopening is forbidden
 *
 * The state of a fiscal year or period.
 */
typedef enum
{
	VENTURE_PERIOD_OPEN,
	VENTURE_PERIOD_CLOSED,
	VENTURE_PERIOD_LOCKED
} VenturePeriodState;

/**
 * venture_period_state_get_type:
 * Returns: the fiscal state enumeration
 */
GType venture_period_state_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_PERIOD_STATE (venture_period_state_get_type())

/**
 * VenturePeriodLength:
 * @VENTURE_PERIOD_MONTHLY: twelve calendar months
 * @VENTURE_PERIOD_QUARTERLY: four calendar quarters
 *
 * Calendar subdivisions measured from the year's start date.
 */
typedef enum
{
	VENTURE_PERIOD_MONTHLY,
	VENTURE_PERIOD_QUARTERLY
} VenturePeriodLength;

/**
 * venture_period_length_get_type:
 * Returns: the fiscal period length enumeration
 */
GType venture_period_length_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_PERIOD_LENGTH (venture_period_length_get_type())

#define VENTURE_TYPE_FISCAL_YEAR (venture_fiscal_year_get_type())
VENTURE_DECLARE_ENTITY(VentureFiscalYear, venture_fiscal_year, FISCAL_YEAR)
#define VENTURE_TYPE_FISCAL_PERIOD (venture_fiscal_period_get_type())
VENTURE_DECLARE_ENTITY(VentureFiscalPeriod, venture_fiscal_period, FISCAL_PERIOD)
#define VENTURE_TYPE_REPORT_SNAPSHOT (venture_report_snapshot_get_type())
VENTURE_DECLARE_ENTITY(VentureReportSnapshot, venture_report_snapshot, REPORT_SNAPSHOT)

/**
 * venture_period_records_register_constraints:
 *
 * Applies organization-scoped numbering metadata after builtin registration.
 * Account's field table remains owned by the journal module.
 */
void venture_period_records_register_constraints(void);

G_END_DECLS
#endif
