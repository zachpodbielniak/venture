/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* The activity is the access-control subject. Private reservation fields and
 * management capabilities never enter a generic action result. */
static VentureEntity *booking_owner_reservation(VentureDatabase *database, VentureEntity *activity, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) reservation = NULL;
	g_autofree gchar *state = NULL;
	gint status, kind;
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "booking_page") == G_TYPE_INVALID ||
		!VENTURE_IS_ACTIVITY(activity) || venture_entity_is_deleted(activity))
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "Booking not found"), NULL;
	g_object_get(activity, "status", &status, "kind", &kind, NULL);
	if (kind != VENTURE_ACTIVITY_KIND_MEETING)
		return refuse(error, VENTURE_ERROR_VALIDATION, "Only a booked meeting can be cancelled here"), NULL;
	query = venture_query_new(VENTURE_TYPE_BOOKING_RESERVATION);
	venture_query_set_organization(query, venture_entity_get_organization_id(activity));
	venture_query_add_filter_int(query, "activity-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(activity), NULL);
	reservation = venture_database_find_one(database, query, error);
	if (reservation == NULL)
	{
		if (error == NULL || *error == NULL) refuse(error, VENTURE_ERROR_NOT_FOUND, "Booking not found");
		return NULL;
	}
	state = booking_string(reservation, "state");
	if (!((status == VENTURE_ACTIVITY_STATUS_PLANNED && g_strcmp0(state, "confirmed") == 0) ||
		(status == VENTURE_ACTIVITY_STATUS_CANCELLED && g_strcmp0(state, "cancelled") == 0)))
		return refuse(error, VENTURE_ERROR_CONFLICT, "Only a confirmed booking can be cancelled"), NULL;
	return g_steal_pointer(&reservation);
}

static gboolean booking_cancel_allowed(VentureAction *action, VentureEntity *entity,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) reservation = booking_owner_reservation(venture_action_get_data(action), entity, error);
	(void)actor;
	return reservation != NULL;
}

static VentureEntity *booking_cancel_invoke(VentureAction *action, VentureEntity *entity,
	GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureDatabase *database = venture_action_get_data(action);
	g_autoptr(VentureBookingService) service = venture_booking_service_new(database);
	g_autoptr(VentureEntity) reservation = NULL;
	g_autofree gchar *state = NULL;
	(void)params;
	/* Registry dispatch has acquired the save lock and reloaded the activity.
	 * Reload its reservation too, so an intervening cancellation is a no-op. */
	reservation = booking_owner_reservation(database, entity, error);
	if (reservation == NULL) return NULL;
	state = booking_string(reservation, "state");
	if (g_strcmp0(state, "cancelled") != 0 && !booking_cancel_records(service, reservation, entity, actor, error)) return NULL;
	return g_object_ref(entity);
}

static void booking_register_cancel(VentureDatabase *database)
{
	g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAction) action = g_object_new(VENTURE_TYPE_ACTION,
		"data-class", VENTURE_DATA_CLASS_TENANT, "type-name", "activity", "name", "cancel_booking",
		"label", "Cancel booking", "description", "Cancel this booked meeting and release its reserved capacity; no refund or email is sent",
		"parameters", parameters, "stageable", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	/* The registry belongs to this database, so callback data is borrowed. */
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action,
		booking_cancel_allowed, booking_cancel_invoke, database, NULL, &error))
		g_error("Booking cancellation action registration: %s", error->message);
}
