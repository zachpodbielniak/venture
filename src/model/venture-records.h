/*
 * venture-records.h - Every built-in record type
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The record types are declared together rather than one per header because
 * each is a field table and a name -- there is no per-type API to document
 * in isolation, since every operation on them is generic. Grouping them puts
 * the whole data model on one page, which is the view that actually helps
 * when you are deciding where a new piece of information belongs.
 *
 * The model is organised in layers:
 *
 *   Structure   Organization, Venture
 *   Catalogue   Product, InventoryItem, InventoryTxn
 *   Revenue     Sale
 *   Money       Expense, Account, LedgerEntry, TaxCategory
 *   Relations   Contact, Interaction, Deal
 *   Growth      Campaign, Newsletter, Subscriber, Post
 *   Thinking    Idea, ResearchNote, Task, Document
 *   Access      User, ApiToken, AuditEntry
 *
 * Every one derives from #VentureEntity and therefore carries the same
 * identity spine: id, uuid, organization, timestamps, version, soft deletion
 * and custom attributes.
 */

#ifndef VENTURE_RECORDS_H
#define VENTURE_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>

#include "model/venture-entity.h"
#include "model/venture-entity-macros.h"

G_BEGIN_DECLS

/* --- Structure ----------------------------------------------------------- */

#define VENTURE_TYPE_ORGANIZATION (venture_organization_get_type())
VENTURE_DECLARE_ENTITY(VentureOrganization, venture_organization, ORGANIZATION)

#define VENTURE_TYPE_VENTURE (venture_venture_get_type())
VENTURE_DECLARE_ENTITY(VentureVenture, venture_venture, VENTURE)

/* --- Catalogue ----------------------------------------------------------- */

#define VENTURE_TYPE_PRODUCT (venture_product_get_type())
VENTURE_DECLARE_ENTITY(VentureProduct, venture_product, PRODUCT)

#define VENTURE_TYPE_INVENTORY_ITEM (venture_inventory_item_get_type())
VENTURE_DECLARE_ENTITY(VentureInventoryItem, venture_inventory_item, INVENTORY_ITEM)

#define VENTURE_TYPE_INVENTORY_TXN (venture_inventory_txn_get_type())
VENTURE_DECLARE_ENTITY(VentureInventoryTxn, venture_inventory_txn, INVENTORY_TXN)

/* --- Revenue ------------------------------------------------------------- */

#define VENTURE_TYPE_SALE (venture_sale_get_type())
VENTURE_DECLARE_ENTITY(VentureSale, venture_sale, SALE)

/**
 * venture_sale_get_net:
 * @self: a #VentureSale
 * @error: (out) (optional): return location for a #GError
 *
 * Computes gross less fees, shipping cost, refunds and tax remitted. This is
 * what actually reaches the bank, and it is what the P&L counts as revenue.
 *
 * Returns: (transfer full) (nullable): the net proceeds, or %NULL on error
 */
VentureMoney *
venture_sale_get_net(
	VentureSale	 *self,
	GError		**error
);

/* --- Money --------------------------------------------------------------- */

#define VENTURE_TYPE_EXPENSE (venture_expense_get_type())
VENTURE_DECLARE_ENTITY(VentureExpense, venture_expense, EXPENSE)

/**
 * venture_expense_get_deductible_amount:
 * @self: a #VentureExpense
 * @error: (out) (optional): return location for a #GError
 *
 * Computes the deductible portion: the full amount when fully deductible,
 * the business-use percentage of it when partial, and zero otherwise.
 * Expenses awaiting review count as zero, so an unreviewed import can never
 * inflate a deduction.
 *
 * Returns: (transfer full) (nullable): the deductible amount
 */
VentureMoney *
venture_expense_get_deductible_amount(
	VentureExpense	 *self,
	GError		**error
);

#define VENTURE_TYPE_ACCOUNT (venture_account_get_type())
VENTURE_DECLARE_ENTITY(VentureAccount, venture_account, ACCOUNT)

#define VENTURE_TYPE_LEDGER_ENTRY (venture_ledger_entry_get_type())
VENTURE_DECLARE_ENTITY(VentureLedgerEntry, venture_ledger_entry, LEDGER_ENTRY)

#define VENTURE_TYPE_TAX_CATEGORY (venture_tax_category_get_type())
VENTURE_DECLARE_ENTITY(VentureTaxCategory, venture_tax_category, TAX_CATEGORY)

/* --- Relations ----------------------------------------------------------- */

#define VENTURE_TYPE_CONTACT (venture_contact_get_type())
VENTURE_DECLARE_ENTITY(VentureContact, venture_contact, CONTACT)

#define VENTURE_TYPE_INTERACTION (venture_interaction_get_type())
VENTURE_DECLARE_ENTITY(VentureInteraction, venture_interaction, INTERACTION)

#define VENTURE_TYPE_DEAL (venture_deal_get_type())
VENTURE_DECLARE_ENTITY(VentureDeal, venture_deal, DEAL)

/**
 * venture_deal_get_weighted_value:
 * @self: a #VentureDeal
 * @error: (out) (optional): return location for a #GError
 *
 * Computes the deal value multiplied by its probability, which is what a
 * pipeline forecast sums. A closed-won deal weighs its full value and a
 * closed-lost one weighs nothing, regardless of the stored probability.
 *
 * Returns: (transfer full) (nullable): the weighted value
 */
VentureMoney *
venture_deal_get_weighted_value(
	VentureDeal	 *self,
	GError		**error
);

/* --- Growth -------------------------------------------------------------- */

#define VENTURE_TYPE_CAMPAIGN (venture_campaign_get_type())
VENTURE_DECLARE_ENTITY(VentureCampaign, venture_campaign, CAMPAIGN)

/**
 * venture_campaign_get_roi:
 * @self: a #VentureCampaign
 *
 * Computes return on spend as a ratio, where 1.0 means the campaign earned
 * back exactly what it cost. A campaign with no spend returns 0.0 rather
 * than an infinity.
 *
 * Returns: the ratio of revenue to spend, less one
 */
gdouble
venture_campaign_get_roi(VentureCampaign *self);

#define VENTURE_TYPE_NEWSLETTER (venture_newsletter_get_type())
VENTURE_DECLARE_ENTITY(VentureNewsletter, venture_newsletter, NEWSLETTER)

#define VENTURE_TYPE_SUBSCRIBER (venture_subscriber_get_type())
VENTURE_DECLARE_ENTITY(VentureSubscriber, venture_subscriber, SUBSCRIBER)

#define VENTURE_TYPE_POST (venture_post_get_type())
VENTURE_DECLARE_ENTITY(VenturePost, venture_post, POST)

/* --- Thinking ------------------------------------------------------------ */

#define VENTURE_TYPE_IDEA (venture_idea_get_type())
VENTURE_DECLARE_ENTITY(VentureIdea, venture_idea, IDEA)

/**
 * venture_idea_get_score:
 * @self: a #VentureIdea
 *
 * Combines the idea's opportunity, confidence and effort ratings into a
 * single comparable score, so a list of ideas can be ranked rather than
 * merely listed. Effort divides, so a cheap idea of moderate promise can
 * outrank an expensive one of high promise -- which is usually the right
 * answer when the constraint is your own time.
 *
 * Returns: the score, higher being better
 */
gdouble
venture_idea_get_score(VentureIdea *self);

#define VENTURE_TYPE_RESEARCH_NOTE (venture_research_note_get_type())
VENTURE_DECLARE_ENTITY(VentureResearchNote, venture_research_note, RESEARCH_NOTE)

#define VENTURE_TYPE_TASK (venture_task_get_type())
VENTURE_DECLARE_ENTITY(VentureTask, venture_task, TASK)

#define VENTURE_TYPE_DOCUMENT (venture_document_get_type())
VENTURE_DECLARE_ENTITY(VentureDocument, venture_document, DOCUMENT)

/* --- Access -------------------------------------------------------------- */

#define VENTURE_TYPE_USER (venture_user_get_type())
VENTURE_DECLARE_ENTITY(VentureUser, venture_user, USER)

/**
 * venture_user_set_password:
 * @self: a #VentureUser
 * @password: the plaintext password
 * @iterations: the PBKDF2 iteration count
 * @error: (out) (optional): return location for a #GError
 *
 * Hashes @password and stores the result. The plaintext is never retained.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_user_set_password(
	VentureUser	 *self,
	const gchar	 *password,
	guint		  iterations,
	GError		**error
);

/**
 * venture_user_check_password:
 * @self: a #VentureUser
 * @password: the plaintext password to check
 *
 * Returns: %TRUE if @password matches the stored hash
 */
gboolean
venture_user_check_password(
	VentureUser	*self,
	const gchar	*password
);

#define VENTURE_TYPE_API_TOKEN (venture_api_token_get_type())
VENTURE_DECLARE_ENTITY(VentureApiToken, venture_api_token, API_TOKEN)

/**
 * venture_api_token_generate:
 * @self: a #VentureApiToken
 *
 * Generates a fresh secret, stores only its hash on the record, and returns
 * the secret itself. This is the only moment the plaintext exists: it is
 * shown once and never recoverable, so a leaked database yields no usable
 * tokens.
 *
 * Returns: (transfer full): the plaintext token
 */
gchar *
venture_api_token_generate(VentureApiToken *self);

/**
 * venture_api_token_matches:
 * @self: a #VentureApiToken
 * @presented: the token presented by a client
 *
 * Compares a presented token against the stored hash in constant time.
 *
 * Returns: %TRUE if the token is correct and has not expired
 */
gboolean
venture_api_token_matches(
	VentureApiToken	*self,
	const gchar	*presented
);

#define VENTURE_TYPE_AUDIT_ENTRY (venture_audit_entry_get_type())
VENTURE_DECLARE_ENTITY(VentureAuditEntry, venture_audit_entry, AUDIT_ENTRY)

/**
 * venture_audit_entry_new_for_change:
 * @action: what happened
 * @actor_kind: who caused it
 * @actor: (nullable): a human-readable actor label
 * @target: (nullable): the record that changed
 * @diff: (nullable): the change, as produced by venture_entity_diff()
 *
 * Builds an audit record for a change. This is the constructor the
 * repository, the AI tool layer and the automation engine all use, so every
 * mutation is recorded the same way regardless of what caused it.
 *
 * Returns: (transfer full): a new audit entry
 */
VentureAuditEntry *
venture_audit_entry_new_for_change(
	VentureAuditAction	 action,
	VentureActorKind	 actor_kind,
	const gchar		*actor,
	VentureEntity		*target,
	JsonNode		*diff
);

G_END_DECLS

#endif /* VENTURE_RECORDS_H */
