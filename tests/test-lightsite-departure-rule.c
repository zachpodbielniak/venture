/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* The leaving rule alone: every state against every action, and the six
 * calendar months a closed business may come back in. No database. */
#include <venture.h>

typedef struct {
	VentureDepartureState state;
	VentureDepartureAction action;
	VentureDepartureOutcome outcome;
	VentureDepartureState next;
} Row;

#define A VENTURE_DEPARTURE_STATE_ACTIVE
#define G VENTURE_DEPARTURE_STATE_CLOSING
#define C VENTURE_DEPARTURE_STATE_CLOSED
#define P VENTURE_DEPARTURE_STATE_PURGED
#define CHANGED VENTURE_DEPARTURE_OUTCOME_CHANGED
#define SAME VENTURE_DEPARTURE_OUTCOME_UNCHANGED
#define CONFLICT VENTURE_DEPARTURE_OUTCOME_CONFLICT
#define GONE VENTURE_DEPARTURE_OUTCOME_GONE

static const Row table[] = {
	{ A, VENTURE_DEPARTURE_ACTION_LEAVE, CHANGED, G },
	{ G, VENTURE_DEPARTURE_ACTION_LEAVE, SAME, G },
	{ C, VENTURE_DEPARTURE_ACTION_LEAVE, CONFLICT, C },
	{ P, VENTURE_DEPARTURE_ACTION_LEAVE, CONFLICT, P },
	{ A, VENTURE_DEPARTURE_ACTION_CLOSE, CONFLICT, A },
	{ G, VENTURE_DEPARTURE_ACTION_CLOSE, CHANGED, C },
	{ C, VENTURE_DEPARTURE_ACTION_CLOSE, SAME, C },
	{ P, VENTURE_DEPARTURE_ACTION_CLOSE, CONFLICT, P },
	{ A, VENTURE_DEPARTURE_ACTION_RETURN, CONFLICT, A },
	{ G, VENTURE_DEPARTURE_ACTION_RETURN, CONFLICT, G },
	{ C, VENTURE_DEPARTURE_ACTION_RETURN, CHANGED, A },
	{ P, VENTURE_DEPARTURE_ACTION_RETURN, GONE, P },
	{ A, VENTURE_DEPARTURE_ACTION_PURGE, CONFLICT, A },
	{ G, VENTURE_DEPARTURE_ACTION_PURGE, CONFLICT, G },
	{ C, VENTURE_DEPARTURE_ACTION_PURGE, CHANGED, P },
	{ P, VENTURE_DEPARTURE_ACTION_PURGE, SAME, P },
};

static GDateTime *
at(const gchar *text)
{
	GDateTime *when = g_date_time_new_from_iso8601(text, NULL);
	g_assert_nonnull(when);
	return when;
}

static void
test_table(void)
{
	g_autoptr(GDateTime) left = at("2026-03-10T12:00:00Z");
	g_autoptr(GDateTime) now = at("2026-04-01T12:00:00Z");
	guint i;
	for (i = 0; i < G_N_ELEMENTS(table); i++) {
		VentureDepartureState next = (VentureDepartureState)-1;
		VentureDepartureOutcome outcome = venture_departure_decide(table[i].state, table[i].action, left, now, &next);
		g_test_message("row %u", i);
		g_assert_cmpint(outcome, ==, table[i].outcome);
		g_assert_cmpint(next, ==, table[i].next);
	}
}

static void
test_six_months(void)
{
	g_autoptr(GDateTime) left = at("2026-03-10T12:00:00Z");
	g_autoptr(GDateTime) last = at("2026-09-10T12:00:00Z");
	g_autoptr(GDateTime) late = at("2026-09-10T12:00:01Z");
	g_autoptr(GDateTime) month_end = at("2026-08-31T09:00:00Z");
	g_autoptr(GDateTime) february = at("2027-02-28T09:00:00Z");
	g_autoptr(GDateTime) march = at("2027-03-01T09:00:00Z");
	VentureDepartureState next;
	g_assert_cmpint(venture_departure_decide(C, VENTURE_DEPARTURE_ACTION_RETURN, left, last, &next), ==, CHANGED);
	g_assert_cmpint(next, ==, A);
	g_assert_cmpint(venture_departure_decide(C, VENTURE_DEPARTURE_ACTION_RETURN, left, late, &next), ==, GONE);
	g_assert_cmpint(next, ==, C);
	/* Calendar months: August 31st plus six is the end of February. */
	g_assert_cmpint(venture_departure_decide(C, VENTURE_DEPARTURE_ACTION_RETURN, month_end, february, &next), ==, CHANGED);
	g_assert_cmpint(venture_departure_decide(C, VENTURE_DEPARTURE_ACTION_RETURN, month_end, march, &next), ==, GONE);
	/* A closed business with no closing time cannot be shown inside the window. */
	g_assert_cmpint(venture_departure_decide(C, VENTURE_DEPARTURE_ACTION_RETURN, NULL, last, &next), ==, GONE);
}

static void
test_names(void)
{
	VentureDepartureAction action;
	g_assert_true(venture_departure_action_from_string("leave", &action));
	g_assert_cmpint(action, ==, VENTURE_DEPARTURE_ACTION_LEAVE);
	g_assert_true(venture_departure_action_from_string("close", &action));
	g_assert_cmpint(action, ==, VENTURE_DEPARTURE_ACTION_CLOSE);
	g_assert_true(venture_departure_action_from_string("return", &action));
	g_assert_cmpint(action, ==, VENTURE_DEPARTURE_ACTION_RETURN);
	g_assert_true(venture_departure_action_from_string("purge", &action));
	g_assert_cmpint(action, ==, VENTURE_DEPARTURE_ACTION_PURGE);
	g_assert_false(venture_departure_action_from_string("Leave", &action));
	g_assert_false(venture_departure_action_from_string("", &action));
	g_assert_false(venture_departure_action_from_string(NULL, &action));
	g_assert_cmpstr(venture_enum_to_nick(VENTURE_TYPE_DEPARTURE_STATE, A), ==, "active");
	g_assert_cmpstr(venture_enum_to_nick(VENTURE_TYPE_DEPARTURE_STATE, G), ==, "closing");
	g_assert_cmpstr(venture_enum_to_nick(VENTURE_TYPE_DEPARTURE_STATE, C), ==, "closed");
	g_assert_cmpstr(venture_enum_to_nick(VENTURE_TYPE_DEPARTURE_STATE, P), ==, "purged");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/lightsite-departure-rule/table", test_table);
	g_test_add_func("/lightsite-departure-rule/six-months", test_six_months);
	g_test_add_func("/lightsite-departure-rule/names", test_names);
	return g_test_run();
}
