/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "sequences/venture-sequence-service-private.h"

/* --- The channel hook ------------------------------------------------------ */

/* Process-wide, like the mail preview kinds: a channel is code, not data.
 * Registered on the main thread at start-up (or by a test). */
typedef struct { VentureSequenceChannelExecute execute; gpointer data; } Channel;
static GHashTable *channels;

void
venture_sequence_channel_register(gint channel, VentureSequenceChannelExecute execute, gpointer data)
{
	Channel *entry;
	g_return_if_fail(execute != NULL);
	g_return_if_fail(channel != 0 && channel != 4);
	if (channels == NULL)
		channels = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
	entry = g_new0(Channel, 1);
	entry->execute = execute;
	entry->data = data;
	g_hash_table_replace(channels, GINT_TO_POINTER(channel), entry);
}

void
venture_sequence_channel_unregister(gint channel)
{
	if (channels != NULL)
		g_hash_table_remove(channels, GINT_TO_POINTER(channel));
}

gboolean
venture_sequence_channel_lookup(gint channel, VentureSequenceChannelExecute *execute, gpointer *data)
{
	Channel *entry = channels != NULL ? g_hash_table_lookup(channels, GINT_TO_POINTER(channel)) : NULL;
	if (entry == NULL)
		return FALSE;
	if (execute != NULL) *execute = entry->execute;
	if (data != NULL) *data = entry->data;
	return TRUE;
}

/* --- Branches ---------------------------------------------------------------- */

static gint64
number(VentureEntity *row, const gchar *field)
{
	gint64 value = 0;
	g_object_get(row, field, &value, NULL);
	return value;
}

static gint
choice(VentureEntity *row, const gchar *field)
{
	gint value = 0;
	g_object_get(row, field, &value, NULL);
	return value;
}

gboolean
venture_sequence_branch_taken(VentureDatabase *database, VentureEntity *enrollment, VentureEntity *step,
	GPtrArray *history, GDateTime *deadline, gboolean *taken, GError **error)
{
	VentureEntity *email = NULL;
	g_autoptr(GDateTime) sent = NULL;
	gint64 org = venture_entity_get_organization_id(enrollment);
	gint on = choice(step, "branch-on");
	guint i;
	*taken = FALSE;
	/* The latest email of this enrollment before the branch: the one the
	 * person could have opened, clicked or answered. */
	for (i = 0; i < history->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(history, i);
		gint state = choice(row, "state");
		if (choice(row, "channel") == 0 && (state == 0 || state == 1))
			email = row;
	}
	if (email == NULL)
		return TRUE;
	g_object_get(email, "scheduled-at", &sent, NULL);
	if (on == 1 || on == 2)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SEQUENCE_TRACKING_EVENT);
		g_autoptr(GPtrArray) events = NULL;
		g_autofree gchar *cutoff = venture_time_to_string(deadline);
		venture_query_set_organization(query, org);
		venture_query_set_limit(query, 1000);
		venture_query_add_filter_int(query, "delivery-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(email), NULL);
		venture_query_add_filter_string(query, "occurred-at", VENTURE_FILTER_OP_LTE, cutoff, NULL);
		events = venture_database_find(database, query, error);
		if (events == NULL)
			return FALSE;
		for (i = 0; i < events->len && !*taken; i++)
		{
			VentureEntity *event = g_ptr_array_index(events, i);
			g_autofree gchar *machine = NULL;
			g_object_get(event, "machine-reason", &machine, NULL);
			*taken = choice(event, "kind") == (on == 1 ? 0 : 1) && (machine == NULL || *machine == '\0');
		}
		return TRUE;
	}
	if (on == 3)
	{
		/* The one reply rule: anything inbound from them, after the email
		 * and by the end of the wait. */
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INTERACTION);
		g_autoptr(GPtrArray) found = NULL;
		g_autofree gchar *cutoff = venture_time_to_string(deadline);
		venture_query_set_organization(query, org);
		venture_query_set_limit(query, 1000);
		venture_query_add_filter_int(query, "contact-id", VENTURE_FILTER_OP_EQ, number(enrollment, "contact-id"), NULL);
		venture_query_add_filter_string(query, "occurred-at", VENTURE_FILTER_OP_LTE, cutoff, NULL);
		if (sent != NULL)
		{
			g_autofree gchar *after = venture_time_to_string(sent);
			venture_query_add_filter_string(query, "occurred-at", VENTURE_FILTER_OP_GTE, after, NULL);
		}
		found = venture_database_find(database, query, error);
		if (found == NULL)
			return FALSE;
		for (i = 0; i < found->len && !*taken; i++)
		{
			gboolean outbound = TRUE;
			g_object_get(g_ptr_array_index(found, i), "outbound", &outbound, NULL);
			*taken = !outbound;
		}
	}
	return TRUE;
}
typedef struct { VentureSequenceEmailGate gate; gpointer data; } EmailGate;

void
venture_sequence_add_email_gate(VentureDatabase *database, VentureSequenceEmailGate gate, gpointer data)
{
	GArray *gates;
	EmailGate entry;
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	g_return_if_fail(gate != NULL);
	gates = g_object_get_data(G_OBJECT(database), "venture-sequence-email-gates");
	if (gates == NULL)
	{
		gates = g_array_new(FALSE, FALSE, sizeof(EmailGate));
		g_object_set_data_full(G_OBJECT(database), "venture-sequence-email-gates", gates, (GDestroyNotify)g_array_unref);
	}
	entry.gate = gate;
	entry.data = data;
	g_array_append_val(gates, entry);
}

gboolean
venture_sequence_email_gates_ask(VentureDatabase *database, VentureEntity *sequence, VentureEntity *contact,
	GDateTime *as_of, gchar **skip_reason, GError **error)
{
	GArray *gates = g_object_get_data(G_OBJECT(database), "venture-sequence-email-gates");
	guint i;
	*skip_reason = NULL;
	for (i = 0; gates != NULL && i < gates->len && *skip_reason == NULL; i++)
	{
		EmailGate *entry = &g_array_index(gates, EmailGate, i);
		if (!entry->gate(database, sequence, contact, as_of, skip_reason, entry->data, error))
			return FALSE;
	}
	return TRUE;
}
/* --- Triggers ---------------------------------------------------------------- */

static gchar *
text(VentureEntity *row, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(row, field, &value, NULL);
	return value;
}

static gboolean
sequences_on(void)
{
	return venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "sequence_trigger");
}

/* The active triggers of one kind in an organization. */
static GPtrArray *
triggers_of(VentureDatabase *database, gint64 org, gint kind, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SEQUENCE_TRIGGER);
	const gchar *nick = venture_enum_to_nick(venture_sequence_trigger_kind_get_type(), kind);
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, 200);
	venture_query_add_filter_string(query, "kind", VENTURE_FILTER_OP_EQ, nick, NULL);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(database, query, error);
}

static gchar *
reason_for(VentureEntity *trigger, const gchar *what)
{
	g_autofree gchar *name = text(trigger, "name");
	return g_strdup_printf("Trigger %s: %s", name != NULL && *name ? name : "(unnamed)", what);
}

/* Deal stage changed. The stage is the deal's own stage (lead, qualified,
 * proposal, negotiation, won, lost); a trigger naming none takes any move. */
gboolean
venture_sequence_triggers_deal(VentureSequenceService *self, VentureEntity *deal, VentureEntity *previous, GError **error)
{
	VentureDatabase *database = NULL;
	g_autoptr(GPtrArray) triggers = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	const gchar *stage;
	g_autofree gchar *identity = NULL;
	guint i;
	g_object_get(self, "database", &database, NULL);
	if (database == NULL) return TRUE;
	g_object_unref(database);
	if (!sequences_on() || previous == NULL || choice(deal, "stage") == choice(previous, "stage"))
		return TRUE;
	stage = venture_enum_to_nick(venture_deal_stage_get_type(), choice(deal, "stage"));
	triggers = triggers_of(database, venture_entity_get_organization_id(deal), 2, error);
	if (triggers == NULL) return FALSE;
	identity = g_strdup_printf("deal:%s:%" G_GINT64_FORMAT, venture_entity_get_uuid(deal), venture_entity_get_version(deal));
	for (i = 0; i < triggers->len; i++)
	{
		VentureEntity *trigger = g_ptr_array_index(triggers, i);
		g_autofree gchar *wanted = text(trigger, "deal-stage"), *why = NULL;
		if (wanted != NULL && *g_strstrip(wanted) && g_ascii_strcasecmp(wanted, stage != NULL ? stage : ""))
			continue;
		why = g_strdup_printf("deal moved to %s", stage != NULL ? stage : "another stage");
		{
			g_autofree gchar *reason = reason_for(trigger, why);
			if (venture_sequence_trigger_enroll(self, trigger, number(deal, "contact-id"), venture_entity_get_id(deal),
				identity, reason, now, error) < 0)
				return FALSE;
		}
	}
	return TRUE;
}

/* Tag added: the validator sees what the save adds, the saved handler
 * enrols once the contact has an id. Hooks on contacts must not fail a
 * contact's save, so a refusal here is logged, never raised. */
static gboolean
stash_tags(VentureDatabase *database, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	g_autofree gchar *before = NULL, *after = NULL;
	gchar **added;
	(void)database; (void)data; (void)error;
	if (!sequences_on()) return TRUE;
	if (previous != NULL) g_object_get(previous, "tags", &before, NULL);
	g_object_get(row, "tags", &after, NULL);
	added = venture_tags_added(before, after);
	if (added[0] == NULL) { g_strfreev(added); g_object_set_data(G_OBJECT(row), "venture-journey-tags", NULL); return TRUE; }
	g_object_set_data_full(G_OBJECT(row), "venture-journey-tags", added, (GDestroyNotify)g_strfreev);
	return TRUE;
}

static void
fire_tags(VentureDatabase *database, VentureEntity *contact)
{
	gchar **added = g_object_steal_data(G_OBJECT(contact), "venture-journey-tags");
	g_autoptr(GPtrArray) triggers = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	guint i, j;
	if (added == NULL) return;
	triggers = triggers_of(database, venture_entity_get_organization_id(contact), 1, &error);
	for (i = 0; triggers != NULL && i < triggers->len; i++)
	{
		VentureEntity *trigger = g_ptr_array_index(triggers, i);
		g_autofree gchar *tag = text(trigger, "tag");
		for (j = 0; added[j] != NULL; j++)
		{
			g_autofree gchar *key = NULL, *identity = NULL, *why = NULL, *reason = NULL;
			if (!venture_tags_has(added[j], tag)) continue;
			key = g_utf8_casefold(added[j], -1);
			identity = g_strdup_printf("tag:%s:%s:%" G_GINT64_FORMAT, venture_entity_get_uuid(contact), key, venture_entity_get_version(contact));
			why = g_strdup_printf("tag %s added", added[j]);
			reason = reason_for(trigger, why);
			if (venture_sequence_trigger_enroll(venture_sequence_service_get(database), trigger, venture_entity_get_id(contact), 0,
				identity, reason, now, &error) < 0)
				break;
		}
	}
	if (error != NULL)
		g_message("Journey tag trigger did not enrol contact %" G_GINT64_FORMAT ": %s", venture_entity_get_id(contact), error->message);
	g_strfreev(added);
}

/* Form submitted: the response's contact, from a personal link, or the
 * contact its lead was converted into. Once per response. */
static void
fire_form(VentureDatabase *database, VentureEntity *response)
{
	g_autoptr(GPtrArray) triggers = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *identity = g_strdup_printf("form:%s", venture_entity_get_uuid(response));
	gint64 contact = number(response, "contact-id"), form = number(response, "form-id");
	guint i;
	if (contact <= 0 && number(response, "lead-id") > 0)
	{
		g_autoptr(VentureEntity) lead = venture_database_get(database, VENTURE_TYPE_LEAD, number(response, "lead-id"), NULL);
		if (lead != NULL) contact = number(lead, "converted-contact-id");
	}
	if (contact <= 0) return;
	triggers = triggers_of(database, venture_entity_get_organization_id(response), 0, &error);
	for (i = 0; triggers != NULL && i < triggers->len; i++)
	{
		VentureEntity *trigger = g_ptr_array_index(triggers, i);
		g_autofree gchar *reason = NULL;
		if (number(trigger, "form-id") > 0 && number(trigger, "form-id") != form) continue;
		reason = reason_for(trigger, "form submitted");
		if (venture_sequence_trigger_enroll(venture_sequence_service_get(database), trigger, contact, 0,
			identity, reason, now, &error) < 0)
			break;
	}
	if (error != NULL)
		g_message("Journey form trigger did not enrol a contact: %s", error->message);
}

static void
entity_saved(VentureDatabase *database, VentureEntity *entity, gboolean created, gpointer data)
{
	(void)created; (void)data;
	if (!sequences_on()) return;
	if (VENTURE_IS_CONTACT(entity)) fire_tags(database, entity);
	else if (VENTURE_IS_FORM_SUBMISSION(entity)) fire_form(database, entity);
}

static gint64
feed_end(VentureDatabase *database, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BUSINESS_EVENT);
	g_autoptr(VentureEntity) last = NULL;
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	last = venture_database_find_one(database, query, error);
	if (error != NULL && *error != NULL) return -1;
	return last != NULL ? venture_entity_get_id(last) : 0;
}

#define TRIGGER_PERMIT "venture-sequence-trigger-permit"

/* A trigger starts at the end of the feed, so adding one does not enrol
 * everybody who ever paid; its position is the sweep's to move. */
static gboolean
validate_trigger(VentureDatabase *database, VentureEntity *row, VentureEntity *previous, gpointer data, GError **error)
{
	g_autofree gchar *tag = text(row, "tag"), *stage = text(row, "deal-stage");
	gint kind = choice(row, "kind"), parsed = 0;
	gint64 days = number(row, "days");
	(void)data;
	if (g_object_get_data(G_OBJECT(database), TRIGGER_PERMIT) == row)
	{
		g_object_set_data(G_OBJECT(database), TRIGGER_PERMIT, NULL);
		return TRUE;
	}
	if (number(row, "sequence-id") > 0)
	{
		g_autoptr(VentureEntity) sequence = venture_database_get(database, VENTURE_TYPE_SEQUENCE, number(row, "sequence-id"), NULL);
		if (sequence == NULL || venture_entity_get_organization_id(sequence) != venture_entity_get_organization_id(row))
		{
			venture_set_error_validation(error, "sequence_id", "Choose a journey in this organization");
			return FALSE;
		}
	}
	else
	{
		venture_set_error_validation(error, "sequence_id", "Choose the journey it enrols into");
		return FALSE;
	}
	if (kind == 1 && !venture_tags_valid(tag))
	{
		venture_set_error_validation(error, "tag", "Name the tag, up to 64 characters with no comma");
		return FALSE;
	}
	if (kind == 2 && stage != NULL && *g_strstrip(stage) &&
		!venture_enum_from_nick(venture_deal_stage_get_type(), stage, &parsed))
	{
		venture_set_error_validation(error, "deal_stage", "A deal stage is lead, qualified, proposal, negotiation, won or lost");
		return FALSE;
	}
	if (days < 0 || days > 365)
	{
		venture_set_error_validation(error, "days", "Days are 0 to 365; empty is %d", VENTURE_SEQUENCE_QUOTE_DAYS);
		return FALSE;
	}
	if (previous != NULL)
		g_object_set(row, "feed-cursor", number(previous, "feed-cursor"), NULL);
	else
	{
		gint64 end = feed_end(database, venture_entity_get_organization_id(row), error);
		if (end < 0) return FALSE;
		g_object_set(row, "feed-cursor", end, NULL);
	}
	return TRUE;
}

/* The contact an event is about, or 0; @deal is set for a quote's deal. */
static gint64
event_contact(VentureDatabase *database, gint64 org, gint kind, VentureEntity *event, gint64 *deal)
{
	gint64 source = number(event, "source-id");
	*deal = 0;
	if (kind == 4)
	{
		g_autoptr(VentureEntity) invoice = venture_database_get(database, VENTURE_TYPE_INVOICE, source, NULL);
		return invoice != NULL && venture_entity_get_organization_id(invoice) == org ? number(invoice, "contact-id") : 0;
	}
	if (kind == 5)
	{
		g_autoptr(VentureEntity) reservation = venture_database_get(database, VENTURE_TYPE_BOOKING_RESERVATION, source, NULL);
		g_autoptr(VentureEntity) activity = NULL;
		if (reservation == NULL || venture_entity_get_organization_id(reservation) != org) return 0;
		activity = venture_database_get(database, VENTURE_TYPE_ACTIVITY, number(reservation, "activity-id"), NULL);
		return activity != NULL && venture_entity_get_organization_id(activity) == org ? number(activity, "contact-id") : 0;
	}
	{
		/* Quote sent but not accepted: only a quote still waiting. */
		g_autoptr(VentureEntity) quote = venture_database_get(database, VENTURE_TYPE_QUOTE, source, NULL);
		if (quote == NULL || venture_entity_is_deleted(quote) || venture_entity_get_organization_id(quote) != org ||
			choice(quote, "status") != VENTURE_QUOTE_SENT)
			return 0;
		*deal = number(quote, "deal-id");
		return number(quote, "contact-id");
	}
}

gint
venture_sequence_service_run_triggers(VentureSequenceService *self, gint64 organization_id,
	GDateTime *now, const VentureActor *actor, GError **error)
{
	static const struct { gint kind; const gchar *event; const gchar *words; } feed[] = {
		{ 3, "quote.sent", "quote sent and not accepted" }, { 4, "invoice.paid", "invoice paid" }, { 5, "booking.created", "booking made" }
	};
	VentureDatabase *database = NULL;
	gint made = 0;
	guint f, i, k;
	(void)actor;
	g_return_val_if_fail(VENTURE_IS_SEQUENCE_SERVICE(self), -1);
	if (now == NULL || organization_id <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Triggers need a clock and one organization");
		return -1;
	}
	if (!sequences_on()) return 0;
	g_object_get(self, "database", &database, NULL);
	if (database == NULL) return 0;
	g_object_unref(database);
	for (f = 0; f < G_N_ELEMENTS(feed); f++)
	{
		g_autoptr(GPtrArray) triggers = triggers_of(database, organization_id, feed[f].kind, error);
		if (triggers == NULL) return -1;
		for (i = 0; i < triggers->len; i++)
		{
			VentureEntity *trigger = g_ptr_array_index(triggers, i);
			g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BUSINESS_EVENT);
			g_autoptr(GPtrArray) events = NULL;
			g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
			gint64 cursor = number(trigger, "feed-cursor"), start = cursor;
			venture_query_set_organization(query, organization_id);
			venture_query_set_limit(query, VENTURE_SEQUENCE_TRIGGER_PAGE);
			venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_GT, cursor, NULL);
			venture_query_add_filter_string(query, "event", VENTURE_FILTER_OP_EQ, feed[f].event, NULL);
			if (feed[f].kind == 3)
			{
				/* An event is read once it is old enough, never before:
				 * the position only moves past matured quotes. */
				gint64 days = number(trigger, "days");
				g_autoptr(GDateTime) matured = g_date_time_add_days(now, -(gint)(days > 0 ? days : VENTURE_SEQUENCE_QUOTE_DAYS));
				g_autofree gchar *cutoff = venture_time_to_string(matured);
				venture_query_add_filter_string(query, "occurred-at", VENTURE_FILTER_OP_LTE, cutoff, NULL);
			}
			venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
			events = venture_database_find(database, query, error);
			if (events == NULL) return -1;
			for (k = 0; k < events->len; k++)
			{
				VentureEntity *event = g_ptr_array_index(events, k);
				g_autofree gchar *identity = g_strdup_printf("event:%s", venture_entity_get_uuid(event));
				g_autofree gchar *reason = reason_for(trigger, feed[f].words);
				gint64 deal = 0, contact = event_contact(database, organization_id, feed[f].kind, event, &deal);
				gint result = venture_sequence_trigger_enroll(self, trigger, contact, deal, identity, reason, now, error);
				if (result < 0) return -1;
				made += result;
				cursor = venture_entity_get_id(event);
			}
			if (cursor != start)
			{
				g_object_set(trigger, "feed-cursor", cursor, NULL);
				g_object_set_data(G_OBJECT(database), TRIGGER_PERMIT, trigger);
				if (!venture_database_save(database, trigger, NULL, error))
				{
					g_object_set_data(G_OBJECT(database), TRIGGER_PERMIT, NULL);
					return -1;
				}
			}
		}
	}
	return made;
}

void
venture_sequence_journeys_install(VentureDatabase *database)
{
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	if (g_object_get_data(G_OBJECT(database), "venture-sequence-journeys") != NULL) return;
	g_object_set_data(G_OBJECT(database), "venture-sequence-journeys", GINT_TO_POINTER(1));
	venture_database_add_save_validator(database, VENTURE_TYPE_CONTACT, stash_tags, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_SEQUENCE_TRIGGER, validate_trigger, NULL, NULL);
	g_signal_connect(database, "entity-saved", G_CALLBACK(entity_saved), NULL);
}
