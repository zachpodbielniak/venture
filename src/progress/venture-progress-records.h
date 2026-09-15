/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PROGRESS_RECORDS_H
#define VENTURE_PROGRESS_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_PROGRESS_BILLING (venture_progress_billing_get_type())
VENTURE_DECLARE_ENTITY(VentureProgressBilling, venture_progress_billing, PROGRESS_BILLING)
#define VENTURE_TYPE_CUSTOMER_RETAINER (venture_customer_retainer_get_type())
VENTURE_DECLARE_ENTITY(VentureCustomerRetainer, venture_customer_retainer, CUSTOMER_RETAINER)
#define VENTURE_TYPE_CONTRACT_RETENTION (venture_contract_retention_get_type())
VENTURE_DECLARE_ENTITY(VentureContractRetention, venture_contract_retention, CONTRACT_RETENTION)
G_END_DECLS
#endif
