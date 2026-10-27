/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>

#define BOOKING_MAX_SLOTS 500
#define BOOKING_DEFAULT_HORIZON 14
#define BOOKING_MAX_NAME 200
#define BOOKING_MAX_EMAIL 254
#define BOOKING_MAX_NOTES 4000

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
	venture_booking_service_install(database);
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
	JsonArray *resources;
	gchar *selection, *overflow, *call_message;
	gint64 duration, buffer, horizon, capacity, id, ignore;
} Page;
static void page_clear(Page *p)
{
	g_free(p->owner); g_free(p->title);
	g_clear_pointer(&p->zone, g_time_zone_unref);
	g_clear_pointer(&p->availability, json_object_unref);
	g_clear_pointer(&p->resources, json_array_unref);
	g_free(p->selection); g_free(p->overflow); g_free(p->call_message);
}
static gboolean page_read(VentureEntity *page, Page *p, GError **error)
{
	g_autofree gchar *tz = NULL, *availability = NULL;
	g_autoptr(JsonNode) node = NULL;
	memset(p, 0, sizeof *p);
	g_object_get(page, "owner", &p->owner, "title", &p->title, "timezone", &tz, "availability", &availability,
		"duration-minutes", &p->duration, "buffer-minutes", &p->buffer, "horizon-days", &p->horizon, "capacity", &p->capacity, NULL);
	{
		g_autofree gchar *resources = NULL;
		g_autoptr(JsonNode) parsed = NULL;
		guint i, j;
		g_object_get(page, "resources", &resources, "resource-selection", &p->selection,
			"group-overflow", &p->overflow, "group-call-message", &p->call_message, NULL);
		if (!venture_string_is_empty(resources)) parsed = venture_json_parse(resources, NULL);
		if (!venture_string_is_empty(resources) && parsed == NULL)
			return refuse(error, VENTURE_ERROR_CONFIG, "Named resources must be valid JSON");
		if (parsed != NULL && !JSON_NODE_HOLDS_ARRAY(parsed))
			return refuse(error, VENTURE_ERROR_CONFIG, "Named resources must be a JSON array");
		if (parsed != NULL) p->resources = json_array_ref(json_node_get_array(parsed));
		if (p->resources != NULL && json_array_get_length(p->resources) > 100)
			return refuse(error, VENTURE_ERROR_CONFIG, "At most 100 resources are supported");
		if (p->resources != NULL) for (i = 0; i < json_array_get_length(p->resources); i++) {
			JsonObject *resource = json_array_get_object_element(p->resources, i);
			const gchar *name = resource != NULL ? venture_json_object_get_string(resource, "name", NULL) : NULL;
			gint64 min = resource != NULL ? json_object_get_int_member_with_default(resource, "min_group_size", 1) : 0;
			gint64 max = resource != NULL ? json_object_get_int_member_with_default(resource, "max_group_size", 1) : 0;
			if (venture_string_is_empty(name) || strlen(name) > 100 || !g_utf8_validate(name, -1, NULL) ||
			    min < 1 || max < min || max > 10000)
				return refuse(error, VENTURE_ERROR_CONFIG, "Each resource needs a unique name and group limits from 1 to 10000");
			for (j = 0; j < i; j++) {
				JsonObject *other = json_array_get_object_element(p->resources, j);
				if (g_strcmp0(name, venture_json_object_get_string(other, "name", NULL)) == 0)
					return refuse(error, VENTURE_ERROR_CONFIG, "Resource names must be unique");
			}
		}
		if (!venture_string_is_empty(p->selection) && g_strcmp0(p->selection, "auto") && g_strcmp0(p->selection, "pick"))
			return refuse(error, VENTURE_ERROR_CONFIG, "Resource choice must be auto or pick");
		if (!venture_string_is_empty(p->overflow) && g_strcmp0(p->overflow, "call") && g_strcmp0(p->overflow, "multiple"))
			return refuse(error, VENTURE_ERROR_CONFIG, "Large groups must call or use multiple resources");
	}
	p->id = venture_entity_get_id(page);
	if (p->capacity < 0 || p->capacity > 10000) return refuse(error, VENTURE_ERROR_CONFIG, "Capacity must be between 0 and 10000");
	if (p->capacity == 0) p->capacity = 1;
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
typedef struct { GDateTime *from, *to; gint64 page; gchar *resource; JsonArray *resources; } Busy;
static gchar *booking_string(VentureEntity *entity, const gchar *name);
static void busy_free(gpointer data)
{
	Busy *b = data;
	g_clear_pointer(&b->from, g_date_time_unref); g_clear_pointer(&b->to, g_date_time_unref);
	g_clear_pointer(&b->resources, json_array_unref); g_free(b->resource); g_free(b);
}
static JsonArray *booking_names(VentureEntity *entity, const gchar *property)
{
	g_autofree gchar *text = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_object_get(entity, property, &text, NULL);
	if (venture_string_is_empty(text)) return NULL;
	node = venture_json_parse(text, NULL);
	return node != NULL && JSON_NODE_HOLDS_ARRAY(node) ? json_array_ref(json_node_get_array(node)) : NULL;
}
static gchar *booking_names_label(JsonArray *names)
{
	g_autoptr(GString) label = g_string_new(NULL);
	guint i;
	if (names == NULL) return g_strdup("");
	for (i = 0; i < json_array_get_length(names); i++) {
		if (i > 0) g_string_append(label, ", ");
		g_string_append(label, json_array_get_string_element(names, i));
	}
	return g_string_free(g_steal_pointer(&label), FALSE);
}
static gint64 booking_int(VentureEntity *entity, const gchar *name)
{
	gint64 value = 0;
	g_object_get(entity, name, &value, NULL);
	return value;
}
static gchar *booking_string(VentureEntity *entity, const gchar *name)
{
	gchar *value = NULL;
	g_object_get(entity, name, &value, NULL);
	return value;
}
static gboolean reservation_write(VentureBookingService *self, VentureEntity *row, GError **error)
{
	g_object_set(row, "generation", booking_int(row, "generation") + 1, NULL);
	g_object_set_data(G_OBJECT(row), "venture-booking-write", GINT_TO_POINTER(1));
	return venture_database_save(self->database, row, NULL, error);
}

/* Work can already be promised without a technician assignment. Query both
 * ended and open-ended windows so a job starting before now still blocks a
 * future slot, while old jobs do not consume the availability bound. */
static gboolean append_job_busy(VentureBookingService *self, gint64 org, Page *p,
	GDateTime *now, gboolean open_end, GPtrArray *busy, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOB);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GPtrArray) cancelled = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GDateTime) cutoff = NULL;
	g_autoptr(GDateTime) horizon = g_date_time_add_days(now, (gint)p->horizon + 1);
	g_autofree gchar *cutoff_text = NULL;
	g_autofree gchar *horizon_text = venture_time_to_string(horizon);
	gint64 lookback = MIN(p->buffer, (gint64)G_MAXINT - p->duration) + (open_end ? p->duration : 0);
	guint i;
	cutoff = g_date_time_add_minutes(now, -(gint)lookback);
	if (cutoff == NULL) return refuse(error, VENTURE_ERROR_CONFIG, "The booking buffer is too long");
	cutoff_text = venture_time_to_string(cutoff);
	venture_query_set_organization(query, org);
	g_ptr_array_add(cancelled, g_strdup("cancelled"));
	if (!venture_query_add_filter(query, "status", VENTURE_FILTER_OP_NOT_IN, cancelled, error) ||
	    !venture_query_add_filter_string(query, "window-start", VENTURE_FILTER_OP_LT, horizon_text, error)) return FALSE;
	if (open_end)
	{
		if (!venture_query_add_filter(query, "window-end", VENTURE_FILTER_OP_IS_NULL, NULL, error) ||
		    !venture_query_add_filter_string(query, "window-start", VENTURE_FILTER_OP_GT, cutoff_text, error)) return FALSE;
	}
	else if (!venture_query_add_filter_string(query, "window-end", VENTURE_FILTER_OP_GT, cutoff_text, error)) return FALSE;
	venture_query_set_limit(query, 10001);
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL) return FALSE;
	if (rows->len > 10000) return refuse(error, VENTURE_ERROR_CONFLICT, "Too many scheduled jobs to calculate availability");
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		Busy *b = g_new0(Busy, 1);
		g_object_get(row, "window-start", &b->from, "window-end", &b->to, NULL);
		b->resource = booking_string(row, "booking-resource");
		if (b->from == NULL) { busy_free(b); continue; }
		if (b->to == NULL) b->to = g_date_time_add_minutes(b->from, (gint)p->duration);
		g_ptr_array_add(busy, b);
	}
	return TRUE;
}
static GPtrArray *load_busy(VentureBookingService *self, gint64 org, Page *p, GDateTime *now, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACTIVITY);
	g_autoptr(VentureQuery) reservations = venture_query_new(VENTURE_TYPE_BOOKING_RESERVATION);
	g_autoptr(GPtrArray) rows = NULL, holds = NULL;
	g_autoptr(GHashTable) meetings = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autofree gchar *cutoff = venture_time_to_string(now);
	g_autoptr(GPtrArray) busy = g_ptr_array_new_with_free_func(busy_free);
	guint i;
	venture_query_set_organization(reservations, org);
	venture_query_add_filter_string(reservations, "owner", VENTURE_FILTER_OP_EQ, p->owner, NULL);
	venture_query_add_filter_string(reservations, "ends-at", VENTURE_FILTER_OP_GT, cutoff, NULL);
	venture_query_set_limit(reservations, 10001);
	holds = venture_database_find(self->database, reservations, error);
	if (holds == NULL) return NULL;
	if (holds->len > 10000) return refuse(error, VENTURE_ERROR_CONFLICT, "Too many future reservations; shorten the booking horizon"), NULL;
	for (i = 0; i < holds->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(holds, i);
		g_autofree gchar *state = booking_string(row, "state");
		g_autoptr(GDateTime) expires = NULL;
		gint64 activity = booking_int(row, "activity-id");
		if (activity > 0 && g_strcmp0(state, "confirmed") == 0)
		{
			gint64 *key = g_new(gint64, 1); *key = activity;
			g_hash_table_insert(meetings, key, row);
		}
		if (g_strcmp0(state, "held") == 0 && venture_entity_get_id(row) != p->ignore)
		{
			Busy *b;
			g_object_get(row, "expires-at", &expires, NULL);
			if (expires == NULL || g_date_time_compare(expires, now) <= 0) continue;
			b = g_new0(Busy, 1);
			g_object_get(row, "starts-at", &b->from, "ends-at", &b->to, NULL);
			b->page = booking_int(row, "page-id");
			b->resources = booking_names(row, "resource-names");
			if (b->from == NULL || b->to == NULL) { busy_free(b); continue; }
			g_ptr_array_add(busy, b);
		}
	}
	venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, "owner", VENTURE_FILTER_OP_EQ, p->owner, NULL);
	venture_query_add_filter_int(query, "status", VENTURE_FILTER_OP_EQ, VENTURE_ACTIVITY_STATUS_PLANNED, NULL);
	{
		g_autoptr(GPtrArray) kinds = g_ptr_array_new_with_free_func(g_free);
		g_ptr_array_add(kinds, g_strdup("call")); g_ptr_array_add(kinds, g_strdup("meeting"));
		if (!venture_query_add_filter(query, "kind", VENTURE_FILTER_OP_IN, kinds, error)) return NULL;
	}
	venture_query_set_limit(query, 10001);
	rows = venture_database_find(self->database, query, error);
	if (rows == NULL) return NULL;
	if (rows->len > 10000) return refuse(error, VENTURE_ERROR_CONFLICT, "Too many planned activities to calculate availability"), NULL;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i), *reservation;
		g_autoptr(GDateTime) starts = NULL, ends = NULL, due = NULL;
		gint64 id = venture_entity_get_id(row);
		Busy *b;
		gint kind;
		reservation = g_hash_table_lookup(meetings, &id);
		if (reservation != NULL && venture_entity_get_id(reservation) == p->ignore) continue;
		g_object_get(row, "kind", &kind, "starts-at", &starts, "ends-at", &ends, "due-at", &due, NULL);
		if ((kind != VENTURE_ACTIVITY_KIND_CALL && kind != VENTURE_ACTIVITY_KIND_MEETING) || (!starts && !due)) continue;
		b = g_new0(Busy, 1);
		b->from = g_date_time_ref(starts ? starts : due);
		b->to = ends && starts ? g_date_time_ref(ends) : g_date_time_add_minutes(b->from, (gint)p->duration);
		b->page = reservation != NULL ? booking_int(reservation, "page-id") : 0;
		b->resources = reservation != NULL ? booking_names(reservation, "resource-names") : booking_names(row, "booking-resources");
		g_ptr_array_add(busy, b);
	}
	if (p->resources != NULL && json_array_get_length(p->resources) > 0) {
		g_autoptr(VentureQuery) named = venture_query_new(VENTURE_TYPE_ACTIVITY);
		g_autoptr(GPtrArray) named_rows = NULL;
		venture_query_set_organization(named, org);
		venture_query_add_filter_int(named, "status", VENTURE_FILTER_OP_EQ, VENTURE_ACTIVITY_STATUS_PLANNED, NULL);
		if (!venture_query_add_filter(named, "booking-resources", VENTURE_FILTER_OP_NOT_NULL, NULL, error)) return NULL;
		venture_query_set_limit(named, 10001);
		named_rows = venture_database_find(self->database, named, error);
		if (named_rows == NULL) return NULL;
		if (named_rows->len > 10000) return refuse(error, VENTURE_ERROR_CONFLICT, "Too many named meetings to calculate availability"), NULL;
		for (i = 0; i < named_rows->len; i++) {
			VentureEntity *row = g_ptr_array_index(named_rows, i);
			g_autofree gchar *owner = booking_string(row, "owner");
			g_autoptr(GDateTime) starts = NULL, ends = NULL;
			Busy *b;
			if (g_strcmp0(owner, p->owner) == 0) continue;
			b = g_new0(Busy, 1);
			g_object_get(row, "starts-at", &starts, "ends-at", &ends, NULL);
			if (starts == NULL) { busy_free(b); continue; }
			b->from = g_date_time_ref(starts);
			b->to = ends != NULL ? g_date_time_ref(ends) : g_date_time_add_minutes(starts, (gint)p->duration);
			b->resources = booking_names(row, "booking-resources");
			g_ptr_array_add(busy, b);
		}
	}
	if (venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "job") &&
	    (!append_job_busy(self, org, p, now, FALSE, busy, error) ||
	     !append_job_busy(self, org, p, now, TRUE, busy, error))) return NULL;
	return g_steal_pointer(&busy);
}
static gboolean booking_resource_free(GPtrArray *busy, GDateTime *start, GDateTime *end,
	Page *page, const gchar *name)
{
	guint i, j;
	for (i = 0; i < busy->len; i++) {
		Busy *b = g_ptr_array_index(busy, i);
		g_autoptr(GDateTime) from = g_date_time_add_minutes(b->from, -(gint)page->buffer);
		g_autoptr(GDateTime) to = g_date_time_add_minutes(b->to, (gint)page->buffer);
		gboolean applies = TRUE;
		if (g_date_time_compare(from, end) >= 0 || g_date_time_compare(to, start) <= 0) continue;
		if (b->resource != NULL) applies = g_strcmp0(b->resource, name) == 0;
		if (b->resources != NULL) {
			applies = FALSE;
			for (j = 0; j < json_array_get_length(b->resources); j++)
				if (g_strcmp0(json_array_get_string_element(b->resources, j), name) == 0) { applies = TRUE; break; }
		}
		if (applies) return FALSE;
	}
	return TRUE;
}

static gboolean is_free(GPtrArray *busy, GDateTime *start, GDateTime *end, Page *page)
{
	guint i;
	gint64 seats = 0;
	if (page->resources != NULL && json_array_get_length(page->resources) > 0) {
		for (i = 0; i < json_array_get_length(page->resources); i++) {
			JsonObject *resource = json_array_get_object_element(page->resources, i);
			if (booking_resource_free(busy, start, end, page, venture_json_object_get_string(resource, "name", ""))) return TRUE;
		}
		return FALSE;
	}
	for (i = 0; i < busy->len; i++)
	{
		Busy *b = g_ptr_array_index(busy, i);
		g_autoptr(GDateTime) from = g_date_time_add_minutes(b->from, -(gint)page->buffer);
		g_autoptr(GDateTime) to = g_date_time_add_minutes(b->to, (gint)page->buffer);
		if (b->page == page->id && g_date_time_equal(b->from, start) && g_date_time_equal(b->to, end)) seats++;
		else if (g_date_time_compare(from, end) < 0 && g_date_time_compare(to, start) > 0) return FALSE;
	}
	return seats < page->capacity;
}

static gchar *booking_allocate(GPtrArray *busy, GDateTime *start, GDateTime *end,
	Page *page, const gchar *requested, gint64 group_size, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) result = NULL;
	gint64 remaining = group_size;
	gint64 min_total = 0, max_total = 0;
	guint i, pass;
	if (group_size < 1 || group_size > 10000) return refuse(error, VENTURE_ERROR_VALIDATION, "Group size must be from 1 to 10000"), NULL;
	if (page->resources == NULL || json_array_get_length(page->resources) == 0) {
		if (requested != NULL && *requested) return refuse(error, VENTURE_ERROR_VALIDATION, "This page has no named resources"), NULL;
		return g_strdup("[]");
	}
	if (g_strcmp0(page->selection, "pick") == 0 && venture_string_is_empty(requested))
		return refuse(error, VENTURE_ERROR_VALIDATION, "Choose a resource"), NULL;
	json_builder_begin_array(builder);
	for (pass = 0; pass < 2 && remaining > 0; pass++) {
		for (i = 0; i < json_array_get_length(page->resources) && remaining > 0; i++) {
			JsonObject *resource = json_array_get_object_element(page->resources, i);
			const gchar *name = venture_json_object_get_string(resource, "name", "");
			gint64 min = json_object_get_int_member_with_default(resource, "min_group_size", 1);
			gint64 max = json_object_get_int_member_with_default(resource, "max_group_size", 1);
			if (requested != NULL && *requested && ((pass == 0 && g_strcmp0(requested, name)) || (pass == 1 && !g_strcmp0(requested, name)))) continue;
		if ((requested == NULL || !*requested) && pass == 0) continue;
			if (!booking_resource_free(busy, start, end, page, name) || group_size < min_total + min) continue;
			if (remaining > max && g_strcmp0(page->overflow, "multiple") != 0) {
				if (requested != NULL && *requested && pass == 0 && booking_resource_free(busy, start, end, page, name))
					return refuse(error, VENTURE_ERROR_CONFLICT,
						venture_string_is_empty(page->call_message) ? "This group does not fit; please call us" : page->call_message), NULL;
				continue;
			}
		json_builder_add_string_value(builder, name);
			min_total += min; max_total += max;
			remaining = MAX(group_size - max_total, 0);
		if (g_strcmp0(page->overflow, "multiple") != 0) break;
		}
		if (requested != NULL && *requested && pass == 0 && remaining == group_size) {
			return refuse(error, VENTURE_ERROR_CONFLICT, "The chosen resource is not available for this group"), NULL;
		}
	}
	json_builder_end_array(builder);
	if (remaining > 0) {
		const gchar *message = venture_string_is_empty(page->call_message) ? "This group does not fit; please call us" : page->call_message;
		return refuse(error, VENTURE_ERROR_CONFLICT, message), NULL;
	}
	result = json_builder_get_root(builder);
	return json_to_string(result, FALSE);
}

static gboolean booking_assignment_free(GPtrArray *busy, GDateTime *start, GDateTime *end,
	Page *page, JsonArray *names, gint64 group_size)
{
	guint i, j;
	gint64 min_total = 0, max_total = 0;
	if (page->resources == NULL || json_array_get_length(page->resources) == 0)
		return names == NULL || json_array_get_length(names) == 0;
	if (names == NULL || json_array_get_length(names) == 0) return FALSE;
	for (i = 0; i < json_array_get_length(names); i++) {
		const gchar *name = json_array_get_string_element(names, i);
		gboolean known = FALSE;
		if (name == NULL) return FALSE;
		for (j = 0; j < i; j++) if (g_strcmp0(name, json_array_get_string_element(names, j)) == 0) return FALSE;
		for (j = 0; j < json_array_get_length(page->resources); j++) {
			JsonObject *resource = json_array_get_object_element(page->resources, j);
			if (g_strcmp0(name, venture_json_object_get_string(resource, "name", NULL)) == 0) {
				known = TRUE;
				min_total += json_object_get_int_member_with_default(resource, "min_group_size", 1);
				max_total += json_object_get_int_member_with_default(resource, "max_group_size", 1);
				break;
			}
		}
		if (!known || !booking_resource_free(busy, start, end, page, name)) return FALSE;
	}
	return group_size >= min_total && group_size <= max_total &&
		(json_array_get_length(names) == 1 || g_strcmp0(page->overflow, "multiple") == 0);
}

/* Every free slot from now to the horizon, ascending. */
static GPtrArray *compute_slots(VentureBookingService *self, VentureEntity *page, Page *p, GDateTime *now, GError **error)
{
	g_autoptr(GPtrArray) busy = load_busy(self, venture_entity_get_organization_id(page), p, now, error);
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
				if (!is_free(busy, start, end, p)) continue;
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
	g_autoptr(GPtrArray) busy = NULL;
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
	if (p.resources != NULL && json_array_get_length(p.resources) > 0) {
		busy = load_busy(self, venture_entity_get_organization_id(page), &p, reference, error);
		if (busy == NULL) { page_clear(&p); return NULL; }
	}
	json_builder_begin_array(builder);
	for (i = 0; i < slots->len; i++) {
		GDateTime *start = g_ptr_array_index(slots, i);
		g_autoptr(GDateTime) end = g_date_time_add_minutes(start, (gint)p.duration);
		g_autoptr(GDateTime) local = g_date_time_to_timezone(start, p.zone);
		g_autofree gchar *start_text = venture_time_to_string(start), *end_text = venture_time_to_string(end);
		g_autofree gchar *label = g_date_time_format(local, "%a %d %b %Y %H:%M %Z");
		g_autofree gchar *day = g_date_time_format(local, "%Y-%m-%d");
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "start"); json_builder_add_string_value(builder, start_text);
		json_builder_set_member_name(builder, "end"); json_builder_add_string_value(builder, end_text);
		json_builder_set_member_name(builder, "label"); json_builder_add_string_value(builder, label);
		json_builder_set_member_name(builder, "day"); json_builder_add_string_value(builder, day);
		if (busy != NULL) {
			guint j;
			json_builder_set_member_name(builder, "resources"); json_builder_begin_array(builder);
			for (j = 0; j < json_array_get_length(p.resources); j++) {
				JsonObject *resource = json_array_get_object_element(p.resources, j);
				const gchar *name = venture_json_object_get_string(resource, "name", "");
				if (booking_resource_free(busy, start, end, &p, name)) json_builder_add_string_value(builder, name);
			}
			json_builder_end_array(builder);
		}
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

#include "venture-booking-reservations.inc"
