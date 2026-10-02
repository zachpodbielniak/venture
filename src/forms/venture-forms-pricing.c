/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture-forms-private.h"
#include <math.h>
#include <string.h>

#define FORMS_PRICE_MAX 100
#define FORMS_QUANTITY_MAX 1000000

JsonObject *venture_forms_payment(GPtrArray *fields)
{
	return fields != NULL && fields->len > 0 ? ((VentureFormsField *)g_ptr_array_index(fields, 0))->payment : NULL;
}

static gboolean price_refuse(GError **error, const gchar *message)
{
	venture_set_error_validation(error, "Form price", "%s", message);
	return FALSE;
}

static const gchar *price_text(JsonObject *object, const gchar *key)
{
	JsonNode *node = json_object_get_member(object, key);
	return node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}

static gboolean price_choice_exists(const VentureFormsField *field, const gchar *id)
{
	guint i;
	for (i = 0; i < field->choices->len; i++)
		if (g_strcmp0(((VentureFormsChoice *)g_ptr_array_index(field->choices, i))->id, id) == 0) return TRUE;
	return FALSE;
}

static gboolean price_line_valid(GPtrArray *fields, JsonObject *line, const gchar *currency, GError **error)
{
	const gchar *key = price_text(line, "key"), *name = price_text(line, "name");
	const gchar *choice = price_text(line, "choice_field"), *id = price_text(line, "choice_id"), *quantity = price_text(line, "quantity_field");
	JsonNode *product = json_object_get_member(line, "product_id");
	g_autoptr(VentureMoney) money = NULL;
	const VentureFormsField *field;
	if (!venture_forms_key_valid(key) || venture_string_is_empty(name) || strlen(name) > 512 || !g_utf8_validate(name, -1, NULL) ||
	    choice == NULL || id == NULL || quantity == NULL || product == NULL || !JSON_NODE_HOLDS_VALUE(product) ||
	    json_node_get_value_type(product) != G_TYPE_INT64 || json_node_get_int(product) <= 0)
		return price_refuse(error, "use a stable key, bounded description and a product in this organization");
	{
		JsonNode *unit = json_object_get_member(line, "unit_price"), *amount, *exponent;
		JsonObject *object;
		if (unit == NULL || !JSON_NODE_HOLDS_OBJECT(unit)) return price_refuse(error, "a published price needs an exact monetary object");
		object = json_node_get_object(unit); amount = json_object_get_member(object, "amount"); exponent = json_object_get_member(object, "exponent");
		if (amount == NULL || !JSON_NODE_HOLDS_VALUE(amount) || json_node_get_value_type(amount) != G_TYPE_INT64 ||
		    price_text(object, "currency") == NULL || exponent == NULL || !JSON_NODE_HOLDS_VALUE(exponent) || json_node_get_value_type(exponent) != G_TYPE_INT64)
			return price_refuse(error, "published money requires integer amount/exponent and an ISO currency");
	}
	money = venture_money_from_json(json_object_get_member(line, "unit_price"), NULL, error);
	if (money == NULL) return FALSE;
	if (venture_money_get_amount(money) < 0 || !venture_currency_is_iso(venture_money_get_currency(money)) ||
	    (currency != NULL && g_strcmp0(currency, venture_money_get_currency(money)) != 0))
		return price_refuse(error, "prices must be non-negative exact money in one ISO currency");
	if (!venture_string_is_empty(choice))
	{
		field = venture_forms_definition_find(fields, choice);
		if (field == NULL || field->sensitive || field->group_key != NULL || !venture_forms_kind_has_choices(field->kind) || !price_choice_exists(field, id))
			return price_refuse(error, "a conditional price must name a non-sensitive, non-repeated choice question and its stable choice ID");
	}
	else if (!venture_string_is_empty(id)) return price_refuse(error, "a choice ID requires its question key");
	if (!venture_string_is_empty(quantity))
	{
		field = venture_forms_definition_find(fields, quantity);
		if (field == NULL || field->sensitive || field->group_key != NULL || field->kind != VENTURE_FORM_FIELD_NUMBER)
			return price_refuse(error, "quantity must name a non-sensitive number question outside repeating groups");
	}
	return TRUE;
}

gboolean venture_forms_price_restore(GPtrArray *fields, JsonObject *payment, GError **error)
{
	JsonNode *flag = json_object_get_member(payment, "enabled"), *node = json_object_get_member(payment, "lines");
	const gchar *currency = price_text(payment, "currency"), *name = price_text(payment, "name_field"), *email = price_text(payment, "email_field");
	JsonArray *lines;
	guint i, j;
	if (flag == NULL || !JSON_NODE_HOLDS_VALUE(flag) || json_node_get_value_type(flag) != G_TYPE_BOOLEAN ||
	    node == NULL || !JSON_NODE_HOLDS_ARRAY(node) || currency == NULL || name == NULL || email == NULL)
		return price_refuse(error, "the published payment definition is malformed");
	lines = json_node_get_array(node);
	if (json_array_get_length(lines) > FORMS_PRICE_MAX) return price_refuse(error, "at most 100 price lines per form");
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		JsonNode *entry = json_array_get_element(lines, i);
		JsonObject *line;
		if (!JSON_NODE_HOLDS_OBJECT(entry)) return price_refuse(error, "the published price line is malformed");
		line = json_node_get_object(entry);
		if (!price_line_valid(fields, line, currency, error)) return FALSE;
		for (j = 0; j < i; j++)
			if (g_strcmp0(price_text(line, "key"), price_text(json_array_get_object_element(lines, j), "key")) == 0)
				return price_refuse(error, "price keys must be unique within a form");
	}
	if (json_node_get_boolean(flag))
	{
		const VentureFormsField *name_field = venture_forms_definition_find(fields, name), *email_field = venture_forms_definition_find(fields, email);
		if (json_array_get_length(lines) == 0 || name_field == NULL || email_field == NULL ||
		    name_field->kind != VENTURE_FORM_FIELD_SHORT_TEXT || email_field->kind != VENTURE_FORM_FIELD_EMAIL ||
		    name_field->sensitive || email_field->sensitive || !name_field->required || !email_field->required ||
		    name_field->group_key != NULL || email_field->group_key != NULL)
			return price_refuse(error, "paid forms need at least one price and required non-sensitive name/email questions");
	}
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		g_clear_pointer(&field->payment, json_object_unref); field->payment = json_object_ref(payment);
	}
	return TRUE;
}

gboolean venture_forms_price_validate(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error)
{
	g_autoptr(VentureEntity) form = NULL, product = NULL;
	g_autofree gchar *key = venture_forms_get_string(entity, "key"), *name = venture_forms_get_string(entity, "name");
	g_autoptr(VentureMoney) unit = NULL;
	(void)data;
	g_object_get(entity, "unit-price", &unit, NULL);
	if (!venture_forms_key_valid(key) || venture_string_is_empty(name) || strlen(name) > 512 || !g_utf8_validate(name, -1, NULL) ||
	    unit == NULL || venture_money_get_amount(unit) < 0 || !venture_currency_is_iso(venture_money_get_currency(unit)))
		return price_refuse(error, "use a stable key, bounded description and non-negative exact money in an ISO currency");
	if (previous != NULL)
	{
		g_autofree gchar *old = venture_forms_get_string(previous, "key");
		if (g_strcmp0(key, old) != 0 || venture_forms_get_int(previous, "form-id") != venture_forms_get_int(entity, "form-id"))
			return price_refuse(error, "a price key and its form cannot change");
	}
	form = venture_database_get(database, VENTURE_TYPE_FORM, venture_forms_get_int(entity, "form-id"), NULL);
	product = venture_database_get(database, VENTURE_TYPE_PRODUCT, venture_forms_get_int(entity, "product-id"), NULL);
	if (form == NULL || product == NULL || venture_entity_get_organization_id(form) != venture_entity_get_organization_id(entity) ||
	    venture_entity_get_organization_id(product) != venture_entity_get_organization_id(entity))
		return price_refuse(error, "the form and product must belong to this organization");
	return TRUE;
}

gboolean venture_forms_price_load(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_PRICE);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(JsonObject) payment = json_object_new();
	g_autofree gchar *currency = NULL, *name = venture_forms_get_string(form, "payment-name-field"), *email = venture_forms_get_string(form, "payment-email-field");
	JsonArray *lines = json_array_new();
	guint i;
	json_object_set_array_member(payment, "lines", lines);
	if (venture_forms_get_bool(form, "payment-enabled"))
	{
		g_autofree gchar *origin = venture_forms_get_string(form, "public-origin");
		if (!venture_forms_public_origin_valid(origin, error)) return FALSE;
		if (venture_forms_get_bool(form, "double-opt-in"))
			return price_refuse(error, "paid intake and double opt-in signup use separate forms; payment is not marketing permission");
	}

	json_object_set_boolean_member(payment, "enabled", venture_forms_get_bool(form, "payment-enabled"));
	json_object_set_string_member(payment, "name_field", venture_string_is_empty(name) ? "name" : name);
	json_object_set_string_member(payment, "email_field", venture_string_is_empty(email) ? "email" : email);
	venture_query_set_organization(query, venture_entity_get_organization_id(form));
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, FORMS_PRICE_MAX + 1);
	rows = venture_database_find(database, query, error);
	if (rows == NULL) return FALSE;
	if (rows->len > FORMS_PRICE_MAX) return price_refuse(error, "at most 100 price lines per form");
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(VentureMoney) unit = NULL;
		g_autofree gchar *key = venture_forms_get_string(row, "key"), *description = venture_forms_get_string(row, "name");
		g_autofree gchar *choice = venture_forms_get_string(row, "choice-field"), *id = venture_forms_get_string(row, "choice-id"), *quantity = venture_forms_get_string(row, "quantity-field");
		JsonObject *line = json_object_new();
		g_object_get(row, "unit-price", &unit, NULL);
		if (unit == NULL) { json_object_unref(line); return price_refuse(error, "each price needs a unit amount"); }
		if (currency == NULL) currency = g_strdup(venture_money_get_currency(unit));
		json_object_set_string_member(line, "key", key); json_object_set_string_member(line, "name", description);
		json_object_set_string_member(line, "choice_field", choice != NULL ? choice : "");
		json_object_set_string_member(line, "choice_id", id != NULL ? id : "");
		json_object_set_string_member(line, "quantity_field", quantity != NULL ? quantity : "");
		json_object_set_int_member(line, "product_id", venture_forms_get_int(row, "product-id"));
		json_object_set_member(line, "unit_price", venture_money_to_json(unit));
		json_array_add_object_element(lines, line);
	}
	json_object_set_string_member(payment, "currency", currency != NULL ? currency : "");
	return venture_forms_price_restore(fields, payment, error);
}

static gboolean price_selected(JsonNode *answer, const gchar *id)
{
	guint i;
	if (answer == NULL) return FALSE;
	if (JSON_NODE_HOLDS_VALUE(answer) && json_node_get_value_type(answer) == G_TYPE_STRING) return g_strcmp0(json_node_get_string(answer), id) == 0;
	if (JSON_NODE_HOLDS_ARRAY(answer))
		for (i = 0; i < json_array_get_length(json_node_get_array(answer)); i++)
			if (price_selected(json_array_get_element(json_node_get_array(answer), i), id)) return TRUE;
	return FALSE;
}

/* The caller supplies only validated visible answers. Money never passes
 * through a floating-point quantity: only bounded exact whole counts do. */
VentureMoney *venture_forms_price_total(GPtrArray *fields, JsonObject *answers, JsonArray **invoice_lines, GError **error)
{
	JsonObject *payment = venture_forms_payment(fields);
	JsonArray *lines;
	g_autoptr(JsonArray) computed = json_array_new();
	g_autoptr(VentureMoney) total = NULL;
	guint i;
	if (payment == NULL || !json_object_get_boolean_member(payment, "enabled"))
		return price_refuse(error, "payment is not enabled in this published version"), NULL;
	lines = json_object_get_array_member(payment, "lines");
	for (i = 0; i < json_array_get_length(lines); i++)
	{
		JsonObject *line = json_array_get_object_element(lines, i), *output;
		const gchar *choice = price_text(line, "choice_field"), *quantity_key = price_text(line, "quantity_field");
		g_autoptr(VentureMoney) unit = NULL, amount = NULL, sum = NULL;
		gint64 quantity = 1;
		if (!venture_string_is_empty(choice) && !price_selected(json_object_get_member(answers, choice), price_text(line, "choice_id"))) continue;
		if (!venture_string_is_empty(quantity_key))
		{
			JsonNode *node = json_object_get_member(answers, quantity_key);
			gdouble number;
			if (node == NULL) continue;
			if (!JSON_NODE_HOLDS_VALUE(node) || (json_node_get_value_type(node) != G_TYPE_INT64 && json_node_get_value_type(node) != G_TYPE_DOUBLE))
				return price_refuse(error, "quantity must be a whole number from 0 through 1000000"), NULL;
			number = json_node_get_double(node);
			if (!isfinite(number) || number < 0 || number > FORMS_QUANTITY_MAX || floor(number) != number)
				return price_refuse(error, "quantity must be a whole number from 0 through 1000000"), NULL;
			quantity = (gint64)number;
		}
		if (quantity == 0) continue;
		unit = venture_money_from_json(json_object_get_member(line, "unit_price"), NULL, error);
		if (unit == NULL) return NULL;
		amount = venture_money_multiply_int(unit, quantity, error); if (amount == NULL) return NULL;
		sum = total != NULL ? venture_money_add(total, amount, error) : venture_money_copy(amount); if (sum == NULL) return NULL;
		g_clear_pointer(&total, venture_money_free); total = g_steal_pointer(&sum);
		output = json_object_new();
		json_object_set_string_member(output, "key", price_text(line, "key"));
		json_object_set_string_member(output, "description", price_text(line, "name"));
		json_object_set_int_member(output, "product_id", json_object_get_int_member(line, "product_id"));
		json_object_set_int_member(output, "quantity", quantity);
		json_object_set_member(output, "amount", venture_money_to_json(amount)); json_array_add_object_element(computed, output);
	}
	if (total == NULL || venture_money_get_amount(total) <= 0) return price_refuse(error, "choose an order with a positive total"), NULL;
	if (invoice_lines != NULL) *invoice_lines = g_steal_pointer(&computed);
	return g_steal_pointer(&total);
}

/* The nonce identifies one intake, unlike the shared second-resolution fill
 * ticket. Authentication prevents callers choosing another intake's identity. */
gchar *venture_forms_payment_nonce(VentureEntity *form)
{
	g_autofree gchar *id = g_uuid_string_random();
	g_autofree gchar *key = venture_forms_get_string(form, "ticket-key");
	g_autofree gchar *mac = NULL;
	if (venture_string_is_empty(key)) return NULL;
	mac = g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)key, strlen(key), id, -1);
	return g_strdup_printf("%s.%s", id, mac);
}

gboolean venture_forms_payment_nonce_valid(VentureEntity *form, const gchar *nonce)
{
	g_auto(GStrv) parts = NULL;
	g_autofree gchar *key = venture_forms_get_string(form, "ticket-key"), *mac = NULL;
	if (nonce == NULL || strlen(nonce) != 101 || venture_string_is_empty(key)) return FALSE;
	parts = g_strsplit(nonce, ".", 3);
	if (g_strv_length(parts) != 2 || !g_uuid_string_is_valid(parts[0])) return FALSE;
	mac = g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)key, strlen(key), parts[0], -1);
	return venture_constant_time_equal(mac, parts[1]);
}
