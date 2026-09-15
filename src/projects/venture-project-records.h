/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PROJECT_RECORDS_H
#define VENTURE_PROJECT_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_CLIENT_PROJECT (venture_client_project_get_type())
VENTURE_DECLARE_ENTITY(VentureClientProject, venture_client_project, CLIENT_PROJECT)
#define VENTURE_TYPE_PROJECT_RATE (venture_project_rate_get_type())
VENTURE_DECLARE_ENTITY(VentureProjectRate, venture_project_rate, PROJECT_RATE)
#define VENTURE_TYPE_PROJECT_TIME (venture_project_time_get_type())
VENTURE_DECLARE_ENTITY(VentureProjectTime, venture_project_time, PROJECT_TIME)
#define VENTURE_TYPE_PROJECT_COST (venture_project_cost_get_type())
VENTURE_DECLARE_ENTITY(VentureProjectCost, venture_project_cost, PROJECT_COST)
#define VENTURE_TYPE_PROJECT_BILLING (venture_project_billing_get_type())
VENTURE_DECLARE_ENTITY(VentureProjectBilling, venture_project_billing, PROJECT_BILLING)
G_END_DECLS
#endif
