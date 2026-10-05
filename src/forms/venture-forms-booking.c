/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture-forms-private.h"
#include <string.h>

static const VentureFormsField *booking_field(GPtrArray *fields)
{
	guint i;
	for (i = 0; i < fields->len; i++)
	{
		const VentureFormsField *field = g_ptr_array_index(fields, i);
		if (field->kind == VENTURE_FORM_FIELD_BOOKING) return field;
	}
	return NULL;
}
static const gchar *booking_name(const VentureFormsField *field)
{
	return venture_string_is_empty(field->booking_name_field) ? "name" : field->booking_name_field;
}
static const gchar *booking_email(const VentureFormsField *field)
{
	return venture_string_is_empty(field->booking_email_field) ? "email" : field->booking_email_field;
}

gboolean venture_forms_booking_definition(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GError **error)
{
	guint i, bookings = 0;
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		g_autoptr(VentureEntity) page = NULL;
		g_autofree gchar *origin = NULL;
		const VentureFormsField *name, *email;
		if (field->kind != VENTURE_FORM_FIELD_BOOKING) continue;
		bookings++;
		if (bookings > 1 || field->sensitive || field->group_key != NULL || field->allow_prefill ||
		    !venture_string_is_empty(field->default_value) || !venture_string_is_empty(field->maps_to) || field->booking_page_id <= 0) goto invalid;
		page = venture_database_get(database, VENTURE_TYPE_BOOKING_PAGE, field->booking_page_id, NULL);
		if (page == NULL || venture_entity_is_deleted(page) || venture_entity_get_organization_id(page) != venture_entity_get_organization_id(form)) goto invalid;
		name = venture_forms_definition_find(fields, booking_name(field)); email = venture_forms_definition_find(fields, booking_email(field));
		if (name == NULL || email == NULL || name->kind != VENTURE_FORM_FIELD_SHORT_TEXT || email->kind != VENTURE_FORM_FIELD_EMAIL ||
		    name->sensitive || email->sensitive || name->group_key != NULL || email->group_key != NULL || !name->required || !email->required) goto invalid;
		origin = venture_forms_get_string(form, "public-origin");
		if (!venture_forms_public_origin_valid(origin, error)) return FALSE;
	}
	return TRUE;
invalid:
	venture_set_error_validation(error, "Booking question", "use one non-sensitive, non-repeated booking question with a target in this organization and required name/email questions; booking defaults, prefill and lead mapping are not supported");
	return FALSE;
}

gboolean venture_forms_booking_slots(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, GDateTime *now, GError **error)
{
	g_autoptr(VentureBookingService) service = NULL;
	guint i, j;
	for (i = 0; i < fields->len; i++)
	{
		VentureFormsField *field = g_ptr_array_index(fields, i);
		g_autoptr(VentureEntity) page = NULL;
		gboolean active = FALSE;
		if (field->kind != VENTURE_FORM_FIELD_BOOKING) continue;
		g_clear_pointer(&field->booking_slots, json_node_unref);
		g_ptr_array_set_size(field->choices, 0);
		if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "booking_page") == G_TYPE_INVALID) continue;
		page = venture_database_get(database, VENTURE_TYPE_BOOKING_PAGE, field->booking_page_id, NULL);
		if (page != NULL) g_object_get(page, "active", &active, NULL);
		if (page == NULL || !active || venture_entity_is_deleted(page) || venture_entity_get_organization_id(page) != venture_entity_get_organization_id(form)) continue;
		if (service == NULL) service = venture_booking_service_new(database);
		field->booking_slots = venture_booking_service_slots(service, page, now, error);
		if (field->booking_slots == NULL) return FALSE;
		if (venture_forms_payment(fields) != NULL && json_object_get_boolean_member(venture_forms_payment(fields), "enabled"))
		{
			JsonArray *eligible = json_array_new(), *offered = json_node_get_array(field->booking_slots);
			for (j = 0; j < json_array_get_length(offered); j++)
			{
				JsonObject *slot = json_array_get_object_element(offered, j);
				g_autoptr(GDateTime) start = g_date_time_new_from_iso8601(json_object_get_string_member(slot, "start"), NULL);
				if (start != NULL && g_date_time_difference(start, now) > 45 * G_TIME_SPAN_MINUTE)
					json_array_add_element(eligible, json_node_copy(json_array_get_element(offered, j)));
			}
			g_clear_pointer(&field->booking_slots, json_node_unref);
			field->booking_slots = json_node_new(JSON_NODE_ARRAY); json_node_take_array(field->booking_slots, eligible);
		}

		for (j = 0; j < json_array_get_length(json_node_get_array(field->booking_slots)); j++)
		{
			JsonObject *slot = json_array_get_object_element(json_node_get_array(field->booking_slots), j);
			VentureFormsChoice *choice = g_new0(VentureFormsChoice, 1);
			choice->id = g_strdup(json_object_get_string_member(slot, "start"));
			choice->label = g_strdup(json_object_get_string_member(slot, "label"));
			g_ptr_array_add(field->choices, choice);
		}
	}
	return TRUE;
}

gboolean venture_forms_booking_prepare(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, JsonObject *answers,
	GDateTime *now, VentureEntity **hold, JsonObject *errors, GError **error)
{
	const VentureFormsField *field = booking_field(fields);
	const gchar *start, *name, *email;
	g_autoptr(VentureEntity) page = NULL;
	g_autoptr(VentureBookingService) service = NULL;
	g_autoptr(GError) local = NULL;
	*hold = NULL;
	if (field == NULL) return TRUE;
	start = json_object_get_string_member_with_default(answers, field->key, NULL);
	if (venture_string_is_empty(start)) return TRUE;
	name = json_object_get_string_member_with_default(answers, booking_name(field), "");
	email = json_object_get_string_member_with_default(answers, booking_email(field), "");
	if (venture_string_is_empty(name) || strlen(name) > 200 || venture_string_is_empty(email) || strlen(email) > 254)
	{
		json_object_set_string_member(errors, field->key, "Give a name (at most 200 bytes) and email before booking.");
		return FALSE;
	}
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "booking_page") == G_TYPE_INVALID) goto unavailable;
	page = venture_database_get(database, VENTURE_TYPE_BOOKING_PAGE, field->booking_page_id, &local);
	if (page == NULL || venture_entity_is_deleted(page) || venture_entity_get_organization_id(page) != venture_entity_get_organization_id(form)) goto unavailable;
	service = venture_booking_service_new(database);
	if (g_object_get_data(G_OBJECT(form), VENTURE_FORMS_PAYMENT_WRITE) != NULL)
	{
		g_autoptr(GDateTime) begins = g_date_time_new_from_iso8601(start, NULL);
		if (begins == NULL || g_date_time_difference(begins, now) <= 45 * G_TIME_SPAN_MINUTE) goto unavailable;
	}
	*hold = venture_booking_service_reserve(service, page, start,
		g_object_get_data(G_OBJECT(form), VENTURE_FORMS_PAYMENT_WRITE) != NULL ? 2700 : 30, now, &local);
	if (*hold != NULL) return TRUE;
unavailable:
	if (local != NULL && !g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_CONFLICT) && !g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
	{ g_propagate_error(error, g_steal_pointer(&local)); return FALSE; }
	json_object_set_string_member(errors, field->key, "That time is no longer available. Choose another slot.");
	return FALSE;
}

gboolean venture_forms_booking_finish(VentureDatabase *database, VentureEntity *form, GPtrArray *fields, JsonObject *answers,
	VentureEntity *hold, VentureEntity *response, GDateTime *now, JsonObject *errors, GError **error)
{
	const VentureFormsField *field = booking_field(fields);
	g_autoptr(VentureBookingService) service = NULL;
	g_autoptr(VentureEntity) meeting = NULL;
	g_autofree gchar *origin = NULL;
	g_autoptr(GError) local = NULL;
	if (hold == NULL) return TRUE;
	service = venture_booking_service_new(database); origin = venture_forms_get_string(form, "public-origin");
	meeting = venture_booking_service_confirm(service, hold,
		json_object_get_string_member_with_default(answers, booking_name(field), ""),
		json_object_get_string_member_with_default(answers, booking_email(field), ""), NULL, origin, now, NULL, &local);
	if (meeting == NULL)
	{
		if (g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_CONFLICT) || g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
			json_object_set_string_member(errors, field->key, "That time is no longer available. Choose another slot.");
		else g_propagate_error(error, g_steal_pointer(&local));
		return FALSE;
	}
	g_object_set(response, "booking-id", venture_entity_get_id(meeting), NULL);
	return TRUE;
}
