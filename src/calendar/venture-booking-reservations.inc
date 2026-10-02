/* SPDX-License-Identifier: AGPL-3.0-or-later */

static VentureEntity *booking_page_current(VentureBookingService *self, VentureEntity *page, GError **error)
{
	g_autoptr(VentureEntity) fresh = NULL;
	gboolean active = FALSE;
	if (self->database == NULL || venture_entity_registry_lookup(venture_entity_registry_get_default(), "booking_page") == G_TYPE_INVALID)
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "Booking not found"), NULL;
	fresh = venture_database_get(self->database, VENTURE_TYPE_BOOKING_PAGE, venture_entity_get_id(page), NULL);
	if (fresh != NULL) g_object_get(fresh, "active", &active, NULL);
	if (fresh == NULL || !active || venture_entity_is_deleted(fresh) || venture_entity_get_organization_id(fresh) != venture_entity_get_organization_id(page))
		return refuse(error, VENTURE_ERROR_NOT_FOUND, "Booking not found"), NULL;
	return g_steal_pointer(&fresh);
}

static gboolean booking_offered(VentureBookingService *self, VentureEntity *page, Page *shape,
	GDateTime *when, GDateTime *now, GError **error)
{
	g_autoptr(GPtrArray) slots = compute_slots(self, page, shape, now, error);
	guint i;
	if (slots == NULL) return FALSE;
	for (i = 0; i < slots->len; i++) if (g_date_time_equal(g_ptr_array_index(slots, i), when)) return TRUE;
	return refuse(error, VENTURE_ERROR_CONFLICT, "That time is not available; choose another slot");
}

VentureEntity *venture_booking_service_reserve(VentureBookingService *self, VentureEntity *page,
	const gchar *start, guint seconds, GDateTime *now, GError **error)
{
	g_autoptr(VentureEntity) fresh = NULL, hold = NULL;
	g_autoptr(GDateTime) reference = now != NULL ? g_date_time_ref(now) : venture_time_now();
	g_autoptr(GDateTime) when = NULL, ends = NULL, expires = NULL;
	g_autofree gchar *secret = NULL;
	Page shape;
	g_return_val_if_fail(VENTURE_IS_BOOKING_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_BOOKING_PAGE(page), NULL);
	if (seconds == 0 || seconds > 900) return refuse(error, VENTURE_ERROR_VALIDATION, "A hold lasts between 1 and 900 seconds"), NULL;
	when = venture_string_is_empty(start) ? NULL : venture_time_from_string(start, NULL);
	if (when == NULL) return refuse(error, VENTURE_ERROR_VALIDATION, "A start time is required"), NULL;
	if (self->database == NULL || !venture_database_begin(self->database, error)) return NULL;
	fresh = booking_page_current(self, page, error);
	if (fresh == NULL) goto fail;
	if (!page_read(fresh, &shape, error)) { page_clear(&shape); goto fail; }
	if (!booking_offered(self, fresh, &shape, when, reference, error)) { page_clear(&shape); goto fail; }
	ends = g_date_time_add_minutes(when, (gint)shape.duration);
	expires = g_date_time_add_seconds(reference, seconds);
	secret = venture_generate_token(32);
	hold = g_object_new(VENTURE_TYPE_BOOKING_RESERVATION, "organization-id", venture_entity_get_organization_id(fresh),
		"name", "Private booking reservation", "page-id", venture_entity_get_id(fresh), "owner", shape.owner,
		"starts-at", when, "ends-at", ends, "expires-at", expires, "state", "held", "secret", secret, NULL);
	page_clear(&shape);
	if (!reservation_write(self, hold, error)) goto fail;
	if (!venture_database_commit(self->database, error)) return NULL;
	return g_steal_pointer(&hold);
fail:
	venture_database_rollback(self->database);
	return NULL;
}

static gboolean booking_origin_valid(const gchar *origin)
{
	g_autoptr(GUri) uri = NULL;
	g_autoptr(GInetAddress) address = NULL;
	const gchar *host, *scheme, *path;
	if (venture_string_is_empty(origin) || strlen(origin) > 2048) return FALSE;
	uri = g_uri_parse(origin, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL || g_uri_get_userinfo(uri) != NULL || g_uri_get_query(uri) != NULL || g_uri_get_fragment(uri) != NULL) return FALSE;
	host = g_uri_get_host(uri); scheme = g_uri_get_scheme(uri); path = g_uri_get_path(uri);
	if (venture_string_is_empty(host) || (path != NULL && *path != '\0' && g_strcmp0(path, "/") != 0)) return FALSE;
	if (g_strcmp0(scheme, "https") == 0) return TRUE;
	address = g_inet_address_new_from_string(host);
	return g_strcmp0(scheme, "http") == 0 && address != NULL && g_inet_address_get_is_loopback(address);
}

static gchar *booking_capability(VentureEntity *reservation)
{
	g_autofree gchar *secret = booking_string(reservation, "secret");
	g_autofree gchar *payload = NULL, *signature = NULL;
	g_autoptr(GDateTime) start = NULL;
	if (venture_string_is_empty(secret)) return NULL;
	g_object_get(reservation, "starts-at", &start, NULL);
	if (start == NULL) return NULL;
	payload = g_strdup_printf("%s:%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT,
		venture_entity_get_uuid(reservation), g_date_time_to_unix(start) + 86400, booking_int(reservation, "generation"));
	signature = g_compute_hmac_for_string(G_CHECKSUM_SHA256, (const guchar *)secret, strlen(secret), payload, -1);
	return g_strdup_printf("%s:%s", payload, signature);
}

gchar *venture_booking_service_manage_url(VentureBookingService *self, VentureEntity *reservation,
	const gchar *origin, GError **error)
{
	g_autoptr(VentureEntity) page = NULL;
	g_autofree gchar *slug = NULL, *encoded = NULL, *capability = NULL;
	g_return_val_if_fail(VENTURE_IS_BOOKING_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_BOOKING_RESERVATION(reservation), NULL);
	if (!booking_origin_valid(origin)) return refuse(error, VENTURE_ERROR_VALIDATION, "Booking links require an HTTPS origin (or loopback HTTP)"), NULL;
	page = venture_database_get(self->database, VENTURE_TYPE_BOOKING_PAGE, booking_int(reservation, "page-id"), error);
	if (page == NULL) return NULL;
	slug = booking_string(page, "slug"); encoded = g_uri_escape_string(slug, NULL, TRUE); capability = booking_capability(reservation);
	if (capability == NULL) return refuse(error, VENTURE_ERROR_NOT_FOUND, "Booking not found"), NULL;
	return g_strdup_printf("%.*s/book/%s/manage/%s", (gint)(strlen(origin) - (g_str_has_suffix(origin, "/") ? 1 : 0)), origin, encoded, capability);
}

static gboolean booking_mail(VentureBookingService *self, VentureEntity *reservation, VentureEntity *activity,
	const gchar *email, const gchar *origin, GError **error)
{
	VentureMailOutbox *outbox = venture_database_get_mail_outbox(self->database);
	g_autoptr(VentureMailMessage) message = NULL, queued = NULL;
	g_autofree gchar *url = NULL, *body = NULL, *key = NULL, *when_text = NULL;
	g_autoptr(GDateTime) when = NULL;
	if (venture_string_is_empty(origin) || outbox == NULL || venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message") == G_TYPE_INVALID) return TRUE;
	url = venture_booking_service_manage_url(self, reservation, origin, error);
	if (url == NULL) return FALSE;
	g_object_get(activity, "starts-at", &when, NULL); when_text = venture_time_to_string(when);
	body = g_strdup_printf("Your booking is confirmed for %s.\n\nUse this private link to cancel or choose another time:\n%s", when_text, url);
	key = g_strdup_printf("booking:%s:%" G_GINT64_FORMAT, venture_entity_get_uuid(reservation), booking_int(reservation, "generation"));
	message = venture_mail_message_new();
	g_object_set(message, "organization-id", venture_entity_get_organization_id(reservation), "to", email,
		"subject", "Booking confirmation", "text-body", "A private booking confirmation was issued.", "private-text-body", body,
		"idempotency-key", key, "related-type", "booking_reservation", "related-id", venture_entity_get_id(reservation), NULL);
	queued = venture_mail_outbox_enqueue(outbox, message, NULL, error);
	return queued != NULL;
}

VentureEntity *venture_booking_service_confirm(VentureBookingService *self, VentureEntity *reservation,
	const gchar *name, const gchar *email, const gchar *notes, const gchar *origin,
	GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) hold = NULL, page = NULL, fresh = NULL, contact = NULL, activity = NULL;
	g_autoptr(GDateTime) reference = now != NULL ? g_date_time_ref(now) : venture_time_now(), expires = NULL, when = NULL, ends = NULL;
	g_autofree gchar *normal = venture_lead_normalize_email(email), *state = NULL, *subject = NULL, *body = NULL, *base = NULL;
	gint64 org;
	Page shape;
	g_return_val_if_fail(VENTURE_IS_BOOKING_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_BOOKING_RESERVATION(reservation), NULL);
	if (venture_string_is_empty(name) || strlen(name) > BOOKING_MAX_NAME || !g_utf8_validate(name, -1, NULL) ||
	    !*normal || !strchr(normal, '@') || strlen(normal) > BOOKING_MAX_EMAIL || strpbrk(normal, "\r\n") != NULL ||
	    (notes != NULL && (strlen(notes) > BOOKING_MAX_NOTES || !g_utf8_validate(notes, -1, NULL))))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Check the name, email and notes"), NULL;
	if (self->database == NULL || !venture_database_begin(self->database, error)) return NULL;
	hold = venture_database_get(self->database, VENTURE_TYPE_BOOKING_RESERVATION, venture_entity_get_id(reservation), error);
	if (hold == NULL) goto fail;
	state = booking_string(hold, "state");
	g_object_get(hold, "expires-at", &expires, "starts-at", &when, "ends-at", &ends, NULL);
	if (g_strcmp0(state, "held") != 0 || expires == NULL || when == NULL || ends == NULL || g_date_time_compare(reference, expires) >= 0 ||
	    venture_entity_get_organization_id(hold) != venture_entity_get_organization_id(reservation))
	{ refuse(error, VENTURE_ERROR_CONFLICT, "The slot hold has expired or was used"); goto fail; }
	page = venture_database_get(self->database, VENTURE_TYPE_BOOKING_PAGE, booking_int(hold, "page-id"), error);
	if (page == NULL) goto fail;
	fresh = booking_page_current(self, page, error); if (fresh == NULL) goto fail;
	if (!page_read(fresh, &shape, error)) { page_clear(&shape); goto fail; }
	{
		g_autoptr(GDateTime) expected_end = g_date_time_add_minutes(when, (gint)shape.duration);
		if (!g_date_time_equal(expected_end, ends))
		{ page_clear(&shape); refuse(error, VENTURE_ERROR_CONFLICT, "The booking duration changed; choose another slot"); goto fail; }
	}
	shape.ignore = venture_entity_get_id(hold);
	if (!booking_offered(self, fresh, &shape, when, reference, error)) { page_clear(&shape); goto fail; }
	base = !venture_string_is_empty(origin) ? g_strdup(origin) : booking_string(fresh, "public-origin");
	if (!venture_string_is_empty(base) && !booking_origin_valid(base))
	{ page_clear(&shape); refuse(error, VENTURE_ERROR_VALIDATION, "Booking links require an HTTPS origin"); goto fail; }
	org = venture_entity_get_organization_id(fresh);
	{
		g_autoptr(GError) lookup = NULL;
		contact = find_contact(self, org, email, &lookup);
		if (lookup != NULL) { page_clear(&shape); g_propagate_error(error, g_steal_pointer(&lookup)); goto fail; }
	}
	if (contact == NULL)
	{
		contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", org, "name", name, "email", normal, "source", "booking", NULL);
		if (!venture_database_save(self->database, contact, actor, error)) { page_clear(&shape); goto fail; }
	}
	subject = g_strdup_printf("%s with %s", venture_string_is_empty(shape.title) ? "Meeting" : shape.title, name);
	body = g_strdup_printf("Booked through /book: %s <%s>%s%s", name, normal, venture_string_is_empty(notes) ? "" : "\n\n", venture_string_is_empty(notes) ? "" : notes);
	activity = g_object_new(VENTURE_TYPE_ACTIVITY, "organization-id", org, "kind", VENTURE_ACTIVITY_KIND_MEETING, "owner", shape.owner,
		"subject", subject, "body", body, "starts-at", when, "ends-at", ends, "due-at", when, "status", VENTURE_ACTIVITY_STATUS_PLANNED,
		"contact-id", venture_entity_get_id(contact), "related-type", "contact", "related-id", venture_entity_get_id(contact), NULL);
	page_clear(&shape);
	if (!venture_database_save(self->database, activity, actor, error)) goto fail;
	g_object_set(hold, "activity-id", venture_entity_get_id(activity), "state", "confirmed", "public-origin", base, NULL);
	if (!reservation_write(self, hold, error) || !booking_mail(self, hold, activity, email, base, error)) goto fail;
	if (!venture_database_commit(self->database, error)) return NULL;
	return g_steal_pointer(&activity);
fail:
	venture_database_rollback(self->database);
	return NULL;
}

VentureEntity *venture_booking_service_book(VentureBookingService *self, VentureEntity *page, const gchar *start,
	const gchar *name, const gchar *email, const gchar *notes, GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) hold = NULL, activity = NULL;
	g_return_val_if_fail(VENTURE_IS_BOOKING_SERVICE(self), NULL);
	if (self->database == NULL || !venture_database_begin(self->database, error)) return NULL;
	hold = venture_booking_service_reserve(self, page, start, 30, now, error);
	if (hold != NULL) activity = venture_booking_service_confirm(self, hold, name, email, notes, NULL, now, actor, error);
	if (activity == NULL) { venture_database_rollback(self->database); return NULL; }
	if (!venture_database_commit(self->database, error)) return NULL;
	return g_steal_pointer(&activity);
}

VentureEntity *venture_booking_service_lookup_capability(VentureBookingService *self, VentureEntity *page,
	const gchar *capability, GDateTime *now, GError **error)
{
	g_auto(GStrv) parts = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) reservation = NULL, activity = NULL;
	g_autoptr(GDateTime) reference = now != NULL ? g_date_time_ref(now) : venture_time_now();
	g_autofree gchar *expected = NULL, *state = NULL;
	gint64 expiry;
	gint status;
	g_return_val_if_fail(VENTURE_IS_BOOKING_SERVICE(self), NULL);
	if (capability == NULL || strlen(capability) > 256) goto absent;
	parts = g_strsplit(capability, ":", 5);
	if (g_strv_length(parts) != 4 || !g_uuid_string_is_valid(parts[0]) ||
	    !g_ascii_string_to_signed(parts[1], 10, 1, G_MAXINT64, &expiry, NULL) || g_date_time_to_unix(reference) >= expiry) goto absent;
	query = venture_query_new(VENTURE_TYPE_BOOKING_RESERVATION);
	venture_query_set_organization(query, venture_entity_get_organization_id(page));
	venture_query_add_filter_string(query, "uuid", VENTURE_FILTER_OP_EQ, parts[0], NULL);
	venture_query_add_filter_int(query, "page-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(page), NULL);
	reservation = venture_database_find_one(self->database, query, error);
	if (reservation == NULL) { if (error != NULL && *error != NULL) return NULL; goto absent; }
	state = booking_string(reservation, "state"); expected = booking_capability(reservation);
	if (g_strcmp0(state, "confirmed") != 0 || expected == NULL || !venture_constant_time_equal(expected, capability)) goto absent;
	activity = venture_database_get(self->database, VENTURE_TYPE_ACTIVITY, booking_int(reservation, "activity-id"), NULL);
	if (activity == NULL || venture_entity_is_deleted(activity)) goto absent;
	g_object_get(activity, "status", &status, NULL);
	if (status != VENTURE_ACTIVITY_STATUS_PLANNED) goto absent;
	return g_steal_pointer(&reservation);
absent:
	refuse(error, VENTURE_ERROR_NOT_FOUND, "Booking not found");
	return NULL;
}

VentureEntity *venture_booking_service_manage(VentureBookingService *self, VentureEntity *page,
	const gchar *capability, const gchar *start, GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) reservation = NULL, activity = NULL, fresh = NULL, contact = NULL;
	g_autoptr(GDateTime) reference = now != NULL ? g_date_time_ref(now) : venture_time_now(), when = NULL, ends = NULL;
	g_autofree gchar *secret = NULL, *origin = NULL, *email = NULL;
	gboolean cancel = start == NULL;
	Page shape;
	g_return_val_if_fail(VENTURE_IS_BOOKING_SERVICE(self), NULL);
	if (self->database == NULL || !venture_database_begin(self->database, error)) return NULL;
	reservation = venture_booking_service_lookup_capability(self, page, capability, reference, error);
	if (reservation == NULL) goto fail;
	activity = venture_database_get(self->database, VENTURE_TYPE_ACTIVITY, booking_int(reservation, "activity-id"), error);
	if (activity == NULL) goto fail;
	if (cancel)
	{
		g_object_set(reservation, "state", "cancelled", "secret", NULL, NULL);
		g_object_set(activity, "status", VENTURE_ACTIVITY_STATUS_CANCELLED, NULL);
	}
	else
	{
		fresh = booking_page_current(self, page, error); if (fresh == NULL) goto fail;
		when = venture_time_from_string(start, NULL);
		if (when == NULL) { refuse(error, VENTURE_ERROR_VALIDATION, "Choose an offered time"); goto fail; }
		if (!page_read(fresh, &shape, error)) { page_clear(&shape); goto fail; }
		shape.ignore = venture_entity_get_id(reservation);
		if (!booking_offered(self, fresh, &shape, when, reference, error)) { page_clear(&shape); goto fail; }
		ends = g_date_time_add_minutes(when, (gint)shape.duration);
		g_object_set(activity, "owner", shape.owner, "starts-at", when, "ends-at", ends, "due-at", when, NULL);
		secret = venture_generate_token(32);
		g_object_set(reservation, "owner", shape.owner, "starts-at", when, "ends-at", ends, "secret", secret, NULL);
		page_clear(&shape);
	}
	if (!reservation_write(self, reservation, error) || !venture_database_save(self->database, activity, actor, error)) goto fail;
	if (!cancel)
	{
		contact = venture_database_get(self->database, VENTURE_TYPE_CONTACT, booking_int(activity, "contact-id"), error);
		if (contact == NULL) goto fail;
		email = booking_string(contact, "email"); origin = booking_string(reservation, "public-origin");
		if (!booking_mail(self, reservation, activity, email, origin, error)) goto fail;
	}
	if (!venture_database_commit(self->database, error)) return NULL;
	return g_steal_pointer(&activity);
fail:
	venture_database_rollback(self->database);
	return NULL;
}

gboolean venture_booking_service_release(VentureBookingService *self, VentureEntity *reservation, GError **error)
{
	g_autoptr(VentureEntity) fresh = NULL;
	g_autofree gchar *state = NULL;
	g_return_val_if_fail(VENTURE_IS_BOOKING_SERVICE(self), FALSE);
	if (self->database == NULL || !venture_database_begin(self->database, error)) return FALSE;
	fresh = venture_database_get(self->database, VENTURE_TYPE_BOOKING_RESERVATION, venture_entity_get_id(reservation), error);
	if (fresh == NULL) goto fail;
	state = booking_string(fresh, "state");
	if (g_strcmp0(state, "held") == 0)
	{
		g_object_set(fresh, "state", "expired", "secret", NULL, NULL);
		if (!reservation_write(self, fresh, error)) goto fail;
	}
	return venture_database_commit(self->database, error);
fail:
	venture_database_rollback(self->database);
	return FALSE;
}

static gboolean booking_validate_page(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error)
{
	g_autofree gchar *origin = booking_string(entity, "public-origin");
	gboolean active = FALSE, valid;
	Page shape;
	(void)database; (void)previous; (void)data;
	if (booking_int(entity, "capacity") < 0 || booking_int(entity, "capacity") > 10000 ||
	    (!venture_string_is_empty(origin) && !booking_origin_valid(origin)))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Use capacity 0 to 10000 and an HTTPS public origin");
	g_object_get(entity, "active", &active, NULL);
	if (!active) return TRUE;
	valid = page_read(entity, &shape, error); page_clear(&shape);
	return valid;
}

static gboolean booking_validate_reservation(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, gpointer data, GError **error)
{
	(void)database; (void)previous; (void)data;
	return g_object_get_data(G_OBJECT(entity), "venture-booking-write") != NULL ||
		refuse(error, VENTURE_ERROR_PERMISSION_DENIED, "Booking reservations are maintained by the booking service");
}

void venture_booking_service_install(VentureDatabase *database)
{
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	if (g_object_get_data(G_OBJECT(database), "venture-booking-installed") != NULL) return;
	venture_database_add_save_validator(database, VENTURE_TYPE_BOOKING_PAGE, booking_validate_page, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_BOOKING_RESERVATION, booking_validate_reservation, NULL, NULL);
	g_object_set_data(G_OBJECT(database), "venture-booking-installed", GINT_TO_POINTER(1));
}
