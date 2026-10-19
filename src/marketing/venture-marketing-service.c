/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include <stdio.h>
#include "sequences/venture-sequence-tracking-private.h"
#include "marketing/venture-marketing-segments.h"

struct _VentureMarketingService {
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *permit;
	VentureEntity *enqueue_recipient;
	GWeakRef context;
	gboolean installed;
	gint64 machine_seconds;
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
static gboolean has_stamp(VentureEntity *row, const gchar *field)
{
	g_autoptr(GDateTime) at = NULL;
	g_object_get(row, field, &at, NULL);
	return at != NULL;
}
static gboolean bounded(const gchar *value, gsize maximum)
{
	return value && *value && strlen(value) <= maximum && g_utf8_validate(value, -1, NULL);
}
/* A saved search: its field filters as an ordinary query, with the tag
 * and behaviour criteria taken out into @criteria (src/marketing/
 * venture-marketing-segments.c) for segment_members() to apply. */
static VentureQuery *segment_query(VentureEntity *list, VentureMarketingCriteria *criteria, GError **error)
{
	GType types[] = { VENTURE_TYPE_CONTACT, VENTURE_TYPE_COMPANY, VENTURE_TYPE_LEAD };
	g_autofree gchar *filters = string(list, "filters");
	g_autoptr(GHashTable) params = NULL;
	g_autoptr(VentureQuery) query = NULL;
	VentureMarketingCriteria local;
	GHashTableIter iter;
	gpointer key;
	gint target = choice(list, "target");
	gboolean ok;
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
	{
		GObjectClass *klass = g_type_class_ref(types[target]);
		gboolean tagged = g_object_class_find_property(klass, "tags") != NULL;
		g_type_class_unref(klass);
		ok = venture_marketing_criteria_take(params, target == 0, tagged, criteria ? criteria : &local, error);
	}
	if (!criteria && ok) venture_marketing_criteria_clear(&local);
	if (!ok) return NULL;
	if (!venture_query_apply_query_string(query, params, error)) {
		if (criteria) venture_marketing_criteria_clear(criteria);
		return NULL;
	}
	venture_query_set_organization(query, venture_entity_get_organization_id(list));
	venture_query_set_limit(query, 10001);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return g_steal_pointer(&query);
}
/* What a saved search selects at @now, bounded like every preview. */
static GPtrArray *segment_members(VentureMarketingService *self, VentureEntity *list, GDateTime *now, GError **error)
{
	VentureMarketingCriteria criteria;
	g_autoptr(VentureQuery) query = segment_query(list, &criteria, error);
	g_autoptr(GPtrArray) found = NULL;
	GPtrArray *kept;
	gboolean empty = FALSE;
	guint i;
	if (!query) return NULL;
	if (!venture_marketing_criteria_narrow(self->database, venture_entity_get_organization_id(list), &criteria, now, query, &empty, error)) {
		venture_marketing_criteria_clear(&criteria); return NULL;
	}
	kept = g_ptr_array_new_with_free_func(g_object_unref);
	if (!empty) {
		found = venture_database_find(self->database, query, error);
		if (!found) { venture_marketing_criteria_clear(&criteria); g_ptr_array_unref(kept); return NULL; }
		for (i = 0; i < found->len; i++)
			if (venture_marketing_criteria_keep(&criteria, g_ptr_array_index(found, i)))
				g_ptr_array_add(kept, g_object_ref(g_ptr_array_index(found, i)));
		/* The bound is judged on what the query fetched, before the exact
		 * tag check: a segment past it is refused, never truncated. */
		if (found->len > 10000) g_ptr_array_add(kept, g_object_ref(g_ptr_array_index(found, 0)));
	}
	venture_marketing_criteria_clear(&criteria);
	return kept;
}
static gboolean machine_empty(VentureEntity *row)
{
	const gchar *dates[] = { "previewed-at", "approved-at", "next-enqueue-at", "next-delivery-at" };
	const gchar *more[] = { "winner-at", "test-sent-at" };
	g_autofree gchar *by = string(row, "approved-by"), *base = string(row, "public-base"), *winner = string(row, "winner");
	guint i;
	for (i = 0; i < G_N_ELEMENTS(dates); i++) {
		g_autoptr(GDateTime) at = NULL;
		g_object_get(row, dates[i], &at, NULL);
		if (at) return FALSE;
	}
	for (i = 0; i < G_N_ELEMENTS(more); i++) {
		g_autoptr(GDateTime) at = NULL;
		g_object_get(row, more[i], &at, NULL);
		if (at) return FALSE;
	}
	return !choice(row, "state") && !number(row, "eligible-count") && !number(row, "excluded-count") &&
		!number(row, "last-mail-id") && (!by || !*by) && (!base || !*base) && (!winner || !*winner);
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
		g_autoptr(VentureQuery) query = segment_query(row, NULL, error);
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
		if (choice(row, "channel") == 1 && (!bounded(text, 1600) || g_utf8_strlen(text, -1) > 1600))
			return refuse(error, "A text campaign needs a text body of at most 1600 characters");
		if (interval < 1 || interval > 86400) return refuse(error, "Delivery interval must be 1 through 86400 seconds");
		{
			g_autofree gchar *b_subject = string(row, "variant-subject");
			gint64 percent = number(row, "test-percent"), wait = number(row, "winner-wait-hours"), days = number(row, "resend-after-days");
			if (percent < 0 || percent > 100) return refuse(error, "An A/B test share is 0 (no test) to 100 percent");
			if (wait < 0 || wait > 168) return refuse(error, "The A/B winner is picked 1 to 168 hours after the test arms; empty is 4");
			if (b_subject && *b_subject && (!bounded(b_subject, 998) || strpbrk(b_subject, "\r\n")))
				return refuse(error, "The B subject is one line");
			if (flag(row, "local-time") && (number(row, "local-hour") < 0 || number(row, "local-hour") > 23))
				return refuse(error, "The local hour is 0 to 23");
			if (flag(row, "local-time") && percent > 0)
				return refuse(error, "An A/B test and a local-time send cannot be combined: the arms would go out hours apart");
			if (days < 0 || days > 365) return refuse(error, "Resend after 0 to 365 days");
		}
		list = get_row(self, VENTURE_TYPE_MARKETING_LIST, venture_entity_get_organization_id(row), number(row, "list-id"), error);
		return list != NULL;
	}
	return refuse(error, "Consent, recipients and observations are written only by VentureMarketingService");
}

/* Text campaigns (venture-marketing-text.inc): the preview and the run
 * branch to these for a send whose channel is text. */
static gboolean text_send(VentureEntity *send);
static gboolean make_text_recipient(VentureMarketingService *self, VentureEntity *send, VentureEntity *source,
	const gchar *source_key, GHashTable *seen, gint64 *eligible, gint64 *excluded, const VentureActor *actor, GError **error);
static gboolean text_queue_recipient(VentureMarketingService *self, VentureEntity *send, VentureEntity *recipient,
	GDateTime *now, gboolean *queued, const VentureActor *actor, GError **error);
#include "venture-marketing-consent.inc"
#include "venture-marketing-audience.inc"
#include "venture-marketing-preferences.inc"
#include "venture-marketing-audiences.inc"
#include "venture-marketing-delivery.inc"
#include "venture-marketing-tracking.inc"
#include "venture-marketing-actions.inc"
#include "venture-marketing-replies.inc"
#include "venture-marketing-text.inc"

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
	self->machine_seconds = 10;
}
void venture_marketing_service_set_machine_seconds(VentureMarketingService *self, gint64 seconds)
{
	g_return_if_fail(VENTURE_IS_MARKETING_SERVICE(self));
	self->machine_seconds = MAX(seconds, 0);
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
		VENTURE_TYPE_MARKETING_CONSENT, VENTURE_TYPE_MARKETING_SEND, VENTURE_TYPE_MARKETING_RECIPIENT, VENTURE_TYPE_MARKETING_EVENT,
		VENTURE_TYPE_MARKETING_PREFERENCE };
	VentureMarketingService *self = venture_marketing_service_get(database);
	VentureMailOutbox *outbox = venture_database_get_mail_outbox(database);
	guint i;
	if (self->installed) return;
	self->installed = TRUE;
	for (i = 0; i < G_N_ELEMENTS(types); i++) venture_database_add_save_validator(database, types[i], validate, self, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_MAIL_MESSAGE, validate_mail, self, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_SUPPRESSION, validate_suppression, self, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_INTERACTION, validate_reply, self, NULL);
	g_signal_connect_object(outbox, "before-claim", G_CALLBACK(before_claim), self, 0);
	g_signal_connect_object(outbox, "before-send", G_CALLBACK(before_send), self, 0);
	g_signal_connect_object(outbox, "before-transport", G_CALLBACK(before_transport), self, 0);
	venture_deliverability_install(database);
	/* A marketing journey asks the preference centre before each email. */
	venture_sequence_add_email_gate(database, journey_gate, self);
}
gboolean venture_marketing_check_removal(VentureEntity *entity, GError **error)
{
	if (VENTURE_IS_MARKETING_CONSENT(entity) || VENTURE_IS_MARKETING_RECIPIENT(entity) || VENTURE_IS_MARKETING_EVENT(entity) ||
		VENTURE_IS_MARKETING_PREFERENCE(entity) ||
		(VENTURE_IS_MARKETING_SEND(entity) && choice(entity, "state")))
		return refuse(error, "Marketing evidence and approved snapshots cannot be removed");
	return TRUE;
}

void venture_marketing_actions_register(VentureDatabase *database)
{
	register_actions(venture_marketing_service_get(database));
}
