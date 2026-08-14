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
	              VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY(VentureOrganization, venture_organization,
                      venture_organization_fields)

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
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VentureVenture, venture_venture, venture_venture_fields)

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
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VentureProduct, venture_product, venture_product_fields)

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
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VentureInventoryItem, venture_inventory_item,
                      venture_inventory_item_fields)

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

VENTURE_DEFINE_ENTITY(VentureInventoryTxn, venture_inventory_txn,
                      venture_inventory_txn_fields)

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
	/* The platform's own order identifier, indexed and unique so that
	 * re-importing a marketplace export cannot duplicate sales. */
	VENTURE_FIELD("external-id", "Order ID",
	              "The platform's order identifier; prevents double import",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
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
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VentureSale, venture_sale, venture_sale_fields)

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
	VENTURE_FIELD("receipt-url", "Receipt", NULL, VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("external-id", "External ID",
	              "Bank or card transaction identifier; prevents double import",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("reimbursable", "Reimbursable", NULL,
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VentureExpense, venture_expense, venture_expense_fields)

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
	              VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY(VentureAccount, venture_account, venture_account_fields)

static const VentureFieldDecl venture_ledger_entry_fields[] = {
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

VENTURE_DEFINE_ENTITY(VentureLedgerEntry, venture_ledger_entry,
                      venture_ledger_entry_fields)

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
	VENTURE_FIELD_TEXT("description", "Description", NULL),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VentureTaxCategory, venture_tax_category,
                      venture_tax_category_fields)

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
	              VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY(VentureCompany, venture_company, venture_company_fields)

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
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VentureContact, venture_contact, venture_contact_fields)

static const VentureFieldDecl venture_interaction_fields[] = {
	VENTURE_FIELD_REF("contact-id", "Contact", NULL, "contact",
	                  VENTURE_COLUMN_FLAG_NOT_NULL),
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
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};

VENTURE_DEFINE_ENTITY(VentureInteraction, venture_interaction,
                      venture_interaction_fields)

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
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY(VentureDeal, venture_deal, venture_deal_fields)

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

VENTURE_DEFINE_ENTITY(VentureCampaign, venture_campaign, venture_campaign_fields)

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

VENTURE_DEFINE_ENTITY(VentureNewsletter, venture_newsletter,
                      venture_newsletter_fields)

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

VENTURE_DEFINE_ENTITY(VentureSubscriber, venture_subscriber,
                      venture_subscriber_fields)

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

VENTURE_DEFINE_ENTITY(VenturePost, venture_post, venture_post_fields)

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

VENTURE_DEFINE_ENTITY(VentureIdea, venture_idea, venture_idea_fields)

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

VENTURE_DEFINE_ENTITY(VentureResearchNote, venture_research_note,
                      venture_research_note_fields)

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
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
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
	                   "What was actually done about it")
};

VENTURE_DEFINE_ENTITY(VentureTicket, venture_ticket, venture_ticket_fields)

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

VENTURE_DEFINE_ENTITY(VentureTicketComment, venture_ticket_comment,
                      venture_ticket_comment_fields)

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
	              VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY(VentureDocument, venture_document,
                      venture_document_fields)

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
	VENTURE_FIELD_TEXT("description", "Description", NULL)
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
