/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

static const VentureFieldDecl tax_filing_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "Jurisdiction and period this pack covers"),
	VENTURE_FIELD("country", "Country", "ISO country the adapter is selected by",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("jurisdiction", "Jurisdiction", "Filing jurisdiction, for example US-NY",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("fiscal-period-id", "Fiscal period", NULL, "fiscal_period",
		VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("period-start", "Period start", "Inclusive",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("period-end", "Period end", "Exclusive",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("adapter", "Adapter", "Registry key used to prepare this pack",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("rule-id", "Rule id", "Versioned adapter rule recorded on the pack",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("status", "Status",
		"draft, reviewed, submitted or acknowledged; VentureTaxFilingService owns transitions"),
	VENTURE_FIELD("json-pack", "JSON pack", "Exact submitted JSON bytes",
		VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("csv-pack", "CSV pack", "Exact submitted CSV bytes",
		VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("acknowledgment-id", "Acknowledgment", "Id returned after submit",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("amended-from-id", "Amended from", "Prior pack kept on amendment",
		"tax_filing", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("taxable", "Taxable", "Taxable amount in the pack"),
	VENTURE_FIELD_MONEY("tax", "Tax", "Tax amount in the pack"),
	VENTURE_FIELD_NAME("currency", "Currency", "ISO 4217 book currency")
};
VENTURE_DEFINE_ENTITY(VentureTaxFiling, venture_tax_filing, tax_filing_fields)

static const VentureFieldDecl contractor_form_fields[] = {
	VENTURE_FIELD_REF("vendor-id", "Vendor", "Supplier company this TIN belongs to", "company",
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("tin", "TIN", "Stored; never logged or generically exported",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("tin-type", "TIN type", "EIN or SSN",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("legal-name", "Legal name", NULL,
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("active", "Active", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureContractorTaxForm, venture_contractor_tax_form, contractor_form_fields)

static const VentureFieldDecl contractor_pack_fields[] = {
	VENTURE_FIELD_REF("vendor-id", "Vendor", NULL, "company", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("form-id", "Tax form", NULL, "contractor_tax_form", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("year", "Year", "Calendar year", VENTURE_FIELD_KIND_INTEGER,
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("form-kind", "Form", "1099-NEC", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("filing-key", "Filing key", "Derived vendor:year:1099-NEC",
		VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_IMMUTABLE),
	VENTURE_FIELD_NAME("status", "Status",
		"draft, reviewed, approved or exported; VentureTaxFilingService owns transitions"),
	VENTURE_FIELD_MONEY("amount", "Paid", "Calendar-year payments included on the form"),
	VENTURE_FIELD("csv-pack", "CSV pack", "Frozen 1099-NEC CSV, including TIN",
		VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_SENSITIVE)
};
VENTURE_DEFINE_ENTITY(VentureContractorTaxPack, venture_contractor_tax_pack, contractor_pack_fields)
