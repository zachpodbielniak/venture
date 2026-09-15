/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ACCOUNTING_APPROVAL_H
#define VENTURE_ACCOUNTING_APPROVAL_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_ACCOUNTING_APPROVAL_RULE (venture_accounting_approval_rule_get_type())
VENTURE_DECLARE_ENTITY(VentureAccountingApprovalRule, venture_accounting_approval_rule, ACCOUNTING_APPROVAL_RULE)
#define VENTURE_TYPE_ACCOUNTING_APPROVAL (venture_accounting_approval_get_type())
VENTURE_DECLARE_ENTITY(VentureAccountingApproval, venture_accounting_approval, ACCOUNTING_APPROVAL)
G_END_DECLS
#endif
