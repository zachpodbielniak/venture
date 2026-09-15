/*
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef VENTURE_POSTING_RULE_H
#define VENTURE_POSTING_RULE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

#define VENTURE_TYPE_POSTING_RULE (venture_posting_rule_get_type())
G_DECLARE_INTERFACE(VenturePostingRule, venture_posting_rule, VENTURE, POSTING_RULE, GObject)

/**
 * VenturePostingRuleInterface:
 * @parent_iface: parent interface
 * @get_name: stable rule key, usually a source record type
 * @build_lines: build unsaved journal lines; account selection belongs here
 */
struct _VenturePostingRuleInterface
{
	GTypeInterface parent_iface;
	const gchar *(*get_name)(VenturePostingRule *self);
	GPtrArray *(*build_lines)(VenturePostingRule *self, VentureDatabase *database,
		VentureEntity *source, GError **error);
};

/**
 * venture_posting_rule_get_name:
 * @self: a posting rule
 * Returns: (transfer none): its registry key
 */
const gchar *venture_posting_rule_get_name(VenturePostingRule *self);
/**
 * venture_posting_rule_build_lines:
 * @self: a posting rule
 * @database: source database
 * @source: the saved source document
 * @error: (out) (optional): error location
 * Returns: (transfer full) (element-type VentureJournalLine) (nullable): unsaved lines
 */
GPtrArray *venture_posting_rule_build_lines(VenturePostingRule *self,
	VentureDatabase *database, VentureEntity *source, GError **error);

#define VENTURE_TYPE_POSTING_RULE_REGISTRY (venture_posting_rule_registry_get_type())
G_DECLARE_FINAL_TYPE(VenturePostingRuleRegistry, venture_posting_rule_registry,
	VENTURE, POSTING_RULE_REGISTRY, GObject)

/**
 * venture_posting_rule_registry_new:
 * Returns: (transfer full): an empty registry
 */
VenturePostingRuleRegistry *venture_posting_rule_registry_new(void);
/**
 * venture_posting_rule_registry_add:
 * @self: the registry
 * @rule: (transfer full): implementation, replacing an existing key
 */
void venture_posting_rule_registry_add(VenturePostingRuleRegistry *self, VenturePostingRule *rule);
/**
 * venture_posting_rule_registry_lookup:
 * @self: the registry
 * @name: rule key
 * Returns: (transfer none) (nullable): the implementation
 */
VenturePostingRule *venture_posting_rule_registry_lookup(VenturePostingRuleRegistry *self,
	const gchar *name);
/**
 * venture_posting_rule_registry_remove:
 * @self: the registry
 * @name: rule key
 * Returns: whether an implementation was removed
 */
gboolean venture_posting_rule_registry_remove(VenturePostingRuleRegistry *self, const gchar *name);
/**
 * venture_posting_rule_registry_list:
 * @self: the registry
 * Returns: (transfer container) (element-type VenturePostingRule): sorted implementations
 */
GPtrArray *venture_posting_rule_registry_list(VenturePostingRuleRegistry *self);

/**
 * venture_posting_rule_registry_get_revision:
 * @self: the registry
 *
 * The revision changes on construction, addition/replacement and successful
 * removal, invalidating consent across process restarts or callback changes.
 * Registered implementations must be replaced when their behavior changes.
 *
 * Returns: (transfer none): opaque revision, valid until the next mutation
 */
const gchar *venture_posting_rule_registry_get_revision(VenturePostingRuleRegistry *self);

#define VENTURE_TYPE_EXCHANGE_POLICY (venture_exchange_policy_get_type())
G_DECLARE_INTERFACE(VentureExchangePolicy, venture_exchange_policy, VENTURE, EXCHANGE_POLICY, GObject)
/**
 * VentureExchangePolicyInterface:
 * @parent_iface: parent interface
 * @get_name: immutable policy/rate-set identifier recorded on the journal
 * @convert: value an amount in the journal's book currency using exact money
 */
struct _VentureExchangePolicyInterface
{
	GTypeInterface parent_iface;
	const gchar *(*get_name)(VentureExchangePolicy *self);
	VentureMoney *(*convert)(VentureExchangePolicy *self, const VentureMoney *amount,
		const gchar *currency, GDateTime *when, GError **error);
};
/**
 * venture_exchange_policy_get_name:
 * @self: the policy
 * Returns: (transfer none): policy identifier
 */
const gchar *venture_exchange_policy_get_name(VentureExchangePolicy *self);
/**
 * venture_exchange_policy_convert:
 * @self: the policy
 * @amount: original amount
 * @currency: required book currency
 * @when: accounting date
 * @error: (out) (optional): error location
 * Returns: (transfer full) (nullable): valuation in the requested currency
 */
VentureMoney *venture_exchange_policy_convert(VentureExchangePolicy *self,
	const VentureMoney *amount, const gchar *currency, GDateTime *when, GError **error);

typedef struct _VentureDatabase VentureDatabase;

#define VENTURE_TYPE_RATE_TABLE_POLICY (venture_rate_table_policy_get_type())
G_DECLARE_FINAL_TYPE(VentureRateTablePolicy, venture_rate_table_policy,
	VENTURE, RATE_TABLE_POLICY, GObject)

/**
 * venture_rate_table_policy_new:
 * @database: organization-scoped rate table
 * @organization_id: legal entity
 *
 * Looks up dated exchange_rate rows. Missing pairs are refused; no inverse
 * or market rate is invented.
 * Returns: (transfer full): a policy named "exchange_rate"
 */
VentureExchangePolicy *venture_rate_table_policy_new(VentureDatabase *database,
	gint64 organization_id);

/**
 * venture_rate_table_policy_matches:
 * @self: a stored-rate policy
 * @database: the database covered by operation consent
 * @organization_id: the legal entity covered by operation consent
 *
 * Returns: whether conversion reads the exact table scope bound to consent
 */
gboolean venture_rate_table_policy_matches(VentureRateTablePolicy *self,
	VentureDatabase *database, gint64 organization_id);

G_END_DECLS
#endif
