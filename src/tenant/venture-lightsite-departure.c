/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * A hosted business leaving Lightsite: leave, close, return, purge. The
 * state lives on the Venture organization because Venture owns the
 * business; Lightsite only shows it. The rule is venture_departure_decide()
 * and nothing else here decides anything: the rest reads the organization,
 * stores the change and answers.
 */
#include "venture.h"
#include <string.h>

#define RETURN_WINDOW_MONTHS 6

static const gchar *const action_names[] = { "leave", "close", "return", "purge" };

gboolean
venture_departure_action_from_string(const gchar *name, VentureDepartureAction *action)
{
	guint i;
	for (i = 0; name && i < G_N_ELEMENTS(action_names); i++)
		if (strcmp(name, action_names[i]) == 0) {
			*action = (VentureDepartureAction)i;
			return TRUE;
		}
	return FALSE;
}

/* Inside the window up to and including six calendar months after left_at. */
static gboolean
within_window(GDateTime *left_at, GDateTime *now)
{
	g_autoptr(GDateTime) last = NULL;
	if (!left_at || !now) return FALSE;
	last = g_date_time_add_months(left_at, RETURN_WINDOW_MONTHS);
	return last && g_date_time_compare(now, last) <= 0;
}

VentureDepartureOutcome
venture_departure_decide(VentureDepartureState state, VentureDepartureAction action,
	GDateTime *left_at, GDateTime *now, VentureDepartureState *next)
{
	VentureDepartureState from = state, to = state;
	VentureDepartureOutcome outcome = VENTURE_DEPARTURE_OUTCOME_CONFLICT;
	switch (action) {
	case VENTURE_DEPARTURE_ACTION_LEAVE:
		if (from == VENTURE_DEPARTURE_STATE_ACTIVE) { to = VENTURE_DEPARTURE_STATE_CLOSING; outcome = VENTURE_DEPARTURE_OUTCOME_CHANGED; }
		else if (from == VENTURE_DEPARTURE_STATE_CLOSING) outcome = VENTURE_DEPARTURE_OUTCOME_UNCHANGED;
		break;
	case VENTURE_DEPARTURE_ACTION_CLOSE:
		if (from == VENTURE_DEPARTURE_STATE_CLOSING) { to = VENTURE_DEPARTURE_STATE_CLOSED; outcome = VENTURE_DEPARTURE_OUTCOME_CHANGED; }
		else if (from == VENTURE_DEPARTURE_STATE_CLOSED) outcome = VENTURE_DEPARTURE_OUTCOME_UNCHANGED;
		break;
	case VENTURE_DEPARTURE_ACTION_RETURN:
		if (from == VENTURE_DEPARTURE_STATE_PURGED) outcome = VENTURE_DEPARTURE_OUTCOME_GONE;
		else if (from == VENTURE_DEPARTURE_STATE_CLOSED) {
			if (within_window(left_at, now)) { to = VENTURE_DEPARTURE_STATE_ACTIVE; outcome = VENTURE_DEPARTURE_OUTCOME_CHANGED; }
			else outcome = VENTURE_DEPARTURE_OUTCOME_GONE;
		}
		break;
	case VENTURE_DEPARTURE_ACTION_PURGE:
		if (from == VENTURE_DEPARTURE_STATE_CLOSED) { to = VENTURE_DEPARTURE_STATE_PURGED; outcome = VENTURE_DEPARTURE_OUTCOME_CHANGED; }
		else if (from == VENTURE_DEPARTURE_STATE_PURGED) outcome = VENTURE_DEPARTURE_OUTCOME_UNCHANGED;
		break;
	default:
		break;
	}
	if (next) *next = to;
	return outcome;
}

static void
add_time(JsonBuilder *builder, VentureEntity *organization, const gchar *property, const gchar *member)
{
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *text = NULL;
	g_object_get(organization, property, &when, NULL);
	json_builder_set_member_name(builder, member);
	if (when) {
		text = venture_time_to_string(when);
		json_builder_add_string_value(builder, text);
	} else json_builder_add_null_value(builder);
}

static JsonNode *
view(VentureEntity *organization)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	VentureDepartureState state = VENTURE_DEPARTURE_STATE_ACTIVE;
	g_object_get(organization, "departure-state", &state, NULL);
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "organization_id");
	json_builder_add_int_value(builder, venture_entity_get_id(organization));
	json_builder_set_member_name(builder, "state");
	json_builder_add_string_value(builder, venture_enum_to_nick(VENTURE_TYPE_DEPARTURE_STATE, state));
	add_time(builder, organization, "left-at", "left_at");
	add_time(builder, organization, "returned-at", "returned_at");
	add_time(builder, organization, "purged-at", "purged_at");
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

/* The organization, read internally. Callers judge the trusted service
 * first, so an ordinary caller learns nothing about which exist. */
static VentureEntity *
business(VentureDatabase *database, gint64 organization_id, GError **error)
{
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(GError) local = NULL;
	if (organization_id > 0)
		organization = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, organization_id, &local);
	if (local && !g_error_matches(local, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND)) {
		g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}
	if (!organization || venture_entity_is_deleted(organization)) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No such organization");
		return NULL;
	}
	return g_steal_pointer(&organization);
}

JsonNode *
venture_lightsite_departure_read(VentureDatabase *database, gint64 organization_id, GError **error)
{
	g_autoptr(VentureEntity) organization = NULL;
	if (!venture_tenant_service_check_trusted_service(venture_tenant_service_get(database), NULL, error))
		return NULL;
	organization = business(database, organization_id, error);
	return organization ? view(organization) : NULL;
}

JsonNode *
venture_lightsite_departure_act(VentureDatabase *database, gint64 organization_id,
	VentureDepartureAction action, const VentureActor *actor, gboolean *gone, GError **error)
{
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), left_at = NULL;
	VentureDepartureState state = VENTURE_DEPARTURE_STATE_ACTIVE, next;
	VentureDepartureOutcome outcome;
	JsonNode *answer = NULL;
	if (gone) *gone = FALSE;
	if (!venture_tenant_service_check_trusted_service(venture_tenant_service_get(database), NULL, error))
		return NULL;
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	/* Read and write in one transaction, so two calls at once cannot both
	 * see the old state. */
	if (!venture_database_begin(database, error)) return NULL;
	organization = business(database, organization_id, error);
	if (!organization) goto fail;
	g_object_get(organization, "departure-state", &state, "left-at", &left_at, NULL);
	outcome = venture_departure_decide(state, action, left_at, now, &next);
	if (outcome == VENTURE_DEPARTURE_OUTCOME_CONFLICT || outcome == VENTURE_DEPARTURE_OUTCOME_GONE) {
		if (gone) *gone = outcome == VENTURE_DEPARTURE_OUTCOME_GONE;
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Cannot %s a business that is %s%s",
			action_names[action], venture_enum_to_nick(VENTURE_TYPE_DEPARTURE_STATE, state),
			outcome == VENTURE_DEPARTURE_OUTCOME_GONE && state == VENTURE_DEPARTURE_STATE_CLOSED ?
				" for more than six calendar months" : "");
		goto fail;
	}
	if (outcome == VENTURE_DEPARTURE_OUTCOME_CHANGED) {
		g_object_set(organization, "departure-state", next, NULL);
		if (action == VENTURE_DEPARTURE_ACTION_CLOSE) g_object_set(organization, "left-at", now, NULL);
		else if (action == VENTURE_DEPARTURE_ACTION_RETURN) g_object_set(organization, "returned-at", now, NULL);
		else if (action == VENTURE_DEPARTURE_ACTION_PURGE) g_object_set(organization, "purged-at", now, NULL);
		if (!venture_database_save(database, organization, actor, error)) goto fail;
	}
	answer = view(organization);
	if (!venture_database_commit(database, error)) {
		json_node_unref(answer);
		return NULL;
	}
	return answer;
fail:
	venture_database_rollback(database);
	return NULL;
}
