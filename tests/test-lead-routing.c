/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
/* Routing rules, round-robin rotation and the scoring formula. Each test
 * here fails when the rule it names is removed from the lead service. */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
	VentureWebServer *server;
	gchar *state_dir;
	guint16 port;
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	if (f->server != NULL) venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	if (f->state_dir != NULL) { venture_test_remove_tree(f->state_dir); g_free(f->state_dir); }
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static VentureEntity *
record(Fixture *f, const gchar *type, const gchar *name)
{
	GType t = venture_entity_registry_lookup(venture_entity_registry_get_default(), type);
	g_assert_cmpuint(t, !=, G_TYPE_INVALID);
	return g_object_new(t, "name", name, "organization-id", f->org, NULL);
}

static void
save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, e, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
refused(Fixture *f, VentureEntity *e, gint code)
{
	g_autoptr(GError) error = NULL;
	g_assert_false(venture_database_save(f->db, e, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, code);
	g_assert_nonnull(strstr(error->message, "VentureLeadService"));
}

static gchar *
owner_of(VentureEntity *lead)
{
	gchar *owner = NULL;
	g_object_get(lead, "owner", &owner, NULL);
	return owner;
}

/* A routing rule with the given conditions and action. */
static VentureEntity *
rule(Fixture *f, const gchar *name, gint64 position, const gchar *conditions,
	VentureLeadRoutingAction action, const gchar *assign_to, gint64 team, gint64 venture)
{
	VentureEntity *r = record(f, "lead_routing_rule", name);
	g_object_set(r, "position", position, "conditions", conditions, "action", action,
		"assign-to", assign_to, "team-id", team, "venture-id", venture, "active", TRUE, NULL);
	save(f, r);
	return r;
}

static VentureEntity *
lead(Fixture *f, const gchar *name, const gchar *source)
{
	VentureEntity *l = record(f, "lead", name);
	g_object_set(l, "source", source, NULL);
	return l;
}

static gint64
timeline(Fixture *f, VentureEntity *lead_record, const gchar *subject)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_INTERACTION);
	venture_query_set_organization(q, f->org);
	venture_query_add_filter_int(q, "lead-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(lead_record), NULL);
	venture_query_add_filter_string(q, "subject", VENTURE_FILTER_OP_EQ, subject, NULL);
	return venture_database_count(f->db, q, NULL);
}

static gint64
count(Fixture *f, const gchar *type)
{
	g_autoptr(VentureQuery) q = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), type));
	venture_query_set_organization(q, f->org);
	return venture_database_count(f->db, q, NULL);
}

static void
test_records(void)
{
	static const gchar *const names[] = { "lead_routing_rule", "lead_scoring_rule", "lead_score_history" };
	g_autoptr(VentureModuleRegistry) modules = venture_module_registry_new();
	guint i;
	venture_module_registry_register_builtins(modules);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		VentureModule *module;
		g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), names[i]), !=, G_TYPE_INVALID);
		module = venture_module_registry_get_module_for_type(modules, names[i]);
		g_assert_nonnull(module);
		g_assert_cmpstr(venture_module_get_name(module), ==, "leads");
	}
}

/* Rules run in position order and the first match wins; inactive rules and
 * later matches never assign. */
static void
test_first_match(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) off = rule(f, "Off", 0, "source=web", VENTURE_LEAD_ROUTING_ASSIGN_USER, "nobody", 0, 0);
	g_autoptr(VentureEntity) second = rule(f, "Second", 20, "source=web", VENTURE_LEAD_ROUTING_ASSIGN_USER, "alice", 0, 0);
	g_autoptr(VentureEntity) first = rule(f, "First", 10, "source~^w", VENTURE_LEAD_ROUTING_ASSIGN_USER, "bob", 0, 0);
	g_autoptr(VentureEntity) inquiry = lead(f, "Inquiry", "Web");
	g_autofree gchar *owner = NULL;
	gint64 routed = 0;
	(void)data;
	g_object_set(off, "active", FALSE, NULL);
	save(f, off);
	save(f, inquiry);
	owner = owner_of(inquiry);
	g_assert_cmpstr(owner, ==, "bob");
	g_object_get(inquiry, "routing-rule-id", &routed, NULL);
	g_assert_cmpint(routed, ==, venture_entity_get_id(first));
	g_assert_cmpint(timeline(f, inquiry, "Lead routed"), ==, 1);
	(void)second;
}

/* With routing configured and nothing matching, the lead stays unassigned
 * and its timeline says so. */
static void
test_no_match(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) web = rule(f, "Web", 10, "source=web", VENTURE_LEAD_ROUTING_ASSIGN_USER, "alice", 0, 0);
	g_autoptr(VentureEntity) inquiry = lead(f, "Inquiry", "email");
	g_autofree gchar *owner = NULL;
	(void)data; (void)web;
	save(f, inquiry);
	owner = owner_of(inquiry);
	g_assert_true(venture_string_is_empty(owner));
	g_assert_cmpint(timeline(f, inquiry, "No rule matched"), ==, 1);
}

/* An organization without routing rules writes no routing noise and keeps
 * the older assignment rules working. */
static void
test_legacy_fallback(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) rota = record(f, "lead_assignment_rule", "Rota");
	g_autoptr(VentureEntity) inquiry = lead(f, "Inquiry", "web");
	g_autofree gchar *owner = NULL;
	(void)data;
	g_object_set(rota, "active", TRUE, "assignees", "carol", NULL);
	save(f, rota);
	save(f, inquiry);
	owner = owner_of(inquiry);
	g_assert_cmpstr(owner, ==, "carol");
	g_assert_cmpint(count(f, "interaction"), ==, 0);
}

static gboolean
matches(Fixture *f, VentureEntity *subject, const gchar *conditions)
{
	g_autoptr(GError) error = NULL;
	gboolean matched = FALSE;
	g_assert_true(venture_lead_conditions_match(f->db, subject, conditions, &matched, &error));
	g_assert_no_error(error);
	return matched;
}

/* Conditions cover built-in fields, references, enums and custom fields. */
static void
test_conditions(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) campaign = record(f, "campaign", "Spring");
	g_autoptr(VentureEntity) inquiry = lead(f, "Inquiry", "Web");
	g_autoptr(GError) error = NULL;
	gboolean matched = TRUE;
	(void)data;
	save(f, campaign);
	g_object_set(inquiry, "campaign-id", venture_entity_get_id(campaign), "company-name", "Acme Rockets", NULL);
	venture_entity_set_attribute(inquiry, "country", "US");
	venture_entity_set_attribute(inquiry, "company_size", "250");
	g_assert_true(matches(f, inquiry, "source=web"));
	g_assert_true(matches(f, inquiry, "source=WEB"));
	g_assert_false(matches(f, inquiry, "source=email"));
	g_assert_true(matches(f, inquiry, "company_name~rocket"));
	g_assert_true(matches(f, inquiry, "campaign=Spring"));
	{
		g_autofree gchar *by_id = g_strdup_printf("campaign_id=%" G_GINT64_FORMAT, venture_entity_get_id(campaign));
		g_assert_true(matches(f, inquiry, by_id));
	}
	g_assert_false(matches(f, inquiry, "campaign=Autumn"));
	g_assert_true(matches(f, inquiry, "country=us"));
	g_assert_true(matches(f, inquiry, "company_size~^2"));
	g_assert_true(matches(f, inquiry, "source=web; country=US"));
	g_assert_true(matches(f, inquiry, "source=web\ncountry=US"));
	g_assert_false(matches(f, inquiry, "source=web; country=FR"));
	g_assert_true(matches(f, inquiry, "status=new"));
	g_assert_true(matches(f, inquiry, ""));
	g_assert_false(matches(f, inquiry, "country=US; region=east"));
	g_assert_false(venture_lead_conditions_match(f->db, inquiry, "source web", &matched, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_false(matched);
}

/* Malformed rules are refused when written, not discovered at routing time. */
static void
test_invalid_rules(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) syntax = record(f, "lead_routing_rule", "Syntax");
	g_autoptr(VentureEntity) regex = record(f, "lead_routing_rule", "Regex");
	g_autoptr(VentureEntity) nobody = record(f, "lead_routing_rule", "Nobody");
	g_autoptr(VentureEntity) noteam = record(f, "lead_routing_rule", "No team");
	g_autoptr(VentureEntity) noventure = record(f, "lead_routing_rule", "No venture");
	g_autoptr(VentureEntity) scoring = record(f, "lead_scoring_rule", "Bad");
	(void)data;
	g_object_set(syntax, "active", TRUE, "conditions", "source web", "action", VENTURE_LEAD_ROUTING_ASSIGN_USER, "assign-to", "alice", NULL);
	refused(f, syntax, VENTURE_ERROR_VALIDATION);
	g_object_set(regex, "active", TRUE, "conditions", "source~(", "action", VENTURE_LEAD_ROUTING_ASSIGN_USER, "assign-to", "alice", NULL);
	refused(f, regex, VENTURE_ERROR_VALIDATION);
	g_object_set(nobody, "active", TRUE, "conditions", "source=web", "action", VENTURE_LEAD_ROUTING_ASSIGN_USER, NULL);
	refused(f, nobody, VENTURE_ERROR_VALIDATION);
	g_object_set(noteam, "active", TRUE, "conditions", "", "action", VENTURE_LEAD_ROUTING_ROUND_ROBIN, NULL);
	refused(f, noteam, VENTURE_ERROR_VALIDATION);
	g_object_set(noventure, "active", TRUE, "conditions", "", "action", VENTURE_LEAD_ROUTING_ASSIGN_VENTURE, NULL);
	refused(f, noventure, VENTURE_ERROR_VALIDATION);
	g_object_set(scoring, "active", TRUE, "conditions", "source~[", "points", (gint64)10, NULL);
	refused(f, scoring, VENTURE_ERROR_VALIDATION);
	g_assert_cmpint(count(f, "lead_routing_rule"), ==, 0);
	g_assert_cmpint(count(f, "lead_scoring_rule"), ==, 0);
}

static gint64
user(Fixture *f, const gchar *username, gint64 team, VentureEntity **org_member, VentureEntity **team_member)
{
	g_autoptr(VentureEntity) account = g_object_new(VENTURE_TYPE_USER, "username", username, "active", TRUE, NULL);
	VentureEntity *membership;
	VentureEntity *teammate;
	save(f, account);
	membership = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", venture_entity_get_id(account),
		"organization-id", f->org, "role", VENTURE_ORGANIZATION_ROLE_SALES, "active", TRUE, NULL);
	save(f, membership);
	teammate = g_object_new(VENTURE_TYPE_TEAM_MEMBERSHIP, "user-id", venture_entity_get_id(account),
		"organization-id", f->org, "team-id", team, "active", TRUE, NULL);
	save(f, teammate);
	if (org_member != NULL) *org_member = membership; else g_object_unref(membership);
	if (team_member != NULL) *team_member = teammate; else g_object_unref(teammate);
	return venture_entity_get_id(account);
}

static void
expect_owner(Fixture *f, const gchar *expected)
{
	g_autoptr(VentureEntity) inquiry = lead(f, "Inquiry", "web");
	g_autofree gchar *owner = NULL;
	save(f, inquiry);
	owner = owner_of(inquiry);
	g_assert_cmpstr(owner, ==, expected);
}

/* Fair rotation across a team, persisted per rule, skipping members whose
 * team or organization membership is no longer active. */
static void
test_round_robin(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) team = record(f, "team", "Sales");
	g_autoptr(VentureEntity) bob_team = NULL;
	g_autoptr(VentureEntity) carol_org = NULL;
	g_autoptr(VentureEntity) alice_team = NULL;
	g_autoptr(VentureEntity) rota = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureEntity) orphan = lead(f, "Orphan", "web");
	g_autoptr(GError) error = NULL;
	gint64 cursor = 0;
	(void)data;
	save(f, team);
	user(f, "alice", venture_entity_get_id(team), NULL, &alice_team);
	user(f, "bob", venture_entity_get_id(team), NULL, &bob_team);
	user(f, "carol", venture_entity_get_id(team), &carol_org, NULL);
	rota = rule(f, "Rota", 10, "source=web", VENTURE_LEAD_ROUTING_ROUND_ROBIN, NULL, venture_entity_get_id(team), 0);
	expect_owner(f, "alice");
	expect_owner(f, "bob");
	expect_owner(f, "carol");
	expect_owner(f, "alice");
	stored = venture_database_get(f->db, VENTURE_TYPE_LEAD_ROUTING_RULE, venture_entity_get_id(rota), NULL);
	g_object_get(stored, "cursor", &cursor, NULL);
	g_assert_cmpint(cursor, ==, 1);
	/* Bob leaves the team: the rotation continues over the two remaining. */
	g_object_set(bob_team, "active", FALSE, NULL);
	save(f, bob_team);
	expect_owner(f, "carol");
	expect_owner(f, "alice");
	expect_owner(f, "carol");
	/* Carol's organization access is revoked: only Alice remains. */
	g_object_set(carol_org, "active", FALSE, NULL);
	save(f, carol_org);
	expect_owner(f, "alice");
	expect_owner(f, "alice");
	/* An empty rota is a configuration error, not a silent no-op. */
	g_object_set(alice_team, "active", FALSE, NULL);
	save(f, alice_team);
	g_assert_false(venture_database_save(f->db, orphan, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpint(count(f, "lead"), ==, 9);
}

/* Assigning to a venture sets the venture and leaves the owner for people. */
static void
test_assign_venture(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) venture = record(f, "venture", "Rockets");
	g_autoptr(VentureEntity) inquiry = lead(f, "Inquiry", "web");
	g_autoptr(VentureEntity) placed = NULL;
	g_autofree gchar *owner = NULL;
	gint64 venture_id = 0;
	(void)data;
	save(f, venture);
	placed = rule(f, "Rockets", 10, "company_name~rocket", VENTURE_LEAD_ROUTING_ASSIGN_VENTURE, NULL, 0, venture_entity_get_id(venture));
	g_object_set(inquiry, "company-name", "Rocket Labs", NULL);
	save(f, inquiry);
	g_object_get(inquiry, "venture-id", &venture_id, NULL);
	owner = owner_of(inquiry);
	g_assert_cmpint(venture_id, ==, venture_entity_get_id(venture));
	g_assert_true(venture_string_is_empty(owner));
	g_assert_cmpint(timeline(f, inquiry, "Lead routed"), ==, 1);
}

/* An explicit owner is preserved on creation; an explicit re-route runs the
 * rules again and records its verdict either way. */
static void
test_reroute(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) inquiry = lead(f, "Inquiry", "web");
	g_autoptr(VentureEntity) web = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *owner = NULL;
	VentureLeadService *service = venture_database_get_lead_service(f->db);
	(void)data;
	g_object_set(inquiry, "owner", "zed", NULL);
	save(f, inquiry);
	web = rule(f, "Web", 10, "source=web", VENTURE_LEAD_ROUTING_ASSIGN_USER, "alice", 0, 0);
	g_assert_true(venture_lead_service_reroute(service, inquiry, NULL, &error));
	g_assert_no_error(error);
	owner = owner_of(inquiry);
	g_assert_cmpstr(owner, ==, "alice");
	g_assert_cmpint(timeline(f, inquiry, "Lead routed"), ==, 1);
	g_object_set(inquiry, "source", "email", NULL);
	save(f, inquiry);
	g_assert_true(venture_lead_service_reroute(service, inquiry, NULL, &error));
	g_assert_no_error(error);
	g_clear_pointer(&owner, g_free);
	owner = owner_of(inquiry);
	g_assert_true(venture_string_is_empty(owner));
	g_assert_cmpint(timeline(f, inquiry, "No rule matched"), ==, 1);
	/* With no routing match the older assignment rules still place the lead,
	 * and the timeline says who did, not "unassigned". */
	{
		g_autoptr(VentureEntity) legacy = record(f, "lead_assignment_rule", "Legacy");
		g_object_set(legacy, "active", TRUE, "assignees", "carol", "source", "email", NULL);
		save(f, legacy);
		g_assert_true(venture_lead_service_reroute(service, inquiry, NULL, &error));
		g_assert_no_error(error);
		g_clear_pointer(&owner, g_free);
		owner = owner_of(inquiry);
		g_assert_cmpstr(owner, ==, "carol");
		g_assert_cmpint(timeline(f, inquiry, "Lead routed"), ==, 2);
		g_assert_cmpint(timeline(f, inquiry, "No rule matched"), ==, 1);
	}
}

static VentureEntity *
scoring_rule(Fixture *f, const gchar *name, const gchar *conditions, gint64 points)
{
	VentureEntity *r = record(f, "lead_scoring_rule", name);
	g_object_set(r, "conditions", conditions, "points", points, "active", TRUE, NULL);
	save(f, r);
	return r;
}

static VentureEntity *
last_history(Fixture *f, VentureEntity *subject)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_LEAD_SCORE_HISTORY);
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_set_organization(q, f->org);
	venture_query_add_filter_int(q, "lead-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(subject), NULL);
	venture_query_add_order(q, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(q, 1);
	rows = venture_database_find(f->db, q, NULL);
	g_assert_nonnull(rows);
	g_assert_cmpuint(rows->len, ==, 1);
	return g_object_ref(g_ptr_array_index(rows, 0));
}

/* Score is the sum of matching active rules, recomputed on every save, with
 * a history row naming the rules that fired. */
static void
test_scoring(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) web = scoring_rule(f, "Web", "source=web", 30);
	g_autoptr(VentureEntity) us = scoring_rule(f, "US", "country=US", 20);
	g_autoptr(VentureEntity) off = scoring_rule(f, "Off", "", 500);
	g_autoptr(VentureEntity) inquiry = lead(f, "Inquiry", "web");
	g_autoptr(VentureEntity) history = NULL;
	g_autofree gchar *expected = NULL;
	g_autofree gchar *fired = NULL;
	gint64 score = -1, previous = -1;
	gboolean manual = TRUE;
	(void)data;
	g_object_set(off, "active", FALSE, NULL);
	save(f, off);
	venture_entity_set_attribute(inquiry, "country", "US");
	save(f, inquiry);
	g_object_get(inquiry, "score", &score, "score-manual", &manual, NULL);
	g_assert_cmpint(score, ==, 50);
	g_assert_false(manual);
	g_assert_cmpint(count(f, "lead_score_history"), ==, 1);
	history = last_history(f, inquiry);
	g_object_get(history, "score", &score, "previous-score", &previous, "rule-ids", &fired, "manual", &manual, NULL);
	expected = g_strdup_printf("%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT, venture_entity_get_id(web), venture_entity_get_id(us));
	g_assert_cmpint(previous, ==, 0);
	g_assert_cmpint(score, ==, 50);
	g_assert_cmpstr(fired, ==, expected);
	g_assert_false(manual);
	/* An unrelated edit changes nothing and writes no history. */
	g_object_set(inquiry, "notes", "Called back", NULL);
	save(f, inquiry);
	g_assert_cmpint(count(f, "lead_score_history"), ==, 1);
	g_object_set(inquiry, "source", "email", NULL);
	save(f, inquiry);
	g_object_get(inquiry, "score", &score, NULL);
	g_assert_cmpint(score, ==, 20);
	g_assert_cmpint(count(f, "lead_score_history"), ==, 2);
	/* Rules in another organization never count. */
	{
		g_autoptr(VentureEntity) other = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Other", NULL);
		g_autoptr(VentureEntity) foreign = NULL;
		save(f, other);
		foreign = scoring_rule(f, "Foreign", "", 1000);
		g_object_set(foreign, "organization-id", venture_entity_get_id(other), NULL);
		save(f, foreign);
		g_object_set(inquiry, "notes", "Again", NULL);
		save(f, inquiry);
		g_object_get(inquiry, "score", &score, NULL);
		g_assert_cmpint(score, ==, 20);
	}
}

/* A hand-typed score is kept and marked manual until rescored. */
static void
test_manual_override(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) web = scoring_rule(f, "Web", "source=web", 30);
	g_autoptr(VentureEntity) inquiry = lead(f, "Inquiry", "web");
	g_autoptr(VentureEntity) history = NULL;
	g_autoptr(GError) error = NULL;
	VentureLeadService *service = venture_database_get_lead_service(f->db);
	gint64 score = -1;
	gboolean manual = FALSE;
	(void)data; (void)web;
	save(f, inquiry);
	g_object_set(inquiry, "score", (gint64)80, NULL);
	save(f, inquiry);
	g_object_get(inquiry, "score", &score, "score-manual", &manual, NULL);
	g_assert_cmpint(score, ==, 80);
	g_assert_true(manual);
	history = last_history(f, inquiry);
	g_object_get(history, "manual", &manual, NULL);
	g_assert_true(manual);
	/* Recomputation is suspended while the override stands. */
	g_object_set(inquiry, "source", "email", NULL);
	save(f, inquiry);
	g_object_get(inquiry, "score", &score, NULL);
	g_assert_cmpint(score, ==, 80);
	/* Clearing the mark through the ordinary editor recomputes. */
	g_object_set(inquiry, "score-manual", FALSE, NULL);
	save(f, inquiry);
	g_object_get(inquiry, "score", &score, NULL);
	g_assert_cmpint(score, ==, 0);
	g_object_set(inquiry, "score", (gint64)15, "source", "web", NULL);
	save(f, inquiry);
	g_object_get(inquiry, "score", &score, "score-manual", &manual, NULL);
	g_assert_cmpint(score, ==, 15);
	g_assert_true(manual);
	/* The rescore action clears the override and applies the formula. */
	g_assert_true(venture_lead_service_rescore(service, inquiry, NULL, &error));
	g_assert_no_error(error);
	g_object_get(inquiry, "score", &score, "score-manual", &manual, NULL);
	g_assert_cmpint(score, ==, 30);
	g_assert_false(manual);
	g_assert_cmpint(count(f, "lead_score_history"), ==, 5);
}

/* Derived state belongs to the service: history rows and the routing
 * reference cannot be written generically. */
static void
test_generic_refused(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) inquiry = lead(f, "Inquiry", "web");
	g_autoptr(VentureEntity) forged = NULL;
	g_autoptr(VentureEntity) web = rule(f, "Web", 10, "source=web", VENTURE_LEAD_ROUTING_ASSIGN_USER, "alice", 0, 0);
	g_autoptr(VentureEntity) second = lead(f, "Second", "email");
	(void)data;
	save(f, inquiry);
	forged = g_object_new(VENTURE_TYPE_LEAD_SCORE_HISTORY, "organization-id", f->org,
		"lead-id", venture_entity_get_id(inquiry), "score", (gint64)99, NULL);
	refused(f, forged, VENTURE_ERROR_VALIDATION);
	g_assert_cmpint(count(f, "lead_score_history"), ==, 0);
	g_object_set(second, "routing-rule-id", venture_entity_get_id(web), NULL);
	refused(f, second, VENTURE_ERROR_VALIDATION);
	g_object_set(inquiry, "routing-rule-id", (gint64)0, NULL);
	refused(f, inquiry, VENTURE_ERROR_VALIDATION);
}

static gdouble
metric(VentureReportResult *report, const gchar *key)
{
	GPtrArray *metrics = venture_report_result_get_metrics(report);
	guint i;
	for (i = 0; i < metrics->len; i++)
	{
		VentureMetric *m = g_ptr_array_index(metrics, i);
		if (g_str_equal(venture_metric_get_key(m), key)) return venture_metric_get_number(m);
	}
	g_error("Missing metric %s", key);
	return 0;
}

static VentureReportResult *
run_report(Fixture *f, const gchar *name)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDateRange) period = venture_date_range_new_all_time();
	VentureReport *report = venture_report_registry_lookup(venture_context_get_report_registry(f->context), name);
	VentureReportResult *result;
	g_assert_nonnull(report);
	result = venture_report_generate(report, f->context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static gdouble
cell(VentureReportResult *report, guint row, const gchar *key)
{
	const GValue *value = venture_report_result_get_cell(report, row, key);
	g_assert_nonnull(value);
	return g_value_get_double(value);
}

static const gchar *
text(VentureReportResult *report, guint row, const gchar *key)
{
	const GValue *value = venture_report_result_get_cell(report, row, key);
	g_assert_nonnull(value);
	return g_value_get_string(value);
}

/* The routing report counts leads per rule and owner and the unrouted; the
 * scoring report bands scores and measures conversion per band. */
static void
test_reports(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) web = rule(f, "Web", 10, "source=web", VENTURE_LEAD_ROUTING_ASSIGN_USER, "alice", 0, 0);
	g_autoptr(VentureEntity) hot = scoring_rule(f, "Hot", "source=web", 60);
	g_autoptr(VentureEntity) a = lead(f, "A", "web");
	g_autoptr(VentureEntity) b = lead(f, "B", "web");
	g_autoptr(VentureEntity) c = lead(f, "C", "email");
	g_autoptr(VentureEntity) converted = NULL;
	g_autoptr(VentureReportResult) routing = NULL;
	g_autoptr(VentureReportResult) scoring = NULL;
	g_autoptr(GError) error = NULL;
	(void)data; (void)hot;
	save(f, a); save(f, b); save(f, c);
	g_object_set(a, "status", VENTURE_LEAD_QUALIFIED, NULL);
	save(f, a);
	converted = venture_lead_service_convert(venture_database_get_lead_service(f->db), a, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(converted);
	routing = run_report(f, "routing");
	g_assert_cmpuint(venture_report_result_get_row_count(routing), ==, 1);
	g_assert_cmpstr(text(routing, 0, "rule"), ==, "Web");
	g_assert_cmpstr(text(routing, 0, "owner"), ==, "alice");
	g_assert_cmpfloat(cell(routing, 0, "count"), ==, 2);
	g_assert_cmpfloat(metric(routing, "routed"), ==, 2);
	g_assert_cmpfloat(metric(routing, "unrouted"), ==, 1);
	(void)web;
	scoring = run_report(f, "scoring");
	g_assert_cmpuint(venture_report_result_get_row_count(scoring), ==, 2);
	g_assert_cmpstr(text(scoring, 0, "band"), ==, "0-24");
	g_assert_cmpfloat(cell(scoring, 0, "count"), ==, 1);
	g_assert_cmpfloat(cell(scoring, 0, "conversion_rate"), ==, 0);
	g_assert_cmpstr(text(scoring, 1, "band"), ==, "50-74");
	g_assert_cmpfloat(cell(scoring, 1, "count"), ==, 2);
	g_assert_cmpfloat(cell(scoring, 1, "converted"), ==, 1);
	g_assert_cmpfloat(cell(scoring, 1, "conversion_rate"), ==, 50);
	g_assert_cmpfloat(metric(scoring, "leads"), ==, 3);
	g_assert_cmpfloat(metric(scoring, "average_score"), ==, 40);
}

static void
start_http(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) reservation = g_socket_listener_new();
	f->state_dir = g_dir_make_tmp("venture-lead-routing-XXXXXX", NULL);
	f->port = g_socket_listener_add_any_inet_port(reservation, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(reservation);
	g_object_set(f->config, "state-dir", f->state_dir, "server-port", (gint64)f->port,
		"server-bind-address", "127.0.0.1", "security-require-auth", FALSE, NULL);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
}

typedef struct { gboolean done; gchar *out; gchar *err; GError *error; } CliReply;

static void
cli_received(GObject *source, GAsyncResult *result, gpointer data)
{
	CliReply *reply = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &reply->out, &reply->err, &reply->error);
	reply->done = TRUE;
}

static void
cli(Fixture *f, const gchar *noun, const gchar *verb, const gchar *id, gboolean expect_success)
{
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GError) error = NULL;
	CliReply reply = { FALSE, NULL, NULL, NULL };
	process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error,
		"build/debug/venturectl", "--server", venture_web_server_get_base_url(f->server), "--quiet", noun, verb, id, NULL);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, cli_received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	g_test_message("CLI: %s", reply.err != NULL ? reply.err : "");
	g_free(reply.out); g_free(reply.err);
	g_assert_true(g_subprocess_get_successful(process) == expect_success);
}

typedef struct { gboolean done; GBytes *bytes; GError *error; } HttpReply;

static void
http_received(GObject *source, GAsyncResult *result, gpointer data)
{
	HttpReply *reply = data;
	reply->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &reply->error);
	reply->done = TRUE;
}

static gchar *
http_get(Fixture *f, const gchar *path, guint *status)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(f->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	HttpReply reply = { FALSE, NULL, NULL };
	gchar *body;
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_received, &reply);
	while (!reply.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(reply.error);
	body = g_strndup(g_bytes_get_data(reply.bytes, NULL), g_bytes_get_size(reply.bytes));
	g_bytes_unref(reply.bytes);
	*status = soup_message_get_status(message);
	return body;
}

/* The scoring report's band_size option reaches it over REST. */
static void
test_report_http(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) hot = scoring_rule(f, "Hot", "source=web", 60);
	g_autoptr(VentureEntity) inquiry = lead(f, "Inquiry", "web");
	g_autofree gchar *wide = NULL;
	g_autofree gchar *narrow = NULL;
	guint status = 0;
	(void)data; (void)hot;
	save(f, inquiry);
	start_http(f);
	wide = http_get(f, "/api/v1/reports/scoring?period=all&band_size=100", &status);
	g_assert_cmpuint(status, ==, 200);
	g_assert_nonnull(strstr(wide, "\"0-99\""));
	narrow = http_get(f, "/api/v1/reports/scoring?period=all", &status);
	g_assert_cmpuint(status, ==, 200);
	g_assert_nonnull(strstr(narrow, "\"50-74\""));
	g_assert_null(strstr(narrow, "\"0-99\""));
}

/* The real CLI reaches the same service through the REST actions. */
static void
test_cli(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) inquiry = lead(f, "Inquiry", "web");
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureEntity) web = NULL;
	g_autoptr(VentureEntity) hot = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *owner = NULL;
	gint64 score = 0;
	gboolean manual = FALSE;
	(void)data;
	g_object_set(inquiry, "owner", "zed", "score", (gint64)5, NULL);
	save(f, inquiry);
	web = rule(f, "Web", 10, "source=web", VENTURE_LEAD_ROUTING_ASSIGN_USER, "alice", 0, 0);
	hot = scoring_rule(f, "Hot", "source=web", 60);
	start_http(f);
	id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(inquiry));
	cli(f, "leads", "reroute", id, TRUE);
	stored = venture_database_get(f->db, VENTURE_TYPE_LEAD, venture_entity_get_id(inquiry), NULL);
	owner = owner_of(stored);
	g_object_get(stored, "score", &score, "score-manual", &manual, NULL);
	g_assert_cmpstr(owner, ==, "alice");
	g_assert_cmpint(score, ==, 5);
	g_assert_true(manual);
	g_clear_object(&stored);
	cli(f, "lead", "rescore", id, TRUE);
	stored = venture_database_get(f->db, VENTURE_TYPE_LEAD, venture_entity_get_id(inquiry), NULL);
	g_object_get(stored, "score", &score, "score-manual", &manual, NULL);
	g_assert_cmpint(score, ==, 60);
	g_assert_false(manual);
	cli(f, "leads", "reroute", "999999", FALSE);
	cli(f, "leads", "explode", id, FALSE);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/lead-routing/records", test_records);
	g_test_add("/lead-routing/first-match", Fixture, NULL, setup, test_first_match, teardown);
	g_test_add("/lead-routing/no-match", Fixture, NULL, setup, test_no_match, teardown);
	g_test_add("/lead-routing/legacy-fallback", Fixture, NULL, setup, test_legacy_fallback, teardown);
	g_test_add("/lead-routing/conditions", Fixture, NULL, setup, test_conditions, teardown);
	g_test_add("/lead-routing/invalid-rules", Fixture, NULL, setup, test_invalid_rules, teardown);
	g_test_add("/lead-routing/round-robin", Fixture, NULL, setup, test_round_robin, teardown);
	g_test_add("/lead-routing/assign-venture", Fixture, NULL, setup, test_assign_venture, teardown);
	g_test_add("/lead-routing/reroute", Fixture, NULL, setup, test_reroute, teardown);
	g_test_add("/lead-routing/scoring", Fixture, NULL, setup, test_scoring, teardown);
	g_test_add("/lead-routing/manual-override", Fixture, NULL, setup, test_manual_override, teardown);
	g_test_add("/lead-routing/generic-refused", Fixture, NULL, setup, test_generic_refused, teardown);
	g_test_add("/lead-routing/reports", Fixture, NULL, setup, test_reports, teardown);
	g_test_add("/lead-routing/report-http", Fixture, NULL, setup, test_report_http, teardown);
	g_test_add("/lead-routing/cli", Fixture, NULL, setup, test_cli, teardown);
	return g_test_run();
}
