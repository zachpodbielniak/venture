/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

static gboolean
type_on(const gchar *name)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(), name) != G_TYPE_INVALID;
}

static void
add_action(JsonBuilder *builder, const gchar *kind, const gchar *title, gint64 count,
	const gchar *href, const gchar *reason)
{
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "kind");
	json_builder_add_string_value(builder, kind);
	json_builder_set_member_name(builder, "title");
	json_builder_add_string_value(builder, title);
	json_builder_set_member_name(builder, "count");
	json_builder_add_int_value(builder, count);
	json_builder_set_member_name(builder, "href");
	json_builder_add_string_value(builder, href);
	json_builder_set_member_name(builder, "reason");
	json_builder_add_string_value(builder, reason);
	json_builder_end_object(builder);
}

static gint64
count_filter(VentureDatabase *db, GType type, gint64 org, const gchar *field, const gchar *value, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	if (type == G_TYPE_INVALID)
		return 0;
	query = venture_query_new(type);
	venture_query_set_organization(query, org);
	if (field != NULL)
		venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_EQ, value, NULL);
	return venture_database_count(db, query, error);
}

JsonNode *
venture_accounting_home(VentureContext *context, gint64 organization_id, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	VentureDatabase *db;
	gint64 org;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	db = venture_context_get_database(context);
	org = organization_id > 0 ? organization_id : venture_context_get_default_organization_id(context);
	json_builder_begin_array(builder);
	if (type_on("bank_transaction"))
	{
		gint64 n = count_filter(db, VENTURE_TYPE_BANK_TRANSACTION, org, "state", "unmatched", error);
		g_autofree gchar *reason = g_strdup_printf(
			"%s unmatched bank line(s) have no matching receipt, payment or expense yet",
			n == 0 ? "No" : "There are");
		if (n < 0) return NULL;
		if (n == 0)
		{
			g_free(reason);
			reason = g_strdup("Every imported bank line is matched or excluded");
		}
		else
		{
			g_free(reason);
			reason = g_strdup_printf("%" G_GINT64_FORMAT
				" unmatched bank line(s) have no matching receipt, payment or expense yet", n);
		}
		add_action(builder, "unmatched_bank", "Unmatched bank", n, "/banking", reason);
	}
	if (type_on("invoice"))
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE);
		g_autoptr(GPtrArray) rows = NULL;
		g_autoptr(GDateTime) now = venture_time_now();
		gint64 n = 0;
		guint i;
		venture_query_set_organization(query, org);
		rows = venture_database_find(db, query, error);
		if (rows == NULL) return NULL;
		if (rows != NULL)
		{
			for (i = 0; i < rows->len; i++)
			{
				gint status = 0;
				g_autoptr(GDateTime) due = NULL;
				g_object_get(g_ptr_array_index(rows, i), "status", &status, "due-at", &due, NULL);
				if ((status == VENTURE_INVOICE_STATUS_SENT || status == VENTURE_INVOICE_STATUS_PARTIALLY_PAID) &&
					due != NULL && g_date_time_compare(due, now) < 0)
					n++;
			}
		}
		{
			g_autofree gchar *reason = n == 0
				? g_strdup("No sent invoices are past their due date")
				: g_strdup_printf("%" G_GINT64_FORMAT " sent invoice(s) are past due and still open", n);
			add_action(builder, "overdue_invoices", "Overdue invoices", n, "/e/invoice", reason);
		}
	}
	if (type_on("vendor_bill"))
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_VENDOR_BILL);
		g_autoptr(GPtrArray) rows = NULL;
		gint64 n = 0;
		guint i;
		venture_query_set_organization(query, org);
		rows = venture_database_find(db, query, error);
		if (rows == NULL) return NULL;
		if (rows != NULL)
		{
			for (i = 0; i < rows->len; i++)
			{
				g_autofree gchar *status = NULL;
				g_object_get(g_ptr_array_index(rows, i), "status", &status, NULL);
				if (g_strcmp0(status, "approved") == 0 || g_strcmp0(status, "partially_paid") == 0)
					n++;
			}
		}
		{
			g_autofree gchar *reason = n == 0
				? g_strdup("No approved supplier bills are waiting to be paid")
				: g_strdup_printf("%" G_GINT64_FORMAT " approved supplier bill(s) are waiting to be paid", n);
			add_action(builder, "bills_to_pay", "Bills to pay", n, "/payables", reason);
		}
	}
	if (type_on("close_task"))
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CLOSE_TASK);
		gint64 n;
		g_autofree gchar *reason = NULL;
		venture_query_set_organization(query, org);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, "open", NULL);
		n = venture_database_count(db, query, error);
		if (n < 0) return NULL;
		reason = n == 0
			? g_strdup("The close checklist has no open preparer or reviewer tasks")
			: g_strdup_printf("%" G_GINT64_FORMAT " close checklist task(s) are still open", n);
		add_action(builder, "close_checklist", "Close checklist", n, "/close", reason);
	}
	if (type_on("capture_item"))
	{
		gint64 n = count_filter(db, VENTURE_TYPE_CAPTURE_ITEM, org, "status", "inbox", error);
		g_autofree gchar *reason = n == 0
			? g_strdup("The capture inbox is empty")
			: g_strdup_printf("%" G_GINT64_FORMAT " captured receipt(s) or supplier invoice(s) are waiting to become expenses or bills", n);
		if (n < 0) return NULL;
		add_action(builder, "capture_inbox", "Capture inbox", n, "/capture", reason);
	}
	json_builder_end_array(builder);
	return json_builder_get_root(builder);
}
