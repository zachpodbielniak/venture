/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
#include <math.h>

struct _VentureDocumentService
{
	GObject parent_instance;
	VentureDatabase *database;
};
G_DEFINE_FINAL_TYPE(VentureDocumentService, venture_document_service, G_TYPE_OBJECT)

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "VentureDocumentService: %s", message);
	return FALSE;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_DOCUMENT_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureDocumentService *self = VENTURE_DOCUMENT_SERVICE(object);
	if (id == 1)
	{
		self->database = g_value_get_object(value);
		if (self->database != NULL)
			g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	}
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
finalize(GObject *object)
{
	VentureDocumentService *self = VENTURE_DOCUMENT_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_document_service_parent_class)->finalize(object);
}

static void
venture_document_service_class_init(VentureDocumentServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;
	g_object_class_install_property(object_class, 1,
		g_param_spec_object("database", "Database", "Owning database", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_document_service_init(VentureDocumentService *self)
{
	(void)self;
}

VentureDocumentService *
venture_document_service_get(VentureDatabase *database)
{
	VentureDocumentService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-document-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_DOCUMENT_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-document-service", self, g_object_unref);
	}
	return self;
}

static gchar *
next_number(VentureDocumentService *self, gint64 org, GType type, const gchar *prefix)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	gint64 count;
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	count = venture_database_count(self->database, query, NULL);
	return g_strdup_printf("%s%" G_GINT64_FORMAT, prefix, count + 1);
}

static JsonArray *
lines_of(JsonObject *spec, GError **error)
{
	JsonNode *node;
	JsonArray *lines;
	guint i;
	if (spec == NULL)
	{
		refuse(error, "a JSON object is required");
		return NULL;
	}
	node = json_object_get_member(spec, "lines");
	if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node) || json_array_get_length(json_node_get_array(node)) < 1)
	{
		refuse(error, "at least one line is required");
		return NULL;
	}
	lines = json_node_get_array(node);
	/* JSON is untrusted: typed accessors diagnose programmer errors on scalar rows. */
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		JsonNode *row = json_array_get_element(lines, i);
		if (!JSON_NODE_HOLDS_OBJECT(row))
		{
			refuse(error, "each line must be a JSON object");
			return NULL;
		}
	}
	return lines;
}

static gboolean
money_from_row(VentureEntity *line, JsonObject *row, GError **error)
{
	const gchar *price = venture_json_object_get_string(row, "unit_price", NULL);
	if (price == NULL)
		return refuse(error, "each line needs an exact unit_price");
	if (!venture_entity_set_field_from_string(line, "unit-price", price, error))
		return FALSE;
	g_object_set(line, "discount-percent", venture_json_object_get_int(row, "discount_percent", 0),
		"tax-percent", venture_json_object_get_int(row, "tax_percent", 0), NULL);
	return TRUE;
}

static gdouble
row_quantity(JsonObject *row)
{
	JsonNode *node = json_object_get_member(row, "quantity");
	if (node == NULL)
		return 1;
	if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64)
		return (gdouble)json_node_get_int(node);
	if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_DOUBLE)
		return json_node_get_double(node);
	return 0;
}

VentureEntity *
venture_document_service_compose_invoice(VentureDocumentService *self, gint64 organization_id,
	JsonObject *spec, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureInvoice) invoice = NULL;
	g_autofree gchar *number = NULL;
	g_autoptr(GDateTime) now = NULL;
	JsonArray *lines;
	guint i;
	gint64 due_days;
	g_return_val_if_fail(VENTURE_IS_DOCUMENT_SERVICE(self), NULL);
	lines = lines_of(spec, error);
	if (lines == NULL)
		return NULL;
	if (!venture_database_begin(self->database, error))
		return NULL;
	invoice = venture_invoice_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(invoice), organization_id);
	number = g_strdup(venture_json_object_get_string(spec, "number", ""));
	if (number == NULL || number[0] == '\0')
	{
		g_free(number);
		number = next_number(self, organization_id, VENTURE_TYPE_INVOICE, "INV-");
	}
	g_object_set(invoice, "number", number, "company-id",
		venture_json_object_get_int(spec, "company_id", 0),
		"contact-id", venture_json_object_get_int(spec, "contact_id", 0),
		"terms", venture_json_object_get_string(spec, "terms", ""),
		"notes", venture_json_object_get_string(spec, "notes", ""),
		"external-id", venture_json_object_get_string(spec, "external_id", ""), NULL);
	due_days = venture_json_object_get_int(spec, "due_days", 30);
	now = venture_time_now();
	if (due_days > 0)
	{
		g_autoptr(GDateTime) due = g_date_time_add_days(now, (gint)due_days);
		g_object_set(invoice, "due-at", due, NULL);
	}
	if (!venture_database_save(self->database, VENTURE_ENTITY(invoice), actor, error))
		goto fail;
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		JsonObject *row = json_array_get_object_element(lines, i);
		g_autoptr(VentureInvoiceLine) line = venture_invoice_line_new();
		const gchar *description = venture_json_object_get_string(row, "description", NULL);
		gdouble quantity = row_quantity(row);
		if (description == NULL || description[0] == '\0' || !isfinite(quantity) || quantity <= 0)
		{
			refuse(error, "each line needs a description and a positive quantity");
			goto fail;
		}
		venture_entity_set_organization_id(VENTURE_ENTITY(line), organization_id);
		g_object_set(line, "invoice-id", venture_entity_get_id(VENTURE_ENTITY(invoice)),
			"description", description, "quantity", quantity, "position", (gint64)(i + 1),
			"product-id", venture_json_object_get_int(row, "product_id", 0), NULL);
		if (!money_from_row(VENTURE_ENTITY(line), row, error) ||
			!venture_database_save(self->database, VENTURE_ENTITY(line), actor, error))
			goto fail;
	}
	if (venture_json_object_get_bool(spec, "send", FALSE) &&
		!venture_settlement_service_transition(venture_settlement_service_get(self->database),
			invoice, "sent", now, actor, error))
		goto fail;
	if (!venture_database_commit(self->database, error))
		goto fail;
	return VENTURE_ENTITY(g_steal_pointer(&invoice));
fail:
	venture_database_rollback(self->database);
	return NULL;
}

VentureEntity *
venture_document_service_compose_quote(VentureDocumentService *self, gint64 organization_id,
	JsonObject *spec, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureQuote) quote = NULL;
	g_autofree gchar *number = NULL;
	JsonArray *lines;
	guint i;
	gint64 quote_id;
	g_return_val_if_fail(VENTURE_IS_DOCUMENT_SERVICE(self), NULL);
	lines = lines_of(spec, error);
	if (lines == NULL)
		return NULL;
	if (!venture_database_begin(self->database, error))
		return NULL;
	quote = venture_quote_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(quote), organization_id);
	number = g_strdup(venture_json_object_get_string(spec, "number", ""));
	if (number == NULL || number[0] == '\0')
	{
		g_free(number);
		number = next_number(self, organization_id, VENTURE_TYPE_QUOTE, "Q-");
	}
	g_object_set(quote, "number", number, "company-id",
		venture_json_object_get_int(spec, "company_id", 0),
		"contact-id", venture_json_object_get_int(spec, "contact_id", 0),
		"currency", venture_json_object_get_string(spec, "currency", "USD"),
		"terms", venture_json_object_get_string(spec, "terms", ""),
		"notes", venture_json_object_get_string(spec, "notes", ""),
		"billing-mode", venture_json_object_get_string(spec, "billing_mode", ""), NULL);
	if (!venture_database_save(self->database, VENTURE_ENTITY(quote), actor, error))
		goto fail;
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		JsonObject *row = json_array_get_object_element(lines, i);
		g_autoptr(VentureQuoteLine) line = venture_quote_line_new();
		const gchar *description = venture_json_object_get_string(row, "description", NULL);
		gdouble requested = row_quantity(row);
		gint64 quantity;
		if (description == NULL || description[0] == '\0' || !isfinite(requested) ||
			requested <= 0 || requested >= (gdouble)G_MAXINT64 || floor(requested) != requested)
		{
			refuse(error, "each quote line needs a description and a positive whole quantity");
			goto fail;
		}
		quantity = (gint64)requested;
		venture_entity_set_organization_id(VENTURE_ENTITY(line), organization_id);
		g_object_set(line, "quote-id", venture_entity_get_id(VENTURE_ENTITY(quote)),
			"description", description, "quantity", quantity, "position", (gint64)(i + 1),
			"product-id", venture_json_object_get_int(row, "product_id", 0), NULL);
		if (!money_from_row(VENTURE_ENTITY(line), row, error) ||
			!venture_database_save(self->database, VENTURE_ENTITY(line), actor, error))
			goto fail;
	}
	quote_id = venture_entity_get_id(VENTURE_ENTITY(quote));
	if (venture_json_object_get_bool(spec, "send", FALSE))
	{
		g_autoptr(VentureEntity) action = VENTURE_ENTITY(venture_quote_action_new());
		g_autoptr(VentureEntity) current = venture_database_get(self->database, VENTURE_TYPE_QUOTE, quote_id, error);
		if (current == NULL)
			goto fail;
		venture_entity_set_organization_id(action, organization_id);
		g_object_set(action, "quote-id", quote_id, "action", "send",
			"expected-version", venture_entity_get_version(current), NULL);
		if (!venture_quote_service_execute(venture_database_get_quote_service(self->database),
			action, "manual", NULL, actor, error))
			goto fail;
		g_object_unref(quote);
		quote = VENTURE_QUOTE(venture_database_get(self->database, VENTURE_TYPE_QUOTE, quote_id, error));
		if (quote == NULL)
			goto fail;
	}
	/* Sending validates the complete quote; refusal must roll back its header and lines. */
	if (!venture_database_commit(self->database, error))
		goto fail;
	return VENTURE_ENTITY(g_steal_pointer(&quote));
fail:
	venture_database_rollback(self->database);
	return NULL;
}
