/* SPDX-License-Identifier: AGPL-3.0-or-later */

/* Only business data belongs in a migration. This list deliberately names
 * the writable fields instead of exposing arbitrary entity properties. */
typedef struct
{
	const gchar *file;
	const gchar *object;
	GType (*type)(void);
	const gchar *fields;
} ImportModule;

static const ImportModule import_modules[] = {
	{ "leads", "lead", venture_lead_get_type,
	  "name,company-name,email,phone,website,source,owner,status,score,unqualified-reason,first-seen-at,last-activity-at,notes" },
	{ "products", "product", venture_product_get_type,
	  "name,sku,description,category,subcategory,tags,genre,format" },
	{ "price_books", "price_book", venture_price_list_get_type, "name,currency" },
	{ "price_book_entries", "price_book_entry", venture_price_list_item_get_type,
	  "price-list-id,product-id,unit-price,min-quantity" },
	{ "quotes", "quote", venture_quote_get_type,
	  "number,company-id,contact-id,deal-id,status,issued-at,valid-until,currency,terms,notes,service-address" },
	{ "quote_lines", "quote_line", venture_quote_line_get_type,
	  "quote-id,product-id,description,quantity,unit-price,discount-percent,tax-percent,position" },
	{ "sales_orders", "sales_order", venture_sales_order_get_type,
	  "number,company-id,contact-id,status,currency,ordered-at,memo" },
	{ "sales_order_lines", "sales_order_line", venture_sales_order_line_get_type,
	  "sales-order-id,product-id,description,quantity,unit-price,is-service,position" },
	{ "invoices", "invoice", venture_invoice_get_type,
	  "number,company-id,contact-id,issued-at,due-at,terms,notes,owner,external-id,shipping-amount,tax-exempt,tax-exempt-reason" },
	{ "invoice_lines", "invoice_line", venture_invoice_line_get_type,
	  "invoice-id,product-id,description,quantity,unit-price,position,discount-percent,tax-percent" },
	{ "payments", "payment", venture_payment_get_type,
	  "customer-id,invoice-id,date,amount,method,reference,external-id" },
	{ "campaigns", "campaign", venture_campaign_get_type,
	  "name,status,channel,started-at,ended-at,budget,spend,revenue,impressions,clicks,conversions,url,notes" },
	{ "cases", "case", venture_ticket_get_type,
	  "title,description,status,priority,assignee,company-id,contact-id" },
	{ "vendors", "vendor", venture_company_get_type,
	  "name,legal-name,industry,website,email,phone,address,notes" },
	{ "users", "user", venture_user_get_type, "name,email" },
	{ "attachments", "attachment", venture_document_get_type, "title,mime-type" },
	{ "stage_history", "stage_history", venture_deal_stage_entry_get_type,
	  "deal-id,entered-at,by,note" }
};

static const ImportModule *
import_module(const gchar *file)
{
	guint i;
	for (i = 0; i < G_N_ELEMENTS(import_modules); i++)
		if (g_strcmp0(file, import_modules[i].file) == 0)
			return &import_modules[i];
	return NULL;
}

static gchar *
canonical_name(const gchar *name)
{
	gchar *copy = g_strdup(name);
	g_strdelimit(copy, "-", '_');
	return copy;
}

static gchar *
export_header(const gchar *source, const gchar *field)
{
	g_auto(GStrv) words = g_strsplit_set(field, "-_", -1);
	GString *header = g_string_new(NULL);
	guint i;
	for (i = 0; words[i] != NULL; i++)
	{
		if (i > 0 && g_strcmp0(source, "salesforce") != 0)
			g_string_append_c(header, ' ');
		if (words[i][0] != '\0')
			words[i][0] = g_ascii_toupper(words[i][0]);
		g_string_append(header, words[i]);
	}
	return g_string_free(header, FALSE);
}

static JsonObject *
module_default_columns(const gchar *source, const gchar *file)
{
	const ImportModule *module = import_module(file);
	g_auto(GStrv) fields = NULL;
	JsonObject *map;
	guint i;
	gboolean salesforce = g_strcmp0(source, "salesforce") == 0;
	gboolean hubspot = g_strcmp0(source, "hubspot") == 0;
	static const gchar *const common[] = {
		"first_name", "last_name", "company_name", "company_id", "contact_id", "deal_id",
		"owner", "currency", "description", "subject", "body", "number", "status",
		"price_book_id", "product_id", "quote_id", "sales_order_id", "invoice_id",
		"parent_object", "parent_id", "content_base64", "mime_type", "name", "stage",
		"occurred_at", "author", "issued_at", "status_at", "shipped_at", "accepted_by",
		"allocated_qty", "fulfilled_qty", "invoiced_qty", "accounting_source_id", NULL
	};
	if (module == NULL)
		return NULL;
	map = json_object_new();
	fields = g_strsplit(module->fields, ",", -1);
	for (i = 0; fields[i] != NULL; i++)
	{
		g_autofree gchar *canonical = canonical_name(fields[i]);
		g_autofree gchar *header = export_header(source, fields[i]);
		json_object_set_string_member(map, canonical, header);
	}
	for (i = 0; common[i] != NULL; i++)
	{
		g_autofree gchar *header = export_header(source, common[i]);
		if (!json_object_has_member(map, common[i]))
			json_object_set_string_member(map, common[i], header);
	}
	json_object_set_string_member(map, "id", salesforce ? "Id" : (hubspot ? "Record ID" : "Record Id"));
	json_object_set_string_member(map, "currency", salesforce ? "CurrencyIsoCode" : "Currency");
	json_object_set_string_member(map, "owner", salesforce ? "OwnerId" : "Owner");
	json_object_set_string_member(map, "company_id", salesforce ? "AccountId" : (hubspot ? "Associated Company IDs" : "Account Name.id"));
	json_object_set_string_member(map, "contact_id", salesforce ? "ContactId" : (hubspot ? "Associated Contact IDs" : "Contact Name.id"));
	json_object_set_string_member(map, "deal_id", salesforce ? "OpportunityId" : (hubspot ? "Associated Deal IDs" : "Deal Name.id"));
	json_object_set_string_member(map, "occurred_at", salesforce ? "CreatedDate" : (hubspot ? "Activity date" : "Created Time"));
	json_object_set_string_member(map, "stage", salesforce ? "StageName" : (hubspot ? "Deal Stage" : "Stage"));
	if (salesforce)
	{
		json_object_set_string_member(map, "product_id", "Product2Id");
		json_object_set_string_member(map, "price_list_id", "Pricebook2Id");
		json_object_set_string_member(map, "price_book_id", "Pricebook2Id");
	}
	if (g_strcmp0(file, "leads") == 0)
	{
		json_object_set_string_member(map, "company_name", "Company");
		json_object_set_string_member(map, "owner", salesforce ? "OwnerId" : (hubspot ? "Lead owner" : "Lead Owner"));
	}
	if (g_strcmp0(source, "zoho_crm") == 0)
	{
		if (g_strcmp0(file, "products") == 0)
			json_object_set_string_member(map, "name", "Product Name");
		if (g_strcmp0(file, "price_books") == 0)
			json_object_set_string_member(map, "name", "Price Book Name");
		if (g_strcmp0(file, "campaigns") == 0)
			json_object_set_string_member(map, "name", "Campaign Name");
		if (g_strcmp0(file, "vendors") == 0)
			json_object_set_string_member(map, "name", "Vendor Name");
	}
	return map;
}
