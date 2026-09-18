/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>

#define BOOKING_MAX_SLOTS 500
#define BOOKING_DEFAULT_HORIZON 14

struct _VentureBookingService {
	GObject parent_instance;
	VentureDatabase *database;
};
G_DEFINE_FINAL_TYPE(VentureBookingService, venture_booking_service, G_TYPE_OBJECT)

static gboolean refuse(GError **error, VentureError code, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, code, "VentureBookingService: %s", message);
	return FALSE;
}
static void finalize(GObject *object)
{
	VentureBookingService *self = VENTURE_BOOKING_SERVICE(object);
	if (self->database) g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_booking_service_parent_class)->finalize(object);
}
static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	VentureBookingService *self = VENTURE_BOOKING_SERVICE(object);
	if (id != 1) { G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec); return; }
	self->database = g_value_get_object(value);
	if (self->database) g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
}
static void get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	if (id != 1) { G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec); return; }
	g_value_set_object(value, VENTURE_BOOKING_SERVICE(object)->database);
}
static void venture_booking_service_class_init(VentureBookingServiceClass *klass)
{
	GObjectClass *object = G_OBJECT_CLASS(klass);
	object->finalize = finalize;
	object->set_property = set_property;
	object->get_property = get_property;
	g_object_class_install_property(object, 1, g_param_spec_object("database", "Database", "Weak owning database", VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}
static void venture_booking_service_init(VentureBookingService *self) { (void)self; }
VentureBookingService *venture_booking_service_new(VentureDatabase *database)
{
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	return g_object_new(VENTURE_TYPE_BOOKING_SERVICE, "database", database, NULL);
}

VentureEntity *venture_booking_service_find_page(VentureBookingService *self, const gchar *slug, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BOOKING_PAGE);
	g_return_val_if_fail(VENTURE_IS_BOOKING_SERVICE(self), NULL);
	if (venture_string_is_empty(slug)) return NULL;
	venture_query_add_filter_string(query, "slug", VENTURE_FILTER_OP_EQ, slug, NULL);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find_one(self->database, query, error);
}

/* --- The page's shape ----------------------------------------------------- */
typedef struct {
	gchar *owner, *title;
	GTimeZone *zone;
	JsonObject *availability;
	gint64 duration, buffer, horizon;
} Page;
static void page_clear(Page *p)
{
	g_free(p->owner); g_free(p->title);
	g_clear_pointer(&p->zone, g_time_zone_unref);
	g_clear_pointer(&p->availability, json_object_unref);
}
static gboolean page_read(VentureEntity *page, Page *p, GError **error)
{
	g_autofree gchar *tz = NULL, *availability = NULL;
	g_autoptr(JsonNode) node = NULL;
	memset(p, 0, sizeof *p);
	g_object_get(page, "owner", &p->owner, "title", &p->title, "timezone", &tz, "availability", &availability,
		"duration-minutes", &p->duration, "buffer-minutes", &p->buffer, "horizon-days", &p->horizon, NULL);
	if (venture_string_is_empty(p->owner)) return refuse(error, VENTURE_ERROR_CONFIG, "The booking page has no owner");
	if (p->duration <= 0 || p->duration > 24 * 60) return refuse(error, VENTURE_ERROR_CONFIG, "The booking page needs a duration between 1 and 1440 minutes");
	if (p->buffer < 0) p->buffer = 0;
	if (p->horizon <= 0) p->horizon = BOOKING_DEFAULT_HORIZON;
	if (p->horizon > 366) p->horizon = 366;
	p->zone = g_time_zone_new_identifier(venture_string_is_empty(tz) ? "UTC" : tz);
	if (!p->zone) return refuse(error, VENTURE_ERROR_CONFIG, "The booking page's timezone is not an IANA zone");
	node = venture_string_is_empty(availability) ? NULL : venture_json_parse(availability, NULL);
	if (node && JSON_NODE_HOLDS_OBJECT(node)) p->availability = json_object_ref(json_node_get_object(node));
	else p->availability = json_object_new();
	return TRUE;
}
/* "HH:MM-HH:MM[,HH:MM-HH:MM]" for one weekday; bad pieces are skipped. */
static const gchar *day_keys[] = { "mon", "tue", "wed", "thu", "fri", "sat", "sun" };
static gboolean parse_clock(const gchar *text, gint *minutes)
{
	gint h, m;
	if (strlen(text) != 5 || text[2] != ':' || !g_ascii_isdigit(text[0]) || !g_ascii_isdigit(text[1]) || !g_ascii_isdigit(text[3]) || !g_ascii_isdigit(text[4])) return FALSE;
	h = (text[0] - '0') * 10 + (text[1] - '0');
	m = (text[3] - '0') * 10 + (text[4] - '0');
	if (h > 24 || m > 59 || (h == 24 && m > 0)) return FALSE;
	*minutes = h * 60 + m;
	return TRUE;
}
static const gchar *windows_for(Page *p, GDateTime *local_day)
{
	const gchar *key = day_keys[g_date_time_get_day_of_week(local_day) - 1];
	JsonNode *node = json_object_get_member(p->availability, key);
	return node && JSON_NODE_HOLDS_VALUE(node) ? json_node_get_string(node) : NULL;
}

/* --- Busy time: the owner's planned calls and meetings -------------------- */
typedef struct { GDateTime *from, *to; } Busy;
static void busy_free(gpointer data)
{
	Busy *b = data;
	g_date_time_unref(b->from); g_date_time_unref(b->to); g_free(b);
}
static GPtrArray *load_busy(VentureBookingService *self, gint64 org, Page *p, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACTIVITY);
	g_autoptr(GPtrArray) rows = NULL;
	GPtrArray *busy;
	guint i;
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "owner", VENTURE_FILTER_OP_EQ, p->owner, NULL);
	venture_query_add_filter_int(query, "status", VENTURE_FILTER_OP_EQ, VENTURE_ACTIVITY_STATUS_PLANNED, NULL);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(self->database, query, error);
	if (!rows) return NULL;
	busy = g_ptr_array_new_with_free_func(busy_free);
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) starts = NULL, ends = NULL, due = NULL;
		Busy *b;
		gint kind;
		g_object_get(row, "kind", &kind, "starts-at", &starts, "ends-at", &ends, "due-at", &due, NULL);
		if ((kind != VENTURE_ACTIVITY_KIND_CALL && kind != VENTURE_ACTIVITY_KIND_MEETING) || (!starts && !due)) continue;
		b = g_new0(Busy, 1);
		b->from = g_date_time_ref(starts ? starts : due);
		b->to = ends && starts ? g_date_time_ref(ends) : g_date_time_add_minutes(b->from, (gint)p->duration);
		g_ptr_array_add(busy, b);
	}
	return busy;
}
static gboolean is_free(GPtrArray *busy, GDateTime *start, GDateTime *end, gint64 buffer)
{
	guint i;
	for (i = 0; i < busy->len; i++) {
		Busy *b = g_ptr_array_index(busy, i);
		g_autoptr(GDateTime) from = g_date_time_add_minutes(b->from, -(gint)buffer);
		g_autoptr(GDateTime) to = g_date_time_add_minutes(b->to, (gint)buffer);
		if (g_date_time_compare(from, end) < 0 && g_date_time_compare(to, start) > 0) return FALSE;
	}
	return TRUE;
}

/* Every free slot from now to the horizon, ascending. */
static GPtrArray *compute_slots(VentureBookingService *self, VentureEntity *page, Page *p, GDateTime *now, GError **error)
{
	g_autoptr(GPtrArray) busy = load_busy(self, venture_entity_get_organization_id(page), p, error);
	g_autoptr(GDateTime) local_now = NULL;
	GPtrArray *slots;
	gint64 day;
	if (!busy) return NULL;
	slots = g_ptr_array_new_with_free_func((GDestroyNotify)g_date_time_unref);
	local_now = g_date_time_to_timezone(now, p->zone);
	for (day = 0; day < p->horizon && slots->len < BOOKING_MAX_SLOTS; day++) {
		g_autoptr(GDateTime) local_day = g_date_time_add_days(local_now, (gint)day);
		g_auto(GStrv) pieces = NULL;
		const gchar *windows = windows_for(p, local_day);
		guint w;
		if (venture_string_is_empty(windows)) continue;
		pieces = g_strsplit(windows, ",", -1);
		for (w = 0; pieces[w] && slots->len < BOOKING_MAX_SLOTS; w++) {
			g_auto(GStrv) ends = g_strsplit(g_strstrip(pieces[w]), "-", 2);
			gint open, close, t;
			if (!ends[0] || !ends[1] || !parse_clock(g_strstrip(ends[0]), &open) || !parse_clock(g_strstrip(ends[1]), &close) || close <= open) continue;
			for (t = open; t + p->duration <= close && slots->len < BOOKING_MAX_SLOTS; t += (gint)p->duration) {
				g_autoptr(GDateTime) local_start = g_date_time_new(p->zone, g_date_time_get_year(local_day), g_date_time_get_month(local_day),
					g_date_time_get_day_of_month(local_day), t / 60, t % 60, 0.0);
				g_autoptr(GDateTime) start = NULL, end = NULL;
				if (!local_start) continue;
				start = g_date_time_to_utc(local_start);
				end = g_date_time_add_minutes(start, (gint)p->duration);
				if (g_date_time_compare(start, now) <= 0) continue;
				if (!is_free(busy, start, end, p->buffer)) continue;
				g_ptr_array_add(slots, g_date_time_ref(start));
			}
		}
	}
	return slots;
}

JsonNode *venture_booking_service_slots(VentureBookingService *self, VentureEntity *page, GDateTime *now, GError **error)
{
	g_autoptr(GDateTime) reference = NULL;
	g_autoptr(GPtrArray) slots = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	Page p;
	guint i;
	g_return_val_if_fail(VENTURE_IS_BOOKING_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_BOOKING_PAGE(page), NULL);
	if (!self->database) return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), NULL;
	if (!page_read(page, &p, error)) { page_clear(&p); return NULL; }
	reference = now ? g_date_time_ref(now) : venture_time_now();
	slots = compute_slots(self, page, &p, reference, error);
	if (!slots) { page_clear(&p); return NULL; }
	json_builder_begin_array(builder);
	for (i = 0; i < slots->len; i++) {
		GDateTime *start = g_ptr_array_index(slots, i);
		g_autoptr(GDateTime) end = g_date_time_add_minutes(start, (gint)p.duration);
		g_autoptr(GDateTime) local = g_date_time_to_timezone(start, p.zone);
		g_autofree gchar *start_text = venture_time_to_string(start), *end_text = venture_time_to_string(end);
		g_autofree gchar *label = g_date_time_format(local, "%a %d %b %Y %H:%M %Z");
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "start"); json_builder_add_string_value(builder, start_text);
		json_builder_set_member_name(builder, "end"); json_builder_add_string_value(builder, end_text);
		json_builder_set_member_name(builder, "label"); json_builder_add_string_value(builder, label);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	page_clear(&p);
	return json_builder_get_root(builder);
}

/* The way lead deduplication and inbound mail find a person: normalised email. */
static VentureEntity *find_contact(VentureBookingService *self, gint64 org, const gchar *email, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CONTACT);
	g_autoptr(GPtrArray) contacts = NULL;
	g_autofree gchar *wanted = venture_lead_normalize_email(email);
	guint i;
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 0);
	contacts = venture_database_find(self->database, query, error);
	if (!contacts) return NULL;
	for (i = 0; *wanted && i < contacts->len; i++) {
		VentureEntity *contact = g_ptr_array_index(contacts, i);
		g_autofree gchar *have = NULL, *normal = NULL;
		g_object_get(contact, "email", &have, NULL);
		normal = venture_lead_normalize_email(have);
		if (!g_strcmp0(normal, wanted)) return g_object_ref(contact);
	}
	return NULL;
}

VentureEntity *venture_booking_service_book(VentureBookingService *self, VentureEntity *page, const gchar *start, const gchar *name, const gchar *email, const gchar *notes, GDateTime *now, const VentureActor *actor, GError **error)
{
	g_autoptr(GDateTime) reference = NULL, when = NULL, ends = NULL;
	g_autoptr(GPtrArray) slots = NULL;
	g_autoptr(VentureEntity) contact = NULL, activity = NULL;
	g_autofree gchar *normal = NULL, *subject = NULL, *body = NULL;
	gint64 org;
	Page p;
	guint i;
	gboolean offered = FALSE;
	g_return_val_if_fail(VENTURE_IS_BOOKING_SERVICE(self), NULL);
	g_return_val_if_fail(VENTURE_IS_BOOKING_PAGE(page), NULL);
	if (!self->database) return refuse(error, VENTURE_ERROR_DATABASE, "The database has been closed"), NULL;
	if (venture_database_has_transaction(self->database)) return refuse(error, VENTURE_ERROR_CONFLICT, "Booking requires a committed database"), NULL;
	if (!venture_entity_get_id(page)) return refuse(error, VENTURE_ERROR_VALIDATION, "Booking requires a saved page"), NULL;
	normal = venture_lead_normalize_email(email);
	if (venture_string_is_empty(name)) return refuse(error, VENTURE_ERROR_VALIDATION, "A name is required"), NULL;
	if (!*normal || !strchr(normal, '@')) return refuse(error, VENTURE_ERROR_VALIDATION, "An email address is required"), NULL;
	when = venture_string_is_empty(start) ? NULL : venture_time_from_string(start, NULL);
	if (!when) return refuse(error, VENTURE_ERROR_VALIDATION, "A start time is required"), NULL;
	if (!page_read(page, &p, error)) { page_clear(&p); return NULL; }
	reference = now ? g_date_time_ref(now) : venture_time_now();
	slots = compute_slots(self, page, &p, reference, error);
	if (!slots) { page_clear(&p); return NULL; }
	for (i = 0; i < slots->len && !offered; i++) offered = g_date_time_equal(g_ptr_array_index(slots, i), when);
	if (!offered) { page_clear(&p); return refuse(error, VENTURE_ERROR_CONFLICT, "That time is not available"), NULL; }
	ends = g_date_time_add_minutes(when, (gint)p.duration);
	org = venture_entity_get_organization_id(page);
	if (!venture_database_begin(self->database, error)) { page_clear(&p); return NULL; }
	{
		g_autoptr(GError) lookup = NULL;
		contact = find_contact(self, org, email, &lookup);
		if (lookup) { g_propagate_error(error, g_steal_pointer(&lookup)); goto fail; }
	}
	if (!contact) {
		contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", org, "name", name, "email", normal, "source", "booking", NULL);
		if (!venture_database_save(self->database, contact, actor, error)) goto fail;
	}
	subject = g_strdup_printf("%s with %s", venture_string_is_empty(p.title) ? "Meeting" : p.title, name);
	body = g_strdup_printf("Booked through /book: %s <%s>%s%s", name, normal, venture_string_is_empty(notes) ? "" : "\n\n", venture_string_is_empty(notes) ? "" : notes);
	activity = g_object_new(VENTURE_TYPE_ACTIVITY, "organization-id", org, "kind", VENTURE_ACTIVITY_KIND_MEETING, "owner", p.owner,
		"subject", subject, "body", body, "starts-at", when, "ends-at", ends, "due-at", when, "status", VENTURE_ACTIVITY_STATUS_PLANNED,
		"contact-id", venture_entity_get_id(contact), "related-type", "contact", "related-id", venture_entity_get_id(contact), NULL);
	if (!venture_database_save(self->database, activity, actor, error)) goto fail;
	if (!venture_database_commit(self->database, error)) { page_clear(&p); return NULL; }
	page_clear(&p);
	return g_steal_pointer(&activity);
fail:
	venture_database_rollback(self->database);
	page_clear(&p);
	return NULL;
}
