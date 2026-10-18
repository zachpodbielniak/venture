/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* The provider feed, delivery status notifications, the warm-up and the
 * machine-open judgement. Every event ends in
 * venture_marketing_service_feedback(): the evidence, suppression and
 * replay rules live there and only there. docs/deliverability.org. */
#include <venture.h>
#include <string.h>
#include <gmime/gmime.h>
#include "sequences/venture-sequence-tracking-private.h"

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMimeStream, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMimeParser, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GMimeMessage, g_object_unref)

#define FEED_PROVIDER "marketing_feedback"
#define DETAIL_BYTES 1000

static gboolean refuse(GError **error, const gchar *message)
{
	venture_set_error_validation(error, "deliverability", "%s", message);
	return FALSE;
}
static gboolean deny(GError **error)
{
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Deliverability feed delivery refused");
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
static gboolean module_on(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(), "marketing_send") != G_TYPE_INVALID;
}
/* An address as marketing stores it: trimmed, ASCII lower case, and with
 * the "rfc822;" type and angle brackets a report wraps it in removed. */
static gchar *address(const gchar *value)
{
	g_autofree gchar *copy = NULL;
	const gchar *semicolon;
	gchar *result;
	gsize length;
	if (!value) return NULL;
	semicolon = strchr(value, ';');
	copy = g_strdup(semicolon ? semicolon + 1 : value);
	g_strstrip(copy);
	length = strlen(copy);
	if (length >= 2 && copy[0] == '<' && copy[length - 1] == '>') {
		copy[length - 1] = '\0';
		memmove(copy, copy + 1, length - 1);
		g_strstrip(copy);
	}
	if (!*copy || !strchr(copy, '@') || strlen(copy) > 254) return NULL;
	result = g_ascii_strdown(copy, -1);
	return result;
}
/* A Message-ID without its angle brackets or surrounding space. */
static gchar *message_id(const gchar *value)
{
	g_autofree gchar *copy = NULL;
	gchar *start, *end;
	if (!value) return NULL;
	copy = g_strdup(value);
	start = strchr(copy, '<');
	end = start ? strchr(start, '>') : NULL;
	if (start && end) { *end = '\0'; start++; }
	else start = copy;
	g_strstrip(start);
	if (!*start || strlen(start) > 998) return NULL;
	return g_strdup(start);
}
static gchar *bounded_text(const gchar *value, gsize maximum)
{
	g_autoptr(GString) out = NULL;
	const gchar *p;
	gboolean space = FALSE;
	if (!value) return NULL;
	out = g_string_new(NULL);
	/* Folded header text arrives in pieces; collapse runs of white space. */
	for (p = value; *p && out->len < maximum; p++) {
		if (g_ascii_isspace(*p)) { space = out->len > 0; continue; }
		if (space) g_string_append_c(out, ' ');
		space = FALSE;
		g_string_append_c(out, *p);
	}
	if (!g_utf8_validate(out->str, -1, NULL) || !out->len) return NULL;
	return g_string_free(g_steal_pointer(&out), FALSE);
}

VentureDeliverabilityEvent *venture_deliverability_event_new(VentureDeliverabilityKind kind)
{
	VentureDeliverabilityEvent *event = g_new0(VentureDeliverabilityEvent, 1);
	event->kind = kind;
	return event;
}
void venture_deliverability_event_free(VentureDeliverabilityEvent *event)
{
	if (!event) return;
	g_free(event->event_id);
	g_free(event->message_id);
	g_free(event->recipient);
	g_clear_pointer(&event->occurred_at, g_date_time_unref);
	g_free(event->status);
	g_free(event->detail);
	g_free(event);
}
const gchar *venture_deliverability_kind_to_string(VentureDeliverabilityKind kind)
{
	switch (kind) {
	case VENTURE_DELIVERABILITY_HARD_BOUNCE: return "hard_bounce";
	case VENTURE_DELIVERABILITY_SOFT_BOUNCE: return "temporary_bounce";
	case VENTURE_DELIVERABILITY_COMPLAINT: return "complaint";
	case VENTURE_DELIVERABILITY_DELIVERED:
	default: return "delivered";
	}
}

/* ------------------------------------------------------------------ */
/* Adapters. */

static GPtrArray *adapters;
void venture_deliverability_adapter_register(const VentureDeliverabilityAdapter *adapter)
{
	g_return_if_fail(adapter != NULL && adapter->id != NULL && adapter->parse != NULL);
	if (venture_deliverability_adapter_lookup(adapter->id)) return;
	if (!adapters) adapters = g_ptr_array_new();
	g_ptr_array_add(adapters, (gpointer)adapter);
}
const VentureDeliverabilityAdapter *venture_deliverability_adapter_lookup(const gchar *id)
{
	guint i;
	for (i = 0; id && adapters && i < adapters->len; i++) {
		const VentureDeliverabilityAdapter *adapter = g_ptr_array_index(adapters, i);
		if (!g_strcmp0(adapter->id, id)) return adapter;
	}
	return NULL;
}
static const gchar *member_string(JsonObject *object, const gchar *name)
{
	JsonNode *node = json_object_get_member(object, name);
	return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}
GPtrArray *venture_deliverability_parse_generic(GBytes *body, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func((GDestroyNotify)venture_deliverability_event_free);
	JsonNode *root, *list;
	JsonArray *array;
	gsize length = 0;
	const gchar *bytes = body ? g_bytes_get_data(body, &length) : NULL;
	guint i;
	if (!bytes || !length || memchr(bytes, 0, length) || !json_parser_load_from_data(parser, bytes, (gssize)length, NULL)) {
		refuse(error, "Expected a JSON object with an events array"); return NULL;
	}
	root = json_parser_get_root(parser);
	list = JSON_NODE_HOLDS_OBJECT(root) ? json_object_get_member(json_node_get_object(root), "events") : NULL;
	if (!list || !JSON_NODE_HOLDS_ARRAY(list)) { refuse(error, "Expected a JSON object with an events array"); return NULL; }
	array = json_node_get_array(list);
	if (json_array_get_length(array) > VENTURE_DELIVERABILITY_MAX_EVENTS) {
		refuse(error, "A delivery carries at most 100 events"); return NULL;
	}
	for (i = 0; i < json_array_get_length(array); i++) {
		JsonNode *node = json_array_get_element(array, i);
		JsonObject *object;
		const gchar *type, *bounce, *id, *when;
		VentureDeliverabilityEvent *event;
		VentureDeliverabilityKind kind;
		if (!JSON_NODE_HOLDS_OBJECT(node)) { venture_set_error_validation(error, "events", "Event %u is not an object", i + 1); return NULL; }
		object = json_node_get_object(node);
		type = member_string(object, "type"); bounce = member_string(object, "bounce"); id = member_string(object, "id");
		if (!g_strcmp0(type, "delivered")) kind = VENTURE_DELIVERABILITY_DELIVERED;
		else if (!g_strcmp0(type, "complaint")) kind = VENTURE_DELIVERABILITY_COMPLAINT;
		else if (!g_strcmp0(type, "bounce") && !g_strcmp0(bounce, "hard")) kind = VENTURE_DELIVERABILITY_HARD_BOUNCE;
		else if (!g_strcmp0(type, "bounce") && !g_strcmp0(bounce, "soft")) kind = VENTURE_DELIVERABILITY_SOFT_BOUNCE;
		else { venture_set_error_validation(error, "events", "Event %u needs type delivered, complaint, or bounce with bounce hard or soft", i + 1); return NULL; }
		if (!id || !*id || strlen(id) > 200) { venture_set_error_validation(error, "events", "Event %u needs an id of at most 200 characters", i + 1); return NULL; }
		event = venture_deliverability_event_new(kind);
		g_ptr_array_add(events, event);
		event->event_id = g_strdup(id);
		event->message_id = message_id(member_string(object, "message_id"));
		event->recipient = address(member_string(object, "recipient"));
		event->status = bounded_text(member_string(object, "status"), 32);
		event->detail = bounded_text(member_string(object, "detail"), DETAIL_BYTES);
		when = member_string(object, "occurred_at");
		if (when) {
			event->occurred_at = g_date_time_new_from_iso8601(when, NULL);
			if (!event->occurred_at) { venture_set_error_validation(error, "events", "Event %u has an occurred_at that is not ISO 8601 with a zone", i + 1); return NULL; }
		}
	}
	return g_steal_pointer(&events);
}
static const VentureDeliverabilityAdapter generic_adapter = {
	"generic", "Generic JSON events", venture_deliverability_parse_generic
};

/* ------------------------------------------------------------------ */
/* The signed feed. */

gboolean venture_deliverability_verify_signature(const gchar *secret, const gchar *timestamp,
	const gchar *signature, GBytes *body, GDateTime *now, GError **error)
{
	g_autoptr(GHmac) hmac = NULL;
	gint64 seconds = 0, clock;
	gsize length = 0, i;
	const guint8 *bytes = body ? g_bytes_get_data(body, &length) : NULL;
	if (!secret || !*secret || !now || !timestamp || !signature) return deny(error);
	clock = g_date_time_to_unix(now);
	if (g_str_has_prefix(signature, "sha256=")) signature += strlen("sha256=");
	/* Every malformed input is the same refusal, decided before the
	 * HMAC, so a caller learns nothing about which part was wrong. */
	if (!*timestamp || strlen(timestamp) > 12 || strlen(signature) != 64 || !bytes ||
		length > VENTURE_DELIVERABILITY_MAX_BODY) return deny(error);
	for (i = 0; timestamp[i]; i++) if (!g_ascii_isdigit(timestamp[i])) return deny(error);
	for (i = 0; i < 64; i++) if (!g_ascii_isxdigit(signature[i])) return deny(error);
	if (!g_ascii_string_to_signed(timestamp, 10, 0, G_MAXINT64, &seconds, NULL)) return deny(error);
	if (seconds < clock - VENTURE_DELIVERABILITY_TOLERANCE_SECONDS || seconds > clock + VENTURE_DELIVERABILITY_TOLERANCE_SECONDS)
		return deny(error);
	hmac = g_hmac_new(G_CHECKSUM_SHA256, (const guchar *)secret, strlen(secret));
	g_hmac_update(hmac, (const guchar *)timestamp, (gssize)strlen(timestamp));
	g_hmac_update(hmac, (const guchar *)"\n", 1);
	g_hmac_update(hmac, bytes, (gssize)length);
	{
		g_autofree gchar *lower = g_ascii_strdown(signature, -1);
		if (venture_constant_time_equal(g_hmac_get_string(hmac), lower)) return TRUE;
	}
	return deny(error);
}
gchar *venture_deliverability_set_feed_secret(VentureDatabase *database, gint64 organization_id,
	const gchar *secret, const VentureActor *actor, GError **error)
{
	VentureIntegrationService *integrations;
	g_autoptr(VentureIntegrationConnection) current = NULL, saved = NULL;
	g_autoptr(GError) local = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) settings = NULL;
	g_autofree gchar *made = NULL;
	gsize i;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	if (!secret) {
		/* 256 bits from the operating system, the same helper the
		 * tracking and unsubscribe capabilities use. */
		made = venture_sequence_tracking_token(error);
		if (!made) return NULL;
		secret = made;
	}
	if (strlen(secret) < 32 || strlen(secret) > 128) { refuse(error, "A signing secret is 32 to 128 characters"); return NULL; }
	for (i = 0; secret[i]; i++)
		if (!g_ascii_isgraph(secret[i])) { refuse(error, "A signing secret is printable ASCII without spaces"); return NULL; }
	integrations = venture_integration_service_get(database);
	if (!venture_database_begin(database, error)) return NULL;
	current = venture_integration_service_find(integrations, organization_id, FEED_PROVIDER, &local);
	if (!current && local && !g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_CONFIG)) {
		g_propagate_error(error, g_steal_pointer(&local)); goto fail;
	}
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "signing_secret");
	json_builder_add_string_value(builder, secret);
	json_builder_end_object(builder);
	settings = json_builder_get_root(builder);
	saved = venture_integration_service_configure(integrations, organization_id, FEED_PROVIDER, "feed", "live", settings,
		current ? venture_entity_get_version(VENTURE_ENTITY(current)) : 0, actor, error);
	if (!saved) goto fail;
	if (!venture_database_commit(database, error)) return NULL;
	return g_strdup(secret);
fail:
	venture_database_rollback(database);
	return NULL;
}
static gchar *feed_secret(VentureDatabase *database, gint64 organization_id, GError **error)
{
	VentureIntegrationService *integrations = venture_integration_service_get(database);
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_autoptr(JsonNode) settings = NULL;
	const gchar *secret;
	connection = venture_integration_service_find(integrations, organization_id, FEED_PROVIDER, error);
	if (!connection) return NULL;
	settings = venture_integration_service_resolve(integrations, organization_id,
		venture_entity_get_id(VENTURE_ENTITY(connection)), FALSE, error);
	if (!settings || !JSON_NODE_HOLDS_OBJECT(settings)) return NULL;
	secret = venture_json_object_get_string(json_node_get_object(settings), "signing_secret", NULL);
	return secret && *secret ? g_strdup(secret) : NULL;
}
gboolean venture_deliverability_feed_configured(VentureDatabase *database, gint64 organization_id)
{
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	connection = venture_integration_service_find(venture_integration_service_get(database), organization_id, FEED_PROVIDER, NULL);
	return connection != NULL;
}
JsonNode *venture_deliverability_receive(VentureDatabase *database, gint64 organization_id,
	const gchar *provider, const gchar *timestamp, const gchar *signature, GBytes *body,
	GDateTime *now, GError **error)
{
	const VentureDeliverabilityAdapter *adapter = venture_deliverability_adapter_lookup(provider);
	g_autoptr(VentureAccessScope) capability = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *secret = NULL, *source = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	gint64 accepted = 0, ignored = 0;
	guint i;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	if (!adapter || !module_on()) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No such deliverability feed");
		return NULL;
	}
	/* The signature is the authorization: no session, no caller-chosen
	 * scope. The organization in the address only selects whose secret
	 * the signature must match. */
	capability = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	secret = feed_secret(database, organization_id, &local);
	if (!secret) {
		g_message("Deliverability feed for organization %" G_GINT64_FORMAT " has no signing secret; delivery refused", organization_id);
		deny(error);
		return NULL;
	}
	if (!venture_deliverability_verify_signature(secret, timestamp, signature, body, now, error)) {
		g_warning("Deliverability feed for organization %" G_GINT64_FORMAT " refused a delivery: bad or stale signature", organization_id);
		return NULL;
	}
	events = adapter->parse(body, error);
	if (!events) return NULL;
	source = g_strdup_printf("Signed %s provider feed", adapter->id);
	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "results");
	json_builder_begin_array(builder);
	for (i = 0; i < events->len; i++) {
		VentureDeliverabilityEvent *event = g_ptr_array_index(events, i);
		g_autofree gchar *scoped = g_strdup_printf("%s:%s", adapter->id, event->event_id ? event->event_id : "");
		const gchar *outcome;
		g_free(event->event_id);
		event->event_id = g_steal_pointer(&scoped);
		outcome = venture_deliverability_apply(database, organization_id, event, source, now, error);
		if (!outcome) return NULL;
		if (!strcmp(outcome, "recorded")) accepted++;
		else ignored++;
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_string_value(builder, event->event_id + strlen(adapter->id) + 1);
		json_builder_set_member_name(builder, "result");
		json_builder_add_string_value(builder, outcome);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_set_member_name(builder, "accepted");
	json_builder_add_int_value(builder, accepted);
	json_builder_set_member_name(builder, "ignored");
	json_builder_add_int_value(builder, ignored);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

/* ------------------------------------------------------------------ */
/* One event into the marketing evidence. */

const gchar *venture_deliverability_apply(VentureDatabase *database, gint64 organization_id,
	const VentureDeliverabilityEvent *event, const gchar *source, GDateTime *now, GError **error)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) mail = NULL, recipient = NULL, existing = NULL;
	g_autoptr(GDateTime) sent = NULL, occurred = NULL;
	g_autoptr(GError) local = NULL;
	g_autofree gchar *key = NULL, *email = NULL, *related = NULL, *evidence = NULL, *state = NULL;
	g_autoptr(GString) text = NULL;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(event != NULL && now != NULL, NULL);
	if (!module_on()) return "marketing is off";
	if (!event->message_id) return "no Message-ID";
	query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	venture_query_set_organization(query, organization_id);
	venture_query_add_filter_string(query, "message-id", VENTURE_FILTER_OP_EQ, event->message_id, NULL);
	venture_query_add_filter_string(query, "related-type", VENTURE_FILTER_OP_EQ, "marketing_recipient", NULL);
	mail = venture_database_find_one(database, query, error);
	if (error && *error) return NULL;
	if (!mail) return "not a campaign message from this organization";
	recipient = venture_database_get(database, VENTURE_TYPE_MARKETING_RECIPIENT, number(mail, "related-id"), error);
	if (!recipient) return NULL;
	if (venture_entity_get_organization_id(recipient) != organization_id) return "not a campaign message from this organization";
	email = string(recipient, "email");
	/* A forwarded copy bouncing at another address is not evidence about
	 * the person this message was written to. */
	if (event->recipient && g_ascii_strcasecmp(event->recipient, email)) return "names another address";
	state = string(mail, "state");
	g_object_get(mail, "sent-at", &sent, NULL);
	if (g_strcmp0(state, "sent") || !sent) return "the relay never accepted this message";
	/* The evidence time is the event's own, kept between the message's
	 * acceptance and now: a provider clock ahead of ours, or a report
	 * dated before the message existed, cannot place evidence where
	 * the feedback rules would refuse it. */
	occurred = event->occurred_at ? g_date_time_ref(event->occurred_at) : g_date_time_ref(now);
	if (g_date_time_compare(occurred, now) > 0) { g_date_time_unref(occurred); occurred = g_date_time_ref(now); }
	if (g_date_time_compare(occurred, sent) < 0) { g_date_time_unref(occurred); occurred = g_date_time_ref(sent); }
	if (event->event_id && strlen(event->event_id) <= 120) key = g_strdup(event->event_id);
	else {
		g_autofree gchar *material = g_strdup_printf("%s\n%s\n%s", venture_deliverability_kind_to_string(event->kind),
			event->event_id ? event->event_id : "", event->message_id);
		key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, material, -1);
	}
	/* A replayed event is already evidence; answering it again from the
	 * retained row keeps a retry from looking like different evidence. */
	{
		g_autoptr(VentureQuery) replay = venture_query_new(VENTURE_TYPE_MARKETING_EVENT);
		g_autofree gchar *full = g_strconcat("feedback:", key, NULL);
		venture_query_set_organization(replay, organization_id);
		venture_query_set_include_deleted(replay, TRUE);
		venture_query_add_filter_string(replay, "event-key", VENTURE_FILTER_OP_EQ, full, NULL);
		existing = venture_database_find_one(database, replay, error);
		if (error && *error) return NULL;
		if (existing) return "recorded";
	}
	text = g_string_new(source ? source : "Deliverability evidence");
	if (event->status) g_string_append_printf(text, "; status %s", event->status);
	if (event->detail) g_string_append_printf(text, "; %s", event->detail);
	evidence = bounded_text(text->str, 4000);
	if (!venture_marketing_service_feedback(venture_marketing_service_get(database), organization_id,
		venture_entity_get_id(recipient), venture_deliverability_kind_to_string(event->kind),
		evidence, key, occurred, NULL, &local)) {
		if (g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_VALIDATION)) {
			g_message("Deliverability event not recorded: %s", local->message);
			return "refused by the evidence rules";
		}
		g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}
	return "recorded";
}

/* ------------------------------------------------------------------ */
/* Delivery status notifications (RFC 3464) and abuse reports (RFC 5965). */

/* A part's decoded content, or a message part's own text. */
static gchar *part_text(GMimeObject *object)
{
	if (GMIME_IS_PART(object)) {
		GMimeDataWrapper *content = g_mime_part_get_content(GMIME_PART(object));
		g_autoptr(GMimeStream) memory = NULL;
		GByteArray *array;
		if (!content) return NULL;
		memory = g_mime_stream_mem_new();
		if (g_mime_data_wrapper_write_to_stream(content, memory) < 0) return NULL;
		array = g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(memory));
		if (!array || memchr(array->data, 0, array->len)) return NULL;
		return g_strndup((const gchar *)array->data, array->len);
	}
	if (GMIME_IS_MESSAGE_PART(object)) {
		GMimeMessage *inner = g_mime_message_part_get_message(GMIME_MESSAGE_PART(object));
		return inner ? g_mime_object_to_string(GMIME_OBJECT(inner), NULL) : NULL;
	}
	return NULL;
}
/* "Name: value" fields in blank-line separated groups, folded lines
 * joined, names lower case: one GHashTable per group. */
static GPtrArray *field_groups(const gchar *text)
{
	GPtrArray *groups = g_ptr_array_new_with_free_func((GDestroyNotify)g_hash_table_unref);
	g_auto(GStrv) lines = g_strsplit(text ? text : "", "\n", 2000);
	GHashTable *group = NULL;
	gchar *last = NULL;
	guint i;
	for (i = 0; lines[i]; i++) {
		gchar *line = lines[i], *colon;
		gsize length = strlen(line);
		if (length && line[length - 1] == '\r') line[--length] = '\0';
		if (!length) { group = NULL; last = NULL; continue; }
		if ((line[0] == ' ' || line[0] == '\t') && group && last) {
			gchar *value = g_hash_table_lookup(group, last);
			if (value && strlen(value) < 4096) g_hash_table_replace(group, g_strdup(last), g_strconcat(value, " ", g_strstrip(line), NULL));
			continue;
		}
		colon = strchr(line, ':');
		if (!colon || colon == line) continue;
		if (!group) {
			group = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
			g_ptr_array_add(groups, group);
		}
		*colon = '\0';
		g_free(last);
		last = g_ascii_strdown(g_strstrip(line), -1);
		if (!g_hash_table_contains(group, last))
			g_hash_table_replace(group, g_strdup(last), g_strdup(g_strstrip(colon + 1)));
	}
	g_free(last);
	return groups;
}
static const gchar *field(GHashTable *group, const gchar *name)
{
	return group ? g_hash_table_lookup(group, name) : NULL;
}
/* The first group of a headers-only part: the returned message's headers. */
static gchar *returned_header(const gchar *text, const gchar *name)
{
	g_autoptr(GPtrArray) groups = field_groups(text);
	return groups->len ? g_strdup(field(g_ptr_array_index(groups, 0), name)) : NULL;
}
static void add_event(GPtrArray *events, VentureDeliverabilityKind kind, const gchar *recipient,
	const gchar *original, const gchar *status, const gchar *detail, const gchar *report_id, GDateTime *date)
{
	VentureDeliverabilityEvent *event = venture_deliverability_event_new(kind);
	gchar *status_token = NULL;
	if (status) {
		g_auto(GStrv) parts = g_strsplit_set(status, " \t(", 2);
		status_token = parts[0] && *parts[0] ? g_strdup(parts[0]) : NULL;
	}
	event->recipient = address(recipient);
	event->message_id = message_id(original);
	event->status = status_token;
	event->detail = bounded_text(detail, DETAIL_BYTES);
	event->occurred_at = date ? g_date_time_ref(date) : NULL;
	/* One report names a recipient once; the pair is the replay identity. */
	event->event_id = g_strdup_printf("dsn:%s:%s", report_id, event->recipient ? event->recipient : "");
	g_ptr_array_add(events, event);
}
GPtrArray *venture_deliverability_parse_dsn(GBytes *raw, GError **error)
{
	g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func((GDestroyNotify)venture_deliverability_event_free);
	g_autoptr(GMimeStream) stream = NULL;
	g_autoptr(GMimeParser) parser = NULL;
	g_autoptr(GMimeMessage) message = NULL;
	g_autofree gchar *status_text = NULL, *feedback_text = NULL, *returned_id = NULL, *report_id = NULL, *reply_to = NULL;
	g_autoptr(GDateTime) date = NULL;
	GMimeObject *body;
	GMimeContentType *type;
	const gchar *report_type;
	gsize length = 0;
	const guint8 *bytes = raw ? g_bytes_get_data(raw, &length) : NULL;
	gint i, count;
	(void)error;
	if (!bytes || !length) return g_steal_pointer(&events);
	{
		/* GMime's defaults are made by its init; the mailbox sync's
		 * class does it too, but this parser also runs without a sync. */
		static gsize initialized = 0;
		if (g_once_init_enter(&initialized)) { g_mime_init(); g_once_init_leave(&initialized, 1); }
	}
	stream = g_mime_stream_mem_new_with_buffer((const char *)bytes, length);
	parser = g_mime_parser_new_with_stream(stream);
	message = g_mime_parser_construct_message(parser, NULL);
	if (!message) return g_steal_pointer(&events);
	body = g_mime_message_get_mime_part(message);
	if (!body || !GMIME_IS_MULTIPART(body)) return g_steal_pointer(&events);
	type = g_mime_object_get_content_type(body);
	if (!g_mime_content_type_is_type(type, "multipart", "report")) return g_steal_pointer(&events);
	report_type = g_mime_content_type_get_parameter(type, "report-type");
	count = g_mime_multipart_get_count(GMIME_MULTIPART(body));
	for (i = 0; i < count && i < 16; i++) {
		GMimeObject *part = g_mime_multipart_get_part(GMIME_MULTIPART(body), i);
		GMimeContentType *part_type = g_mime_object_get_content_type(part);
		if (g_mime_content_type_is_type(part_type, "message", "delivery-status") ||
			g_mime_content_type_is_type(part_type, "message", "global-delivery-status")) {
			if (!status_text) status_text = part_text(part);
		} else if (g_mime_content_type_is_type(part_type, "message", "feedback-report")) {
			if (!feedback_text) feedback_text = part_text(part);
		} else if (GMIME_IS_MESSAGE_PART(part)) {
			GMimeMessage *inner = g_mime_message_part_get_message(GMIME_MESSAGE_PART(part));
			if (inner && !returned_id && g_mime_message_get_message_id(inner))
				returned_id = g_strdup(g_mime_message_get_message_id(inner));
		} else if (g_mime_content_type_is_type(part_type, "text", "rfc822-headers") ||
			g_mime_content_type_is_type(part_type, "text", "rfc822-header")) {
			g_autofree gchar *headers = part_text(part);
			if (!returned_id) returned_id = returned_header(headers, "message-id");
		}
	}
	report_id = message_id(g_mime_message_get_message_id(message));
	if (!report_id) report_id = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes, length);
	reply_to = g_strdup(g_mime_object_get_header(GMIME_OBJECT(message), "In-Reply-To"));
	date = g_mime_message_get_date(message) ? g_date_time_ref(g_mime_message_get_date(message)) : NULL;
	if (status_text && (!report_type || !g_ascii_strcasecmp(report_type, "delivery-status") ||
		!g_ascii_strcasecmp(report_type, "global-delivery-status"))) {
		g_autoptr(GPtrArray) groups = field_groups(status_text);
		GHashTable *per_message = groups->len ? g_ptr_array_index(groups, 0) : NULL;
		guint g;
		for (g = 1; g < groups->len && g < 101; g++) {
			GHashTable *group = g_ptr_array_index(groups, g);
			const gchar *action = field(group, "action"), *status = field(group, "status");
			const gchar *recipient = field(group, "final-recipient") ? field(group, "final-recipient") : field(group, "original-recipient");
			const gchar *original = field(group, "original-message-id");
			VentureDeliverabilityKind kind;
			if (!action || !recipient) continue;
			if (!original) original = field(per_message, "original-message-id");
			if (!original) original = returned_id;
			if (!original) original = reply_to;
			/* The machine part decides, never the prose: a permanent
			 * failure needs both the action and a 5.x.x status. A failure
			 * with no status is not proof the address is bad. */
			if (!g_ascii_strncasecmp(action, "failed", 6))
				kind = status && status[0] == '5' ? VENTURE_DELIVERABILITY_HARD_BOUNCE : VENTURE_DELIVERABILITY_SOFT_BOUNCE;
			else if (!g_ascii_strncasecmp(action, "delayed", 7))
				kind = VENTURE_DELIVERABILITY_SOFT_BOUNCE;
			else if (!g_ascii_strncasecmp(action, "delivered", 9) || !g_ascii_strncasecmp(action, "relayed", 7))
				kind = VENTURE_DELIVERABILITY_DELIVERED;
			else continue;
			if (kind == VENTURE_DELIVERABILITY_DELIVERED && status && status[0] == '4') kind = VENTURE_DELIVERABILITY_SOFT_BOUNCE;
			{
				const gchar *diagnostic = field(group, "diagnostic-code");
				const gchar *semicolon = diagnostic ? strchr(diagnostic, ';') : NULL;
				add_event(events, kind, recipient, original, status, semicolon ? semicolon + 1 : diagnostic, report_id, date);
			}
		}
	} else if (feedback_text && (!report_type || !g_ascii_strcasecmp(report_type, "feedback-report"))) {
		g_autoptr(GPtrArray) groups = field_groups(feedback_text);
		GHashTable *group = groups->len ? g_ptr_array_index(groups, 0) : NULL;
		const gchar *feedback = field(group, "feedback-type");
		const gchar *recipient = field(group, "original-rcpt-to");
		/* Only an abuse report is a complaint; fraud, virus and other
		 * reports say something about the message, not the reader. */
		if (feedback && !g_ascii_strcasecmp(feedback, "abuse") && recipient)
			add_event(events, VENTURE_DELIVERABILITY_COMPLAINT, recipient, returned_id ? returned_id : reply_to,
				NULL, "Abuse report from a feedback loop", report_id, date);
	}
	return g_steal_pointer(&events);
}
gint venture_deliverability_apply_dsn(VentureDatabase *database, gint64 organization_id,
	GBytes *raw, GDateTime *now, GError **error)
{
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(VentureAccessScope) capability = NULL;
	gint recorded = 0;
	guint i;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), -1);
	if (!module_on()) return 0;
	events = venture_deliverability_parse_dsn(raw, error);
	if (!events) return -1;
	if (!events->len) return 0;
	/* The organization's own mailbox is the evidence's authority, not
	 * whoever pressed Sync now. */
	capability = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	for (i = 0; i < events->len; i++) {
		const gchar *outcome = venture_deliverability_apply(database, organization_id, g_ptr_array_index(events, i),
			"Delivery status notification in the synced mailbox", now, error);
		if (!outcome) return -1;
		if (!strcmp(outcome, "recorded")) recorded++;
	}
	return recorded;
}
static gboolean mailbox_hook(VentureDatabase *database, gint64 organization_id, GBytes *raw, GError **error)
{
	g_autoptr(GDateTime) now = venture_time_now();
	return venture_deliverability_apply_dsn(database, organization_id, raw, now, error) >= 0;
}

/* ------------------------------------------------------------------ */
/* Warm-up. */

gint64 venture_deliverability_ramp_limit(const gchar *schedule, gint64 day, GError **error)
{
	g_auto(GStrv) pairs = NULL;
	gint64 limit = 0, previous = 0;
	guint i;
	if (!schedule || !*schedule) schedule = VENTURE_DELIVERABILITY_DEFAULT_RAMP;
	if (!g_ascii_strcasecmp(schedule, "off")) return 0;
	pairs = g_strsplit(schedule, ",", 64);
	for (i = 0; pairs[i]; i++) {
		g_auto(GStrv) parts = g_strsplit(g_strstrip(pairs[i]), ":", 3);
		gint64 from = 0, value = 0;
		if (!parts[0] || !parts[1] || parts[2] ||
			!g_ascii_string_to_signed(g_strstrip(parts[0]), 10, 1, 3650, &from, NULL) ||
			!g_ascii_string_to_signed(g_strstrip(parts[1]), 10, 0, 10000000, &value, NULL) || from <= previous) {
			refuse(error, "Daily limits are day:limit pairs in increasing day order, for example 1:50,3:100,30:0");
			return -1;
		}
		if (i == 0 && from != 1) { refuse(error, "Daily limits start at day 1"); return -1; }
		previous = from;
		if (day >= from) limit = value;
	}
	return limit;
}
static GDateTime *utc_midnight(GDateTime *at)
{
	g_autoptr(GDateTime) utc = g_date_time_to_utc(at);
	return g_date_time_new_utc(g_date_time_get_year(utc), g_date_time_get_month(utc), g_date_time_get_day_of_month(utc), 0, 0, 0);
}
gboolean venture_deliverability_ramp_status(VentureDatabase *database, gint64 organization_id,
	GDateTime *now, gint64 *day, gint64 *limit, gint64 *used, GError **error)
{
	g_autoptr(VentureEntity) domain = NULL, first = NULL;
	g_autoptr(GDateTime) start = NULL, today = NULL, tomorrow = NULL, day_start = NULL;
	g_autofree gchar *schedule = NULL;
	gint64 age, cap, count = 0;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(now != NULL, FALSE);
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "marketing_sending_domain")) {
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_SENDING_DOMAIN);
		venture_query_set_organization(query, organization_id);
		venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
		domain = venture_database_find_one(database, query, error);
		if (error && *error) return FALSE;
	}
	if (domain) {
		schedule = string(domain, "ramp-schedule");
		g_object_get(domain, "warmup-started-on", &start, NULL);
	}
	if (!start) {
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
		venture_query_set_organization(query, organization_id);
		venture_query_add_filter_string(query, "related-type", VENTURE_FILTER_OP_EQ, "marketing_recipient", NULL);
		venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_EQ, "sent", NULL);
		venture_query_add_order(query, "sent-at", VENTURE_SORT_ASCENDING, NULL);
		first = venture_database_find_one(database, query, error);
		if (error && *error) return FALSE;
		if (first) g_object_get(first, "sent-at", &start, NULL);
	}
	today = utc_midnight(now);
	day_start = start ? utc_midnight(start) : g_date_time_ref(today);
	age = g_date_time_difference(today, day_start) / G_TIME_SPAN_DAY + 1;
	if (age < 1) age = 1;
	cap = venture_deliverability_ramp_limit(schedule, age, error);
	if (cap < 0) return FALSE;
	if (used) {
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
		g_autoptr(VentureDateRange) range = NULL;
		tomorrow = g_date_time_add_days(today, 1);
		range = venture_date_range_new(today, tomorrow);
		venture_query_set_organization(query, organization_id);
		venture_query_add_filter_string(query, "related-type", VENTURE_FILTER_OP_EQ, "marketing_recipient", NULL);
		venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_NE, "cancelled", NULL);
		if (!venture_query_set_date_range(query, "created-at", range, error)) return FALSE;
		count = venture_database_count(database, query, error);
		if (count < 0) return FALSE;
		*used = count;
	}
	if (day) *day = age;
	if (limit) *limit = cap;
	return TRUE;
}

/* ------------------------------------------------------------------ */
/* The machine-open judgement. Read docs/deliverability.org before adding
 * a rule: each one moves people from "opened" to "machine", and a rule
 * that catches people is worse than one that misses machines. */

static const gchar *const machine_agents[] = {
	/* Prefetching mail proxies that fetch on arrival, not on reading. */
	"yahoomailproxy",
	/* Link and attachment scanners at the receiving organization. */
	"barracuda", "mimecast", "proofpoint", "safelinks", "messagelabs", "symantec", "forcepoint",
	"trendmicro", "sophos", "fireeye", "cisco ironport", "zscaler",
	/* Crawlers and scripted clients. */
	"bot", "crawler", "spider", "slurp", "headlesschrome", "phantomjs", "python-", "curl/", "wget/",
	"go-http-client", "java/", "okhttp", "libwww", "httpclient", "axios/", "node-fetch",
	NULL
};
const gchar *venture_marketing_machine_reason(GDateTime *sent_at, GDateTime *observed_at,
	const gchar *user_agent, gint64 window_seconds)
{
	guint i;
	if (user_agent && *user_agent) {
		g_autofree gchar *lower = g_ascii_strdown(user_agent, MIN(strlen(user_agent), (gsize)512));
		for (i = 0; machine_agents[i]; i++)
			if (strstr(lower, machine_agents[i])) return "known scanner or prefetching proxy";
	}
	if (sent_at && observed_at && window_seconds > 0 &&
		g_date_time_difference(observed_at, sent_at) < window_seconds * G_TIME_SPAN_SECOND)
		return "within seconds of acceptance";
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Sending domains. */

static gboolean valid_label(const gchar *start, gsize length)
{
	gsize i;
	if (!length || length > 63 || start[0] == '-' || start[length - 1] == '-') return FALSE;
	for (i = 0; i < length; i++)
		if (!g_ascii_isalnum(start[i]) && start[i] != '-' && start[i] != '_') return FALSE;
	return TRUE;
}
static gboolean valid_domain(const gchar *domain)
{
	const gchar *p = domain, *dot;
	guint labels = 0;
	if (!domain || !*domain || strlen(domain) > 253) return FALSE;
	while (TRUE) {
		dot = strchr(p, '.');
		if (!valid_label(p, dot ? (gsize)(dot - p) : strlen(p))) return FALSE;
		labels++;
		if (!dot) break;
		p = dot + 1;
	}
	return labels >= 2;
}
static gboolean validate_domain(VentureDatabase *database, VentureEntity *row, VentureEntity *previous,
	gpointer data, GError **error)
{
	g_autofree gchar *domain = string(row, "domain"), *selector = string(row, "dkim-selector"), *schedule = string(row, "ramp-schedule");
	g_autofree gchar *lower = NULL;
	(void)database; (void)previous; (void)data;
	lower = g_ascii_strdown(domain ? g_strstrip(domain) : "", -1);
	if (!valid_domain(lower)) return refuse(error, "Enter the domain in your From address, for example example.com");
	if (selector && *g_strstrip(selector) && !valid_label(selector, strlen(selector)))
		return refuse(error, "A DKIM selector is one DNS label: letters, digits, hyphens and underscores");
	if (venture_deliverability_ramp_limit(schedule ? g_strstrip(schedule) : NULL, 1, error) < 0) return FALSE;
	g_object_set(row, "domain", lower, "dkim-selector", selector, "ramp-schedule", schedule, NULL);
	return TRUE;
}

void venture_deliverability_install(VentureDatabase *database)
{
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	venture_deliverability_adapter_register(&generic_adapter);
	venture_mail_sync_add_inbound_hook(mailbox_hook);
	venture_database_add_save_validator(database, VENTURE_TYPE_MARKETING_SENDING_DOMAIN, validate_domain, NULL, NULL);
}
