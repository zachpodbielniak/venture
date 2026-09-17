/*
 * venture-records.c - Every built-in record type
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Each type is a field table. The base class supplies storage, validation,
 * serialisation, diffing and display, so what remains here is the data model
 * itself plus the handful of computations that genuinely belong to a record
 * rather than to a report: what a sale actually nets, how much of an expense
 * is deductible, what a deal is worth after probability.
 */

#include "venture.h"

#include <string.h>

/* ==========================================================================
 * Structure
 * ========================================================================== */

static const VentureFieldDecl venture_organization_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What you call this entity"),
	/*
	 * Entities nest, so that "my side business" can hold the LLC that
	 * trades under it and a report on the parent covers both. A record
	 * belongs to exactly one entity; the hierarchy is what rolls them up.
	 */
	VENTURE_FIELD_REF("parent-id", "Part of",
	                  "A parent entity this one belongs to", "organization",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("slug", "Slug", "URL-safe identifier",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("kind", "Legal form",
	                   "Drives which tax treatment applies",
	                   venture_organization_kind_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("legal-name", "Legal name", "As registered",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	/* A tax identifier is exactly the sort of thing that must never
	 * appear in an API response, a log line or anything the AI reads. */
	VENTURE_FIELD("tax-id", "Tax ID", "EIN, VAT number or equivalent",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("default-currency", "Currency",
	              "ISO 4217 code used when an amount does not name one",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("fiscal-year-start-month", "Fiscal year starts",
	              "Month the fiscal year begins, 1-12",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("email", "Email", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("phone", "Phone", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("website", "Website", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("address", "Address", NULL),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL),
	VENTURE_FIELD("is-default", "Default entity",
	              "Used when a record does not name an organisation",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("quote-valid-days", "Quote validity days", "Zero uses 30 days", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureOrganization, venture_organization, venture_organization_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

static const VentureFieldDecl venture_venture_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What you call this venture"),
	VENTURE_FIELD("slug", "Slug", "URL-safe identifier",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_INDEXED),
	/* The kind of venture is a registry key rather than an enum, so a
	 * YAML file or a plugin can introduce a new one without a rebuild. */
	VENTURE_FIELD("venture-type", "Type",
	              "Registered venture type, e.g. books or etsy",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("status", "Status", "Lifecycle stage",
	                   venture_venture_status_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("description", "Description", NULL),
	VENTURE_FIELD("platform", "Platform",
	              "Where it operates, e.g. etsy, kdp, shopify",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("url", "URL", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("external-id", "External ID",
	              "Identifier on the platform this venture runs on",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("started-at", "Started", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("ended-at", "Ended", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("target-revenue", "Target revenue",
	                    "What you are aiming at overall"),
	VENTURE_FIELD_MONEY("monthly-goal", "Monthly goal",
	                    "What you are aiming at each month"),
	VENTURE_FIELD_ENUM("priority", "Priority", NULL,
	                   venture_priority_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("color", "Colour", "Accent used in the UI",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("idea-id", "Originating idea",
	                  "The idea this venture was promoted from", "idea",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL),
	VENTURE_FIELD_REF("owner-user-id", "Owner", "Responsible user", "user", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("team-id", "Team", "Optional owning team", "team", VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureVenture, venture_venture, venture_venture_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

/* ==========================================================================
 * Catalogue
 * ========================================================================== */

static const VentureFieldDecl venture_product_fields[] = {
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_NAME("name", "Name", "Title of the product"),
	VENTURE_FIELD("sku", "SKU", "Your own stock keeping unit",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_TEXT("description", "Description", NULL),
	VENTURE_FIELD("category", "Category", "Broad grouping",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("subcategory", "Subcategory", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	/* Genre is separate from category because book performance is
	 * analysed by genre specifically, and a book's category on a store
	 * is rarely the genre you actually think in. */
	VENTURE_FIELD("genre", "Genre",
	              "For books and other creative work, analysed separately",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("format", "Format",
	              "Paperback, hardcover, ebook, digital download, print",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("list-price", "List price", "What it sells for"),
	VENTURE_FIELD_MONEY("cost", "Unit cost", "What one unit costs you"),
	VENTURE_FIELD("isbn", "ISBN", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("asin", "ASIN", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("external-id", "External ID",
	              "Listing identifier on the selling platform",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("url", "URL", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("image-url", "Image", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("published-at", "Published", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL),
	VENTURE_FIELD("recognition-policy", "Recognition policy", "0 immediate, 1 deferred, 2 milestone",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("recognition-months", "Recognition months", "Service period for deferred income; 0 means immediate",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureProduct, venture_product, venture_product_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

static const VentureFieldDecl venture_inventory_item_fields[] = {
	VENTURE_FIELD_REF("product-id", "Product", NULL, "product",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("location", "Location", "Where the stock physically is",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("sku", "SKU", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_MONEY("unit-cost", "Unit cost",
	                    "Current carrying cost of one unit"),
	VENTURE_FIELD("reorder-point", "Reorder point",
	              "Quantity at which to restock",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reorder-quantity", "Reorder quantity", NULL,
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("lead-time-days", "Lead time",
	              "Days between ordering and receiving",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("allow-negative", "Allow negative",
	              "When true, stock may go below zero",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureInventoryItem, venture_inventory_item, venture_inventory_item_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), FALSE);)

/*
 * Quantity on hand is deliberately absent as a stored field. It is the sum
 * of the signed transactions below, which means the stock level is
 * auditable, reconstructable at any past date, and cannot drift out of step
 * with its own history.
 */
static const VentureFieldDecl venture_inventory_txn_fields[] = {
	VENTURE_FIELD_REF("inventory-item-id", "Item", NULL, "inventory_item",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_ENUM("kind", "Kind", "Why the quantity changed",
	                   venture_inventory_txn_kind_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("quantity", "Quantity",
	              "Signed: positive adds stock, negative removes it",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("unit-cost", "Unit cost",
	                    "Cost per unit for this movement"),
	VENTURE_FIELD("occurred-at", "Occurred", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("reference", "Reference",
	              "Purchase order, shipment or adjustment reference",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("sale-id", "Sale",
	                  "The sale that consumed this stock", "sale",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureInventoryTxn, venture_inventory_txn, venture_inventory_txn_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), FALSE);)

/* ==========================================================================
 * Revenue
 * ========================================================================== */

static const VentureFieldDecl venture_sale_fields[] = {
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("product-id", "Product", NULL, "product",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("contact-id", "Buyer", NULL, "contact",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("campaign-id", "Campaign",
	                  "The campaign this sale is attributed to", "campaign",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("channel", "Channel", "Where the sale happened",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	/* A nonempty platform identifier is unique within its organization,
	 * including deleted sales, so a repeated import cannot revive revenue. */
	VENTURE_FIELD("external-id", "Order ID",
	              "The platform's order identifier; prevents double import",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE |
	              VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("occurred-at", "Sold", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("quantity", "Quantity", NULL, VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("gross", "Gross", "Item total before anything else"),
	VENTURE_FIELD_MONEY("discount", "Discount", "Reduction given to the buyer"),
	VENTURE_FIELD_MONEY("shipping-collected", "Shipping collected",
	                    "What the buyer paid toward shipping"),
	VENTURE_FIELD_MONEY("shipping-cost", "Shipping cost",
	                    "What shipping actually cost you"),
	VENTURE_FIELD_MONEY("fees", "Fees", "Platform and payment fees"),
	VENTURE_FIELD_MONEY("tax-collected", "Tax collected",
	                    "Sales tax taken from the buyer; a liability, not income"),
	VENTURE_FIELD_MONEY("tax-remitted", "Tax remitted",
	                    "Sales tax you actually paid on"),
	VENTURE_FIELD_MONEY("refunded", "Refunded", "Amount returned to the buyer"),
	VENTURE_FIELD("country", "Country", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("buyer-name", "Buyer name", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL),
	VENTURE_FIELD("refunded-at", "Refund date", "Falls back to the sale date", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureSale, venture_sale, venture_sale_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), FALSE);)

/*
 * Reads a money-valued property, substituting zero for an unset one so the
 * arithmetic below does not need a null check per term.
 */
static VentureMoney *
venture_record_get_money(
	gpointer	 object,
	const gchar	*property,
	const gchar	*currency
){
	g_autoptr(VentureMoney) value = NULL;

	g_object_get(object, property, &value, NULL);

	if (NULL == value)
		return venture_money_new_zero(currency);

	return g_steal_pointer(&value);
}

/*
 * Determines which currency a record's arithmetic should be carried out in,
 * preferring whichever of its amounts is actually set over the process
 * default, so a record kept in EUR does not silently produce a USD total.
 */
static gchar *
venture_record_currency_hint(
	gpointer		 object,
	const gchar * const	*properties
){
	gsize i;

	for (i = 0; NULL != properties[i]; i++)
	{
		g_autoptr(VentureMoney) value = NULL;

		g_object_get(object, properties[i], &value, NULL);

		if (NULL != value)
			return g_strdup(venture_money_get_currency(value));
	}

	return g_strdup(venture_money_get_default_currency());
}

VentureMoney *
venture_sale_get_net(
	VentureSale	 *self,
	GError		**error
){
	static const gchar *const money_fields[] = {
		"gross", "shipping-collected", "fees", "shipping-cost",
		"refunded", "tax-remitted", "discount", NULL
	};
	g_autofree gchar *currency = NULL;
	g_autoptr(VentureMoney) total = NULL;
	gsize i;

	/* Terms that add to the proceeds, then terms that subtract. Keeping
	 * them as two lists rather than one long expression makes the
	 * definition of "net" auditable at a glance, which matters because
	 * this number is what the P&L calls revenue. */
	static const gchar *const positive[] = {
		"gross", "shipping-collected", NULL
	};
	static const gchar *const negative[] = {
		"discount", "fees", "shipping-cost", "refunded", "tax-remitted", NULL
	};

	g_return_val_if_fail(VENTURE_IS_SALE(self), NULL);

	currency = venture_record_currency_hint(self, money_fields);
	total = venture_money_new_zero(currency);

	for (i = 0; NULL != positive[i]; i++)
	{
		g_autoptr(VentureMoney) term = NULL;
		g_autoptr(VentureMoney) next = NULL;

		term = venture_record_get_money(self, positive[i], currency);
		next = venture_money_add(total, term, error);

		if (NULL == next)
			return NULL;

		g_clear_pointer(&total, venture_money_free);
		total = g_steal_pointer(&next);
	}

	for (i = 0; NULL != negative[i]; i++)
	{
		g_autoptr(VentureMoney) term = NULL;
		g_autoptr(VentureMoney) next = NULL;

		term = venture_record_get_money(self, negative[i], currency);
		next = venture_money_subtract(total, term, error);

		if (NULL == next)
			return NULL;

		g_clear_pointer(&total, venture_money_free);
		total = g_steal_pointer(&next);
	}

	return g_steal_pointer(&total);
}

/* ==========================================================================
 * Money
 * ========================================================================== */

static const VentureFieldDecl venture_expense_fields[] = {
	VENTURE_FIELD_REF("venture-id", "Venture",
	                  "Leave unset for an expense that spans ventures",
	                  "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_NAME("description", "Description", "What it was for"),
	VENTURE_FIELD("vendor", "Vendor", "Who you paid",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_MONEY("amount", "Amount", "What you paid"),
	VENTURE_FIELD("occurred-at", "Date", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("category", "Category", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("tax-category-id", "Tax category", NULL,
	                  "tax_category", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("deductibility", "Deductibility",
	                   "How this is treated for tax",
	                   venture_deductibility_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("business-use-percent", "Business use",
	              "Percentage used for business, for a partial deduction",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("payment-method", "Paid with", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("cash-account-id", "Cash account",
	                  "Ledger cash or card account this payment used; empty means code 1000",
	                  "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("receipt-url", "Receipt", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("external-id", "External ID",
	              "Bank or card transaction identifier; prevents double import",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED |
	              VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("reimbursable", "Reimbursable", NULL,
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("acquisition", "Customer acquisition", "Include this expense in customer acquisition spend",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureExpense, venture_expense, venture_expense_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), FALSE);)

VentureMoney *
venture_expense_get_deductible_amount(
	VentureExpense	 *self,
	GError		**error
){
	g_autoptr(VentureMoney) amount = NULL;
	VentureDeductibility deductibility;
	gint64 business_use;

	g_return_val_if_fail(VENTURE_IS_EXPENSE(self), NULL);

	g_object_get(self,
	             "amount", &amount,
	             "deductibility", &deductibility,
	             "business-use-percent", &business_use,
	             NULL);

	if (NULL == amount)
		return venture_money_new_zero(NULL);

	switch (deductibility)
	{
	case VENTURE_DEDUCTIBILITY_FULL:
		return venture_money_copy(amount);

	case VENTURE_DEDUCTIBILITY_PARTIAL:
		/* Clamp rather than trust the stored figure: a business-use
		 * percentage above 100 would claim more than was spent. */
		if (business_use < 0)
			business_use = 0;

		if (business_use > 100)
			business_use = 100;

		return venture_money_multiply_rational(amount, business_use, 100,
		                                       error);

	/* Capitalised costs are depreciated rather than expensed, and
	 * anything still awaiting review has not been confirmed deductible.
	 * Both count as zero here so that an unreviewed import can never
	 * quietly inflate a deduction. */
	case VENTURE_DEDUCTIBILITY_CAPITAL:
	case VENTURE_DEDUCTIBILITY_REVIEW:
	case VENTURE_DEDUCTIBILITY_NONE:
	default:
		return venture_money_new_zero(venture_money_get_currency(amount));
	}
}

static const VentureFieldDecl venture_account_fields[] = {
	VENTURE_FIELD("code", "Code", "Chart of accounts number",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_INDEXED |
	              VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_ENUM("kind", "Class",
	                   "Asset, liability, equity, income or expense",
	                   venture_account_kind_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("parent-id", "Parent account", NULL, "account",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("description", "Description", NULL),
	VENTURE_FIELD_MONEY("opening-balance", "Opening balance", NULL),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("cash-equivalent", "Cash equivalent",
		"Include with Cash on the cash-flow statement",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("cash-flow-class", "Cash-flow class",
		"operating, investing or financing; empty infers from the account class",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureAccount, venture_account, venture_account_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), FALSE);)

static const VentureFieldDecl venture_ledger_entry_fields[] = {
	VENTURE_FIELD_REF("journal-line-id", "Journal line",
		"Read-only projection of an authoritative journal line",
		"journal_line", VENTURE_COLUMN_FLAG_INDEXED),
	/* Lines sharing a transaction identifier form one double-entry
	 * transaction, and the repository refuses to commit a set whose
	 * debits and credits do not balance. */
	VENTURE_FIELD("transaction-id", "Transaction",
	              "Groups the lines of one balanced transaction",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("account-id", "Account", NULL, "account",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_ENUM("side", "Side", "Debit or credit",
	                   venture_ledger_side_get_type,
	                   VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Amount", "Always positive; the side carries the sign"),
	VENTURE_FIELD("occurred-at", "Date", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("memo", "Memo", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("source-type", "Source type",
	              "The record this line was posted from",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("source-id", "Source ID", NULL,
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureLedgerEntry, venture_ledger_entry, venture_ledger_entry_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), FALSE);)

static const VentureFieldDecl venture_tax_category_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD("code", "Code", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("deductibility", "Default deductibility", NULL,
	                   venture_deductibility_get_type,
	                   VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("default-business-use-percent", "Default business use",
	              NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("schedule-line", "Schedule line",
	              "Where this lands on the tax form",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("acquisition", "Default customer acquisition", "Classify new expenses and bill lines in this category as acquisition",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("description", "Description", NULL),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureTaxCategory, venture_tax_category, venture_tax_category_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), FALSE);)

static const VentureFieldDecl venture_tax_code_fields[] = {
	VENTURE_FIELD("code", "Code", "Organization-unique tax code", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD("jurisdiction", "Jurisdiction", "Filing jurisdiction, for example US-NY or EU-DE",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("rate-numerator", "Rate numerator", "Exact rate numerator; 8875/100000 is 8.875%",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("rate-denominator", "Rate denominator", "Exact rate denominator, never zero",
		VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("recoverable", "Recoverable", "Purchase tax is an asset rather than extra expense",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureTaxCode, venture_tax_code, venture_tax_code_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), FALSE);)

gboolean
venture_tax_code_get_rate(VentureTaxCode *self, gint64 *numerator, gint64 *denominator, GError **error)
{
	gint64 num = 0;
	gint64 den = 0;

	g_return_val_if_fail(VENTURE_IS_TAX_CODE(self), FALSE);
	g_object_get(self, "rate-numerator", &num, "rate-denominator", &den, NULL);
	if (num < 0 || den <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"A tax code rate must be a nonnegative numerator over a positive denominator");
		return FALSE;
	}
	if (numerator != NULL)
		*numerator = num;
	if (denominator != NULL)
		*denominator = den;
	return TRUE;
}

VentureMoney *
venture_tax_code_levy(VentureTaxCode *self, const VentureMoney *net, GError **error)
{
	gint64 numerator = 0;
	gint64 denominator = 0;

	if (net == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Tax is computed from a net amount");
		return NULL;
	}
	if (!venture_tax_code_get_rate(self, &numerator, &denominator, error))
		return NULL;
	return venture_money_multiply_rational(net, numerator, denominator, error);
}

/* ==========================================================================
 * Relations
 * ========================================================================== */

/*
 * An outside business you deal with: a customer, a supplier, a marketplace
 * you sell on, a partner.
 *
 * Distinct from #VentureOrganization, which is one of *your* entities. That
 * distinction is easy to blur and expensive to blur: one of them appears on
 * your tax return and the other does not.
 */
static const VentureFieldDecl venture_company_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What you call them"),
	VENTURE_FIELD_ENUM("kind", "Relationship", "What they are to you",
	                   venture_company_kind_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("legal-name", "Legal name", "As registered, if it differs",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("industry", "Industry", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("website", "Website", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("email", "Email", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("phone", "Phone", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("venture-id", "Venture",
	                  "The venture this relationship belongs to", "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	/* Their identifier on whatever platform you met them through, so a
	 * marketplace buyer can be matched back to an order. */
	VENTURE_FIELD("external-id", "External ID", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("source", "Source", "How you found each other",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("lifetime-value", "Lifetime value",
	                    "What they have been worth so far"),
	VENTURE_FIELD("tags", "Tags", "Comma separated",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_TEXT("address", "Address", NULL),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("owner-user-id", "Owner", "Responsible user", "user", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("team-id", "Team", "Optional owning team", "team", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("default-price-list-id", "Default price list", "Customer-specific quote pricing", "price_list", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("campaign-id", "Campaign", "Original acquisition campaign", "campaign", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("tax-exempt", "Tax exempt", "Non-profit or other exemption: invoices freeze zero tax",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("tax-exempt-reason", "Exemption reason", "Certificate or statutory basis",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureCompany, venture_company, venture_company_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

static const VentureFieldDecl venture_contact_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD("email", "Email", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("phone", "Phone", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("company-id", "Company", NULL, "company",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("role", "Role", "Their job title there",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("source", "Source", "How you came to know them",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("tags", "Tags", "Comma separated",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("website", "Website", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("address", "Address", NULL),
	VENTURE_FIELD_MONEY("lifetime-value", "Lifetime value",
	                    "Total net proceeds attributable to this contact"),
	VENTURE_FIELD("first-seen-at", "First seen", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-seen-at", "Last seen", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("subscribed", "Subscribed", NULL,
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL),
	VENTURE_FIELD_REF("owner-user-id", "Owner", "Responsible user", "user", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("team-id", "Team", "Optional owning team", "team", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("campaign-id", "Campaign", "Original acquisition campaign", "campaign", VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureContact, venture_contact, venture_contact_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

static const VentureFieldDecl venture_interaction_fields[] = {
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("company-id", "Company", NULL, "company",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("kind", "Kind", NULL,
	                   venture_interaction_kind_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("subject", "Subject", NULL),
	VENTURE_FIELD_TEXT("body", "Body", NULL),
	VENTURE_FIELD("occurred-at", "When", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("deal-id", "Deal", NULL, "deal",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("campaign-id", "Campaign", NULL, "campaign",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("outbound", "Outbound", "You initiated it",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("lead-id", "Lead", "Inquiry before conversion", "lead", VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureInteraction, venture_interaction, venture_interaction_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

static const VentureFieldDecl venture_deal_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact",
	                  VENTURE_COLUMN_FLAG_NONE),
	/* The account as well as the person: people move, accounts persist,
	 * and the pipeline is usually read by account. */
	VENTURE_FIELD_REF("company-id", "Company", NULL, "company",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("stage", "Stage", NULL, venture_deal_stage_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("value", "Value", "What it is worth if it closes"),
	VENTURE_FIELD("probability", "Probability",
	              "Percentage chance of closing, 0-100",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("expected-close-at", "Expected close", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("closed-at", "Closed", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("source", "Source", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL),
	VENTURE_FIELD_REF("owner-user-id", "Owner", "Responsible user", "user", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("team-id", "Team", "Optional owning team", "team", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("campaign-id", "Campaign", "Original acquisition campaign", "campaign", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("pipeline-id", "Pipeline", NULL, "pipeline", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("stage-id", "Pipeline stage", "Use VentureDealService to move", "pipeline_stage", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("next-step", "Next step", NULL),
	VENTURE_FIELD("next-action-at", "Next action", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("loss-reason-id", "Loss reason", NULL, "loss_reason", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("lost-note", "Lost note", NULL),
	VENTURE_FIELD("owner", "Owner", "Username, like a ticket assignee", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_ASSIGNED_USERNAME),
	VENTURE_FIELD("probability-overridden", "Override probability", "Keep the deal probability on stage moves", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("committed", "Committed", "Include in the committed forecast", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)

};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureDeal, venture_deal, venture_deal_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

VentureMoney *
venture_deal_get_weighted_value(
	VentureDeal	 *self,
	GError		**error
){
	g_autoptr(VentureMoney) value = NULL;
	VentureDealStage stage;
	gint64 probability;

	g_return_val_if_fail(VENTURE_IS_DEAL(self), NULL);

	g_object_get(self,
	             "value", &value,
	             "stage", &stage,
	             "probability", &probability,
	             NULL);

	if (NULL == value)
		return venture_money_new_zero(NULL);

	/* A closed deal's outcome is known, so the stored probability -- which
	 * nobody updates on the way out -- must not override it. */
	if (VENTURE_DEAL_STAGE_WON == stage)
		return venture_money_copy(value);

	if (VENTURE_DEAL_STAGE_LOST == stage)
		return venture_money_new_zero(venture_money_get_currency(value));

	if (probability < 0)
		probability = 0;

	if (probability > 100)
		probability = 100;

	return venture_money_multiply_rational(value, probability, 100, error);
}

/* ==========================================================================
 * Growth
 * ========================================================================== */

static const VentureFieldDecl venture_campaign_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("status", "Status", NULL,
	                   venture_campaign_status_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("channel", "Channel",
	              "Where it ran: ads, newsletter, social, promotion",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("started-at", "Started", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("ended-at", "Ended", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("budget", "Budget", NULL),
	VENTURE_FIELD_MONEY("spend", "Spend", "What it actually cost"),
	VENTURE_FIELD_MONEY("revenue", "Revenue", "Attributed net proceeds"),
	VENTURE_FIELD("impressions", "Impressions", NULL,
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("clicks", "Clicks", NULL, VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("conversions", "Conversions", NULL,
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("url", "URL", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureCampaign, venture_campaign, venture_campaign_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

gdouble
venture_campaign_get_roi(VentureCampaign *self)
{
	g_autoptr(VentureMoney) spend = NULL;
	g_autoptr(VentureMoney) revenue = NULL;
	gdouble spend_value;
	gdouble revenue_value;

	g_return_val_if_fail(VENTURE_IS_CAMPAIGN(self), 0.0);

	g_object_get(self, "spend", &spend, "revenue", &revenue, NULL);

	spend_value = (NULL != spend) ? venture_money_to_double(spend) : 0.0;
	revenue_value = (NULL != revenue) ? venture_money_to_double(revenue) : 0.0;

	/* No spend means no return on it. Reporting an infinity here would
	 * put a free campaign at the top of every ranking. */
	if (0.0 == spend_value)
		return 0.0;

	return (revenue_value - spend_value) / spend_value;
}

static const VentureFieldDecl venture_newsletter_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("description", "Description", NULL),
	VENTURE_FIELD("from-email", "From address", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("from-name", "From name", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("provider", "Provider",
	              "The sending service, if you use one",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("external-id", "External ID", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureNewsletter, venture_newsletter, venture_newsletter_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

static const VentureFieldDecl venture_subscriber_fields[] = {
	VENTURE_FIELD_REF("newsletter-id", "Newsletter", NULL, "newsletter",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("email", "Email", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED |
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("name", "Name", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_ENUM("status", "Status", NULL,
	                   venture_subscriber_status_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("subscribed-at", "Subscribed", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("unsubscribed-at", "Unsubscribed", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("source", "Source", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("tags", "Tags", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact",
	                  VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureSubscriber, venture_subscriber, venture_subscriber_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

static const VentureFieldDecl venture_post_fields[] = {
	VENTURE_FIELD_NAME("title", "Title", NULL),
	VENTURE_FIELD("slug", "Slug", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("newsletter-id", "Newsletter",
	                  "Set when this is a newsletter issue", "newsletter",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("status", "Status", NULL,
	                   venture_post_status_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("body", "Body", NULL),
	VENTURE_FIELD_TEXT("excerpt", "Excerpt", NULL),
	VENTURE_FIELD("url", "URL", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("published-at", "Published", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("scheduled-at", "Scheduled", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("author", "Author", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("tags", "Tags", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("views", "Views", NULL, VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VenturePost, venture_post, venture_post_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

/* ==========================================================================
 * Thinking
 * ========================================================================== */

static const VentureFieldDecl venture_idea_fields[] = {
	VENTURE_FIELD_NAME("title", "Title", NULL),
	VENTURE_FIELD_TEXT("description", "Description", NULL),
	VENTURE_FIELD_ENUM("status", "Status", NULL,
	                   venture_idea_status_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("priority", "Priority", NULL,
	                   venture_priority_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("opportunity", "Opportunity",
	              "How big it could be, 1-10",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("confidence", "Confidence",
	              "How sure you are it would work, 1-10",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("effort", "Effort", "What it would take, 1-10",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("estimated-revenue", "Estimated revenue", NULL),
	VENTURE_FIELD_MONEY("estimated-cost", "Estimated cost", NULL),
	VENTURE_FIELD("source", "Source", "Where the idea came from",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("promoted-venture-id", "Became venture",
	                  "Set when the idea was promoted to a real venture",
	                  "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureIdea, venture_idea, venture_idea_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

gdouble
venture_idea_get_score(VentureIdea *self)
{
	gint64 opportunity;
	gint64 confidence;
	gint64 effort;

	g_return_val_if_fail(VENTURE_IS_IDEA(self), 0.0);

	g_object_get(self,
	             "opportunity", &opportunity,
	             "confidence", &confidence,
	             "effort", &effort,
	             NULL);

	/* An unrated idea scores zero rather than dividing by zero. Treating
	 * unrated effort as 1 would flatter every idea nobody has assessed. */
	if ((opportunity <= 0) || (confidence <= 0) || (effort <= 0))
		return 0.0;

	/* Effort divides, so a cheap idea of moderate promise can outrank an
	 * expensive one of high promise -- usually the right answer when the
	 * binding constraint is your own time rather than capital. */
	return ((gdouble)opportunity * (gdouble)confidence) / (gdouble)effort;
}

static const VentureFieldDecl venture_research_note_fields[] = {
	VENTURE_FIELD_NAME("title", "Title", NULL),
	VENTURE_FIELD_TEXT("body", "Body", NULL),
	VENTURE_FIELD_REF("idea-id", "Idea", NULL, "idea",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("source-url", "Source", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("author", "Author", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("tags", "Tags", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("occurred-at", "Date", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureResearchNote, venture_research_note, venture_research_note_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)


/* ==========================================================================
 * Knowledge bases
 * ========================================================================== */

/*
 * A knowledge base: a named set of documents the AI is allowed to read.
 *
 * More than one exists because the useful question is rarely "what do we
 * know" but "what do the API docs say" -- and because a base is the unit of
 * scope for retrieval. Asking one base beats asking all of them the moment
 * two of them disagree, which is normal: last year's handbook and this
 * year's both describe a real policy.
 *
 * The slug is what somebody types after '#' in the chat, so it is unique,
 * indexed and deliberately not the display name. A name can be edited to fix
 * a typo without silently breaking every saved prompt that referenced it.
 */
static const VentureFieldDecl venture_knowledge_base_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD("slug", "Slug",
	              "What to type after # in the chat; lowercase, no spaces",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE |
	              VENTURE_COLUMN_FLAG_INDEXED |
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_TEXT("description", "Description",
	                   "What is in here, and when to reach for it"),
	/*
	 * Shown to the model when this base is selected, ahead of the
	 * retrieved passages. The place to say "cite the section number" or
	 * "this is the 2025 policy, superseded in April".
	 */
	VENTURE_FIELD_TEXT("instructions", "AI instructions",
	                   "Given to the assistant along with the passages"),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_INDEXED),
	/*
	 * The model that embedded this base's chunks.
	 *
	 * Recorded per base rather than read from config at search time
	 * because vectors from two models are not comparable -- the numbers
	 * are in different spaces and cosine between them is meaningless
	 * noise, not a weak match. Changing the configured model must
	 * therefore force a re-embed, and this is what detects that.
	 */
	VENTURE_FIELD("embedding-model", "Embedding model", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("embedding-dims", "Dimensions", NULL,
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	/*
	 * A directory on the server to sync from. Optional: a base can be
	 * written entirely in the browser and never touch the disk.
	 */
	VENTURE_FIELD("source-path", "Source directory",
	              "Synced from this directory on the server, when set",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("synced-at", "Last synced", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	/* Whether the assistant may reach for this base without being asked
	 * for it by name. A base of drafts usually should not. */
	VENTURE_FIELD("auto-retrieve", "Offer to the AI",
	              "Searched even when the prompt does not name it",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("tags", "Tags", "Comma separated",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE)
};

VENTURE_DEFINE_ENTITY(VentureKnowledgeBase, venture_knowledge_base,
                      venture_knowledge_base_fields)

/*
 * One document in a knowledge base.
 *
 * The body is the extracted text in every case, including for a PDF or a
 * .docx: search, embedding, export and editing all want one representation,
 * and keeping the original bytes as the source of truth would mean
 * re-extracting on every read. The original file is kept alongside when
 * there was one, named by source-path.
 *
 * source-hash is what makes sync cheap and idempotent. It is of the file's
 * bytes, not of the extracted text, so a PDF re-saved with identical text is
 * still seen as changed -- the alternative, hashing the extraction, means
 * every sync pays the extraction cost for every file just to decide it had
 * nothing to do.
 */
static const VentureFieldDecl venture_kb_article_fields[] = {
	VENTURE_FIELD_NAME("title", "Title", NULL),
	VENTURE_FIELD_REF("kb-id", "Knowledge base", NULL, "knowledge_base",
	                  VENTURE_COLUMN_FLAG_NOT_NULL |
	                  VENTURE_COLUMN_FLAG_INDEXED),
	/* Unique within a base, not globally: two bases may both have a
	 * "getting-started", and that is not a collision. */
	VENTURE_FIELD("slug", "Slug", "Stable name within the base",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED |
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_ENUM("format", "Format", NULL,
	                   venture_kb_format_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("status", "Status", NULL,
	                   venture_kb_article_status_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("body", "Body", "The article's text"),
	VENTURE_FIELD_TEXT("summary", "Summary",
	                   "Shown in search results ahead of the passage"),
	VENTURE_FIELD("source-path", "Source file",
	              "Where it came from, for sync", VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("source-hash", "Checksum",
	              "SHA-256 of the file's bytes, for change detection",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("mime-type", "Type", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("size-bytes", "Size", NULL, VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("source-url", "Source URL", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	/*
	 * What this was written from, when something wrote it.
	 *
	 * Polymorphic for the same reason a ticket relation is: an article can
	 * be generated from a research note, a ticket, a deal or a record type
	 * a plugin added last week. It is provenance rather than a link to
	 * follow, so it gets no picker and no reverse panel -- the useful
	 * direction is the cross-reference in kb_link, which is computed.
	 */
	VENTURE_FIELD("origin-type", "Generated from",
	              "Record type this was written from, if any",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("origin-id", "Generated from id", NULL,
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("tags", "Tags", "Comma separated",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("author", "Author", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	/*
	 * When the body was last turned into vectors, and by which model.
	 * Null means never: the article is stored and searchable by keyword,
	 * and invisible to vector search until it is indexed.
	 */
	VENTURE_FIELD("embedded-at", "Indexed", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("embedding-model", "Indexed with", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY(VentureKbArticle, venture_kb_article,
                      venture_kb_article_fields)

/*
 * One embedded passage of an article.
 *
 * A row per chunk rather than a vector per article because an article is
 * usually longer than the embedding model's context, and because the useful
 * answer to "where does it say that" is a passage rather than a document.
 *
 * The vector is base64 of little-endian float32, in a text column. There is
 * no vector type here: SQLite has none, and the PostgreSQL one lives in an
 * extension this deployment does not install. Brute-force cosine over a few
 * thousand rows is microseconds, so the index nobody can build is not yet
 * missed -- and when it is, this column is what an ANN index would be built
 * from anyway. JSON would have been the obvious alternative and is four
 * times the size for the same numbers.
 */
static const VentureFieldDecl venture_kb_chunk_fields[] = {
	VENTURE_FIELD_REF("article-id", "Article", NULL, "kb_article",
	                  VENTURE_COLUMN_FLAG_NOT_NULL |
	                  VENTURE_COLUMN_FLAG_INDEXED),
	/*
	 * Denormalised from the article so that searching one base is a
	 * filter on this table alone. The join it removes is on the hot path
	 * of every retrieval, and a chunk cannot change base without its
	 * article being rewritten.
	 */
	VENTURE_FIELD_REF("kb-id", "Knowledge base", NULL, "knowledge_base",
	                  VENTURE_COLUMN_FLAG_NOT_NULL |
	                  VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("ordinal", "Position", "Order within the article",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("text", "Text", "The passage, as embedded"),
	/* The heading this passage sits under, kept so a result can say where
	 * in the document it came from without re-parsing the body. */
	VENTURE_FIELD("heading", "Heading", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("embedding", "Vector",
	                   "base64 of little-endian float32"),
	VENTURE_FIELD("dims", "Dimensions", NULL, VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("embedding-model", "Model", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("char-count", "Characters", NULL,
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY(VentureKbChunk, venture_kb_chunk,
                      venture_kb_chunk_fields)

/*
 * A computed connection between an article and any other record.
 *
 * This is the cross-reference: the reason an idea's page can say which
 * passages of which handbook bear on it. The subject is named by type and
 * id, so it reaches every registered record type including ones a plugin
 * added -- and, like #VentureTicketRelation, that shape has no database
 * constraint behind it. Create these only through venture_kb_link_create().
 *
 * The score is cosine similarity at the time it was computed, kept so the
 * list can be ordered and thresholded without recomputing, and so a link
 * that was strong under a previous embedding model is visibly stale rather
 * than silently wrong.
 */
static const VentureFieldDecl venture_kb_link_fields[] = {
	VENTURE_FIELD_REF("article-id", "Article", NULL, "kb_article",
	                  VENTURE_COLUMN_FLAG_NOT_NULL |
	                  VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("kb-id", "Knowledge base", NULL, "knowledge_base",
	                  VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("subject-type", "Subject type",
	              "Registered record type this bears on",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL |
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("subject-id", "Subject id", NULL,
	              VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_NOT_NULL |
	              VENTURE_COLUMN_FLAG_INDEXED),
	/*
	 * The subject's name as it was when the link was made, so the row
	 * still reads correctly once the subject has been renamed or
	 * deleted -- which is exactly when somebody is working out what it
	 * meant. Same reasoning as VentureTicketRelation's subject-label.
	 */
	VENTURE_FIELD("subject-label", "Subject", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("score", "Score", "Cosine similarity when computed",
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("chunk-ordinal", "Passage",
	              "Which passage of the article matched",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("excerpt", "Excerpt",
	                   "The passage that matched, for display"),
	VENTURE_FIELD("embedding-model", "Model", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("computed-at", "Computed", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY(VentureKbLink, venture_kb_link, venture_kb_link_fields)

/*
 * A ticket: something to do, in a state, assigned to somebody.
 *
 * One type covers internal project tasks and external support requests
 * because the work is the same shape. What differs is who raised it and who
 * may read the replies, and those are a field and a flag rather than two
 * systems to keep in step -- which matters when a support ticket turns out
 * to be a bug, and the bug is the work.
 */
static const VentureFieldDecl venture_ticket_fields[] = {
	VENTURE_FIELD_NAME("title", "Title", NULL),
	VENTURE_FIELD_ENUM("kind", "Kind", "Internal work, or somebody else's request",
	                   venture_ticket_kind_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("status", "Status", NULL,
	                   venture_ticket_status_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("priority", "Priority", NULL,
	                   venture_priority_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("description", "Description", NULL),
	VENTURE_FIELD("assignee", "Assignee", "Who is doing it",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_ASSIGNED_USERNAME),
	/* Who asked. Set for an external ticket, empty for your own work. */
	VENTURE_FIELD_REF("contact-id", "Raised by", NULL, "contact",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("company-id", "Company", NULL, "company",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("idea-id", "Idea", NULL, "idea",
	                  VENTURE_COLUMN_FLAG_NONE),
	/* A ticket can hang off another, so an epic and its pieces, or a bug
	 * split out of a support request, stay connected. */
	VENTURE_FIELD_REF("parent-id", "Part of", NULL, "ticket",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("due-at", "Due", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("resolved-at", "Resolved", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("estimate-hours", "Estimate", "Hours",
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("tags", "Tags", "Comma separated",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	/*
	 * Position within its board column. Kept as a sparse double so a card
	 * dropped between two others is placed by halving the gap, without
	 * renumbering the column on every move.
	 */
	VENTURE_FIELD("board-order", "Board order", NULL,
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("resolution", "Resolution",
	                   "What was actually done about it"),
	/*
	 * What shape of work this is -- a different question from "kind".
	 *
	 * Kind says whose problem it is, which is what decides who may read
	 * the replies. This says how big the piece is and therefore how it
	 * should be approached, which is what a forge rule keys on. The two
	 * are orthogonal: an external bug is both, and common.
	 *
	 * Appended rather than placed beside "kind" because field order is
	 * display order, and the list view shows the first seven columns --
	 * inserting here would silently push a column off every ticket list.
	 */
	VENTURE_FIELD_ENUM("issue-type", "Issue type",
	                   "Epic, story, task, subtask, research or bug",
	                   venture_issue_type_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	/*
	 * Where this work belongs, chosen before anything has been pushed.
	 *
	 * The link record carries the issue number and the branch; this is
	 * the intent, and it has to be separate because rule resolution
	 * happens before a link exists -- the rule is what decides whether to
	 * create the issue and the branch at all. Reading the repository off
	 * the link would make the rule depend on the thing it decides.
	 */
	VENTURE_FIELD_REF("repo-id", "Repository", NULL, "forge_repo",
	                  VENTURE_COLUMN_FLAG_NONE),
	/* The factory's two questions of a ticket: which increment it is
	 * planned for, and which release carried it out. Both soft -- the
	 * factory module may be off -- so the picker is simply empty then. */
	VENTURE_FIELD_REF("milestone-id", "Milestone", "Planned for",
	                  "milestone", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("release-id", "Fixed in", "The release that shipped it",
	                  "release", VENTURE_COLUMN_FLAG_NONE),
	/*
	 * The sprint it is planned into and what it weighs there. Points are
	 * a separate number from the hour estimate above because they answer
	 * a different question -- relative size, agreed as a team -- and a
	 * board that burns down hours is not the board people asked for.
	 */
	VENTURE_FIELD_REF("sprint-id", "Sprint", "Planned into", "sprint",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("story-points", "Points", "Relative size, for the sprint",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	/*
	 * The service-level clocks. The due times are set once, when the
	 * ticket is first saved and a policy covers it, and left alone: a
	 * deadline that moved every time the priority was edited would be
	 * no deadline. First response is stamped by the first reply that
	 * whoever raised it could read. Breached is written by the sweep so
	 * that a list can filter on it; the live state is always computed.
	 */
	VENTURE_FIELD("first-response-due-at", "Respond by",
	              "When the first reply is due under the service level",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("resolution-due-at", "Resolve by",
	              "When the ticket is due to be resolved under the service level",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("first-responded-at", "First reply",
	              "When the first visible reply was made",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("sla-breached", "Breached",
	              "Set once the resolution target passed while it was open",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	/* Maintained from the worklogs, so a list can sort and filter on it
	 * without summing rows; the worklog is the truth. */
	VENTURE_FIELD("logged-hours", "Logged",
	              "Hours logged against it, summed from the worklogs",
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_NONE),
	/*
	 * What whoever raised it made of how it went. Two fields rather than
	 * a survey record: a rating belongs to the ticket it rates, there is
	 * exactly one, and a table of one-row surveys buys nothing but a
	 * join. The comment is where the score stops being a number.
	 */
	VENTURE_FIELD_ENUM("satisfaction", "Satisfaction",
	                   "What whoever raised it made of how it went",
	                   venture_satisfaction_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("satisfaction-comment", "Satisfaction comment",
	                   "What they said about it"),
	VENTURE_FIELD_REF("owner-user-id", "Owner", "Responsible user", "user", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("team-id", "Team", "Optional owning team", "team", VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureTicket, venture_ticket, venture_ticket_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

/*
 * One message on a ticket.
 *
 * The internal flag is the whole reason this is a record rather than a text
 * field on the ticket: a reply to the person who raised it and a note to
 * yourself have to live in the same thread, in order, and be
 * distinguishable at a glance.
 */
static const VentureFieldDecl venture_ticket_comment_fields[] = {
	VENTURE_FIELD_REF("ticket-id", "Ticket", NULL, "ticket",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_TEXT("body", "Comment", NULL),
	VENTURE_FIELD("author", "Author", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("internal", "Internal note",
	              "Not shown to whoever raised the ticket",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("occurred-at", "When", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureTicketComment, venture_ticket_comment, venture_ticket_comment_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

/*
 * A ticket related to anything else in the system.
 *
 * The ticket already references the handful of things it is usually about
 * -- a contact, a company, a venture, an idea, a repository -- as ordinary
 * foreign keys, because those are the common cases and a real reference
 * gets a picker in the form and a section on the target's page for free.
 *
 * This is for everything else. A ticket can be about an invoice whose total
 * looks wrong, an expense that needs a receipt, a product that keeps being
 * returned. Adding a field per type would mean the ticket table grew a
 * column every time a record type was added, and nineteen of the twenty
 * would be empty on any given row.
 *
 * So the subject is a type name and an id, the same shape the audit log
 * uses for its target and for the same reason: the set of things that can
 * be pointed at is the set of registered types, which is not known when the
 * field table is written.
 *
 * The cost is real and worth stating. A polymorphic pair gets none of what
 * a declared reference gets: no picker, no automatic reverse section, no
 * rendered link. Every one of those is written by hand against this type,
 * which is why the ordinary references above stay ordinary references
 * rather than being folded in here.
 *
 * The label is denormalised deliberately. It is what the row displays as,
 * so the list renders without resolving anything -- and it still reads
 * correctly after the subject is deleted, which is exactly when somebody is
 * trying to work out what a ticket was about.
 */
static const VentureFieldDecl venture_ticket_relation_fields[] = {
	VENTURE_FIELD_REF("ticket-id", "Ticket", NULL, "ticket",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	/* The registered entity name, e.g. "invoice". Validated against the
	 * registry rather than trusted: an unknown type here is a row that
	 * can never resolve to anything. */
	VENTURE_FIELD("subject-type", "Type", "Which kind of record",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("subject-id", "Record", "Its id",
	              VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("subject-label", "Subject",
	              "What it was called when it was linked",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("note", "Why", "What the connection is",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY(VentureTicketRelation, venture_ticket_relation,
                      venture_ticket_relation_fields)

/* ==========================================================================
 * The software factory
 *
 * The loop a software venture runs: an idea becomes tickets, tickets are
 * planned into a milestone, worked on a branch -- by a person or by a
 * coding run -- built by the forge's CI, assembled into a release, deployed
 * to an environment, and, when something goes wrong there, raised as an
 * incident that becomes a ticket again. Every step is a record, so every
 * step is queryable, reportable, linkable and reachable by the AI, and the
 * lead-time report can measure the loop end to end.
 * ========================================================================== */

/*
 * A planned increment: what a set of tickets adds up to.
 */
static const VentureFieldDecl venture_milestone_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "e.g. 1.2, Q3 launch"),
	VENTURE_FIELD_TEXT("description", "Description", "What it delivers"),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("product-id", "Product", NULL, "product",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("status", "Status", NULL,
	                   venture_milestone_status_get_type,
	                   VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("due-on", "Due", NULL, VENTURE_FIELD_KIND_DATE,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("completed-at", "Completed", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureMilestone, venture_milestone, venture_milestone_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

/*
 * A version that went, or will go, out of the door.
 *
 * The version number is the name, so a release lists and links as "1.2.0". The
 * tag and the external id are what the forge calls it, written back by the
 * webhook when the forge publishes it or by Publish when VENTURE does.
 */
static const VentureFieldDecl venture_release_fields[] = {
	/* "number", not "version": the entity spine owns a property called
	 * version, the optimistic-concurrency counter. */
	VENTURE_FIELD_NAME("number", "Version", "e.g. 1.2.0"),
	VENTURE_FIELD("name", "Title", "A name for the release, if it has one",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("product-id", "Product", NULL, "product",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("repo-id", "Repository", "Where the tag lives",
	                  "forge_repo", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("milestone-id", "Milestone", NULL, "milestone",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("status", "Status", NULL,
	                   venture_release_status_get_type,
	                   VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("tag", "Tag", "The git tag, e.g. v1.2.0",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("released-at", "Released", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("url", "URL", "The release page on the forge",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("changelog", "Changelog",
	                   "What changed; Draft fills it from the tickets"),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL),
	VENTURE_FIELD("external-id", "Forge id", "The forge's id for the release",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureRelease, venture_release, venture_release_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

/*
 * One run of a CI workflow, or a build somebody recorded by hand.
 *
 * Mostly written by the forge webhook: a workflow_run event creates the
 * row when the run is requested and updates it when it completes, matched
 * on the repository and the forge's own run id, so a retried delivery
 * updates rather than duplicates.
 */
static const VentureFieldDecl venture_build_fields[] = {
	VENTURE_FIELD("title", "Title", "The commit or workflow title",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("repo-id", "Repository", NULL, "forge_repo",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("release-id", "Release", "The release it built",
	                  "release", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("run-id", "Coding run", "The run whose branch it built",
	                  "forge_run", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("status", "Status", NULL, venture_build_status_get_type,
	                   VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("trigger", "Trigger", "What started it",
	                   venture_build_trigger_get_type,
	                   VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("workflow", "Workflow", "The CI workflow's name",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("external-id", "Forge run id", "The forge's id for the run",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("number", "Run number", NULL, VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("ref", "Branch", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("commit", "Commit", "The sha that was built",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("url", "URL", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("started-at", "Started", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("finished-at", "Finished", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("log-excerpt", "Log", "The part of the log that matters")
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureBuild, venture_build, venture_build_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), FALSE);)

/*
 * Somewhere a release runs.
 */
static const VentureFieldDecl venture_environment_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "e.g. production, staging"),
	VENTURE_FIELD_ENUM("kind", "Kind", NULL, venture_environment_kind_get_type,
	                   VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("product-id", "Product", NULL, "product",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("url", "URL", "Where it answers", VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("description", "Description", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureEnvironment, venture_environment, venture_environment_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

/*
 * A release arriving in an environment. The latest one that succeeded is
 * what the environment is running.
 */
static const VentureFieldDecl venture_deployment_fields[] = {
	VENTURE_FIELD_REF("release-id", "Release", NULL, "release",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("environment-id", "Environment", NULL, "environment",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("build-id", "Build", "The build that was deployed",
	                  "build", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("status", "Status", NULL,
	                   venture_deployment_status_get_type,
	                   VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("deployed-at", "Deployed", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("deployed-by", "Deployed by", "A person, a rule, a runner",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("url", "URL", "The pipeline or log", VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureDeployment, venture_deployment, venture_deployment_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), FALSE);)

/*
 * Something going wrong where a release runs. Closes the loop: an incident
 * names the environment, the release and the deployment it came from, and
 * the ticket raised to fix it.
 */
static const VentureFieldDecl venture_incident_fields[] = {
	VENTURE_FIELD_NAME("title", "Title", "What is wrong, in a line"),
	VENTURE_FIELD_ENUM("severity", "Severity", NULL,
	                   venture_incident_severity_get_type,
	                   VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("status", "Status", NULL,
	                   venture_incident_status_get_type,
	                   VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("environment-id", "Environment", NULL, "environment",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("release-id", "Release", "The release that was running",
	                  "release", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("deployment-id", "Deployment", "The deployment that "
	                  "introduced it", "deployment", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("ticket-id", "Ticket", "The fix", "ticket",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("started-at", "Started", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("resolved-at", "Resolved", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("summary", "Summary", "What happened"),
	VENTURE_FIELD_TEXT("postmortem", "Postmortem", "Why, and what changes")
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureIncident, venture_incident, venture_incident_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

/*
 * A link between any two records.
 *
 * The generalisation of ticket_relation: both ends are named by type and
 * id, so a release can point at the tickets it shipped, an incident at the
 * deployment that caused it, an expense at the invoice it was billed
 * through, and a plugin's record at anything at all -- without a foreign
 * key per pair, which for thirty types is four hundred and thirty-five.
 *
 * The kind gives the link a direction and a meaning, and every directed
 * kind has an inverse, so one row is enough: standing on the target, the
 * link reads as the inverse. Both labels are stored, for the reason the
 * ticket relation stores one -- a link still has to read correctly after
 * one end has been deleted.
 *
 * The checks that a polymorphic pair cannot get from the schema -- that
 * both types are registered and on, that both records exist, that a record
 * is not linked to itself, that the same pair is not linked twice -- run as
 * a save validator (see venture_record_link_install_validator()), so they
 * apply to every writer: the form, the API, a staged approval, the AI.
 */
static const VentureFieldDecl venture_record_link_fields[] = {
	VENTURE_FIELD("source-type", "From type", "The record type at this end",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("source-id", "From", "Its id", VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("source-label", "From (name)",
	              "What it was called when it was linked",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_ENUM("kind", "Kind", "What the link means, read from here",
	                   venture_link_kind_get_type,
	                   VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("target-type", "To type", "The record type at the other end",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("target-id", "To", "Its id", VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("target-label", "To (name)",
	              "What it was called when it was linked",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("note", "Note", "Why, in a few words",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE)
};

/*
 * "Invoice INV-42 blocks Ticket #7", so a link reads as a sentence in a
 * list, an audit entry or a staged-change card.
 */
static gchar *
venture_record_link_get_display_name(VentureEntity *self)
{
	g_autofree gchar *source_label = NULL;
	g_autofree gchar *target_label = NULL;
	g_autofree gchar *source_type = NULL;
	g_autofree gchar *target_type = NULL;
	VentureLinkKind kind;
	gint64 source_id = 0;
	gint64 target_id = 0;

	g_object_get(self,
	             "source-type", &source_type, "source-id", &source_id,
	             "source-label", &source_label,
	             "target-type", &target_type, "target-id", &target_id,
	             "target-label", &target_label,
	             "kind", &kind,
	             NULL);

	if (venture_string_is_empty(source_label))
	{
		g_free(source_label);
		source_label = g_strdup_printf("%s #%" G_GINT64_FORMAT,
		                               (NULL != source_type) ? source_type
		                                                     : "?",
		                               source_id);
	}

	if (venture_string_is_empty(target_label))
	{
		g_free(target_label);
		target_label = g_strdup_printf("%s #%" G_GINT64_FORMAT,
		                               (NULL != target_type) ? target_type
		                                                     : "?",
		                               target_id);
	}

	return g_strdup_printf("%s %s %s", source_label,
	                       venture_link_kind_to_label(kind), target_label);
}

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureRecordLink, venture_record_link,
                                venture_record_link_fields,
	VENTURE_ENTITY_CLASS(klass)->get_display_name =
		venture_record_link_get_display_name;
)

/* ==========================================================================
 * Dashboards
 *
 * A dashboard is a named page of widgets, and a widget is a small,
 * declarative question -- "the open incidents", "this month's releases
 * report", "how many tickets are mine" -- answered from whatever record
 * type or report it names. Both are ordinary records, so a dashboard is
 * built in the browser, exported as JSON, created by venturectl, and read by
 * the assistant, with nothing written per surface. What each widget kind
 * makes of its settings is decided in core/venture-dashboard.c.
 * ========================================================================== */

static gboolean
venture_dashboard_before_save(
	VentureEntity	 *self,
	GError		**error
);

static const VentureFieldDecl venture_dashboard_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "e.g. Factory, Month end, My work"),
	VENTURE_FIELD("slug", "Slug",
	              "Its address under /dashboards/; made from the name if "
	              "left blank",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("description", "Description",
	                   "What the page is for, shown under its title"),
	VENTURE_FIELD_ENUM("purpose", "Purpose",
	                   "Overview, reporting or work; sorts the list",
	                   venture_dashboard_purpose_get_type,
	                   VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("layout", "Layout", "How many columns across",
	                   venture_dashboard_layout_get_type,
	                   VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("home", "Home page",
	              "Show this dashboard at / instead of the built-in overview",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("personal", "Personal",
	              "Only its owner can see it; otherwise every user can",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("owner-user-id", "Owner", "Who made it", "user",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("venture-id", "Venture",
	                  "Narrow every widget that can be to one venture",
	                  "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("position", "Position", "Order in the sidebar; lowest first",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureDashboard, venture_dashboard,
                                venture_dashboard_fields,
	VENTURE_ENTITY_CLASS(klass)->before_save = venture_dashboard_before_save;
)

/*
 * A dashboard's slug is its address: /dashboards/factory. It is derived from
 * the name when left blank, here rather than in a form handler, so a
 * dashboard created over the API or imported from a file gets one too.
 */
static gboolean
venture_dashboard_before_save(
	VentureEntity	 *self,
	GError		**error
){
	g_autofree gchar *slug = NULL;
	g_autofree gchar *name = NULL;

	g_object_get(self, "slug", &slug, "name", &name, NULL);

	if (venture_string_is_empty(slug))
	{
		g_autofree gchar *derived = NULL;

		derived = venture_slugify(name);

		if (venture_string_is_empty(derived))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_VALIDATION,
			                    "A dashboard needs a name a slug can be "
			                    "made from");
			return FALSE;
		}

		g_object_set(self, "slug", derived, NULL);
	}
	else
	{
		g_autofree gchar *normalised = NULL;

		/* Typed slugs are normalised the same way, so "My Board" and
		 * "my-board" cannot be two addresses for one page. */
		normalised = venture_slugify(slug);

		if (venture_string_is_empty(normalised))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "\"%s\" cannot be made into a slug", slug);
			return FALSE;
		}

		if (0 != g_strcmp0(normalised, slug))
			g_object_set(self, "slug", normalised, NULL);
	}

	return VENTURE_ENTITY_CLASS(venture_dashboard_parent_class)->before_save(
		self, error);
}


/*
 * One widget on a dashboard.
 *
 * The kind is a string, not an enum, so a plugin can register a widget kind
 * the way it registers a record type or a report; the save validator in
 * core/venture-dashboard.c refuses a kind nobody registered. The settings
 * are plain columns rather than one JSON blob because a column has a label,
 * a help line and a form box for free, and because "which record type" and
 * "which report" are questions every kind answers the same way. What each
 * kind reads is listed by the kind itself, and the editor shows only those.
 */
static const VentureFieldDecl venture_dashboard_widget_fields[] = {
	VENTURE_FIELD_REF("dashboard-id", "Dashboard", NULL, "dashboard",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("title", "Title",
	              "Shown on the card; the kind supplies one if left blank",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("kind", "Kind",
	              "What the widget shows: list, count, metric, report, ...",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("position", "Position", "Order on the page; lowest first",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("span", "Width", "One column, two, or the whole row; "
	                   "the grid width below overrides it when set",
	                   venture_widget_span_get_type,
	                   VENTURE_COLUMN_FLAG_NOT_NULL),
	/*
	 * Where on the grid. Zero means "wherever fits": a widget with no
	 * placement flows into the first free cells after the placed ones,
	 * which is how a template lays out without knowing the layout. The
	 * page never fails on a bad placement -- one that overlaps or falls
	 * off the edge is treated as unplaced -- so these are hints the
	 * editor keeps consistent, not invariants the schema enforces.
	 */
	VENTURE_FIELD("grid-col", "Grid column", "1-based; 0 to place automatically",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("grid-row", "Grid row", "1-based; 0 to place automatically",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("grid-width", "Grid width", "Columns spanned; 0 follows Width",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("grid-height", "Grid height", "Rows spanned; 0 is one",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("entity-type", "Record type",
	              "The record type the widget reads, e.g. ticket, release",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("record-id", "Record id",
	              "One record's id, for the kinds that show a single record",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("report-name", "Report",
	              "A report's name, e.g. pnl, releases, lead_time",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("period", "Period",
	              "this_month, last_30_days, ytd, all_time, 2026-Q2 ...",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("filter", "Filter",
	              "As on a list page: status=open&assignee={me}",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("order", "Order",
	              "A field to sort by; a leading - sorts descending",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("limit", "Limit", "How many rows; 0 for the kind's default",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("field", "Field",
	              "The field the kind groups, counts, dates or charts by",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("columns", "Columns",
	              "Comma-separated fields to show; the kind's default if blank",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("body", "Body",
	                   "Text for a note, or one action per line as "
	                   "Label | /path"),
	VENTURE_FIELD("refresh-seconds", "Refresh",
	              "Reload the widget every so many seconds; 0 never",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("options", "Options",
	                   "Extra settings as JSON, e.g. {\"days\": 30}")
};

VENTURE_DEFINE_ENTITY(VentureDashboardWidget, venture_dashboard_widget,
                      venture_dashboard_widget_fields)

/*
 * One forge server: a Forgejo or Gitea instance you have an account on.
 *
 * A record rather than configuration because there is more than one, because
 * each carries a credential that has to be settable without a restart, and
 * because a repository has to point at one. The token and the webhook secret
 * are why this type is owner-only everywhere and invisible to the AI.
 */
/* ==========================================================================
 * The workdesk
 *
 * What a helpdesk and a tracker have that a list of tickets does not: a
 * place a person is told things, the choice of what to be told about, a
 * clock on how quickly somebody is answered, a reply that can be given in
 * one press, the hours a ticket took, and the fortnight it was planned
 * into. Each is a record, so each is on the API, in the CLI and readable by
 * the assistant without anything written per surface.
 * ========================================================================== */

/*
 * A list somebody wants back: a record type plus the query string that
 * filtered, sorted and searched it. The URL was always bookmarkable; this
 * is the bookmark with a name, shared or kept, and pinned into the sidebar
 * when it is looked at every day.
 */
static const VentureFieldDecl venture_saved_view_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "e.g. My urgent tickets, Unpaid invoices"),
	VENTURE_FIELD("entity-type", "Record type",
	              "The type the view lists, e.g. ticket, invoice",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("query", "Query",
	              "The list's query string: field filters, order, search",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("board", "Board",
	              "Open as the ticket board rather than a list",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("description", "Description", NULL),
	VENTURE_FIELD("personal", "Personal",
	              "Only its owner sees it; otherwise every user does",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("pinned", "Pinned", "Show it in the sidebar",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("owner-user-id", "Owner", "Who saved it", "user",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("position", "Position", "Order in the sidebar; lowest first",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY(VentureSavedView, venture_saved_view,
                      venture_saved_view_fields)

/*
 * A person following a record. Polymorphic like a record link, because
 * anything can be watched -- a ticket, an invoice, a release -- and a
 * column per type would be the ticket table's problem all over again.
 */
static const VentureFieldDecl venture_watch_fields[] = {
	VENTURE_FIELD_REF("user-id", "User", NULL, "user",
	                  VENTURE_COLUMN_FLAG_PERSONAL_OWNER | VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("target-type", "Record type", NULL,
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("target-id", "Record", NULL, VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("target-label", "Label", "What it was called when watched",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY(VentureWatch, venture_watch, venture_watch_fields)

/*
 * One thing one person is told. Written by the machinery -- a mention, an
 * assignment, a change to something watched, a service level about to be
 * missed, a budget line crossed, a run finishing -- and read from the
 * inbox. Per-user like a chat thread, and guarded the same way.
 */
static const VentureFieldDecl venture_notification_fields[] = {
	VENTURE_FIELD_REF("user-id", "User", "Who it is for", "user",
	                  VENTURE_COLUMN_FLAG_PERSONAL_OWNER | VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("kind", "Kind", NULL,
	                   venture_notification_kind_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("title", "Title", NULL),
	VENTURE_FIELD_TEXT("body", "Body", NULL),
	VENTURE_FIELD("actor", "By", "Who caused it",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("target-type", "Record type", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("target-id", "Record", NULL, VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("target-label", "About", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("read-at", "Read", "Empty while unread",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("occurred-at", "When", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY(VentureNotification, venture_notification,
                      venture_notification_fields)

/*
 * How quickly a ticket must be answered and resolved. Matched on the
 * ticket's kind and priority, most specific policy first, so "external
 * urgent: reply in one hour" beats "everything: reply in a day". Hours
 * are wall-clock hours; business hours are a calendar this install does
 * not have, and a promise made in wall-clock time is one a customer can
 * check.
 */
static const VentureFieldDecl venture_sla_policy_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "e.g. Support urgent, Internal default"),
	VENTURE_FIELD_ENUM("kind", "Ticket kind", "Which tickets it covers",
	                   venture_ticket_kind_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("all-kinds", "Every kind",
	              "Ignore the kind and cover internal and external alike",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("priority", "Priority", "Which priority it covers",
	                   venture_priority_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("all-priorities", "Every priority",
	              "Ignore the priority and cover them all",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("first-response-hours", "Respond within",
	              "Hours until the first visible reply is due; 0 for no target",
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("resolution-hours", "Resolve within",
	              "Hours until the ticket is due to be resolved; 0 for none",
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VentureSlaPolicy, venture_sla_policy,
                      venture_sla_policy_fields)

/*
 * A reply and a set of changes, applied to a ticket in one press. The
 * reply is optional and so is every change, because "close as duplicate"
 * needs no words and "send the how-to" changes nothing. The apply flags
 * exist because an enum cannot say "leave it alone".
 */
static const VentureFieldDecl venture_macro_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "e.g. Ask for logs, Close as fixed"),
	VENTURE_FIELD_TEXT("description", "Description", "When to use it"),
	VENTURE_FIELD_TEXT("body", "Reply",
	                   "The comment to add; {ticket}, {title} and {me} are "
	                   "filled in"),
	VENTURE_FIELD("internal", "Internal note",
	              "Add the reply as a note whoever raised it cannot see",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("apply-status", "Change status", NULL,
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("status", "Status", "The status to set",
	                   venture_ticket_status_get_type,
	                   VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("apply-priority", "Change priority", NULL,
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("priority", "Priority", "The priority to set",
	                   venture_priority_get_type,
	                   VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("assignee", "Assign to",
	              "A username, or {me} for whoever applies it; blank leaves it",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("add-tags", "Add tags", "Comma separated",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("position", "Position", "Order in the menu; lowest first",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY(VentureMacro, venture_macro, venture_macro_fields)

/*
 * Hours spent on a ticket, by whom, when. Rows rather than a running total
 * because the total is derivable and the rows are not; the ticket's
 * logged-hours is kept in step from these.
 */
static const VentureFieldDecl venture_worklog_fields[] = {
	VENTURE_FIELD_REF("ticket-id", "Ticket", NULL, "ticket",
	                  VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("author", "Who", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("hours", "Hours", NULL, VENTURE_FIELD_KIND_DOUBLE,
	              VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("occurred-at", "When", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("note", "Note", "What the time went on")
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureWorklog, venture_worklog, venture_worklog_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), FALSE);)

/*
 * A fixed window of work with a goal. Tickets are planned into it by their
 * sprint-id; progress is counted from them, in points when they carry
 * points and in tickets when they do not.
 */
static const VentureFieldDecl venture_sprint_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "e.g. Sprint 12, September week 2"),
	VENTURE_FIELD_TEXT("goal", "Goal", "What done looks like at the end"),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("status", "Status", NULL,
	                   venture_sprint_status_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("starts-on", "Starts", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("ends-on", "Ends", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("capacity-points", "Capacity",
	              "Points the team expects to finish; 0 for unplanned",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureSprint, venture_sprint, venture_sprint_fields,
	venture_entity_class_set_federation_access(VENTURE_ENTITY_CLASS(klass), TRUE);)

/* ==========================================================================
 * Webhooks out
 *
 * VENTURE has always accepted webhooks from a forge. This is the other
 * direction: a record changed here, and something outside wants to know.
 * A webhook is a URL, a set of events and a shared secret; a delivery is
 * what happened when one fired, kept because "did it go out" is a
 * question somebody asks at exactly the moment nothing is working.
 * ========================================================================== */

static const VentureFieldDecl venture_webhook_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "e.g. Ops chat, Billing sync"),
	VENTURE_FIELD("url", "URL", "Where the POST goes",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL),
	/*
	 * Which events, as a comma-separated list of "type.action" patterns:
	 * "ticket.created", "invoice.*", or "*" for everything. A list
	 * rather than a flag per event, because the events are the record
	 * types crossed with three actions and a plugin can add a type --
	 * the same reason nothing else in this tree writes down the nouns.
	 */
	VENTURE_FIELD("events", "Events",
	              "Comma separated, e.g. ticket.created, invoice.*, or * "
	              "for everything",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	/* Signed with this, so the far end can tell a delivery from a forgery.
	 * Sensitive: it never reaches a response, a form or the AI. */
	VENTURE_FIELD("secret", "Signing secret",
	              "Deliveries are signed with it; generated when left blank",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("secret-set-at", "Secret set", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("include-record", "Send the record",
	              "Include the whole record in the body, not just its "
	              "type, id and label",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_INDEXED),
	/*
	 * Consecutive failures, and when the last delivery was attempted.
	 * The count is what switches a dead endpoint off rather than
	 * retrying it against every write for a week; a success resets it.
	 */
	VENTURE_FIELD("failure-count", "Consecutive failures", NULL,
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("last-delivery-at", "Last delivery", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("description", "Description", "What is at the far end")
};

VENTURE_DEFINE_ENTITY(VentureWebhook, venture_webhook, venture_webhook_fields)

/*
 * One attempt at one delivery. Evidence, like a coding run: what was
 * sent, what came back, how long it took.
 */
static const VentureFieldDecl venture_webhook_delivery_fields[] = {
	VENTURE_FIELD_REF("webhook-id", "Webhook", NULL, "webhook",
	                  VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("event", "Event", "e.g. ticket.created",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("state", "State", NULL,
	                   venture_delivery_state_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("target-type", "Record type", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("target-id", "Record", NULL, VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("target-label", "About", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("status-code", "Status", "What the endpoint answered",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("duration-ms", "Took", "Milliseconds",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("attempted-at", "Attempted", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("request-body", "Sent", "The body that went out"),
	VENTURE_FIELD_TEXT("response-excerpt", "Answered",
	                   "The beginning of what came back"),
	VENTURE_FIELD_TEXT("failure-reason", "Error", "Why it did not arrive")
};

VENTURE_DEFINE_ENTITY(VentureWebhookDelivery, venture_webhook_delivery,
                      venture_webhook_delivery_fields)

/*
 * Who a new ticket goes to.
 *
 * Matched like a service-level policy -- most specific first -- and
 * applied only when the ticket arrives with nobody on it, so a ticket
 * raised with an assignee keeps the one it was given. The cursor is the
 * round robin's memory; it is a field rather than a global because two
 * rules covering different queues take turns independently.
 */
static const VentureFieldDecl venture_routing_rule_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "e.g. Support rota, Bugs to the team"),
	VENTURE_FIELD_ENUM("kind", "Ticket kind", "Which tickets it covers",
	                   venture_ticket_kind_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("all-kinds", "Every kind",
	              "Ignore the kind and cover internal and external alike",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("priority", "Priority", "Which priority it covers",
	                   venture_priority_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("all-priorities", "Every priority",
	              "Ignore the priority and cover them all",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("tag", "Tag", "Only tickets carrying this tag; blank for any",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("strategy", "Strategy", NULL,
	                   venture_routing_strategy_get_type,
	                   VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("assignees", "Assign to",
	              "Usernames, comma separated, in rota order",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("cursor", "Rota position",
	              "Where the round robin has got to", VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VentureRoutingRule, venture_routing_rule,
                      venture_routing_rule_fields)

static const VentureFieldDecl venture_forge_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What you call this server"),
	VENTURE_FIELD_ENUM("kind", "Software",
	                   "Forgejo and Gitea speak the same API",
	                   venture_forge_kind_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	/* The origin every request is built from, and the only host this
	 * install will ever send its token to. No /api/v1 -- the client
	 * appends that. */
	VENTURE_FIELD("base-url", "Base URL", "https://git.example.com",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE |
	              VENTURE_COLUMN_FLAG_INDEXED),
	/*
	 * Where git clones from, when that is not where the API lives.
	 *
	 * These are routinely different hosts. A Forgejo behind a reverse
	 * proxy answers its API on https://git.example.com while SSH goes
	 * straight to the daemon at git@git-ssh.example.com, because the
	 * proxy speaks HTTP and sshd does not. Deriving one from the other
	 * guesses wrong for every install shaped like that.
	 *
	 * Accepts either form. "git@host" composes the scp-like
	 * git@host:owner/repo.git; a URL composes <url>/owner/repo.git.
	 * Empty falls back to the base URL, which is right for the simple
	 * case where one host does both.
	 */
	VENTURE_FIELD("clone-base-url", "Clone base",
	              "git@git-ssh.example.com, or empty to use the base URL",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	/*
	 * Set through /forges/:id/token, never through the generated form: a
	 * sensitive field is skipped both when the form is rendered and when
	 * it is applied, so a value typed into a box that does not exist
	 * could not be saved. The companion timestamp is what the page shows
	 * instead of the value.
	 */
	VENTURE_FIELD("token", "Access token", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("token-set-at", "Token set", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("webhook-secret", "Webhook secret", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("webhook-secret-set-at", "Secret set", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	/*
	 * The login the access token belongs to, fetched from the forge
	 * rather than typed. This is the webhook loop guard: an event whose
	 * sender is this account was caused by VENTURE itself. Until it is
	 * set, VENTURE cannot tell its own writes from anybody else's, which
	 * is why the detail page warns while it is empty.
	 */
	VENTURE_FIELD("bot-username", "Bot account",
	              "The account the token belongs to; its own events are ignored",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("verified-at", "Last verified",
	              "When the token last authenticated",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VentureForge, venture_forge, venture_forge_fields)

/*
 * One repository on one forge.
 *
 * "name" holds owner/repo verbatim rather than two columns, because that is
 * the only key a webhook payload carries, it is what a person copies out of
 * the address bar, and it is what the display-name resolver picks up -- so a
 * related-records list reads "zach/venture" rather than "forge_repo #7".
 * Neither half may contain a slash upstream, so splitting on the first one is
 * exact.
 */
static const VentureFieldDecl venture_forge_repo_fields[] = {
	VENTURE_FIELD_NAME("name", "Repository",
	                   "owner/repo, exactly as the forge spells it"),
	VENTURE_FIELD_REF("forge-id", "Forge", NULL, "forge",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("default-branch", "Default branch",
	              "Branches are cut from this unless a rule says otherwise",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("branch-prefix", "Branch prefix",
	              "Prepended to every branch VENTURE creates, e.g. venture/",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	/*
	 * The exact URL to clone this one repository from.
	 *
	 * Almost always empty: the forge's clone base plus owner/repo is
	 * right. It exists for the repository that does not follow its
	 * forge's pattern -- a mirror, a repository reached through a
	 * different host, a path a proxy rewrites -- because discovering
	 * that after the fact should not mean adding a second forge record
	 * that differs in one field.
	 */
	VENTURE_FIELD("clone-url", "Clone URL",
	              "Empty: composed from the forge's clone base",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	/*
	 * What this code is for, in business terms. Gives the venture page a
	 * Repositories section for nothing, and -- more importantly -- is
	 * what scopes a webhook-created ticket to the right organisation. A
	 * webhook carries no session and no cookie, so without this the
	 * ticket would fall through to the default organisation and vanish
	 * from every list scoped to a specific entity.
	 */
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("product-id", "Product", NULL, "product",
	                  VENTURE_COLUMN_FLAG_NONE),
	/* A checkout the CLI runner works in. Empty disables the CLI runner
	 * for this repository, which is the state in a container that was
	 * built without the agent CLIs. */
	VENTURE_FIELD("workspace-path", "Workspace",
	              "Checkout the CLI runner works in; empty disables it here",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("push-issues", "File issues upstream",
	              "Linking a ticket here opens a forge issue for it",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("accept-issues", "Accept issues from upstream",
	              "A webhook issue becomes a ticket",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VentureForgeRepo, venture_forge_repo,
                      venture_forge_repo_fields)

/*
 * What an AI may do about a ticket, decided per repository and per issue
 * type -- because a bug is not a task. A bug wants a reproduction and a
 * regression test; an epic wants breaking up and no code at all.
 */
static const VentureFieldDecl venture_forge_rule_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What this rule is for"),
	/* A repository rule. Leave empty and set the forge for a forge-wide
	 * default. A rule with neither matches nothing, deliberately: a rule
	 * that applied everywhere because a row was saved half-filled is the
	 * failure that costs money. */
	VENTURE_FIELD_REF("repo-id", "Repository",
	                  "Leave empty for a forge-wide rule", "forge_repo",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("forge-id", "Forge",
	                  "Set on a forge-wide rule; ignored when a repository "
	                  "is named", "forge", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("issue-type", "Issue type", NULL,
	                   venture_issue_type_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	/*
	 * The catch-all switch.
	 *
	 * A GObject enum property has no null -- every enum is installed with
	 * a default of zero -- so "leave the type blank to match everything"
	 * is not expressible with the enum above. The alternative was a
	 * second, parallel enum carrying an "any" value, which is exactly the
	 * two-tables-to-keep-in-step drift this codebase avoids everywhere
	 * else. A boolean is uglier to read and impossible to get wrong.
	 */
	VENTURE_FIELD("all-issue-types", "All issue types",
	              "Ignore the issue type and apply to every ticket in scope",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	/* Off is a rule kept for reference: it never runs, and a broader rule
	 * may then apply. Said here because this is where it is discovered. */
	VENTURE_FIELD("enabled", "Enabled",
	              "Off never runs; a broader rule may then apply",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("runner", "Runner",
	                   "The in-process agent, or a coding CLI in a checkout",
	                   venture_forge_runner_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	/* Matched against the configured allow list by exact equality and
	 * spawned with an explicit argv, never a shell. This field is
	 * editable by anybody who can edit rules, and a rule that names a
	 * binary to run is the shell tool the AI is denied, with a longer
	 * fuse because it is persisted. */
	VENTURE_FIELD("runner-command", "Command",
	              "CLI runner only. Must be on the allowed list in settings",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("provider", "Provider",
	              "Empty uses the provider from settings",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("model", "Model", "Empty uses the model from settings",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("prompt", "Prompt",
	                   "Prepended to the ticket. This is where a bug and a "
	                   "task actually differ"),
	VENTURE_FIELD_ENUM("outcome", "On success",
	                   "How far a successful run goes",
	                   venture_forge_run_outcome_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("trigger", "Trigger", "What starts a run",
	                   venture_forge_trigger_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("branch-template", "Branch name",
	              "{type}/{id}-{slug} by default", VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("base-branch", "Base branch",
	              "Empty cuts from the repository's default branch",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("labels", "Labels",
	              "Comma separated; applied to the forge issue",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("max-turns", "Turn limit", "0 uses the AI setting",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("timeout-seconds", "Timeout",
	              "Seconds a run may take; 0 uses the setting",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	/* The runaway guard. A webhook that fires on every comment, a rule
	 * that opens a draft pull request each time, and a model that
	 * comments back is a loop with a monthly invoice attached. */
	VENTURE_FIELD("max-runs-per-day", "Daily limit",
	              "Runs this rule may start in a day; 0 is unlimited",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("require-approval", "Ask before running",
	              "A run is queued and waits for a person to start it",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VentureForgeRule, venture_forge_rule,
                      venture_forge_rule_fields)

/*
 * What ties a ticket to a repository: the issue upstream, the branch the
 * work is on, and the pull request if one is open.
 *
 * One record rather than three fields on the ticket, because all three are
 * states of the same link and a ticket can be worked in two repositories at
 * once -- a fix in the library and a version bump in the application.
 * Putting any of them on the ticket would force one repository and need a
 * migration later. It is also exactly the row the webhook loop guard reads.
 */
static const VentureFieldDecl venture_ticket_link_fields[] = {
	VENTURE_FIELD_REF("ticket-id", "Ticket", NULL, "ticket",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("repo-id", "Repository", NULL, "forge_repo",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	/* The number, not the forge's internal id: the number is what every
	 * API path uses, what every webhook payload carries, and what a
	 * person types. Zero means linked but not yet filed. */
	VENTURE_FIELD("issue-number", "Issue", "The number upstream",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	/* Stored and shown, never fetched. A URL out of a payload is data. */
	VENTURE_FIELD("issue-url", "Issue URL", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("branch", "Branch", "The branch this work is on",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("pull-request-number", "Pull request", "0 until one opens",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("pull-request-url", "Pull request URL", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	/* Which side raised it. Half the loop guard. */
	VENTURE_FIELD_ENUM("origin", "Origin", NULL,
	                   venture_forge_link_origin_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	/* The delivery id of the last event applied. A forge retries a
	 * delivery it thinks failed; this is what makes the retry a no-op. */
	VENTURE_FIELD("last-delivery-id", "Last delivery", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("synced-at", "Last synced", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY(VentureTicketLink, venture_ticket_link,
                      venture_ticket_link_fields)

/*
 * One attempt by a runner at one ticket.
 *
 * Evidence, not configuration: what was asked, what came back, which branch
 * and pull request it produced, and what it cost. Written by the runner as
 * it goes, and refused writes through the generic surfaces for the same
 * reason the audit log is -- an editable run record is one somebody can
 * rewrite after the fact.
 */
static const VentureFieldDecl venture_forge_run_fields[] = {
	VENTURE_FIELD_REF("ticket-id", "Ticket", NULL, "ticket",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("link-id", "Link", NULL, "ticket_link",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("rule-id", "Rule", "The rule that chose to run",
	                  "forge_rule", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("state", "State", NULL,
	                   venture_forge_run_state_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	/* Recorded as run, not looked up through the rule: a rule edited
	 * afterwards must not rewrite what happened last week. */
	VENTURE_FIELD_ENUM("runner", "Runner", NULL,
	                   venture_forge_runner_get_type,
	                   VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("outcome", "Outcome", NULL,
	                   venture_forge_run_outcome_get_type,
	                   VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("branch", "Branch", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("pull-request-number", "Pull request", NULL,
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("started-at", "Started", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("finished-at", "Finished", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("provider", "Provider", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("model", "Model", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("input-tokens", "Input tokens", NULL,
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("output-tokens", "Output tokens", NULL,
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("turns", "Turns", NULL, VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_NONE),
	/* Money, not a double -- and this is an ERP, so what the models cost
	 * is a figure the books can eventually see rather than a line in a
	 * log. Sub-cent runs are the norm, so the currency's exponent is
	 * doing real work here. */
	VENTURE_FIELD_MONEY("cost", "Cost", "What the provider charged"),
	VENTURE_FIELD_TEXT("prompt", "Prompt",
	                   "What the runner was actually asked, after the rule's "
	                   "prompt and the ticket were composed"),
	VENTURE_FIELD_TEXT("summary", "Summary", "What the run says it did"),
	VENTURE_FIELD_TEXT("log", "Log", "Trimmed transcript or CLI output"),
	VENTURE_FIELD_TEXT("failure-reason", "Error",
	                   "Why it stopped, when it did")
};

VENTURE_DEFINE_ENTITY(VentureForgeRun, venture_forge_run,
                      venture_forge_run_fields)

/*
 * A cap on what the coding runs may cost.
 *
 * This is an ERP, so what the models charge is an expense with a line in
 * the books, and an expense gets a budget. One row per limit: a repository
 * or every repository, a window, an amount. The spend is never stored here
 * -- it is summed from the runs' own cost field at the moment of asking,
 * so a run whose cost is corrected afterwards corrects the budget too.
 * The two stamps below say when the warning and the stop were sent, so
 * that each is sent once per window rather than on every run.
 */
static const VentureFieldDecl venture_agent_budget_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "e.g. Monthly agent spend"),
	VENTURE_FIELD_REF("repo-id", "Repository",
	                  "Only runs against this repository; blank for every run",
	                  "forge_repo", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("period", "Period", "The window the limit covers",
	                   venture_budget_period_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("limit", "Limit", "What the runs may cost in the window"),
	VENTURE_FIELD("warn-percent", "Warn at",
	              "Percent of the limit at which a warning is sent; 0 for none",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("hard-stop", "Hard stop",
	              "Refuse new runs once the limit is reached",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("warned-at", "Warned",
	              "When the warning for the current window went out",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("exhausted-at", "Exhausted",
	              "When the current window ran out",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VentureAgentBudget, venture_agent_budget,
                      venture_agent_budget_fields)

static const VentureFieldDecl venture_document_fields[] = {
	VENTURE_FIELD_NAME("title", "Title", NULL),
	VENTURE_FIELD("kind", "Kind",
	              "Receipt, contract, licence, artwork, manuscript",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("path", "Path", "Where it lives on disk",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("url", "URL", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("mime-type", "Type", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("expense-id", "Expense",
	                  "Set when this is a receipt", "expense",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("description", "Description", NULL),
	VENTURE_FIELD("size-bytes", "Size", NULL, VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("hash", "Checksum", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED),
	/*
	 * The document's text, pulled out at upload time -- from the PDF, or
	 * verbatim for a text file. Stored rather than re-extracted because
	 * it is what makes a filed invoice searchable, and what the AI reads
	 * when a conversation about an attachment resumes next week: the
	 * model re-fetches the document record, not the file.
	 */
	VENTURE_FIELD_TEXT("extracted-text", "Extracted text",
	                   "Text content pulled from the file at upload")
};

VENTURE_DEFINE_ENTITY(VentureDocument, venture_document,
                      venture_document_fields)

/* ==========================================================================
 * Invoicing
 * ========================================================================== */

/*
 * An invoice: a claim on somebody else's money, with a paper trail.
 *
 * Distinct from a sale on purpose. A sale records money that arrived; an
 * invoice records money that is owed, and the day it is paid the revenue
 * becomes a sale -- created by the mark-paid transition, so the books and
 * the invoicing can never quietly disagree about what was actually earned.
 */
static const VentureFieldDecl venture_invoice_fields[] = {
	VENTURE_FIELD("number", "Number", "Yours to allocate; INV-2026-001 "
	              "style works",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION |
	              VENTURE_COLUMN_FLAG_INDEXED |
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_ENUM("status", "Status", NULL,
	                   venture_invoice_status_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("company-id", "Bill to", NULL, "company",
	                  VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("contact-id", "Attention of", NULL, "contact",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture",
	                  VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("issued-at", "Issued", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("due-at", "Due", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("paid-at", "Paid", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("terms", "Terms", "Payment terms shown on the "
	                   "printed invoice"),
	VENTURE_FIELD_TEXT("notes", "Notes", "Internal; never printed"),
	VENTURE_FIELD("workflow-state", "Workflow state",
		"Set through VentureSettlementService; plugins may extend the lifecycle",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_MONEY("shipping-amount", "Shipping", "Optional shipping frozen at issuance"),
	VENTURE_FIELD("tax-exempt", "Tax exempt", "Frozen at issue from the customer or this invoice",
		VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("tax-exempt-reason", "Exemption reason", "Certificate or statutory basis frozen at issue",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("external-id", "External ID",
		"Connector order identifier; nonempty values are unique per organization",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION)
};

VENTURE_DEFINE_ENTITY(VentureInvoice, venture_invoice, venture_invoice_fields)

/*
 * One line of an invoice. The amount is quantity times unit price, computed
 * -- never stored, because a stored product of two stored factors is a
 * third copy of the truth waiting to disagree with the other two.
 */
static const VentureFieldDecl venture_invoice_line_fields[] = {
	VENTURE_FIELD_REF("invoice-id", "Invoice", NULL, "invoice",
	                  VENTURE_COLUMN_FLAG_NOT_NULL |
	                  VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("description", "Description", NULL,
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL |
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	/* Thousandths, so 2.5 hours and 0.125 of a day are exact. */
	VENTURE_FIELD("quantity", "Quantity", "Up to three decimal places",
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("unit-price", "Unit price", NULL),
	VENTURE_FIELD("position", "Position", "Order on the invoice",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("product-id", "Product", "Catalog item priced by the payment provider", "product", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("discount-percent", "Discount percent", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("tax-percent", "Tax percent", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("tax-code-id", "Tax code", "Exact rate and jurisdiction; used instead of tax-percent when set",
		"tax_code", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("income-amount", "Frozen income", "Net after discount, frozen at issuance"),
	VENTURE_FIELD_MONEY("discount-amount", "Frozen discount", "Frozen at issuance"),
	VENTURE_FIELD_MONEY("tax-amount", "Frozen tax", "Frozen at issuance"),
	VENTURE_FIELD_MONEY("shipping-amount", "Frozen shipping", "Frozen at issuance")
};

VENTURE_DEFINE_ENTITY(VentureInvoiceLine, venture_invoice_line,
                      venture_invoice_line_fields)

VentureMoney *
venture_invoice_line_get_amount(
	VentureInvoiceLine	 *self,
	GError			**error
){
	g_autoptr(VentureMoney) unit_price = NULL;
	gdouble quantity;
	gint64 thousandths;

	g_return_val_if_fail(VENTURE_IS_INVOICE_LINE(self), NULL);

	g_object_get(self, "quantity", &quantity,
	             "unit-price", &unit_price, NULL);

	if (NULL == unit_price)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_VALIDATION,
		                    "The line has no unit price");
		return NULL;
	}

	/*
	 * The quantity multiplies as the exact rational n/1000, rounded half
	 * to even once -- the same discipline as every other money
	 * computation here. Multiplying by the double directly would let
	 * 0.1 + binary representation error into an invoice total.
	 */
	thousandths = (gint64)(quantity * 1000.0 +
	                       ((quantity >= 0.0) ? 0.5 : -0.5));

	{
		g_autoptr(VentureMoney) subtotal = venture_money_multiply_rational(unit_price, thousandths, 1000, error);
		g_autoptr(VentureMoney) frozen_net = NULL;
		g_autoptr(VentureMoney) frozen_tax = NULL;
		gint64 discount_percent;
		gint64 tax_percent;
		g_object_get(self, "discount-percent", &discount_percent, "tax-percent", &tax_percent,
			"income-amount", &frozen_net, "tax-amount", &frozen_tax, NULL);
		if (subtotal == NULL)
			return NULL;
		if (frozen_net != NULL && frozen_tax != NULL)
			return venture_money_add(frozen_net, frozen_tax, error);
		return venture_quote_apply_percentages(subtotal, discount_percent, tax_percent, error);
	}
}

/*
 * One plugin's runtime configuration, as YAML.
 *
 * A record rather than a file so that changing it is an audited, ordinary
 * write -- and so the plugin manager can raise a signal the moment it
 * changes, which is what lets a running plugin pick up new settings without
 * a restart. Secrets do not belong here: like the server's own
 * configuration, a plugin setting that is secret should name an environment
 * variable instead.
 */
static const VentureFieldDecl venture_plugin_config_fields[] = {
	VENTURE_FIELD("name", "Plugin", "The plugin's name, as the plugin "
	              "list shows it", VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE |
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("config", "Configuration", "YAML, whatever shape "
	                   "the plugin documents"),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VenturePluginConfig, venture_plugin_config,
                      venture_plugin_config_fields)

/* ==========================================================================
 * AI conversations
 * ========================================================================== */

/*
 * One AI conversation, owned by one account.
 *
 * Stored as records rather than browser state so a conversation survives a
 * restart, a different machine, and next Tuesday -- the panel's resume list
 * is just a query. The user-id is what scopes it: chat is addressed to a
 * person, not to a business entity, and the dedicated chat routes refuse to
 * serve a thread whose user-id is not the caller's.
 */
static const VentureFieldDecl venture_chat_thread_fields[] = {
	VENTURE_FIELD_NAME("title", "Title",
	                   "Taken from the first message unless renamed"),
	VENTURE_FIELD_REF("user-id", "User", NULL, "user",
	                  VENTURE_COLUMN_FLAG_PERSONAL_OWNER | VENTURE_COLUMN_FLAG_NOT_NULL |
	                  VENTURE_COLUMN_FLAG_INDEXED),
	/*
	 * Denormalised from the newest message so the resume list can be one
	 * query ordered by this column, rather than a per-thread subquery for
	 * "when did it last say anything".
	 */
	VENTURE_FIELD("last-activity-at", "Last activity", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY(VentureChatThread, venture_chat_thread,
                      venture_chat_thread_fields)

/*
 * One line of a conversation, either side.
 *
 * The body is what the operator saw, and it is also what is replayed to the
 * provider when the thread resumes -- so the stored transcript is the
 * context, not a paraphrase of it.
 */
static const VentureFieldDecl venture_chat_message_fields[] = {
	VENTURE_FIELD_REF("thread-id", "Thread", NULL, "chat_thread",
	                  VENTURE_COLUMN_FLAG_PERSONAL_OWNER | VENTURE_COLUMN_FLAG_NOT_NULL |
	                  VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("role", "Role", NULL, venture_chat_role_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("body", "Message", NULL)
};

VENTURE_DEFINE_ENTITY(VentureChatMessage, venture_chat_message,
                      venture_chat_message_fields)

/*
 * A skill is a saved way of asking: a slash trigger, and the prompt it
 * expands to. "/reply firm" becomes the whole paragraph about drafting a
 * reply, with "firm" as the instruction, and the record on the screen as
 * context the way every question carries it. A handful are built in; the
 * records add to them, and a record with a built-in's trigger replaces it.
 */
static const VentureFieldDecl venture_ai_skill_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "e.g. Chase an invoice"),
	VENTURE_FIELD("trigger", "Trigger",
	              "What to type after / in the chat; lowercase, no spaces",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE |
	              VENTURE_COLUMN_FLAG_INDEXED |
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("description", "Description",
	              "One line, shown in the / menu",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_TEXT("prompt", "Prompt",
	                   "What the model is asked. {input} is replaced by "
	                   "whatever follows the trigger; without it, the input "
	                   "is appended as instructions."),
	VENTURE_FIELD("enabled", "Enabled", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY(VentureAiSkill, venture_ai_skill,
                      venture_ai_skill_fields)

/*
 * An agent-harness session: a coding agent, kept open, in a workspace.
 *
 * A forge_run is one shot -- a ticket goes in, a branch comes out, nobody
 * is watching. A session is the other half of the same machinery: the
 * same providers and the same thread, driven a turn at a time by somebody
 * who is reading the output and deciding what to ask next. What makes it
 * a record rather than a thing in memory is that the transcript of what
 * an agent was told to do in a repository is worth as much afterwards as
 * a run's log, and for the same reasons.
 */
static const VentureFieldDecl venture_agent_session_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What this session is for"),
	VENTURE_FIELD_ENUM("state", "State", NULL,
	                   venture_agent_session_state_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("provider", "Provider",
	              "claude-code, codex, cursor, opencode, or an API provider",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("model", "Model", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),

	/*
	 * How hard to think, for the CLI providers that take a flag for it.
	 * An HTTP provider has none, and cursor bakes the level into the
	 * model id, so for those this is empty and unused.
	 */
	VENTURE_FIELD("effort", "Effort", "low, medium, high or max",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),

	/*
	 * Where the agent works, resolved and absolute. Stored rather than
	 * recomputed: a session opened against a checkout that has since
	 * been removed from the allow-list should still say where it ran.
	 */
	VENTURE_FIELD("workspace", "Workspace", "The directory the agent works in",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("cloned", "Cloned",
	              "The workspace is a checkout this session made, and is "
	              "disposable", VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_NONE),

	/* What it is about, when it is about something. */
	VENTURE_FIELD_REF("repo-id", "Repository", NULL, "forge_repo",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("ticket-id", "Ticket", NULL, "ticket",
	                  VENTURE_COLUMN_FLAG_NONE),

	VENTURE_FIELD("user-id", "User", "Who opened it", VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("started-at", "Started", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("last-activity-at", "Last activity", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("turns", "Turns", NULL, VENTURE_FIELD_KIND_INTEGER,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("input-tokens", "Input tokens", NULL,
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("output-tokens", "Output tokens", NULL,
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("cost", "Cost", "What the provider charged"),
	VENTURE_FIELD_TEXT("failure-reason", "Error", "Why it stopped, when it did")
};

VENTURE_DEFINE_ENTITY(VentureAgentSession, venture_agent_session,
                      venture_agent_session_fields)

/*
 * One turn of a session: what was asked, or what came back.
 *
 * Split from the session rather than appended to a log field because a
 * turn is the unit everything cares about -- replaying it to the model,
 * showing it in the page, counting what a session cost -- and a text
 * column that four things parse is a text column that three of them
 * parse slightly differently.
 */
static const VentureFieldDecl venture_agent_turn_fields[] = {
	VENTURE_FIELD_REF("session-id", "Session", NULL, "agent_session",
	                  VENTURE_COLUMN_FLAG_NOT_NULL |
	                  VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("role", "Role", NULL, venture_chat_role_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("body", "Body", NULL)
};

VENTURE_DEFINE_ENTITY(VentureAgentTurn, venture_agent_turn,
                      venture_agent_turn_fields)

/* ==========================================================================
 * Access
 * ========================================================================== */

static const VentureFieldDecl venture_user_fields[] = {
	VENTURE_FIELD("username", "Username", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE |
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("email", "Email", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("display-name", "Display name", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_ENUM("role", "Role", NULL, venture_user_role_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	/* Marked sensitive, so it is omitted from every serialisation that
	 * is not explicitly persisting to storage. */
	VENTURE_FIELD("password-hash", "Password", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("last-login-at", "Last login", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	/*
	 * Sessions issued before this instant are refused. Signing out sets
	 * it, which is what makes signing out mean something: the cookie is
	 * stateless, so without a cut-off the only thing logout does is ask
	 * the browser nicely to forget it.
	 */
	VENTURE_FIELD("sessions-invalidated-at", "Sessions invalidated", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("timezone", "Timezone", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VentureUser, venture_user, venture_user_fields)

gboolean
venture_user_set_password(
	VentureUser	 *self,
	const gchar	 *password,
	guint		  iterations,
	GError		**error
){
	g_autofree gchar *hash = NULL;

	g_return_val_if_fail(VENTURE_IS_USER(self), FALSE);
	g_return_val_if_fail(NULL != password, FALSE);

	hash = venture_hash_password(password, iterations, error);

	if (NULL == hash)
		return FALSE;

	g_object_set(self, "password-hash", hash, NULL);

	return TRUE;
}

gboolean
venture_user_check_password(
	VentureUser	*self,
	const gchar	*password
){
	g_autofree gchar *hash = NULL;
	gboolean active;

	g_return_val_if_fail(VENTURE_IS_USER(self), FALSE);

	if (NULL == password)
		return FALSE;

	g_object_get(self, "password-hash", &hash, "active", &active, NULL);

	/* A deactivated account fails authentication regardless of the
	 * password, and does so before the hash is even compared. */
	if (!active)
		return FALSE;

	if (venture_string_is_empty(hash))
		return FALSE;

	return venture_verify_password(password, hash);
}

static const VentureFieldDecl venture_api_token_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What this token is for"),
	/* Only the hash is stored. A leaked database therefore yields no
	 * usable tokens, the same property a password table needs. */
	VENTURE_FIELD("token-hash", "Token", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_SENSITIVE),
	/* The leading characters are stored in the clear so a token can be
	 * identified in a list and in logs without revealing it. */
	VENTURE_FIELD("prefix", "Prefix", "First characters, for identification",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("user-id", "User", NULL, "user",
	                  VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("role", "Role", NULL, venture_user_role_get_type,
	                   VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("scopes", "Scopes", "Comma-separated permissions",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("expires-at", "Expires", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("last-used-at", "Last used", NULL,
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("description", "Description", NULL),
	VENTURE_FIELD("membership-snapshot", "Minted memberships", "Organization authority at token creation", VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_SENSITIVE | VENTURE_COLUMN_FLAG_IMMUTABLE)
};

VENTURE_DEFINE_ENTITY(VentureApiToken, venture_api_token,
                      venture_api_token_fields)

gchar *
venture_api_token_generate(VentureApiToken *self)
{
	g_autofree gchar *secret = NULL;
	g_autofree gchar *hash = NULL;
	g_autofree gchar *prefix = NULL;
	g_autofree gchar *presented = NULL;

	g_return_val_if_fail(VENTURE_IS_API_TOKEN(self), NULL);

	/* 32 bytes of CSPRNG output is 256 bits, which is not brute forceable
	 * and leaves no reason to make it shorter. */
	secret = venture_generate_token(32);

	/* The token is hashed the same way a password is. The iteration count
	 * is low compared with a password because the secret is full entropy
	 * rather than something a human chose, so there is nothing to guess;
	 * the hashing is here to make a database leak useless, not to slow a
	 * dictionary attack that cannot work. */
	hash = venture_hash_password(secret, 100000, NULL);

	if (NULL == hash)
		return NULL;

	prefix = g_strndup(secret, 8);
	presented = g_strdup_printf("vk_%s", secret);

	g_object_set(self,
	             "token-hash", hash,
	             "prefix", prefix,
	             "active", TRUE,
	             NULL);

	return g_steal_pointer(&presented);
}

gboolean
venture_api_token_matches(
	VentureApiToken	*self,
	const gchar	*presented
){
	g_autofree gchar *hash = NULL;
	g_autoptr(GDateTime) expires_at = NULL;
	g_autoptr(GDateTime) now = NULL;
	const gchar *secret;
	gboolean active;

	g_return_val_if_fail(VENTURE_IS_API_TOKEN(self), FALSE);

	if (NULL == presented)
		return FALSE;

	g_object_get(self,
	             "token-hash", &hash,
	             "active", &active,
	             "expires-at", &expires_at,
	             NULL);

	if (!active || venture_string_is_empty(hash))
		return FALSE;

	if (NULL != expires_at)
	{
		now = venture_time_now();

		if (g_date_time_compare(now, expires_at) >= 0)
			return FALSE;
	}

	/* Accept the token with or without its display prefix, so a client
	 * that stored the value verbatim and one that stripped it both work. */
	secret = g_str_has_prefix(presented, "vk_") ? (presented + 3) : presented;

	return venture_verify_password(secret, hash);
}

static const VentureFieldDecl venture_audit_entry_fields[] = {
	VENTURE_FIELD_ENUM("action", "Action", NULL,
	                   venture_audit_action_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("actor-kind", "Actor kind",
	                   "Whether a person, the AI or an automation did it",
	                   venture_actor_kind_get_type,
	                   VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("actor", "Actor", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("target-type", "Target type", NULL,
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("target-id", "Target ID", NULL,
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("target-label", "Target", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("diff", "Change", "The change, as JSON",
	              VENTURE_FIELD_KIND_JSON, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("occurred-at", "When", NULL, VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("source", "Source", "Which subsystem recorded it",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	/* The prompt that caused an AI-driven change is retained, because
	 * "why did this number change" is unanswerable without it. */
	VENTURE_FIELD_TEXT("prompt", "Prompt",
	                   "The instruction that caused an AI-driven change"),
	VENTURE_FIELD("request-id", "Request", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED),
	/* Set only for a change that went through the confirmation queue, so
	 * "which of these did somebody actually approve" is a filter rather
	 * than an inference from the actor kind. */
	VENTURE_FIELD("approved-by", "Approved by",
	              "Who approved a staged change; empty for a direct write",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("ip-address", "Address", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY(VentureAuditEntry, venture_audit_entry,
                      venture_audit_entry_fields)

VentureAuditEntry *
venture_audit_entry_new_for_change(
	VentureAuditAction	 action,
	VentureActorKind	 actor_kind,
	const gchar		*actor,
	VentureEntity		*target,
	JsonNode		*diff
){
	VentureAuditEntry *entry;
	g_autoptr(GDateTime) now = NULL;

	entry = venture_audit_entry_new();
	now = venture_time_now();

	g_object_set(entry,
	             "action", action,
	             "actor-kind", actor_kind,
	             "actor", actor,
	             "occurred-at", now,
	             NULL);

	if (NULL != target)
	{
		g_autofree gchar *label = NULL;

		label = venture_entity_get_display_name(target);

		g_object_set(entry,
		             "target-type", venture_entity_get_entity_name(target),
		             "target-id", venture_entity_get_id(target),
		             "target-label", label,
		             NULL);

		/* The audit entry belongs to the same organisation as the
		 * record it describes, so an organisation-scoped query sees
		 * its own history and nobody else's. */
		venture_entity_set_organization_id(VENTURE_ENTITY(entry),
			venture_entity_get_organization_id(target));
	}

	if (NULL != diff)
	{
		g_autofree gchar *text = NULL;

		text = venture_json_to_string(diff, FALSE);
		g_object_set(entry, "diff", text, NULL);
	}

	return entry;
}
