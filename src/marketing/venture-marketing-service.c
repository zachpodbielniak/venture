/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include "sequences/venture-sequence-tracking-private.h"

struct _VentureMarketingService {
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *permit;
	VentureEntity *enqueue_recipient;
	GWeakRef context;
	gboolean installed;
};
G_DEFINE_FINAL_TYPE(VentureMarketingService, venture_marketing_service, G_TYPE_OBJECT)

static gboolean cancel_address(VentureMarketingService *self, gint64 org,
	const gchar *email, gint reason, GDateTime *now, const VentureActor *actor, GError **error);
static void register_actions(VentureMarketingService *self);

static gboolean refuse(GError **error, const gchar *message)
{
	venture_set_error_validation(error, "marketing", "%s", message);
	return FALSE;
}
static gchar *string(VentureEntity *row, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(row, field, &value, NULL);
	return value;
}
static gint64 number(VentureEntity *row, const gchar *field)
{
	gint64 value = 0;
	g_object_get(row, field, &value, NULL);
	return value;
}
static gint choice(VentureEntity *row, const gchar *field)
{
	gint value = 0;
	g_object_get(row, field, &value, NULL);
	return value;
}
static gboolean flag(VentureEntity *row, const gchar *field)
{
	gboolean value = FALSE;
	g_object_get(row, field, &value, NULL);
	return value;
}
static gchar *normalize(const gchar *address)
{
	g_autofree gchar *copy = g_strdup(address ? address : "");
	g_strstrip(copy);
	return g_ascii_strdown(copy, -1);
}
static gboolean valid_address(const gchar *address)
{
	const gchar *at = address ? strchr(address, '@') : NULL;
	return at && at != address && at[1] && !strchr(at + 1, '@') && strlen(address) <= 254 &&
		!strpbrk(address, " \t\r\n,;<>\"()\\") && g_utf8_validate(address, -1, NULL);
}
static const gchar *subject_field(GType type)
{
	if (type == VENTURE_TYPE_CONTACT) return "contact-id";
	if (type == VENTURE_TYPE_COMPANY) return "company-id";
	if (type == VENTURE_TYPE_LEAD) return "lead-id";
	return NULL;
}
static VentureEntity *get_row(VentureMarketingService *self, GType type, gint64 org, gint64 id, GError **error)
{
	VentureEntity *row = venture_database_get(self->database, type, id, error);
	if (row && (venture_entity_is_deleted(row) || (type != VENTURE_TYPE_ORGANIZATION && venture_entity_get_organization_id(row) != org)))
		g_clear_object(&row);
	if (!row && (!error || !*error))
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Marketing record not found");
	return row;
}
static VentureEntity *subject(VentureMarketingService *self, VentureEntity *row, GError **error)
{
	GType types[] = { VENTURE_TYPE_CONTACT, VENTURE_TYPE_COMPANY, VENTURE_TYPE_LEAD };
	guint i, found = 0;
	g_autoptr(VentureEntity) result = NULL;
	for (i = 0; i < G_N_ELEMENTS(types); i++) {
		gint64 id = number(row, subject_field(types[i]));
		if (!id) continue;
		if (++found > 1) { refuse(error, "Choose exactly one contact, company or lead"); return NULL; }
		result = get_row(self, types[i], venture_entity_get_organization_id(row), id, error);
		if (!result) return NULL;
	}
	if (!result) refuse(error, "Choose exactly one contact, company or lead");
	return g_steal_pointer(&result);
}
static gboolean allowed(VentureMarketingService *self, gint64 org, GError **error)
{
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN,
		VENTURE_ORGANIZATION_ROLE_EDITOR };
	VentureAccessPolicy *policy;
	const VentureAuthPrincipal *principal;
	if (!self->database) return refuse(error, "Marketing database is unavailable");
	policy = venture_database_get_access_policy(self->database);
	principal = venture_access_policy_get_actor(policy);
	if (org <= 0 || !venture_entity_registry_lookup(venture_entity_registry_get_default(), "marketing_send"))
		return refuse(error, "Marketing requires an enabled module and an exact organization");
	if (principal && !venture_access_policy_has_organization_role(policy, principal, org, roles, G_N_ELEMENTS(roles))) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Organization editor authorization is required for marketing");
		return FALSE;
	}
	return TRUE;
}
static gboolean save(VentureMarketingService *self, VentureEntity *row, const VentureActor *actor, GError **error)
{
	gboolean result;
	self->permit = row;
	result = venture_database_save(self->database, row, actor, error);
	self->permit = NULL;
	return result;
}
static VentureEntity *find_string(VentureMarketingService *self, GType type, gint64 org,
	const gchar *field, const gchar *value, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_include_deleted(query, TRUE);
	if (org > 0) venture_query_set_organization(query, org);
	venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_EQ, value, NULL);
	return venture_database_find_one(self->database, query, error);
}
static GPtrArray *children(VentureMarketingService *self, GType type, gint64 org,
	const gchar *field, gint64 id, guint limit, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, limit);
	venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(self->database, query, error);
}
static gboolean valid_time(GDateTime *at, GError **error)
{
	g_autoptr(GDateTime) now = venture_time_now();
	return (at && g_date_time_get_year(at) >= 1970 && g_date_time_compare(at, now) <= 0) ||
		refuse(error, "Evidence requires an actual nonfuture timestamp");
}
static gboolean bounded(const gchar *value, gsize maximum)
{
	return value && *value && strlen(value) <= maximum && g_utf8_validate(value, -1, NULL);
}
static VentureQuery *segment_query(VentureEntity *list, GError **error)
{
	GType types[] = { VENTURE_TYPE_CONTACT, VENTURE_TYPE_COMPANY, VENTURE_TYPE_LEAD };
	g_autofree gchar *filters = string(list, "filters");
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(VentureQuery) query = NULL;
	GHashTableIter iter;
	gpointer key;
	gint target = choice(list, "target");
	if (target < 0 || target >= (gint)G_N_ELEMENTS(types) || (filters && strlen(filters) > 4096)) {
		refuse(error, "Invalid segment type or oversized filters"); return NULL;
	}
	params = soup_form_decode(filters ? filters : "");
	query = venture_query_new(types[target]);
	g_hash_table_iter_init(&iter, params);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		const gchar *name = key;
		if (g_str_has_prefix(name, "organization") || !strcmp(name, "limit") || !strcmp(name, "offset") ||
			!strcmp(name, "page") || !strcmp(name, "order") || !strcmp(name, "include_deleted")) {
			refuse(error, "A segment cannot override organization, history or pagination"); return NULL;
		}
	}
	if (!venture_query_apply_query_string(query, params, error)) return NULL;
	venture_query_set_organization(query, venture_entity_get_organization_id(list));
	venture_query_set_limit(query, 10001);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return g_steal_pointer(&query);
}
static gboolean machine_empty(VentureEntity *row)
{
	const gchar *dates[] = { "previewed-at", "approved-at", "next-enqueue-at", "next-delivery-at" };
	g_autofree gchar *by = string(row, "approved-by"), *base = string(row, "public-base");
	guint i;
	for (i = 0; i < G_N_ELEMENTS(dates); i++) {
		g_autoptr(GDateTime) at = NULL;
		g_object_get(row, dates[i], &at, NULL);
		if (at) return FALSE;
	}
	return !choice(row, "state") && !number(row, "eligible-count") && !number(row, "excluded-count") &&
		!number(row, "last-mail-id") && (!by || !*by) && (!base || !*base);
}
static gboolean validate(VentureDatabase *database, VentureEntity *row, VentureEntity *previous,
	gpointer data, GError **error)
{
	VentureMarketingService *self = data;
	gboolean permitted = self->permit == row;
	self->permit = NULL;
	if (previous && venture_entity_get_organization_id(row) != venture_entity_get_organization_id(previous))
		return refuse(error, "Marketing records cannot change organization");
	if (permitted) return TRUE;
	if (VENTURE_IS_MARKETING_LIST(row)) {
		g_autoptr(VentureQuery) query = segment_query(row, error);
		g_autofree gchar *filters = string(row, "filters");
		if (!query) return FALSE;
		return choice(row, "mode") != 0 || !filters || !*filters || refuse(error, "Static lists use member records, not search filters");
	}
	if (VENTURE_IS_MARKETING_MEMBER(row)) {
		g_autoptr(VentureEntity) list = get_row(self, VENTURE_TYPE_MARKETING_LIST, venture_entity_get_organization_id(row), number(row, "list-id"), error);
		g_autoptr(VentureEntity) source = NULL;
		g_autofree gchar *key = NULL;
		if (!list) return FALSE;
		if (choice(list, "mode")) return refuse(error, "Saved-search segments do not have manual members");
		source = subject(self, row, error);
		if (!source) return FALSE;
		key = g_strdup_printf("%s:%s:%" G_GINT64_FORMAT, venture_entity_get_uuid(list), subject_field(G_OBJECT_TYPE(source)), venture_entity_get_id(source));
		g_object_set(row, "member-key", key, NULL);
		return TRUE;
	}
	if (VENTURE_IS_MARKETING_SEND(row)) {
		g_autoptr(VentureEntity) list = NULL;
		g_autofree gchar *title = string(row, "subject"), *text = string(row, "text-body"), *html = string(row, "html-body");
		gint64 interval = number(row, "interval-seconds");
		if ((previous && choice(previous, "state")) || !machine_empty(row))
			return refuse(error, "Use marketing actions; previewed content and audience are immutable");
		if (!bounded(title, 998) || strpbrk(title, "\r\n") || (!bounded(text, 65536) && !bounded(html, 65536)) ||
			(text && strlen(text) > 65536) || (html && strlen(html) > 65536))
			return refuse(error, "Send requires a bounded subject and message body");
		if (!interval) { interval = 60; g_object_set(row, "interval-seconds", interval, NULL); }
		if (interval < 1 || interval > 86400) return refuse(error, "Delivery interval must be 1 through 86400 seconds");
		list = get_row(self, VENTURE_TYPE_MARKETING_LIST, venture_entity_get_organization_id(row), number(row, "list-id"), error);
		return list != NULL;
	}
	return refuse(error, "Consent, recipients and observations are written only by VentureMarketingService");
}

#include "venture-marketing-consent.inc"
#include "venture-marketing-audience.inc"
#include "venture-marketing-delivery.inc"
#include "venture-marketing-tracking.inc"
#include "venture-marketing-actions.inc"

static void finalize(GObject *object)
{
	VentureMarketingService *self = VENTURE_MARKETING_SERVICE(object);
	if (self->database) g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	g_weak_ref_clear(&self->context);
	G_OBJECT_CLASS(venture_marketing_service_parent_class)->finalize(object);
}
static void set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	VentureMarketingService *self = VENTURE_MARKETING_SERVICE(object);
	if (id != 1) { G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec); return; }
	self->database = g_value_get_object(value);
	if (self->database) g_object_add_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
}
static void get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	VentureMarketingService *self = VENTURE_MARKETING_SERVICE(object);
	if (id == 1) g_value_set_object(value, self->database);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void venture_marketing_service_class_init(VentureMarketingServiceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = finalize;
	G_OBJECT_CLASS(klass)->set_property = set_property;
	G_OBJECT_CLASS(klass)->get_property = get_property;
	g_object_class_install_property(G_OBJECT_CLASS(klass), 1, g_param_spec_object("database", "Database",
		"Weak owning database", VENTURE_TYPE_DATABASE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}
static void venture_marketing_service_init(VentureMarketingService *self)
{
	g_weak_ref_init(&self->context, NULL);
}
VentureMarketingService *venture_marketing_service_get(VentureDatabase *database)
{
	VentureMarketingService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-marketing-service");
	if (!self) {
		self = g_object_new(VENTURE_TYPE_MARKETING_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-marketing-service", self, g_object_unref);
	}
	return self;
}
void venture_marketing_service_configure(VentureMarketingService *self, VentureContext *context)
{
	g_return_if_fail(VENTURE_IS_MARKETING_SERVICE(self));
	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	g_weak_ref_set(&self->context, context);
}
void venture_marketing_install(VentureDatabase *database)
{
	GType types[] = { VENTURE_TYPE_MARKETING_LIST, VENTURE_TYPE_MARKETING_MEMBER,
		VENTURE_TYPE_MARKETING_CONSENT, VENTURE_TYPE_MARKETING_SEND, VENTURE_TYPE_MARKETING_RECIPIENT, VENTURE_TYPE_MARKETING_EVENT };
	VentureMarketingService *self = venture_marketing_service_get(database);
	VentureMailOutbox *outbox = venture_database_get_mail_outbox(database);
	guint i;
	if (self->installed) return;
	self->installed = TRUE;
	for (i = 0; i < G_N_ELEMENTS(types); i++) venture_database_add_save_validator(database, types[i], validate, self, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_MAIL_MESSAGE, validate_mail, self, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_SUPPRESSION, validate_suppression, self, NULL);
	g_signal_connect_object(outbox, "before-claim", G_CALLBACK(before_claim), self, 0);
	g_signal_connect_object(outbox, "before-send", G_CALLBACK(before_send), self, 0);
	g_signal_connect_object(outbox, "before-transport", G_CALLBACK(before_transport), self, 0);
}
gboolean venture_marketing_check_removal(VentureEntity *entity, GError **error)
{
	if (VENTURE_IS_MARKETING_CONSENT(entity) || VENTURE_IS_MARKETING_RECIPIENT(entity) || VENTURE_IS_MARKETING_EVENT(entity) ||
		(VENTURE_IS_MARKETING_SEND(entity) && choice(entity, "state")))
		return refuse(error, "Marketing evidence and approved snapshots cannot be removed");
	return TRUE;
}

void venture_marketing_actions_register(VentureDatabase *database)
{
	register_actions(venture_marketing_service_get(database));
}
