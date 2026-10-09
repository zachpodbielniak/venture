/*
 * venture-marketdata-alerts.c - Alert rules over market data, and their hits
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Three threads of work, kept apart the way the feeds module keeps them
 * (venture-marketdata-alerts.h has the overview):
 *
 * - freezing, on the main thread: a source's organization's enabled rules
 *   become AlertsRule structs -- keys, venue, group, threshold in minor
 *   units -- and its open listings AlertsListing structs, read under the
 *   system's scope whenever the feeds service freezes the source;
 * - evaluating, on whatever thread holds the store: alerts_evaluate_venue()
 *   and alerts_evaluate_entries() read a store handle and the frozen data
 *   and add candidates (plain JSON) to a sink, nothing else;
 * - writing, on the main thread, outside any transaction and outside any
 *   automation handler: each candidate held to its rule's cooldown and the
 *   per-run cap, saved as an alert_hit by the system, and told to the
 *   rule's recipient.
 */

#include "venture.h"
#include "marketdata/venture-marketdata-private.h"

#include <math.h>
#include <string.h>

#define ALERTS_INSTALLED_KEY "venture-alerts-installed"
#define ALERTS_CONTEXT_KEY "venture-alerts-context"

/* How long a run's hits wait between looks for a moment when nothing --
 * no automation handler, no transaction -- is in the way. */
#define ALERTS_RETRY_MS (50)

/* Rows asked of the store at a time when a category scope is listed. */
#define ALERTS_PAGE (500)

/* The most matches an evaluation's report lists. */
#define ALERTS_REPORT_MATCHES (50)

/* The longest notification title, in characters. */
#define ALERTS_TITLE_CHARS (200)

/* The longest key a store holds: the push contract's bound. */
#define ALERTS_MAX_KEY_BYTES (512)

/* Account kinds that judge every account but the shared ones: a warband
 * bank or a guild bank is opened through a character, so its own last
 * sighting says nothing about whether anybody has logged in. */
#define ALERTS_SHARED_KIND "shared"
#define ALERTS_GUILD_KIND "guild"

/* --- Kinds ----------------------------------------------------------------- */

/* The kinds about the operator's own accounts -- the account tables of a
 * series store -- rather than about what a venue offers. */
static gboolean
alerts_kind_reads_accounts(VentureAlertKind kind)
{
	return (VENTURE_ALERT_KIND_POSITION_EXPIRING == kind) ||
	       (VENTURE_ALERT_KIND_INBOUND_EXPIRING == kind) ||
	       (VENTURE_ALERT_KIND_ACCOUNT_STALE == kind) || (VENTURE_ALERT_KIND_COLLECT_READY == kind);
}

/* Undercut rules are scoped by the listings they compare, entry_match
 * rules by their pattern and the account kinds by the operator's
 * accounts; every other kind needs instruments to watch. */
static gboolean
alerts_kind_needs_scope(VentureAlertKind kind)
{
	return (VENTURE_ALERT_KIND_UNDERCUT != kind) && (VENTURE_ALERT_KIND_ENTRY_MATCH != kind) &&
	       !alerts_kind_reads_accounts(kind);
}

/* Whether a watchlist, an instrument or a category means anything to a
 * kind. account_stale and collect_ready are about an account as a whole:
 * an instrument narrows nothing there, so naming one is refused rather
 * than left looking as if it did. */
static gboolean
alerts_kind_takes_scope(VentureAlertKind kind)
{
	return (VENTURE_ALERT_KIND_ACCOUNT_STALE != kind) && (VENTURE_ALERT_KIND_COLLECT_READY != kind);
}

/* The kinds whose threshold is a price. */
static gboolean
alerts_kind_reads_price(VentureAlertKind kind)
{
	return (VENTURE_ALERT_KIND_BELOW == kind) || (VENTURE_ALERT_KIND_ABOVE == kind) ||
	       (VENTURE_ALERT_KIND_SPREAD == kind);
}

static const gchar *
alerts_kind_nick(VentureAlertKind kind)
{
	const gchar *nick;

	nick = venture_enum_to_nick(VENTURE_TYPE_ALERT_KIND, (gint)kind);

	return (NULL != nick) ? nick : "unknown";
}

static const gchar *
alerts_basis_nick(VentureMarketdataBasis basis)
{
	const gchar *nick;

	nick = venture_enum_to_nick(VENTURE_TYPE_MARKETDATA_BASIS, (gint)basis);

	return (NULL != nick) ? nick : "market";
}

static gint64
alerts_int(
	VentureEntity	*entity,
	const gchar	*property
){
	gint64 value;

	value = 0;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);

	return value;
}

static void
alerts_weak_ref_free(gpointer data)
{
	g_weak_ref_clear(data);
	g_free(data);
}

/* The last context made over @database, for the action, which is
 * registered per database. */
static VentureContext *
alerts_context_for(VentureDatabase *database)
{
	GWeakRef *ref;

	ref = g_object_get_data(G_OBJECT(database), ALERTS_CONTEXT_KEY);

	return (NULL != ref) ? g_weak_ref_get(ref) : NULL;
}

/* --- The rule's own checks ----------------------------------------------------- */

static gboolean
alerts_refuse_unused(
	const gchar	 *label,
	VentureAlertKind  kind,
	GError		**error
){
	venture_set_error_validation(error, label, "does not apply to a %s rule; clear it",
	                             alerts_kind_nick(kind));
	return FALSE;
}

/* An active user called @username who may read @organization_id, read as
 * the system: a rule's author may not be allowed to list users. A hit
 * carries the organization's instrument names and prices to the
 * recipient's inbox, so a user of another organization is no recipient;
 * "no such user" and "not a member" are one answer, so the refusal does
 * not probe who exists elsewhere. Without organization memberships (the
 * type switched off) every active user reads every organization. */
static gboolean
alerts_user_may_hear(
	VentureDatabase	*database,
	const gchar	*username,
	gint64		 organization_id
){
	static const gint any_role[] = {
		VENTURE_ORGANIZATION_ROLE_VIEWER, VENTURE_ORGANIZATION_ROLE_OWNER,
		VENTURE_ORGANIZATION_ROLE_ADMIN, VENTURE_ORGANIZATION_ROLE_EDITOR,
		VENTURE_ORGANIZATION_ROLE_FINANCE, VENTURE_ORGANIZATION_ROLE_SALES,
		VENTURE_ORGANIZATION_ROLE_SUPPORT, VENTURE_ORGANIZATION_ROLE_ACCOUNTANT
	};
	VentureAccessPolicy *policy;
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) user = NULL;
	VentureAuthPrincipal principal;
	VentureUserRole role;
	gboolean active = FALSE;

	policy = venture_database_get_access_policy(database);
	internal = venture_access_policy_enter(policy, NULL);
	query = venture_query_new(VENTURE_TYPE_USER);
	venture_query_set_limit(query, 1);

	if (!venture_query_add_filter_string(query, "username", VENTURE_FILTER_OP_EQ, username, NULL))
		return FALSE;

	user = venture_database_find_one(database, query, NULL);

	if (NULL == user)
		return FALSE;

	g_object_get(user, "active", &active, "role", &role, NULL);

	if (!active)
		return FALSE;

	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(),
	                                             "organization_membership"))
		return TRUE;

	memset(&principal, 0, sizeof(principal));
	principal.user_id = venture_entity_get_id(user);
	principal.token_id = 0;
	principal.role = role;
	principal.name = NULL;
	principal.authenticated = TRUE;

	return venture_access_policy_has_organization_role(policy, &principal, organization_id, any_role,
	                                                   G_N_ELEMENTS(any_role));
}

/*
 * A rule's rules. It watches something -- a watchlist, an instrument or a
 * category, any of them, at one venue or in one group, never both -- in
 * its own organization; and it carries exactly the thresholds its kind
 * reads: the ones it does not read must be empty, so a price on an
 * out_of_stock rule is refused rather than left looking as if it
 * mattered. A pattern is plain text, bounded, matched without regard to
 * case; never a regular expression.
 */
static gboolean
alerts_validate_rule(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureMoney) threshold = NULL;
	g_autofree gchar *group_key = NULL;
	g_autofree gchar *pattern = NULL;
	g_autofree gchar *notify = NULL;
	VentureAlertKind kind;
	VentureMarketdataBasis basis;
	gdouble number;
	gint64 window;
	gint64 cooldown;
	gint64 watchlist;
	gint64 instrument;
	gint64 category;
	gint64 venue;
	gint64 source;

	(void)user_data;

	g_object_get(entity, "kind", &kind, "basis", &basis, "threshold", &threshold,
	             "threshold-number", &number, "pattern", &pattern, "window-hours", &window,
	             "cooldown-minutes", &cooldown, "group-key", &group_key,
	             "notify-username", &notify, "watchlist-id", &watchlist,
	             "instrument-id", &instrument, "category-id", &category, "venue-id", &venue,
	             "data-source-id", &source, NULL);

	/* What it watches. */
	if (alerts_kind_needs_scope(kind) && (watchlist <= 0) && (instrument <= 0) && (category <= 0))
	{
		venture_set_error_validation(error, "Watchlist",
			"a %s rule watches something: name a watchlist, an instrument or a category",
			alerts_kind_nick(kind));
		return FALSE;
	}

	if (!alerts_kind_takes_scope(kind) && ((watchlist > 0) || (instrument > 0) || (category > 0)))
	{
		venture_set_error_validation(error,
			(watchlist > 0) ? "Watchlist" : (instrument > 0) ? "Instrument" : "Category",
			"does not apply to a %s rule, which watches accounts, not instruments; clear it",
			alerts_kind_nick(kind));
		return FALSE;
	}

	if ((venue > 0) && !venture_string_is_empty(group_key))
	{
		venture_set_error_validation(error, "Group",
			"name a venue or a group, not both: a venue is in one group already");
		return FALSE;
	}

	if (!venture_marketdata_check_text(group_key, "Group", error) ||
	    !venture_marketdata_check_same_organization(database, entity, previous, "watchlist-id",
	                                                VENTURE_TYPE_WATCHLIST, "Watchlist", error) ||
	    !venture_marketdata_check_same_organization(database, entity, previous, "instrument-id",
	                                                VENTURE_TYPE_INSTRUMENT, "Instrument", error) ||
	    !venture_marketdata_check_same_organization(database, entity, previous, "venue-id",
	                                                VENTURE_TYPE_VENUE, "Venue", error) ||
	    !venture_marketdata_check_same_organization(database, entity, previous, "data-source-id",
	                                                VENTURE_TYPE_DATA_SOURCE, "Data source", error))
		return FALSE;

	/* A venue in another source than the one the rule is narrowed to
	 * would never be seen; judged when either is written. */
	if ((venue > 0) && (source > 0) &&
	    ((NULL == previous) || (alerts_int(previous, "venue-id") != venue) ||
	     (alerts_int(previous, "data-source-id") != source)))
	{
		g_autoptr(VentureEntity) record = NULL;

		record = venture_database_get(database, VENTURE_TYPE_VENUE, venue, NULL);

		if ((NULL != record) && (alerts_int(record, "data-source-id") > 0) &&
		    (alerts_int(record, "data-source-id") != source))
		{
			venture_set_error_validation(error, "Venue",
				"#%" G_GINT64_FORMAT " is data source #%" G_GINT64_FORMAT "'s, and the rule "
				"reads only #%" G_GINT64_FORMAT, venue, alerts_int(record, "data-source-id"),
				source);
			return FALSE;
		}
	}

	/* The price threshold. A deal reads a least profit, a least return or
	 * both, and at least one: a deal rule with neither fires on every
	 * cheap unit there is. */
	if (VENTURE_ALERT_KIND_DEAL == kind)
	{
		if ((NULL == threshold) && (0.0 == number))
		{
			venture_set_error_validation(error, "Threshold number",
				"a deal rule needs a least return (threshold number, a percent of the buy "
				"price), a least profit (threshold price), or both");
			return FALSE;
		}

		if ((NULL != threshold) &&
		    (venture_money_is_negative(threshold) || venture_money_is_zero(threshold)))
		{
			venture_set_error_validation(error, "Threshold price",
				"is the least profit worth hearing about, above nothing");
			return FALSE;
		}
	}
	else if (alerts_kind_reads_price(kind))
	{
		if (NULL == threshold)
		{
			venture_set_error_validation(error, "Threshold price", "is required for a %s rule",
			                             alerts_kind_nick(kind));
			return FALSE;
		}

		if (venture_money_is_negative(threshold))
		{
			venture_set_error_validation(error, "Threshold price", "cannot be negative");
			return FALSE;
		}
	}
	else if (NULL != threshold)
	{
		return alerts_refuse_unused("Threshold price", kind, error);
	}

	/* The number threshold. */
	if (!isfinite(number))
	{
		venture_set_error_validation(error, "Threshold number", "must be a number");
		return FALSE;
	}

	switch (kind)
	{
	case VENTURE_ALERT_KIND_PCT_VS_REFERENCE:
		if ((number <= 0.0) || (number > 1000.0))
		{
			venture_set_error_validation(error, "Threshold number",
				"must be a percent of the reference above 0 and at most 1000: 80 fires at "
				"20%% under it");
			return FALSE;
		}
		break;
	case VENTURE_ALERT_KIND_SHORTAGE:
		if ((number < 1.0) || (number > 1e15))
		{
			venture_set_error_validation(error, "Threshold number",
				"must be at least 1: the units on offer below which a venue is short");
			return FALSE;
		}
		break;
	case VENTURE_ALERT_KIND_SPIKE:
		if ((0.0 == number) || (fabs(number) > 100000.0))
		{
			venture_set_error_validation(error, "Threshold number",
				"must be a percent change other than 0, at most 100000 either way: 25 fires "
				"on a rise of a quarter, -25 on a drop of one");
			return FALSE;
		}
		break;
	case VENTURE_ALERT_KIND_POSITION_EXPIRING:
	case VENTURE_ALERT_KIND_INBOUND_EXPIRING:
		if ((number <= 0.0) || (number > (gdouble)VENTURE_ALERTS_MAX_EXPIRING_HOURS))
		{
			venture_set_error_validation(error, "Threshold number",
				"must be the hours ahead to warn, above 0 and at most %d: 2 fires on what "
				"runs out within two hours", VENTURE_ALERTS_MAX_EXPIRING_HOURS);
			return FALSE;
		}
		break;
	case VENTURE_ALERT_KIND_ACCOUNT_STALE:
		if ((number <= 0.0) || (number > (gdouble)VENTURE_ALERTS_MAX_STALE_DAYS))
		{
			venture_set_error_validation(error, "Threshold number",
				"must be the days an account may go unseen, above 0 and at most %d",
				VENTURE_ALERTS_MAX_STALE_DAYS);
			return FALSE;
		}
		break;
	case VENTURE_ALERT_KIND_DEAL:
		if ((number < 0.0) || (number > 100000.0))
		{
			venture_set_error_validation(error, "Threshold number",
				"must be the least return in percent of the buy price, 0 (none) to 100000: "
				"20 fires when the sale after the cut makes a fifth over the price paid");
			return FALSE;
		}
		break;
	default:
		if (0.0 != number)
			return alerts_refuse_unused("Threshold number", kind, error);
		break;
	}

	/* The pattern: stored stripped, so what is matched is what is shown. */
	if (VENTURE_ALERT_KIND_ENTRY_MATCH == kind)
	{
		const gchar *p;

		if (NULL != pattern)
			g_strstrip(pattern);

		if (venture_string_is_empty(pattern))
		{
			venture_set_error_validation(error, "Pattern",
				"is required for an entry_match rule: the text an entry contains");
			return FALSE;
		}

		if (!g_utf8_validate(pattern, -1, NULL) ||
		    (g_utf8_strlen(pattern, -1) > VENTURE_ALERTS_MAX_PATTERN))
		{
			venture_set_error_validation(error, "Pattern",
				"must be text of at most %d characters", VENTURE_ALERTS_MAX_PATTERN);
			return FALSE;
		}

		for (p = pattern; '\0' != *p; p++)
		{
			if (g_ascii_iscntrl(*p))
			{
				venture_set_error_validation(error, "Pattern", "cannot contain control characters");
				return FALSE;
			}
		}

		g_object_set(entity, "pattern", pattern, NULL);
	}
	else if (!venture_string_is_empty(pattern))
	{
		return alerts_refuse_unused("Pattern", kind, error);
	}

	/* The basis: an enum has no empty value, so a kind that does not read
	 * it ignores it rather than refusing the default. */
	switch (kind)
	{
	case VENTURE_ALERT_KIND_PCT_VS_REFERENCE:
		if (venture_marketdata_basis_is_number(basis) || (VENTURE_MARKETDATA_BASIS_MIN == basis))
		{
			venture_set_error_validation(error, "Basis",
				"%s cannot be the reference of a pct_vs_reference rule: the minimum is held "
				"against a price other than itself (market, region_median, market_14d...)",
				alerts_basis_nick(basis));
			return FALSE;
		}
		break;
	case VENTURE_ALERT_KIND_SPREAD:
		if (venture_marketdata_basis_is_number(basis))
		{
			venture_set_error_validation(error, "Basis",
				"%s is a number, not a price a spread can be sold at", alerts_basis_nick(basis));
			return FALSE;
		}
		break;
	case VENTURE_ALERT_KIND_SPIKE:
		if ((VENTURE_MARKETDATA_BASIS_MARKET != basis) && (VENTURE_MARKETDATA_BASIS_MIN != basis) &&
		    (VENTURE_MARKETDATA_BASIS_QUANTITY != basis))
		{
			venture_set_error_validation(error, "Basis",
				"a spike compares the hourly series, which keeps market, min and quantity; "
				"not %s", alerts_basis_nick(basis));
			return FALSE;
		}
		break;
	default:
		break;
	}

	/* The window and the cooldown. */
	if (VENTURE_ALERT_KIND_SPIKE == kind)
	{
		if ((window < 1) || (window > VENTURE_ALERTS_MAX_WINDOW_HOURS))
		{
			venture_set_error_validation(error, "Window hours",
				"is required for a spike rule: 1 to %d hours", VENTURE_ALERTS_MAX_WINDOW_HOURS);
			return FALSE;
		}
	}
	else if (0 != window)
	{
		return alerts_refuse_unused("Window hours", kind, error);
	}

	if ((cooldown < 0) || (cooldown > VENTURE_ALERTS_MAX_COOLDOWN_MINUTES))
	{
		venture_set_error_validation(error, "Cooldown minutes",
			"must be 0 to %d (thirty days)", VENTURE_ALERTS_MAX_COOLDOWN_MINUTES);
		return FALSE;
	}

	/* Somebody to tell, judged when written: a user removed since keeps
	 * the rule editable, and the hit is still recorded. */
	if (!venture_string_is_empty(notify))
	{
		g_autofree gchar *was = NULL;

		if (NULL != previous)
			g_object_get(previous, "notify-username", &was, NULL);

		if (((NULL == previous) || (0 != g_strcmp0(was, notify))) &&
		    (!venture_marketdata_check_text(notify, "Tell", error) ||
		     !alerts_user_may_hear(database, notify, venture_entity_get_organization_id(entity))))
		{
			if ((NULL != error) && (NULL == *error))
				venture_set_error_validation(error, "Tell",
					"no active user of this organization is called %s", notify);
			return FALSE;
		}
	}

	return TRUE;
}

/* A hit is the system's to write; this holds the two things the reference
 * check does not: an organization, which the webhook needs to publish it,
 * and a rule. */
static gboolean
alerts_validate_hit(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	(void)database;
	(void)previous;
	(void)user_data;

	if (venture_entity_get_organization_id(entity) <= 0)
	{
		venture_set_error_validation(error, "Organization",
			"an alert hit belongs to the organization of the rule that fired");
		return FALSE;
	}

	if (alerts_int(entity, "rule-id") <= 0)
	{
		venture_set_error_validation(error, "Rule", "is required");
		return FALSE;
	}

	/* A store key is at most 512 bytes (the push contract); anything
	 * longer did not come from a store. */
	{
		g_autofree gchar *account_key = NULL;

		g_object_get(entity, "account-key", &account_key, NULL);

		if ((NULL != account_key) && (strlen(account_key) > ALERTS_MAX_KEY_BYTES))
		{
			venture_set_error_validation(error, "Account key", "is at most %d bytes",
			                             ALERTS_MAX_KEY_BYTES);
			return FALSE;
		}
	}

	return TRUE;
}

#ifdef VENTURE_HAVE_SQLITE

/* ==========================================================================
 * Frozen rules: plain data, made on the main thread, read on any
 * ========================================================================== */

/* For qsort() and g_ptr_array_sort(): both hand over pointers to the
 * string pointers. */
static gint
alerts_compare_keys(
	gconstpointer	a,
	gconstpointer	b
){
	return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

static void
alerts_unref_nullable(gpointer object)
{
	if (NULL != object)
		g_object_unref(object);
}

/* Filled field by field, as every hand-made actor is (AGENTS.md). */
static const VentureActor *
alerts_system_actor(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_SYSTEM;
	actor->name = "alerts";
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;

	return actor;
}

/* Inside an automation handler a hit's on_created would be swallowed by
 * the cascade guard, and inside somebody's transaction it would be rolled
 * back with it after its webhook had gone: either way, not now. */
static gboolean
alerts_must_wait(VentureContext *context)
{
	return venture_automation_is_dispatching(venture_context_get_automation(context)) ||
	       venture_database_has_transaction(venture_context_get_database(context));
}


typedef struct
{
	gint64			 id;
	VentureAlertKind	 kind;
	VentureMarketdataBasis	 basis;
	gboolean		 scoped;		/* named a watchlist, instrument or category */
	GHashTable		*keys;			/* instrument keys in scope; NULL unscoped */
	gchar			*category_prefix;	/* the category's store path, or NULL */
	gchar			*venue_key;		/* NULL: every venue */
	gchar			*group_key;		/* NULL: every group */
	gint64			 threshold;		/* minor units, or VENTURE_SERIES_NONE */
	gchar			 currency[VENTURE_MONEY_CURRENCY_LEN];
	gdouble			 number;
	gchar			*pattern;		/* case-folded */
	gint64			 window_hours;
} AlertsRule;

typedef struct
{
	gint64		 id;
	gchar		*venue_key;
	GPtrArray	*instrument_keys;	/* gchar* */
	gint64		 price;			/* minor units */
	gchar		 currency[VENTURE_MONEY_CURRENCY_LEN];
} AlertsListing;

typedef struct
{
	gint64		 source_id;
	gint64		 organization_id;
	GPtrArray	*rules;			/* AlertsRule */
	GPtrArray	*listings;		/* AlertsListing, newest first */
	GHashTable	*venue_listings;	/* venue key -> GPtrArray of borrowed AlertsListing */
	gboolean	 listings_capped;	/* more open listings than were frozen */
	VentureMarketdataIgnores *ignores;	/* the accounts the account rules pass over */
	gint64		 stale_after;		/* seconds: series.stale_minutes, for deal rules */
} AlertsFrozen;

static void
alerts_rule_free(gpointer data)
{
	AlertsRule *rule = data;

	g_clear_pointer(&rule->keys, g_hash_table_unref);
	g_free(rule->category_prefix);
	g_free(rule->venue_key);
	g_free(rule->group_key);
	g_free(rule->pattern);
	g_free(rule);
}

static void
alerts_listing_free(gpointer data)
{
	AlertsListing *listing = data;

	g_free(listing->venue_key);
	g_ptr_array_unref(listing->instrument_keys);
	g_free(listing);
}

static void
alerts_frozen_free(gpointer data)
{
	AlertsFrozen *frozen = data;

	if (NULL == frozen)
		return;

	g_ptr_array_unref(frozen->rules);
	g_clear_pointer(&frozen->venue_listings, g_hash_table_unref);
	g_ptr_array_unref(frozen->listings);
	g_clear_pointer(&frozen->ignores, venture_marketdata_ignores_free);
	g_free(frozen);
}

/* A price in the currency's own minor units -- the store's -- or FALSE
 * when it cannot be said exactly there. */
static gboolean
alerts_minor_units(
	const VentureMoney	*money,
	gint64			*out_amount,
	gchar			*out_currency
){
	g_autoptr(VentureMoney) scaled = NULL;
	const gchar *currency;

	currency = venture_money_get_currency(money);
	scaled = venture_money_rescale(money, venture_currency_get_exponent(currency), NULL);

	if (NULL == scaled)
		return FALSE;

	*out_amount = venture_money_get_amount(scaled);
	g_strlcpy(out_currency, venture_money_get_currency(scaled), VENTURE_MONEY_CURRENCY_LEN);

	return TRUE;
}

/* An instrument record's key, when it is read in @source_id's store: its
 * own source, or none (then its key is taken to mean the same thing in
 * every source of the organization). */
static gchar *
alerts_instrument_key(
	VentureDatabase	*database,
	gint64		 instrument_id,
	gint64		 source_id
){
	g_autoptr(VentureEntity) instrument = NULL;
	g_autofree gchar *key = NULL;
	gint64 instrument_source;

	instrument = venture_database_get(database, VENTURE_TYPE_INSTRUMENT, instrument_id, NULL);

	if ((NULL == instrument) || venture_entity_is_deleted(instrument))
		return NULL;

	g_object_get(instrument, "key", &key, "data-source-id", &instrument_source, NULL);

	if (venture_string_is_empty(key) || ((instrument_source > 0) && (instrument_source != source_id)))
		return NULL;

	return g_steal_pointer(&key);
}

/* Every instrument key a list of instrument records comes to in this
 * source. */
static void
alerts_add_instrument_keys(
	VentureDatabase	*database,
	GPtrArray	*instruments,
	gint64		 source_id,
	GHashTable	*keys
){
	guint i;

	for (i = 0; (NULL != instruments) && (i < instruments->len); i++)
	{
		VentureEntity *instrument = g_ptr_array_index(instruments, i);
		g_autofree gchar *key = NULL;
		gint64 instrument_source;

		g_object_get(instrument, "key", &key, "data-source-id", &instrument_source, NULL);

		if (venture_string_is_empty(key) || ((instrument_source > 0) && (instrument_source != source_id)))
			continue;

		g_hash_table_add(keys, g_steal_pointer(&key));
	}

	(void)database;
}

/* "Materials / Ore" as the store writes it: "Materials/Ore". */
static gchar *
alerts_store_path(const gchar *path)
{
	g_auto(GStrv) parts = NULL;

	if (venture_string_is_empty(path))
		return NULL;

	parts = g_strsplit(path, VENTURE_CATEGORY_PATH_SEPARATOR, -1);

	return g_strjoinv("/", parts);
}

/* The instruments filed under a category or beneath it, in one query. */
static GPtrArray *
alerts_category_instruments(
	VentureDatabase	*database,
	gint64		 organization_id,
	gint64		 category_id
){
	g_autoptr(GArray) ids = NULL;
	g_autoptr(GPtrArray) values = NULL;
	g_autoptr(VentureQuery) query = NULL;
	guint i;

	ids = venture_category_descendants(database, VENTURE_TYPE_CATEGORY, category_id, TRUE, NULL);

	if ((NULL == ids) || (0 == ids->len))
		return NULL;

	values = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < ids->len; i++)
		g_ptr_array_add(values, g_strdup_printf("%" G_GINT64_FORMAT, g_array_index(ids, gint64, i)));

	query = venture_query_new(VENTURE_TYPE_INSTRUMENT);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, VENTURE_ALERTS_MAX_ROWS);

	if (!venture_query_add_filter(query, "category-id", VENTURE_FILTER_OP_IN, values, NULL))
		return NULL;

	return venture_database_find(database, query, NULL);
}

/*
 * One rule as the worker will see it for @source_id, or NULL when it says
 * nothing about that source: narrowed to another source, its venue in
 * another, or its scope coming to no instrument there.
 */
static AlertsRule *
alerts_freeze_rule(
	VentureDatabase	*database,
	VentureEntity	*record,
	gint64		 source_id
){
	g_autoptr(VentureMoney) threshold = NULL;
	g_autofree gchar *group_key = NULL;
	g_autofree gchar *pattern = NULL;
	AlertsRule *rule;
	gint64 rule_source;
	gint64 watchlist_id;
	gint64 instrument_id;
	gint64 category_id;
	gint64 venue_id;

	g_object_get(record, "data-source-id", &rule_source, "watchlist-id", &watchlist_id,
	             "instrument-id", &instrument_id, "category-id", &category_id,
	             "venue-id", &venue_id, "threshold", &threshold, "group-key", &group_key,
	             "pattern", &pattern, NULL);

	if ((rule_source > 0) && (rule_source != source_id))
		return NULL;

	rule = g_new0(AlertsRule, 1);
	rule->id = venture_entity_get_id(record);
	rule->threshold = VENTURE_SERIES_NONE;
	g_object_get(record, "kind", &rule->kind, "basis", &rule->basis,
	             "threshold-number", &rule->number, "window-hours", &rule->window_hours, NULL);

	if ((NULL != threshold) && !alerts_minor_units(threshold, &rule->threshold, rule->currency))
	{
		g_message("alerts: rule %" G_GINT64_FORMAT "'s threshold cannot be said in its "
		          "currency's minor units; it is not evaluated", rule->id);
		alerts_rule_free(rule);
		return NULL;
	}

	if (!venture_string_is_empty(pattern))
		rule->pattern = g_utf8_casefold(pattern, -1);

	if (!venture_string_is_empty(group_key))
		rule->group_key = g_steal_pointer(&group_key);

	/* A venue: its key, when its prices are in this source. */
	if (venue_id > 0)
	{
		g_autoptr(VentureEntity) venue = NULL;
		g_autofree gchar *key = NULL;
		gint64 venue_source = 0;

		venue = venture_database_get(database, VENTURE_TYPE_VENUE, venue_id, NULL);

		if ((NULL != venue) && !venture_entity_is_deleted(venue))
			g_object_get(venue, "key", &key, "data-source-id", &venue_source, NULL);

		if (venture_string_is_empty(key) || ((venue_source > 0) && (venue_source != source_id)))
		{
			alerts_rule_free(rule);
			return NULL;
		}

		rule->venue_key = g_steal_pointer(&key);
	}

	/* What it watches, as keys in this store. */
	rule->scoped = (watchlist_id > 0) || (instrument_id > 0) || (category_id > 0);

	if (rule->scoped)
		rule->keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	if (instrument_id > 0)
	{
		gchar *key = alerts_instrument_key(database, instrument_id, source_id);

		if (NULL != key)
			g_hash_table_add(rule->keys, key);
	}

	if (watchlist_id > 0)
	{
		g_autoptr(VentureEntity) watchlist = NULL;
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) entries = NULL;
		guint i;

		watchlist = venture_database_get(database, VENTURE_TYPE_WATCHLIST, watchlist_id, NULL);

		/* The list's group, when the rule names neither a group nor a
		 * venue of its own. Not for an account kind: a list's group is
		 * a group of venues, and an account's group is its own (a realm
		 * where a venue's is a region), so the two would never meet. */
		if ((NULL != watchlist) && (NULL == rule->group_key) && (NULL == rule->venue_key) &&
		    !alerts_kind_reads_accounts(rule->kind))
		{
			g_autofree gchar *list_group = NULL;

			g_object_get(watchlist, "group-key", &list_group, NULL);

			if (!venture_string_is_empty(list_group))
				rule->group_key = g_steal_pointer(&list_group);
		}

		query = venture_query_new(VENTURE_TYPE_WATCHLIST_ENTRY);
		venture_query_set_limit(query, VENTURE_ALERTS_MAX_ROWS);

		if (venture_query_add_filter_int(query, "watchlist-id", VENTURE_FILTER_OP_EQ,
		                                 watchlist_id, NULL))
			entries = venture_database_find(database, query, NULL);

		for (i = 0; (NULL != entries) && (i < entries->len); i++)
		{
			gchar *key = alerts_instrument_key(database,
				alerts_int(g_ptr_array_index(entries, i), "instrument-id"), source_id);

			if (NULL != key)
				g_hash_table_add(rule->keys, key);
		}
	}

	if (category_id > 0)
	{
		g_autoptr(GPtrArray) instruments = NULL;
		g_autofree gchar *path = NULL;

		path = venture_category_path(database, VENTURE_TYPE_CATEGORY, category_id, NULL);
		rule->category_prefix = alerts_store_path(path);
		instruments = alerts_category_instruments(database, venture_entity_get_organization_id(record),
		                                          category_id);
		alerts_add_instrument_keys(database, instruments, source_id, rule->keys);
	}

	if (rule->scoped && (0 == g_hash_table_size(rule->keys)) && (NULL == rule->category_prefix))
	{
		alerts_rule_free(rule);
		return NULL;
	}

	return rule;
}

/* The store instrument a listing the mirror wrote for this source stands
 * for: its mirror-state names it, so no query is needed. NULL for any
 * other listing. */
static gchar *
alerts_mirrored_instrument(
	VentureEntity	*record,
	gint64		 source_id
){
	g_autoptr(JsonParser) parser = NULL;
	g_autofree gchar *state = NULL;
	JsonNode *root;
	const gchar *key;

	if (alerts_int(record, "data-source-id") != source_id)
		return NULL;

	g_object_get(record, "mirror-state", &state, NULL);

	if (venture_string_is_empty(state))
		return NULL;

	parser = json_parser_new();

	if (!json_parser_load_from_data(parser, state, -1, NULL))
		return NULL;

	root = json_parser_get_root(parser);

	if ((NULL == root) || !JSON_NODE_HOLDS_OBJECT(root))
		return NULL;

	key = json_object_get_string_member_with_default(json_node_get_object(root), "instrument", NULL);

	return venture_string_is_empty(key) ? NULL : g_strdup(key);
}

/* The store keys of the instrument records naming @product_id in this
 * source, sorted; asked once per product however many listings it has. */
static GPtrArray *
alerts_product_keys(
	VentureDatabase	*database,
	AlertsFrozen	*frozen,
	gint64		 product_id
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) instruments = NULL;
	g_autoptr(GHashTable) keys = NULL;
	GPtrArray *sorted;
	GHashTableIter iter;
	gpointer key;

	query = venture_query_new(VENTURE_TYPE_INSTRUMENT);
	venture_query_set_organization(query, frozen->organization_id);
	venture_query_set_limit(query, 20);

	if (!venture_query_add_filter_int(query, "product-id", VENTURE_FILTER_OP_EQ, product_id, NULL))
		return NULL;

	instruments = venture_database_find(database, query, NULL);
	keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	alerts_add_instrument_keys(database, instruments, frozen->source_id, keys);

	if (0 == g_hash_table_size(keys))
		return NULL;

	sorted = g_ptr_array_new_with_free_func(g_free);
	g_hash_table_iter_init(&iter, keys);

	while (g_hash_table_iter_next(&iter, &key, NULL))
		g_ptr_array_add(sorted, g_strdup(key));

	g_ptr_array_sort(sorted, alerts_compare_keys);

	return sorted;
}

/* One listing as an undercut rule compares it, or NULL when it means
 * nothing in this store: no price, a venue priced elsewhere, a product no
 * instrument here stands for. */
static AlertsListing *
alerts_freeze_listing(
	VentureDatabase	*database,
	AlertsFrozen	*frozen,
	VentureEntity	*record,
	GHashTable	*venues,
	GHashTable	*products
){
	g_autoptr(VentureMoney) price = NULL;
	g_autofree gchar *mirrored = NULL;
	AlertsListing *listing;
	const gchar *venue_key;
	GPtrArray *keys;
	gint64 venue_id;
	gint64 product_id;
	guint i;

	venue_id = alerts_int(record, "venue-id");
	product_id = alerts_int(record, "product-id");
	g_object_get(record, "unit-price", &price, NULL);

	if (NULL == price)
		return NULL;

	/* The venue's key here, once per venue; "" when it has none. */
	if (!g_hash_table_contains(venues, &venue_id))
	{
		g_autoptr(VentureEntity) venue = NULL;
		g_autofree gchar *found = NULL;
		gint64 venue_source = 0;

		venue = venture_database_get(database, VENTURE_TYPE_VENUE, venue_id, NULL);

		if ((NULL != venue) && !venture_entity_is_deleted(venue))
			g_object_get(venue, "key", &found, "data-source-id", &venue_source, NULL);

		if ((venue_source > 0) && (venue_source != frozen->source_id))
			g_clear_pointer(&found, g_free);

		g_hash_table_insert(venues, g_memdup2(&venue_id, sizeof(venue_id)),
		                    g_strdup((NULL != found) ? found : ""));
	}

	venue_key = g_hash_table_lookup(venues, &venue_id);

	if (venture_string_is_empty(venue_key))
		return NULL;

	/* What it sells, as store keys: the mirror's own word for a listing
	 * it wrote here, else the product's instruments, once per product. */
	mirrored = alerts_mirrored_instrument(record, frozen->source_id);
	keys = NULL;

	if (NULL == mirrored)
	{
		gpointer cached;

		if (product_id <= 0)
			return NULL;

		if (g_hash_table_lookup_extended(products, &product_id, NULL, &cached))
		{
			keys = cached;
		}
		else
		{
			keys = alerts_product_keys(database, frozen, product_id);
			g_hash_table_insert(products, g_memdup2(&product_id, sizeof(product_id)), keys);
		}

		if (NULL == keys)
			return NULL;
	}

	listing = g_new0(AlertsListing, 1);
	listing->id = venture_entity_get_id(record);
	listing->venue_key = g_strdup(venue_key);
	listing->instrument_keys = g_ptr_array_new_with_free_func(g_free);

	if (!alerts_minor_units(price, &listing->price, listing->currency))
	{
		alerts_listing_free(listing);
		return NULL;
	}

	if (NULL != mirrored)
		g_ptr_array_add(listing->instrument_keys, g_steal_pointer(&mirrored));

	for (i = 0; (NULL != keys) && (i < keys->len); i++)
		g_ptr_array_add(listing->instrument_keys, g_strdup(g_ptr_array_index(keys, i)));

	return listing;
}

static void
alerts_ptr_array_unref_nullable(gpointer array)
{
	if (NULL != array)
		g_ptr_array_unref(array);
}

/*
 * The organization's open listings at a venue this source prices, with
 * the instrument keys they come to here and their asking price in minor
 * units: what an undercut rule compares. A listing whose venue or product
 * means nothing in this store is left out.
 *
 * Newest first, a page at a time, up to VENTURE_ALERTS_MAX_LISTINGS --
 * the one just posted is the one most likely to be undercut, and the
 * oldest-first single read this replaced lost exactly those once an
 * operator held a thousand. A listing the mirror wrote for this source
 * names its store instrument in its mirror-state; any other costs one
 * instrument query per distinct product, never one per listing. Reaching
 * the bound is remembered and said on the run.
 */
static void
alerts_freeze_listings(
	VentureDatabase	*database,
	AlertsFrozen	*frozen
){
	g_autoptr(GHashTable) venues = NULL;
	g_autoptr(GHashTable) products = NULL;
	guint offset;

	venues = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	products = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
	                                 alerts_ptr_array_unref_nullable);
	offset = 0;

	/* One row past the bound, so reaching it is told from ending on it. */
	while (offset <= VENTURE_ALERTS_MAX_LISTINGS)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) page = NULL;
		guint asked;
		guint i;

		asked = MIN((guint)ALERTS_PAGE, (guint)VENTURE_ALERTS_MAX_LISTINGS + 1 - offset);
		query = venture_query_new(VENTURE_TYPE_LISTING);
		venture_query_set_organization(query, frozen->organization_id);
		venture_query_set_limit(query, asked);
		venture_query_set_offset(query, offset);

		if (!venture_query_add_filter_int(query, "outcome", VENTURE_FILTER_OP_EQ,
		                                  VENTURE_LISTING_OUTCOME_OPEN, NULL) ||
		    !venture_query_add_filter_int(query, "venue-id", VENTURE_FILTER_OP_GT, 0, NULL) ||
		    !venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL))
			return;

		page = venture_database_find(database, query, NULL);

		if (NULL == page)
			return;

		for (i = 0; i < page->len; i++)
		{
			AlertsListing *listing;

			if (offset + i >= VENTURE_ALERTS_MAX_LISTINGS)
			{
				frozen->listings_capped = TRUE;
				break;
			}

			listing = alerts_freeze_listing(database, frozen, g_ptr_array_index(page, i), venues,
			                                products);

			if (NULL != listing)
				g_ptr_array_add(frozen->listings, listing);
		}

		if (frozen->listings_capped || (page->len < asked))
			break;

		offset += page->len;
	}

	/* By venue, so a venue's evaluation walks its own listings. */
	{
		guint i;

		for (i = 0; i < frozen->listings->len; i++)
		{
			AlertsListing *listing = g_ptr_array_index(frozen->listings, i);
			GPtrArray *at;

			at = g_hash_table_lookup(frozen->venue_listings, listing->venue_key);

			if (NULL == at)
			{
				at = g_ptr_array_new();
				g_hash_table_insert(frozen->venue_listings, g_strdup(listing->venue_key), at);
			}

			g_ptr_array_add(at, listing);
		}
	}
}

/*
 * What the worker needs about a source's alerts, or NULL when there is
 * nothing to evaluate there. @only_rule freezes that one rule, enabled or
 * not, for an evaluation asked for by hand; otherwise every enabled rule
 * of the source's organization, oldest first, up to the cap.
 */
static AlertsFrozen *
alerts_freeze(
	VentureContext	*context,
	VentureEntity	*data_source,
	VentureEntity	*only_rule
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(GPtrArray) records = NULL;
	VentureDatabase *database;
	AlertsFrozen *frozen;
	gboolean undercut;
	guint i;

	if (!venture_context_module_enabled(context, "marketdata"))
		return NULL;

	database = venture_context_get_database(context);
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);

	if (NULL != only_rule)
	{
		records = g_ptr_array_new_with_free_func(g_object_unref);
		g_ptr_array_add(records, g_object_ref(only_rule));
	}
	else
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GError) error = NULL;

		query = venture_query_new(VENTURE_TYPE_ALERT_RULE);
		venture_query_set_organization(query, venture_entity_get_organization_id(data_source));
		venture_query_set_limit(query, VENTURE_ALERTS_MAX_RULES);

		if (!venture_query_add_filter_string(query, "enabled", VENTURE_FILTER_OP_EQ, "true", &error) ||
		    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, &error))
		{
			g_message("alerts: could not ask for the rules: %s", error->message);
			return NULL;
		}

		records = venture_database_find(database, query, &error);

		if (NULL == records)
		{
			/* Read by the feeds freeze, which must not fail a source
			 * over it: the source still runs, without alerts. */
			g_message("alerts: could not read the rules: %s", error->message);
			return NULL;
		}
	}

	frozen = g_new0(AlertsFrozen, 1);
	frozen->source_id = venture_entity_get_id(data_source);
	frozen->organization_id = venture_entity_get_organization_id(data_source);
	frozen->rules = g_ptr_array_new_with_free_func(alerts_rule_free);
	frozen->listings = g_ptr_array_new_with_free_func(alerts_listing_free);
	frozen->venue_listings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                               (GDestroyNotify)g_ptr_array_unref);
	undercut = FALSE;

	for (i = 0; i < records->len; i++)
	{
		AlertsRule *rule = alerts_freeze_rule(database, g_ptr_array_index(records, i),
		                                      frozen->source_id);

		if (NULL == rule)
			continue;

		undercut = undercut || (VENTURE_ALERT_KIND_UNDERCUT == rule->kind);
		g_ptr_array_add(frozen->rules, rule);
	}

	if (0 == frozen->rules->len)
	{
		alerts_frozen_free(frozen);
		return NULL;
	}

	if (undercut)
		alerts_freeze_listings(database, frozen);

	/* Read here, on the main thread, like the rules: an ignored
	 * character's listings, mail and absence are nobody's business. */
	frozen->ignores = venture_marketdata_ignores_load(context, frozen->organization_id);

	/* The age past which Deals calls a price stale, read where the
	 * configuration may be read; a deal rule judges neither side older. */
	frozen->stale_after = venture_marketdata_stale_seconds(context);

	return frozen;
}

/* ==========================================================================
 * Evaluation: a store handle and frozen data in, candidates out
 * ========================================================================== */

/*
 * Where candidates gather, keyed "<rule>|<subject>": the same thing seen
 * again in a later unit of the run replaces the earlier sighting rather
 * than counting twice. Past the cap they are only counted.
 */
typedef struct
{
	JsonObject	*candidates;
	guint		 over;
	GPtrArray	*notes;		/* gchar*, said once each: a bound reached, a read cut short */
} AlertsSink;

/* A note for the run (or the evaluation's report), once however often the
 * evaluation reaches it. */
static void
alerts_sink_note(
	AlertsSink	*sink,
	const gchar	*note
){
	guint i;

	for (i = 0; i < sink->notes->len; i++)
	{
		if (0 == g_strcmp0(g_ptr_array_index(sink->notes, i), note))
			return;
	}

	g_ptr_array_add(sink->notes, g_strdup(note));
}

static void
alerts_sink_add(
	AlertsSink		*sink,
	const AlertsRule	*rule,
	JsonObject		*candidate
){
	g_autofree gchar *key = NULL;

	key = g_strdup_printf("%" G_GINT64_FORMAT "|%s", rule->id,
	                      json_object_get_string_member(candidate, "subject"));

	if (!json_object_has_member(sink->candidates, key) &&
	    (json_object_get_size(sink->candidates) >= VENTURE_ALERTS_MAX_CANDIDATES))
	{
		sink->over++;
		json_object_unref(candidate);
		return;
	}

	json_object_set_object_member(sink->candidates, key, candidate);
}

/* A candidate's common half: which rule, about what, seen when. */
static JsonObject *
alerts_candidate_new(
	const AlertsRule	*rule,
	const gchar		*subject,
	const gchar		*venue_key,
	const gchar		*instrument_key,
	const gchar		*instrument_name,
	gint64			 observed_at
){
	JsonObject *candidate;

	candidate = json_object_new();
	json_object_set_int_member(candidate, "rule_id", rule->id);
	json_object_set_int_member(candidate, "kind", (gint64)rule->kind);
	json_object_set_int_member(candidate, "basis", (gint64)rule->basis);
	json_object_set_string_member(candidate, "subject", subject);
	json_object_set_string_member(candidate, "venue_key", (NULL != venue_key) ? venue_key : "");
	json_object_set_string_member(candidate, "instrument_key",
	                              (NULL != instrument_key) ? instrument_key : "");
	json_object_set_string_member(candidate, "instrument_name",
	                              (NULL != instrument_name) ? instrument_name : "");
	json_object_set_int_member(candidate, "observed_at", observed_at);

	return candidate;
}

/* A candidate about one store row: its venue's name rides along, so the
 * message names the realm as a person knows it ("Silver Hand"), not by
 * the store's key ("3676"). */
static JsonObject *
alerts_row_candidate_new(
	const AlertsRule	*rule,
	const gchar		*subject,
	const VentureSeriesRow	*row
){
	JsonObject *candidate;

	candidate = alerts_candidate_new(rule, subject, row->venue_key, row->instrument_key,
	                                 row->instrument_name, row->taken_at);

	if (!venture_string_is_empty(row->venue_name))
		json_object_set_string_member(candidate, "venue_name", row->venue_name);

	return candidate;
}

/* A price as two members: minor units and the currency; nothing when the
 * store had none. */
static void
alerts_set_price(
	JsonObject	*candidate,
	const gchar	*member,
	gint64		 amount,
	const gchar	*currency
){
	g_autofree gchar *currency_member = NULL;

	if (VENTURE_SERIES_NONE == amount)
		return;

	currency_member = g_strconcat(member, "_currency", NULL);
	json_object_set_int_member(candidate, member, amount);
	json_object_set_string_member(candidate, currency_member, currency);
}

static gchar *
alerts_row_subject(const VentureSeriesRow *row)
{
	return g_strdup_printf("%s|%s", row->venue_key, row->instrument_key);
}

static gboolean
alerts_category_matches(
	const gchar	*prefix,
	const gchar	*category
){
	gsize length;

	if ((NULL == prefix) || (NULL == category))
		return FALSE;

	length = strlen(prefix);

	return (0 == strncmp(category, prefix, length)) &&
	       (('\0' == category[length]) || ('/' == category[length]));
}

/*
 * A reference price for @row in the row's own currency, or
 * VENTURE_SERIES_NONE: the venue's current figure, its daily history, or
 * its group's region figure. Nothing is converted -- a reference in
 * another currency is no reference.
 */
static gboolean
alerts_reference_price(
	VentureSeriesStore		 *store,
	const VentureSeriesRow		 *row,
	VentureMarketdataBasis		  basis,
	gint64				  now,
	gint64				 *out,
	GError				**error
){
	*out = VENTURE_SERIES_NONE;

	switch (basis)
	{
	case VENTURE_MARKETDATA_BASIS_MIN:
		*out = row->min_price;
		return TRUE;

	case VENTURE_MARKETDATA_BASIS_MARKET:
		*out = row->market_value;
		return TRUE;

	case VENTURE_MARKETDATA_BASIS_MARKET_14D:
	case VENTURE_MARKETDATA_BASIS_HISTORICAL_60D:
	case VENTURE_MARKETDATA_BASIS_SALE_AVG:
	{
		VentureSeriesReference reference;

		memset(&reference, 0, sizeof(reference));

		if (!venture_series_store_reference(store, row->venue_key, NULL, row->instrument_key,
		                                    now, &reference, error))
			return FALSE;

		if (0 != g_ascii_strcasecmp(reference.currency, row->currency))
			return TRUE;

		*out = (VENTURE_MARKETDATA_BASIS_MARKET_14D == basis) ? reference.market_14d
		     : (VENTURE_MARKETDATA_BASIS_HISTORICAL_60D == basis) ? reference.historical_60d
		     : reference.sale_avg;
		return TRUE;
	}

	case VENTURE_MARKETDATA_BASIS_REGION_MEDIAN:
	case VENTURE_MARKETDATA_BASIS_REGION_P33:
	case VENTURE_MARKETDATA_BASIS_REGION_MARKET_AVG:
	{
		VentureSeriesRegion region;

		memset(&region, 0, sizeof(region));

		if (!venture_series_store_get_region(store, row->group_key, row->instrument_key,
		                                     row->currency, &region, error))
			return FALSE;

		if (!region.found)
			return TRUE;

		*out = (VENTURE_MARKETDATA_BASIS_REGION_MEDIAN == basis) ? region.median_min
		     : (VENTURE_MARKETDATA_BASIS_REGION_P33 == basis) ? region.p33
		     : region.market_avg;
		return TRUE;
	}

	default:
		return TRUE;
	}
}

/*
 * spread: within the group, buy at the venue with the cheapest unit and
 * sell at the best other venue's reference figure (its market value by
 * default; a region basis is the group's one figure). Net of nothing --
 * fees and transfer belong to the arbitrage engine -- and in the
 * threshold's currency only. The candidate is about the buy venue.
 */
static gboolean
alerts_check_spread(
	VentureSeriesStore	 *store,
	const AlertsRule	 *rule,
	const VentureSeriesRow	 *row,
	gint64			  now,
	AlertsSink		 *sink,
	GError			**error
){
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *subject = NULL;
	VentureSeriesRow *buy;
	const gchar *group;
	const gchar *sell_venue;
	const gchar *sell_name;
	JsonObject *candidate;
	gint64 best;
	gint64 spread;
	guint i;

	group = (NULL != rule->group_key) ? rule->group_key
	      : (('\0' != row->group_key[0]) ? row->group_key : NULL);
	rows = venture_series_store_other_venues(store, row->instrument_key, group, error);

	if (NULL == rows)
		return FALSE;

	buy = NULL;

	for (i = 0; i < rows->len; i++)
	{
		VentureSeriesRow *candidate_row = g_ptr_array_index(rows, i);

		if ((VENTURE_SERIES_NONE == candidate_row->min_price) || (candidate_row->quantity <= 0) ||
		    (0 != g_ascii_strcasecmp(candidate_row->currency, rule->currency)))
			continue;

		if ((NULL == buy) || (candidate_row->min_price < buy->min_price))
			buy = candidate_row;
	}

	if (NULL == buy)
		return TRUE;

	best = VENTURE_SERIES_NONE;
	sell_venue = NULL;
	sell_name = NULL;

	for (i = 0; i < rows->len; i++)
	{
		VentureSeriesRow *other = g_ptr_array_index(rows, i);
		gint64 value;

		if ((other == buy) || (0 == g_strcmp0(other->venue_key, buy->venue_key)) ||
		    (0 != g_ascii_strcasecmp(other->currency, rule->currency)))
			continue;

		if (!alerts_reference_price(store, other, rule->basis, now, &value, error))
			return FALSE;

		if ((VENTURE_SERIES_NONE != value) && ((VENTURE_SERIES_NONE == best) || (value > best)))
		{
			best = value;
			sell_venue = other->venue_key;
			sell_name = other->venue_name;
		}
	}

	if ((VENTURE_SERIES_NONE == best) || __builtin_sub_overflow(best, buy->min_price, &spread) ||
	    (spread < rule->threshold))
		return TRUE;

	subject = alerts_row_subject(buy);
	candidate = alerts_row_candidate_new(rule, subject, buy);
	json_object_set_int_member(candidate, "observed_at", row->taken_at);
	alerts_set_price(candidate, "observed", buy->min_price, buy->currency);
	alerts_set_price(candidate, "reference", best, buy->currency);
	alerts_set_price(candidate, "spread", spread, buy->currency);
	alerts_set_price(candidate, "threshold", rule->threshold, rule->currency);
	json_object_set_string_member(candidate, "other_venue_key", sell_venue);

	if (!venture_string_is_empty(sell_name))
		json_object_set_string_member(candidate, "other_venue_name", sell_name);

	alerts_sink_add(sink, rule, candidate);

	return TRUE;
}

/*
 * spike: the figure now against the newest hourly point at or before the
 * window's start. A rise for a positive threshold, a drop for a negative
 * one, as a percent of the earlier figure; prices in one currency.
 */
static gboolean
alerts_check_spike(
	VentureSeriesStore	 *store,
	const AlertsRule	 *rule,
	const VentureSeriesRow	 *row,
	AlertsSink		 *sink,
	GError			**error
){
	g_autoptr(GArray) points = NULL;
	g_autofree gchar *subject = NULL;
	const VentureSeriesPoint *then;
	JsonObject *candidate;
	gint64 cutoff;
	gint64 now_value;
	gint64 then_value;
	gdouble change;
	gboolean quantity;
	guint i;

	cutoff = row->taken_at - rule->window_hours * 3600;
	points = venture_series_store_hourly(store, row->venue_key, row->instrument_key,
	                                     cutoff - 86400, error);

	if (NULL == points)
		return FALSE;

	then = NULL;

	for (i = points->len; i > 0; i--)
	{
		const VentureSeriesPoint *point = &g_array_index(points, VentureSeriesPoint, i - 1);

		if (point->at <= cutoff)
		{
			then = point;
			break;
		}
	}

	if (NULL == then)
		return TRUE;

	quantity = (VENTURE_MARKETDATA_BASIS_QUANTITY == rule->basis);

	switch (rule->basis)
	{
	case VENTURE_MARKETDATA_BASIS_MIN:
		now_value = row->min_price;
		then_value = then->min_price;
		break;
	case VENTURE_MARKETDATA_BASIS_QUANTITY:
		now_value = row->quantity;
		then_value = then->quantity;
		break;
	case VENTURE_MARKETDATA_BASIS_MARKET:
	default:
		now_value = row->market_value;
		then_value = then->market_value;
		break;
	}

	if ((VENTURE_SERIES_NONE == now_value) || (VENTURE_SERIES_NONE == then_value) ||
	    (then_value <= 0) ||
	    (!quantity && (0 != g_ascii_strcasecmp(then->currency, row->currency))))
		return TRUE;

	/* A ratio, so a double; the prices themselves stay integers. */
	change = ((gdouble)now_value - (gdouble)then_value) * 100.0 / (gdouble)then_value;

	if ((rule->number > 0.0) ? (change < rule->number) : (change > rule->number))
		return TRUE;

	subject = alerts_row_subject(row);
	candidate = alerts_row_candidate_new(rule, subject, row);

	if (quantity)
	{
		json_object_set_int_member(candidate, "quantity", now_value);
		json_object_set_int_member(candidate, "then_quantity", then_value);
	}
	else
	{
		alerts_set_price(candidate, "observed", now_value, row->currency);
		alerts_set_price(candidate, "reference", then_value, row->currency);
	}

	json_object_set_double_member(candidate, "observed_number", change);
	json_object_set_double_member(candidate, "reference_number", rule->number);
	json_object_set_int_member(candidate, "window_hours", rule->window_hours);
	json_object_set_int_member(candidate, "then_at", then->at);
	alerts_sink_add(sink, rule, candidate);

	return TRUE;
}

/*
 * deal: buy the venue's cheapest unit, sell where Deals would sell it --
 * venture_marketdata_deal_sell_side(), the page's own reckoning, never a
 * second formula -- and fire when the profit after the cut and the return
 * on the buy price reach what the rule asks. Nothing stale is a deal:
 * the buy side's snapshot and every sell venue's must be newer than
 * series.stale_minutes, because a realm whose feed stopped reads exactly
 * like a cheap one (and a dear one). The candidate is about the buy
 * venue, so the cooldown is kept per realm and item.
 */
static gboolean
alerts_check_deal(
	VentureSeriesStore	 *store,
	const AlertsFrozen	 *frozen,
	const AlertsRule	 *rule,
	const VentureSeriesRow	 *row,
	gint64			  now,
	AlertsSink		 *sink,
	GError			**error
){
	g_autoptr(VentureSeriesRow) sell = NULL;
	g_autofree gchar *subject = NULL;
	JsonObject *candidate;
	gint64 fresh_after;
	gint64 profit;
	gdouble roi;

	(void)error;

	if ((VENTURE_SERIES_NONE == row->min_price) || (row->quantity <= 0) || (row->min_price <= 0))
		return TRUE;

	/* A threshold in another currency says nothing about this row. */
	if ((VENTURE_SERIES_NONE != rule->threshold) && (0 != g_ascii_strcasecmp(row->currency, rule->currency)))
		return TRUE;

	fresh_after = now - frozen->stale_after;

	if (row->taken_at < fresh_after)
		return TRUE;

	sell = venture_marketdata_deal_sell_side(store, row, NULL, VENTURE_MARKETDATA_DEALS_CUT_PCT,
	                                         fresh_after, &profit, &roi);

	if ((NULL == sell) || (profit <= 0) ||
	    ((VENTURE_SERIES_NONE != rule->threshold) && (profit < rule->threshold)) ||
	    ((rule->number > 0.0) && (roi < rule->number)))
		return TRUE;

	subject = alerts_row_subject(row);
	candidate = alerts_row_candidate_new(rule, subject, row);
	alerts_set_price(candidate, "observed", row->min_price, row->currency);
	alerts_set_price(candidate, "reference", sell->min_price, sell->currency);
	alerts_set_price(candidate, "profit", profit, row->currency);
	json_object_set_double_member(candidate, "observed_number", roi);
	json_object_set_double_member(candidate, "reference_number", rule->number);
	json_object_set_double_member(candidate, "cut_pct", VENTURE_MARKETDATA_DEALS_CUT_PCT);
	json_object_set_string_member(candidate, "other_venue_key", sell->venue_key);
	json_object_set_int_member(candidate, "other_taken_at", sell->taken_at);

	if (!venture_string_is_empty(sell->venue_name))
		json_object_set_string_member(candidate, "other_venue_name", sell->venue_name);

	alerts_sink_add(sink, rule, candidate);

	return TRUE;
}

/* Every kind that is about one row of the newest snapshot at a venue. */
static gboolean
alerts_check_row(
	VentureSeriesStore	 *store,
	const AlertsFrozen	 *frozen,
	const AlertsRule	 *rule,
	const VentureSeriesRow	 *row,
	gint64			  now,
	AlertsSink		 *sink,
	GError			**error
){
	g_autofree gchar *subject = NULL;
	JsonObject *candidate;

	switch (rule->kind)
	{
	case VENTURE_ALERT_KIND_BELOW:
	case VENTURE_ALERT_KIND_ABOVE:
		if ((VENTURE_SERIES_NONE == row->min_price) ||
		    (0 != g_ascii_strcasecmp(row->currency, rule->currency)) ||
		    ((VENTURE_ALERT_KIND_BELOW == rule->kind) ? (row->min_price > rule->threshold)
		                                              : (row->min_price < rule->threshold)))
			return TRUE;

		subject = alerts_row_subject(row);
		candidate = alerts_row_candidate_new(rule, subject, row);
		alerts_set_price(candidate, "observed", row->min_price, row->currency);
		alerts_set_price(candidate, "reference", rule->threshold, rule->currency);
		alerts_sink_add(sink, rule, candidate);
		return TRUE;

	case VENTURE_ALERT_KIND_PCT_VS_REFERENCE:
	{
		gint64 reference;
		gdouble percent;

		if (VENTURE_SERIES_NONE == row->min_price)
			return TRUE;

		if (!alerts_reference_price(store, row, rule->basis, now, &reference, error))
			return FALSE;

		if ((VENTURE_SERIES_NONE == reference) || (reference <= 0))
			return TRUE;

		percent = (gdouble)row->min_price * 100.0 / (gdouble)reference;

		if (percent > rule->number)
			return TRUE;

		subject = alerts_row_subject(row);
		candidate = alerts_row_candidate_new(rule, subject, row);
		alerts_set_price(candidate, "observed", row->min_price, row->currency);
		alerts_set_price(candidate, "reference", reference, row->currency);
		json_object_set_double_member(candidate, "observed_number", percent);
		json_object_set_double_member(candidate, "reference_number", rule->number);
		alerts_sink_add(sink, rule, candidate);
		return TRUE;
	}

	case VENTURE_ALERT_KIND_SHORTAGE:
		if ((VENTURE_SERIES_NONE == row->quantity) || ((gdouble)row->quantity >= rule->number))
			return TRUE;

		subject = alerts_row_subject(row);
		candidate = alerts_row_candidate_new(rule, subject, row);
		json_object_set_int_member(candidate, "quantity", row->quantity);
		json_object_set_double_member(candidate, "observed_number", (gdouble)row->quantity);
		json_object_set_double_member(candidate, "reference_number", rule->number);
		alerts_sink_add(sink, rule, candidate);
		return TRUE;

	case VENTURE_ALERT_KIND_OUT_OF_STOCK:
	case VENTURE_ALERT_KIND_BACK_IN_STOCK:
	{
		gboolean out;

		/* The store stamps the snapshot that crossed zero; this one
		 * did when the stamp is the row's own time. */
		out = (VENTURE_ALERT_KIND_OUT_OF_STOCK == rule->kind);

		if ((row->stock_changed_at != row->taken_at) ||
		    (out ? (0 != row->quantity) : (row->quantity <= 0)))
			return TRUE;

		subject = alerts_row_subject(row);
		candidate = alerts_row_candidate_new(rule, subject, row);
		json_object_set_int_member(candidate, "quantity", row->quantity);
		json_object_set_double_member(candidate, "observed_number", (gdouble)row->quantity);

		if (!out)
			alerts_set_price(candidate, "observed", row->min_price, row->currency);

		alerts_sink_add(sink, rule, candidate);
		return TRUE;
	}

	case VENTURE_ALERT_KIND_SPIKE:
		return alerts_check_spike(store, rule, row, sink, error);

	case VENTURE_ALERT_KIND_SPREAD:
		return alerts_check_spread(store, rule, row, now, sink, error);

	case VENTURE_ALERT_KIND_DEAL:
		return alerts_check_deal(store, frozen, rule, row, now, sink, error);

	case VENTURE_ALERT_KIND_UNDERCUT:
	case VENTURE_ALERT_KIND_ENTRY_MATCH:
	default:
		return TRUE;
	}
}

/* Whether a store instrument is in a scoped rule's scope. */
static gboolean
alerts_in_scope(
	const AlertsRule	*rule,
	const gchar		*instrument_key,
	const gchar		*category
){
	if (!rule->scoped)
		return TRUE;

	if (NULL == instrument_key)
		return FALSE;

	return g_hash_table_contains(rule->keys, instrument_key) ||
	       alerts_category_matches(rule->category_prefix, category);
}

/*
 * undercut: each frozen open listing at this venue, against the cheapest
 * unit the venue's newest snapshot offers of the listing's instrument, in
 * the listing's currency. Your own listing is among the units on offer,
 * so a cheapest unit below it is somebody else's. Many listings sell the
 * same thing at the same venue, so each instrument's row is read once.
 */
static gboolean
alerts_evaluate_undercut(
	VentureSeriesStore	 *store,
	const AlertsFrozen	 *frozen,
	const AlertsRule	 *rule,
	const gchar		 *venue_key,
	gint64			  taken_at,
	AlertsSink		 *sink,
	GError			**error
){
	g_autoptr(GHashTable) rows = NULL;
	GPtrArray *listings;
	guint i;
	guint j;

	listings = g_hash_table_lookup(frozen->venue_listings, venue_key);

	if (NULL == listings)
		return TRUE;

	rows = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
	                             (GDestroyNotify)venture_series_row_free);

	for (i = 0; i < listings->len; i++)
	{
		const AlertsListing *listing = g_ptr_array_index(listings, i);

		for (j = 0; j < listing->instrument_keys->len; j++)
		{
			const gchar *instrument_key = g_ptr_array_index(listing->instrument_keys, j);
			g_autofree gchar *subject = NULL;
			VentureSeriesRow *row;
			JsonObject *candidate;
			gpointer cached;

			/* The listing owns the key for longer than this table. */
			if (g_hash_table_lookup_extended(rows, instrument_key, NULL, &cached))
			{
				row = cached;
			}
			else
			{
				row = NULL;

				if (!venture_series_store_get_current(store, venue_key, instrument_key, &row, error))
					return FALSE;

				g_hash_table_insert(rows, (gpointer)instrument_key, row);
			}

			if ((NULL == row) || (row->taken_at != taken_at) ||
			    (VENTURE_SERIES_NONE == row->min_price) || (row->quantity <= 0) ||
			    (0 != g_ascii_strcasecmp(row->currency, listing->currency)) ||
			    (row->min_price >= listing->price) ||
			    !alerts_in_scope(rule, row->instrument_key, row->category))
				continue;

			subject = g_strdup_printf("%s|%s|listing:%" G_GINT64_FORMAT, row->venue_key,
			                          row->instrument_key, listing->id);
			candidate = alerts_row_candidate_new(rule, subject, row);
			alerts_set_price(candidate, "observed", row->min_price, row->currency);
			alerts_set_price(candidate, "reference", listing->price, listing->currency);
			json_object_set_int_member(candidate, "listing_id", listing->id);
			alerts_sink_add(sink, rule, candidate);
		}
	}

	return TRUE;
}

/*
 * One rule at one venue whose newest snapshot was taken at @taken_at:
 * every row of that snapshot in the rule's scope. Rows the snapshot did
 * not touch -- an incomplete snapshot's absentees -- are not looked at;
 * they were judged when they were new.
 */
static gboolean
alerts_evaluate_venue(
	VentureSeriesStore	 *store,
	const AlertsFrozen	 *frozen,
	const AlertsRule	 *rule,
	const gchar		 *venue_key,
	const gchar		 *venue_group,
	gint64			  taken_at,
	gint64			  now,
	AlertsSink		 *sink,
	GError			**error
){
	g_autoptr(GHashTable) seen = NULL;
	guint examined;

	if ((VENTURE_ALERT_KIND_ENTRY_MATCH == rule->kind) || alerts_kind_reads_accounts(rule->kind) ||
	    ((NULL != rule->venue_key) && (0 != g_strcmp0(rule->venue_key, venue_key))) ||
	    ((NULL != rule->group_key) && (0 != g_strcmp0(rule->group_key, venue_group))))
		return TRUE;

	if (VENTURE_ALERT_KIND_UNDERCUT == rule->kind)
		return alerts_evaluate_undercut(store, frozen, rule, venue_key, taken_at, sink, error);

	seen = g_hash_table_new(g_str_hash, g_str_equal);
	examined = 0;

	/* The named instruments, in key order so a capped run is the same
	 * run twice. */
	if (NULL != rule->keys)
	{
		g_autofree gpointer *keys = NULL;
		guint n_keys;
		guint i;

		keys = g_hash_table_get_keys_as_array(rule->keys, &n_keys);
		qsort(keys, n_keys, sizeof(gpointer), alerts_compare_keys);

		for (i = 0; (i < n_keys) && (examined < VENTURE_ALERTS_MAX_ROWS); i++)
		{
			g_autoptr(VentureSeriesRow) row = NULL;

			g_hash_table_add(seen, keys[i]);
			examined++;

			if (!venture_series_store_get_current(store, venue_key, keys[i], &row, error))
				return FALSE;

			if ((NULL != row) && (row->taken_at == taken_at) &&
			    !alerts_check_row(store, frozen, rule, row, now, sink, error))
				return FALSE;
		}
	}

	/* And the category's rows the store files under it, newest first,
	 * stopping at the first row older than the snapshot. */
	if (NULL != rule->category_prefix)
	{
		const gchar *venues[] = { venue_key, NULL };
		VentureSeriesFilter filter;
		gboolean older;

		venture_series_filter_init(&filter);
		filter.venue_keys = venues;
		filter.category_prefix = rule->category_prefix;
		filter.sort = VENTURE_SERIES_SORT_UPDATED;
		filter.descending = TRUE;
		filter.count = ALERTS_PAGE;
		older = FALSE;

		while (!older && (examined < VENTURE_ALERTS_MAX_ROWS))
		{
			g_autoptr(GPtrArray) rows = NULL;
			guint i;

			rows = venture_series_store_list_current(store, &filter, error);

			if (NULL == rows)
				return FALSE;

			for (i = 0; (i < rows->len) && (examined < VENTURE_ALERTS_MAX_ROWS); i++)
			{
				VentureSeriesRow *row = g_ptr_array_index(rows, i);

				if (row->taken_at != taken_at)
				{
					older = TRUE;
					break;
				}

				if (g_hash_table_contains(seen, row->instrument_key))
					continue;

				examined++;

				if (!alerts_check_row(store, frozen, rule, row, now, sink, error))
					return FALSE;
			}

			if (rows->len < ALERTS_PAGE)
				break;

			filter.offset += ALERTS_PAGE;
		}
	}

	return TRUE;
}

/*
 * entry_match: each entry the store received, its title or summary held
 * to the pattern as a substring, both sides case-folded. Scoped rules
 * match only entries about an instrument in scope; a venue or group
 * narrows by the entry's venue.
 */
static gboolean
alerts_evaluate_entries(
	VentureSeriesStore	 *store,
	const AlertsRule	 *rule,
	GPtrArray		 *entries,
	GHashTable		 *venue_groups,
	AlertsSink		 *sink,
	GError			**error
){
	guint i;

	for (i = 0; i < entries->len; i++)
	{
		const VentureSeriesEntryRow *entry = g_ptr_array_index(entries, i);
		g_autofree gchar *title = NULL;
		g_autofree gchar *summary = NULL;
		g_autofree gchar *subject = NULL;
		const gchar *category = NULL;
		g_autoptr(VentureSeriesInstrumentRow) instrument = NULL;
		JsonObject *candidate;

		if ((NULL != rule->venue_key) && (0 != g_strcmp0(rule->venue_key, entry->venue_key)))
			continue;

		if ((NULL != rule->group_key) &&
		    ((NULL == entry->venue_key) ||
		     (0 != g_strcmp0(rule->group_key, g_hash_table_lookup(venue_groups, entry->venue_key)))))
			continue;

		if (rule->scoped && (NULL != entry->instrument_key) && (NULL != rule->category_prefix) &&
		    !g_hash_table_contains(rule->keys, entry->instrument_key))
		{
			if (!venture_series_store_get_instrument(store, entry->instrument_key, &instrument, error))
				return FALSE;

			category = (NULL != instrument) ? instrument->category : NULL;
		}

		if (!alerts_in_scope(rule, entry->instrument_key, category))
			continue;

		title = g_utf8_casefold((NULL != entry->title) ? entry->title : "", -1);
		summary = g_utf8_casefold((NULL != entry->summary) ? entry->summary : "", -1);

		if ((NULL == strstr(title, rule->pattern)) && (NULL == strstr(summary, rule->pattern)))
			continue;

		subject = g_strdup_printf("entry:%s", entry->key);
		candidate = alerts_candidate_new(rule, subject, entry->venue_key, entry->instrument_key,
		                                 NULL, entry->fetched_at);
		json_object_set_string_member(candidate, "entry_key", entry->key);
		json_object_set_string_member(candidate, "title", (NULL != entry->title) ? entry->title : "");

		if (NULL != entry->url)
			json_object_set_string_member(candidate, "url", entry->url);

		alerts_sink_add(sink, rule, candidate);
	}

	return TRUE;
}

/* Venue key to group, for narrowing by group. */
static GHashTable *
alerts_venue_groups(
	VentureSeriesStore	 *store,
	GError			**error
){
	g_autoptr(GPtrArray) venues = NULL;
	GHashTable *groups;
	guint i;

	venues = venture_series_store_list_venues(store, error);

	if (NULL == venues)
		return NULL;

	groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	for (i = 0; i < venues->len; i++)
	{
		VentureSeriesVenueRow *venue = g_ptr_array_index(venues, i);

		g_hash_table_insert(groups, g_strdup(venue->key),
		                    g_strdup((NULL != venue->group_key) ? venue->group_key : ""));
	}

	return groups;
}

/* --- The operator's accounts ------------------------------------------------------ */

/* What one account has waiting to be collected, from its inbound rows. */
typedef struct
{
	gint64	 mails;
	gint64	 items;
	GArray	*money;		/* VentureSeriesAmount, one per currency */
} AlertsCollect;

static void
alerts_collect_free(gpointer data)
{
	AlertsCollect *collect = data;

	g_array_unref(collect->money);
	g_free(collect);
}

/*
 * The store's accounts, read once per evaluation for every account rule
 * in it, and -- only when a collect_ready rule asks -- what each has
 * waiting. Rows belong to @rows; @by_key borrows them.
 */
typedef struct
{
	GPtrArray	*rows;		/* VentureSeriesAccountRow; NULL until read */
	GHashTable	*by_key;	/* key -> borrowed VentureSeriesAccountRow */
	GHashTable	*collect;	/* account key -> AlertsCollect; NULL until read */
	const VentureMarketdataIgnores *ignores;	/* borrowed from the frozen data */
	GHashTable	*ignored;	/* keys of the accounts left out of @rows */
} AlertsAccounts;

static void
alerts_accounts_clear(AlertsAccounts *accounts)
{
	g_clear_pointer(&accounts->by_key, g_hash_table_unref);
	g_clear_pointer(&accounts->rows, g_ptr_array_unref);
	g_clear_pointer(&accounts->collect, g_hash_table_unref);
	g_clear_pointer(&accounts->ignored, g_hash_table_unref);
}

static gboolean
alerts_accounts_load(
	VentureSeriesStore	 *store,
	AlertsAccounts		 *accounts,
	gint64			  now,
	GError			**error
){
	guint i;

	if (NULL != accounts->rows)
		return TRUE;

	accounts->rows = venture_series_store_list_accounts(store, NULL, NULL, NULL, now, error);

	/* The store answers every account or refuses (past
	 * venture_series_accounts_get_max_accounts()): never the first part,
	 * so there is no "only some were judged" to say here. */
	if (NULL == accounts->rows)
		return FALSE;

	/* The ignored accounts are not judged: out of @rows for the rules
	 * that walk accounts, and named in @ignored for those that walk
	 * listings and mail. */
	{
		g_auto(GStrv) keys = venture_marketdata_ignores_split(accounts->ignores, accounts->rows, FALSE);

		accounts->ignored = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

		for (i = 0; NULL != keys[i]; i++)
			g_hash_table_add(accounts->ignored, g_strdup(keys[i]));
	}

	accounts->by_key = g_hash_table_new(g_str_hash, g_str_equal);

	for (i = 0; i < accounts->rows->len; i++)
	{
		VentureSeriesAccountRow *row = g_ptr_array_index(accounts->rows, i);

		g_hash_table_insert(accounts->by_key, row->key, row);
	}

	return TRUE;
}

/*
 * Whether a row about @account passes the rule's narrowing: the venue (a
 * position's own, else the account's) and the group (always the
 * account's). An account the read did not reach has no group, so a rule
 * narrowed to one says nothing about it.
 */
static gboolean
alerts_account_matches(
	const AlertsRule		*rule,
	const VentureSeriesAccountRow	*account,
	const gchar			*venue_key
){
	if (NULL == venue_key)
		venue_key = (NULL != account) ? account->venue_key : NULL;

	if ((NULL != rule->venue_key) && (0 != g_strcmp0(rule->venue_key, venue_key)))
		return FALSE;

	if ((NULL != rule->group_key) &&
	    ((NULL == account) || (0 != g_strcmp0(rule->group_key, account->group_key))))
		return FALSE;

	return TRUE;
}

/* "Drgold (Thorium Brotherhood)": the account's name, else its key, and
 * where it is -- its group, else its venue. */
static gchar *
alerts_account_label(
	const gchar			*key,
	const VentureSeriesAccountRow	*account
){
	const gchar *name;
	const gchar *where;

	name = ((NULL != account) && !venture_string_is_empty(account->name)) ? account->name : key;
	where = NULL;

	if (NULL != account)
		where = !venture_string_is_empty(account->group_key) ? account->group_key
		      : !venture_string_is_empty(account->venue_key) ? account->venue_key : NULL;

	return (NULL != where) ? g_strdup_printf("%s (%s)", name, where) : g_strdup(name);
}

static void
alerts_set_account(
	JsonObject			*candidate,
	const gchar			*key,
	const VentureSeriesAccountRow	*account
){
	g_autofree gchar *label = alerts_account_label(key, account);

	json_object_set_string_member(candidate, "account_key", key);
	json_object_set_string_member(candidate, "account_label", label);
}

/* Whether a row about @instrument_key is in a scoped rule's scope: the
 * named keys, or the category, read from the store's instrument. A row
 * about no instrument is in no scope. */
static gboolean
alerts_key_in_scope(
	VentureSeriesStore	 *store,
	const AlertsRule	 *rule,
	const gchar		 *instrument_key,
	gboolean		 *out,
	GError			**error
){
	g_autoptr(VentureSeriesInstrumentRow) instrument = NULL;

	*out = !rule->scoped;

	if (!rule->scoped || (NULL == instrument_key))
		return TRUE;

	if (g_hash_table_contains(rule->keys, instrument_key))
	{
		*out = TRUE;
		return TRUE;
	}

	if (NULL == rule->category_prefix)
		return TRUE;

	if (!venture_series_store_get_instrument(store, instrument_key, &instrument, error))
		return FALSE;

	*out = (NULL != instrument) && alerts_category_matches(rule->category_prefix, instrument->category);

	return TRUE;
}

/* The end of the window an expiring rule watches: @now plus its hours. */
static gint64
alerts_horizon(
	const AlertsRule	*rule,
	gint64			 now
){
	return now + (gint64)floor(rule->number * 3600.0);
}

/*
 * position_expiring: each of the operator's open positions whose expiry
 * is after @now and at most the rule's hours after it. Soonest first, as
 * the store lists them; positions already expired are skipped, being
 * collect_ready's to tell. The subject is the position, so one listing is
 * told about once per cooldown however many runs see it.
 */
static gboolean
alerts_evaluate_positions(
	VentureSeriesStore	 *store,
	const AlertsRule	 *rule,
	AlertsAccounts		 *accounts,
	gint64			  now,
	AlertsSink		 *sink,
	GError			**error
){
	VentureSeriesPositionFilter filter;
	guint read;
	guint i;

	venture_series_position_filter_init(&filter);

	/* The store's bound is strict, the window's end is not. */
	filter.expires_before = alerts_horizon(rule, now) + 1;
	filter.venue_key = rule->venue_key;
	filter.count = ALERTS_PAGE;
	read = 0;

	for (;;)
	{
		g_autoptr(GPtrArray) page = NULL;

		page = venture_series_store_list_positions(store, &filter, error);

		if (NULL == page)
			return FALSE;

		for (i = 0; i < page->len; i++)
		{
			VentureSeriesPositionRow *row = g_ptr_array_index(page, i);
			const VentureSeriesAccountRow *account;
			g_autofree gchar *subject = NULL;
			JsonObject *candidate;
			gboolean in_scope;

			if ((row->expires_at <= now) || g_hash_table_contains(accounts->ignored, row->account_key))
				continue;

			account = g_hash_table_lookup(accounts->by_key, row->account_key);

			if (!alerts_account_matches(rule, account, row->venue_key))
				continue;

			if (!alerts_key_in_scope(store, rule, row->instrument_key, &in_scope, error))
				return FALSE;

			if (!in_scope)
				continue;

			subject = g_strdup_printf("position:%s", row->key);
			candidate = alerts_candidate_new(rule, subject, row->venue_key, row->instrument_key,
			                                 row->instrument_name, now);
			alerts_set_account(candidate, row->account_key, account);
			json_object_set_string_member(candidate, "position_key", row->key);
			json_object_set_int_member(candidate, "quantity", row->quantity);
			json_object_set_int_member(candidate, "expires_at", row->expires_at);

			if ('\0' != row->currency[0])
				alerts_set_price(candidate, "observed", row->unit_price, row->currency);

			json_object_set_double_member(candidate, "observed_number",
			                              (gdouble)(row->expires_at - now) / 3600.0);
			json_object_set_double_member(candidate, "reference_number", rule->number);
			alerts_sink_add(sink, rule, candidate);
		}

		read += page->len;

		if (page->len < ALERTS_PAGE)
			break;

		if (read >= VENTURE_ALERTS_MAX_ACCOUNT_ROWS)
		{
			g_autofree gchar *note = g_strdup_printf("only the first %d positions were "
			                                         "judged", VENTURE_ALERTS_MAX_ACCOUNT_ROWS);

			alerts_sink_note(sink, note);
			break;
		}

		filter.offset += page->len;
	}

	return TRUE;
}

/*
 * inbound_expiring: each inbound row -- a mail with money or an item --
 * lost after @now and at most the rule's hours after it. A scoped rule
 * hears only about rows carrying an instrument in scope; a venue narrows
 * by the account's venue, since a mail has none of its own.
 */
static gboolean
alerts_evaluate_inbound(
	VentureSeriesStore	 *store,
	const AlertsRule	 *rule,
	AlertsAccounts		 *accounts,
	gint64			  now,
	AlertsSink		 *sink,
	GError			**error
){
	VentureSeriesInboundFilter filter;
	guint read;
	guint i;

	venture_series_inbound_filter_init(&filter);
	filter.expires_before = alerts_horizon(rule, now) + 1;
	filter.count = ALERTS_PAGE;
	read = 0;

	for (;;)
	{
		g_autoptr(GPtrArray) page = NULL;

		page = venture_series_store_list_inbound(store, &filter, error);

		if (NULL == page)
			return FALSE;

		for (i = 0; i < page->len; i++)
		{
			VentureSeriesInboundRow *row = g_ptr_array_index(page, i);
			const VentureSeriesAccountRow *account;
			g_autofree gchar *subject = NULL;
			JsonObject *candidate;
			gboolean in_scope;

			if ((row->expires_at <= now) || g_hash_table_contains(accounts->ignored, row->account_key))
				continue;

			account = g_hash_table_lookup(accounts->by_key, row->account_key);

			if (!alerts_account_matches(rule, account, NULL))
				continue;

			if (!alerts_key_in_scope(store, rule, row->instrument_key, &in_scope, error))
				return FALSE;

			if (!in_scope)
				continue;

			subject = g_strdup_printf("inbound:%s", row->key);
			candidate = alerts_candidate_new(rule, subject,
			                                 (NULL != account) ? account->venue_key : NULL,
			                                 row->instrument_key, row->instrument_name, now);
			alerts_set_account(candidate, row->account_key, account);
			json_object_set_string_member(candidate, "inbound_key", row->key);
			json_object_set_int_member(candidate, "expires_at", row->expires_at);
			json_object_set_boolean_member(candidate, "returned", row->returned);

			if (NULL != row->sender)
				json_object_set_string_member(candidate, "sender", row->sender);

			if (NULL != row->subject)
				json_object_set_string_member(candidate, "mail_subject", row->subject);

			if ((NULL != row->instrument_key) && (VENTURE_SERIES_NONE != row->quantity))
				json_object_set_int_member(candidate, "quantity", row->quantity);

			if ('\0' != row->currency[0])
			{
				if ((VENTURE_SERIES_NONE != row->money) && (row->money > 0))
					alerts_set_price(candidate, "observed", row->money, row->currency);

				if ((VENTURE_SERIES_NONE != row->cod) && (row->cod > 0))
					alerts_set_price(candidate, "cod", row->cod, row->currency);
			}

			json_object_set_double_member(candidate, "observed_number",
			                              (gdouble)(row->expires_at - now) / 3600.0);
			json_object_set_double_member(candidate, "reference_number", rule->number);
			alerts_sink_add(sink, rule, candidate);
		}

		read += page->len;

		if (page->len < ALERTS_PAGE)
			break;

		if (read >= VENTURE_ALERTS_MAX_ACCOUNT_ROWS)
		{
			g_autofree gchar *note = g_strdup_printf("only the first %d inbound rows were "
			                                         "judged", VENTURE_ALERTS_MAX_ACCOUNT_ROWS);

			alerts_sink_note(sink, note);
			break;
		}

		filter.offset += page->len;
	}

	return TRUE;
}

/*
 * account_stale: each account not seen for longer than the rule's days --
 * its last sighting by the source, else the store's first. Shared and
 * guild accounts are opened through a character, so their own sighting
 * proves nothing and they are not judged. The hit is observed at the
 * sighting, not at the evaluation: the cooldown never tells the same
 * spell twice, and seeing the account again starts a new one.
 */
static gboolean
alerts_evaluate_stale(
	const AlertsRule	*rule,
	AlertsAccounts		*accounts,
	gint64			 now,
	AlertsSink		*sink
){
	gint64 cutoff;
	guint i;

	cutoff = now - (gint64)floor(rule->number * 86400.0);

	for (i = 0; i < accounts->rows->len; i++)
	{
		VentureSeriesAccountRow *account = g_ptr_array_index(accounts->rows, i);
		g_autofree gchar *subject = NULL;
		JsonObject *candidate;
		gint64 seen;

		if ((0 == g_strcmp0(account->kind, ALERTS_SHARED_KIND)) ||
		    (0 == g_strcmp0(account->kind, ALERTS_GUILD_KIND)) ||
		    !alerts_account_matches(rule, account, NULL))
			continue;

		seen = (VENTURE_SERIES_NONE != account->last_seen) ? account->last_seen : account->first_seen;

		if (seen >= cutoff)
			continue;

		subject = g_strdup_printf("account:%s", account->key);
		candidate = alerts_candidate_new(rule, subject, account->venue_key, NULL, NULL, seen);
		alerts_set_account(candidate, account->key, account);
		json_object_set_int_member(candidate, "judged_at", now);
		json_object_set_double_member(candidate, "observed_number", (gdouble)(now - seen) / 86400.0);
		json_object_set_double_member(candidate, "reference_number", rule->number);
		alerts_sink_add(sink, rule, candidate);
	}

	return TRUE;
}

/* What every account has waiting in its inbound, read once: rows still to
 * be had (not expired) with money or an item. Cash on delivery alone is
 * a bill, not something to collect. */
static gboolean
alerts_accounts_load_collect(
	VentureSeriesStore	 *store,
	AlertsAccounts		 *accounts,
	gint64			  now,
	AlertsSink		 *sink,
	GError			**error
){
	VentureSeriesInboundFilter filter;
	guint read;
	guint i;
	guint c;

	if (NULL != accounts->collect)
		return TRUE;

	accounts->collect = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, alerts_collect_free);
	venture_series_inbound_filter_init(&filter);
	filter.count = ALERTS_PAGE;
	read = 0;

	for (;;)
	{
		g_autoptr(GPtrArray) page = NULL;

		page = venture_series_store_list_inbound(store, &filter, error);

		if (NULL == page)
			return FALSE;

		for (i = 0; i < page->len; i++)
		{
			VentureSeriesInboundRow *row = g_ptr_array_index(page, i);
			gboolean money;
			AlertsCollect *collect;

			money = (VENTURE_SERIES_NONE != row->money) && (row->money > 0) && ('\0' != row->currency[0]);

			if (((VENTURE_SERIES_NONE != row->expires_at) && (row->expires_at <= now)) ||
			    (!money && (NULL == row->instrument_key)))
				continue;

			collect = g_hash_table_lookup(accounts->collect, row->account_key);

			if (NULL == collect)
			{
				collect = g_new0(AlertsCollect, 1);
				collect->money = g_array_new(FALSE, TRUE, sizeof(VentureSeriesAmount));
				g_hash_table_insert(accounts->collect, g_strdup(row->account_key), collect);
			}

			collect->mails++;

			if (NULL != row->instrument_key)
				collect->items += ((VENTURE_SERIES_NONE != row->quantity) && (row->quantity > 0))
				                  ? row->quantity : 1;

			if (!money)
				continue;

			for (c = 0; c < collect->money->len; c++)
			{
				VentureSeriesAmount *amount = &g_array_index(collect->money, VentureSeriesAmount, c);

				if (0 == g_ascii_strcasecmp(amount->currency, row->currency))
				{
					/* Saturating: a total past the integer is still "a lot". */
					if (__builtin_add_overflow(amount->amount, row->money, &amount->amount))
						amount->amount = G_MAXINT64;
					break;
				}
			}

			if (c == collect->money->len)
			{
				VentureSeriesAmount amount;

				memset(&amount, 0, sizeof(amount));
				g_strlcpy(amount.currency, row->currency, sizeof(amount.currency));
				amount.amount = row->money;
				g_array_append_val(collect->money, amount);
			}
		}

		read += page->len;

		if (page->len < ALERTS_PAGE)
			break;

		if (read >= VENTURE_ALERTS_MAX_ACCOUNT_ROWS)
		{
			g_autofree gchar *note = g_strdup_printf("only the first %d inbound rows were "
			                                         "judged", VENTURE_ALERTS_MAX_ACCOUNT_ROWS);

			alerts_sink_note(sink, note);
			break;
		}

		filter.offset += page->len;
	}

	return TRUE;
}

/*
 * collect_ready: each account with something to pick up at its next
 * login -- mail with money or goods, or positions whose expiry has passed
 * while the store still holds them ("expired, awaiting login": a game
 * returns them to the mailbox, and the source learns so only when the
 * character is next seen). One candidate per account, summarising.
 */
static gboolean
alerts_evaluate_collect(
	VentureSeriesStore	 *store,
	const AlertsRule	 *rule,
	AlertsAccounts		 *accounts,
	gint64			  now,
	AlertsSink		 *sink,
	GError			**error
){
	guint i;
	guint c;

	if (!alerts_accounts_load_collect(store, accounts, now, sink, error))
		return FALSE;

	for (i = 0; i < accounts->rows->len; i++)
	{
		VentureSeriesAccountRow *account = g_ptr_array_index(accounts->rows, i);
		const AlertsCollect *collect;
		g_autofree gchar *subject = NULL;
		JsonObject *candidate;
		JsonArray *money;
		gint64 mails;
		gint64 expired;

		if (!alerts_account_matches(rule, account, NULL))
			continue;

		collect = g_hash_table_lookup(accounts->collect, account->key);
		mails = (NULL != collect) ? collect->mails : 0;
		expired = account->positions_expired;

		if ((mails <= 0) && (expired <= 0))
			continue;

		subject = g_strdup_printf("collect:%s", account->key);
		candidate = alerts_candidate_new(rule, subject, account->venue_key, NULL, NULL, now);
		alerts_set_account(candidate, account->key, account);
		json_object_set_int_member(candidate, "mails", mails);
		json_object_set_int_member(candidate, "items", (NULL != collect) ? collect->items : 0);
		json_object_set_int_member(candidate, "expired_positions", expired);
		money = json_array_new();

		for (c = 0; (NULL != collect) && (c < collect->money->len); c++)
		{
			const VentureSeriesAmount *amount = &g_array_index(collect->money, VentureSeriesAmount, c);
			JsonObject *each = json_object_new();

			json_object_set_int_member(each, "amount", amount->amount);
			json_object_set_string_member(each, "currency", amount->currency);
			json_array_add_object_element(money, each);

			if (0 == c)
				alerts_set_price(candidate, "observed", amount->amount, amount->currency);
		}

		json_object_set_array_member(candidate, "money", money);
		json_object_set_double_member(candidate, "observed_number", (gdouble)(mails + expired));
		alerts_sink_add(sink, rule, candidate);
	}

	return TRUE;
}

/* One account rule against the store at @now. */
static gboolean
alerts_evaluate_account_rule(
	VentureSeriesStore	 *store,
	const AlertsRule	 *rule,
	AlertsAccounts		 *accounts,
	gint64			  now,
	AlertsSink		 *sink,
	GError			**error
){
	if (!alerts_accounts_load(store, accounts, now, error))
		return FALSE;

	switch (rule->kind)
	{
	case VENTURE_ALERT_KIND_POSITION_EXPIRING:
		return alerts_evaluate_positions(store, rule, accounts, now, sink, error);
	case VENTURE_ALERT_KIND_INBOUND_EXPIRING:
		return alerts_evaluate_inbound(store, rule, accounts, now, sink, error);
	case VENTURE_ALERT_KIND_ACCOUNT_STALE:
		return alerts_evaluate_stale(rule, accounts, now, sink);
	case VENTURE_ALERT_KIND_COLLECT_READY:
		return alerts_evaluate_collect(store, rule, accounts, now, sink, error);
	default:
		return TRUE;
	}
}

/*
 * Every frozen rule at every venue in @venue_keys, and the entry_match
 * rules over @entries. @done remembers "venue@time" so a venue whose
 * snapshot was judged in an earlier unit of the run is not judged again.
 */
static gboolean
alerts_evaluate(
	VentureSeriesStore	 *store,
	const AlertsFrozen	 *frozen,
	const gchar *const	 *venue_keys,
	GPtrArray		 *entries,
	JsonObject		 *done,
	gint64			  now,
	gint64			  accounts_now,
	AlertsSink		 *sink,
	GError			**error
){
	g_autoptr(GHashTable) groups = NULL;
	guint i;
	guint r;

	groups = alerts_venue_groups(store, error);

	if (NULL == groups)
		return FALSE;

	for (i = 0; (NULL != venue_keys) && (NULL != venue_keys[i]); i++)
	{
		g_autofree gchar *done_key = NULL;
		VentureSeriesVenueState state;
		const gchar *group;

		memset(&state, 0, sizeof(state));

		if (!venture_series_store_get_venue_state(store, venue_keys[i], &state, error))
			return FALSE;

		if (!state.found)
			continue;

		done_key = g_strdup_printf("%s@%" G_GINT64_FORMAT, venue_keys[i], state.last_taken_at);

		if (NULL != done)
		{
			if (json_object_has_member(done, done_key))
				continue;

			json_object_set_boolean_member(done, done_key, TRUE);
		}

		group = g_hash_table_lookup(groups, venue_keys[i]);

		for (r = 0; r < frozen->rules->len; r++)
		{
			if (!alerts_evaluate_venue(store, frozen, g_ptr_array_index(frozen->rules, r),
			                           venue_keys[i], (NULL != group) ? group : "",
			                           state.last_taken_at, now, sink, error))
				return FALSE;
		}
	}

	for (r = 0; (NULL != entries) && (r < frozen->rules->len); r++)
	{
		const AlertsRule *rule = g_ptr_array_index(frozen->rules, r);

		if ((VENTURE_ALERT_KIND_ENTRY_MATCH == rule->kind) &&
		    !alerts_evaluate_entries(store, rule, entries, groups, sink, error))
			return FALSE;
	}

	/* The account kinds are about the store as it stands, not about a
	 * venue's snapshot: every evaluation judges them, at @accounts_now. */
	{
		AlertsAccounts accounts;
		gboolean ok;

		memset(&accounts, 0, sizeof(accounts));
		accounts.ignores = frozen->ignores;
		ok = TRUE;

		for (r = 0; ok && (r < frozen->rules->len); r++)
		{
			const AlertsRule *rule = g_ptr_array_index(frozen->rules, r);

			if (alerts_kind_reads_accounts(rule->kind))
				ok = alerts_evaluate_account_rule(store, rule, &accounts, accounts_now, sink, error);
		}

		alerts_accounts_clear(&accounts);

		if (!ok)
			return FALSE;
	}

	return TRUE;
}

static gboolean
alerts_frozen_has_kind(
	const AlertsFrozen	*frozen,
	VentureAlertKind	 kind
){
	guint i;

	for (i = 0; i < frozen->rules->len; i++)
	{
		if (((AlertsRule *)g_ptr_array_index(frozen->rules, i))->kind == kind)
			return TRUE;
	}

	return FALSE;
}

/* ==========================================================================
 * The feeds hook: freeze on the main thread, find on the worker
 * ========================================================================== */

static gpointer
alerts_hook_freeze(
	VentureContext	 *context,
	VentureEntity	 *data_source,
	gpointer	  user_data,
	GDestroyNotify	 *out_free
){
	(void)user_data;

	*out_free = alerts_frozen_free;

	return alerts_freeze(context, data_source, NULL);
}

/*
 * On the worker, after each unit's commit: the run's venues not judged
 * yet, and the entries that arrived since the run began, against the
 * frozen rules. Candidates gather in the run's "alerts" payload --
 * {"done": {...}, "candidates": {...}, "over": n} -- for the main thread.
 * A store read that fails is a note on the run, never a g_warning: this
 * thread's failures are recorded, not raised.
 */
static void
alerts_hook_commit(
	VentureSeriesStore	*store,
	VentureFeedSource	*source,
	gconstpointer		 frozen_data,
	VentureFeedRun		*run,
	gpointer		 user_data
){
	const AlertsFrozen *frozen = frozen_data;
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(GError) error = NULL;
	JsonNode *payload;
	JsonObject *root;
	AlertsSink sink;
	gint64 accounts_now;

	(void)source;
	(void)user_data;

	if ((NULL == frozen) || (0 == frozen->rules->len))
		return;

	payload = venture_feed_run_get_payload(run, VENTURE_ALERTS_HOOK);

	if (NULL == payload)
	{
		JsonObject *object = json_object_new();

		json_object_set_object_member(object, "done", json_object_new());
		json_object_set_object_member(object, "candidates", json_object_new());
		json_object_set_int_member(object, "over", 0);
		payload = json_node_init_object(json_node_alloc(), object);
		json_object_unref(object);

		/* The run owns it from here; this thread is its only user until
		 * the run is handed back. */
		venture_feed_run_set_payload(run, VENTURE_ALERTS_HOOK, payload);
	}

	root = json_node_get_object(payload);
	sink.candidates = json_object_get_object_member(root, "candidates");
	sink.over = 0;
	sink.notes = g_ptr_array_new_with_free_func(g_free);

	/* Said once a run, not once a unit. */
	if (frozen->listings_capped && !json_object_has_member(root, "listings_capped"))
	{
		g_autofree gchar *note = g_strdup_printf("alerts: undercut rules compared only the newest %d "
		                                         "open listings", VENTURE_ALERTS_MAX_LISTINGS);

		json_object_set_boolean_member(root, "listings_capped", TRUE);
		venture_feed_run_add_note(run, note);
	}

	if (alerts_frozen_has_kind(frozen, VENTURE_ALERT_KIND_ENTRY_MATCH))
	{
		entries = venture_series_store_list_new_entries(store, venture_feed_run_get_started_at(run),
		                                                VENTURE_SERIES_MAX_PAGE, &error);

		if (NULL == entries)
		{
			g_autofree gchar *note = g_strdup_printf("alerts: entries not read: %s", error->message);

			venture_feed_run_add_note(run, note);
			g_clear_error(&error);
		}
		else if ((entries->len >= VENTURE_SERIES_MAX_PAGE) &&
		         !json_object_has_member(root, "entries_capped"))
		{
			json_object_set_boolean_member(root, "entries_capped", TRUE);
			venture_feed_run_add_note(run, "alerts: only the first 1000 new entries were matched");
		}
	}

	/* The account kinds judge at the run's time: the moment the source was
	 * asked, which is what the positions it brought are as of. */
	accounts_now = venture_feed_run_get_started_at(run);

	if (accounts_now <= 0)
		accounts_now = g_get_real_time() / G_USEC_PER_SEC;

	if (!alerts_evaluate(store, frozen, venture_feed_run_get_venues(run), entries,
	                     json_object_get_object_member(root, "done"),
	                     g_get_real_time() / G_USEC_PER_SEC, accounts_now, &sink, &error))
	{
		g_autofree gchar *note = g_strdup_printf("alerts: not evaluated: %s", error->message);

		g_message("%s", note);
		venture_feed_run_add_note(run, note);
	}

	/* Each bound reached, once a run however many units reach it. */
	{
		JsonObject *noted;
		guint i;

		if (!json_object_has_member(root, "noted"))
			json_object_set_object_member(root, "noted", json_object_new());

		noted = json_object_get_object_member(root, "noted");

		for (i = 0; i < sink.notes->len; i++)
		{
			const gchar *note = g_ptr_array_index(sink.notes, i);

			if (json_object_has_member(noted, note))
				continue;

			json_object_set_boolean_member(noted, note, TRUE);
			{
				g_autofree gchar *line = g_strdup_printf("alerts: %s", note);

				venture_feed_run_add_note(run, line);
			}
		}

		g_ptr_array_unref(sink.notes);
	}

	if (sink.over > 0)
	{
		gint64 over = json_object_get_int_member(root, "over") + (gint64)sink.over;

		json_object_set_int_member(root, "over", over);
	}
}

/* ==========================================================================
 * Writing hits: main thread, as the system
 * ========================================================================== */

typedef struct
{
	VentureContext	*context;
	VentureDatabase	*database;
	gint64		 organization_id;
	gint64		 source_id;
	gboolean	 record;		/* write hits, or only say */
	gboolean	 enabled_only;		/* a feed run: switched-off rules stay quiet */
	GHashTable	*rules;			/* id -> VentureEntity, or NULL for "not this one" */
	GHashTable	*recipients;		/* rule id -> user id */
	GHashTable	*instruments;		/* key -> id, this source */
	GHashTable	*venues;		/* key -> id, this source */
	gchar		*source_uuid;		/* this source's, for its mirrored listings */
	guint		 candidates;
	guint		 written;
	guint		 cooled;
	guint		 over_cap;
	guint		 failed;
	JsonArray	*hits;
	JsonArray	*matches;
} AlertsWrite;

static void
alerts_write_init(
	AlertsWrite	*w,
	VentureContext	*context,
	gint64		 organization_id,
	gboolean	 record,
	gboolean	 enabled_only
){
	memset(w, 0, sizeof(*w));
	w->context = context;
	w->database = venture_context_get_database(context);
	w->organization_id = organization_id;
	w->record = record;
	w->enabled_only = enabled_only;
	w->rules = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
	                                 alerts_unref_nullable);
	w->recipients = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
	w->hits = json_array_new();
	w->matches = json_array_new();
}

/* The per-source caches, emptied when the source changes. */
static void
alerts_write_set_source(
	AlertsWrite	*w,
	gint64		 source_id
){
	g_autoptr(VentureEntity) source = NULL;

	w->source_id = source_id;
	g_clear_pointer(&w->instruments, g_hash_table_unref);
	g_clear_pointer(&w->venues, g_hash_table_unref);
	g_clear_pointer(&w->source_uuid, g_free);
	w->instruments = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	w->venues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	source = venture_database_get(w->database, VENTURE_TYPE_DATA_SOURCE, source_id, NULL);

	if (NULL != source)
		w->source_uuid = g_strdup(venture_entity_get_uuid(source));
}

static void
alerts_write_clear(AlertsWrite *w)
{
	g_clear_pointer(&w->rules, g_hash_table_unref);
	g_clear_pointer(&w->recipients, g_hash_table_unref);
	g_clear_pointer(&w->instruments, g_hash_table_unref);
	g_clear_pointer(&w->venues, g_hash_table_unref);
	g_clear_pointer(&w->source_uuid, g_free);
	g_clear_pointer(&w->hits, json_array_unref);
	g_clear_pointer(&w->matches, json_array_unref);
}

/* The rule a candidate names, as it is now: deleted since, moved to
 * another organization or (for a run) switched off since the freeze, and
 * it says nothing. */
static VentureEntity *
alerts_write_rule(
	AlertsWrite	*w,
	gint64		 rule_id
){
	gpointer cached;
	VentureEntity *rule;
	gboolean enabled = FALSE;

	if (g_hash_table_lookup_extended(w->rules, &rule_id, NULL, &cached))
		return cached;

	rule = venture_database_get(w->database, VENTURE_TYPE_ALERT_RULE, rule_id, NULL);

	if (NULL != rule)
		g_object_get(rule, "enabled", &enabled, NULL);

	if ((NULL != rule) &&
	    (venture_entity_is_deleted(rule) ||
	     (venture_entity_get_organization_id(rule) != w->organization_id) ||
	     (w->enabled_only && !enabled)))
		g_clear_object(&rule);

	g_hash_table_insert(w->rules, g_memdup2(&rule_id, sizeof(rule_id)), rule);

	return rule;
}

/* The live record of @type in this source with @key, or 0: a store row
 * nobody promoted has a key and no record. */
static gint64
alerts_write_record_id(
	AlertsWrite	*w,
	GType		 type,
	GHashTable	*cache,
	const gchar	*key
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	gpointer cached;
	gint64 id;
	guint i;

	if (venture_string_is_empty(key))
		return 0;

	if (g_hash_table_lookup_extended(cache, key, NULL, &cached))
		return *(gint64 *)cached;

	id = 0;
	query = venture_query_new(type);
	venture_query_set_organization(query, w->organization_id);
	venture_query_set_limit(query, 10);

	if (venture_query_add_filter_string(query, "key", VENTURE_FILTER_OP_EQ, key, NULL) &&
	    venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL))
		rows = venture_database_find(w->database, query, NULL);

	/* This source's own first; one that names no source after. */
	for (i = 0; (NULL != rows) && (i < rows->len) && (0 == id); i++)
	{
		if (alerts_int(g_ptr_array_index(rows, i), "data-source-id") == w->source_id)
			id = venture_entity_get_id(g_ptr_array_index(rows, i));
	}

	for (i = 0; (NULL != rows) && (i < rows->len) && (0 == id); i++)
	{
		if (alerts_int(g_ptr_array_index(rows, i), "data-source-id") <= 0)
			id = venture_entity_get_id(g_ptr_array_index(rows, i));
	}

	g_hash_table_insert(cache, g_strdup(key), g_memdup2(&id, sizeof(id)));

	return id;
}

/*
 * Who a rule's hits are told to: its notify-username, else whoever created
 * it -- the creation's audit actor, an API token standing for its owner.
 * 0 when that is nobody who can read an inbox; the hit is recorded and
 * published either way.
 */
static gint64
alerts_write_recipient(
	AlertsWrite	*w,
	VentureEntity	*rule
){
	g_autofree gchar *username = NULL;
	gpointer cached;
	gint64 rule_id;
	gint64 user_id;

	rule_id = venture_entity_get_id(rule);

	if (g_hash_table_lookup_extended(w->recipients, &rule_id, NULL, &cached))
		return *(gint64 *)cached;

	g_object_get(rule, "notify-username", &username, NULL);
	user_id = 0;

	if (!venture_string_is_empty(username))
	{
		/* Judged again at every hit: a membership revoked since the rule
		 * was saved stops the delivery. The hit is still recorded. */
		if (alerts_user_may_hear(w->database, username, venture_entity_get_organization_id(rule)))
			user_id = venture_notify_user_id_for_username(w->context, username);
	}
	else
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(VentureEntity) created = NULL;
		g_autofree gchar *actor = NULL;

		query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
		venture_query_set_limit(query, 1);

		if (venture_query_add_filter_string(query, "target-type", VENTURE_FILTER_OP_EQ,
		                                    "alert_rule", NULL) &&
		    venture_query_add_filter_int(query, "target-id", VENTURE_FILTER_OP_EQ, rule_id, NULL) &&
		    venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL))
			created = venture_database_find_one(w->database, query, NULL);

		if (NULL != created)
			g_object_get(created, "actor", &actor, NULL);

		if ((NULL != actor) && g_str_has_prefix(actor, "API token #"))
		{
			g_autoptr(VentureEntity) token = NULL;
			gint64 token_id = 0;

			if (g_ascii_string_to_signed(actor + strlen("API token #"), 10, 1, G_MAXINT64,
			                             &token_id, NULL))
				token = venture_database_get(w->database, VENTURE_TYPE_API_TOKEN, token_id, NULL);

			if (NULL != token)
				user_id = alerts_int(token, "user-id");
		}
		else
		{
			user_id = venture_notify_user_id_for_username(w->context, actor);
		}
	}

	g_hash_table_insert(w->recipients, g_memdup2(&rule_id, sizeof(rule_id)),
	                    g_memdup2(&user_id, sizeof(user_id)));

	return user_id;
}

/*
 * Whether the rule already spoke about this subject recently enough: a hit
 * for the same snapshot (or an older one) always counts, and with a
 * cooldown any hit observed less than that long before. Deleted hits
 * count -- deleting one is not asking to be told again at once.
 */
static gboolean
alerts_write_cooling(
	AlertsWrite	*w,
	VentureEntity	*rule,
	const gchar	*subject,
	gint64		 observed_at
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) last = NULL;
	g_autoptr(GDateTime) at = NULL;
	gint64 cooldown;
	gint64 last_at;

	query = venture_query_new(VENTURE_TYPE_ALERT_HIT);
	venture_query_set_organization(query, w->organization_id);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 1);

	if (!venture_query_add_filter_int(query, "rule-id", VENTURE_FILTER_OP_EQ,
	                                  venture_entity_get_id(rule), NULL) ||
	    !venture_query_add_filter_string(query, "subject", VENTURE_FILTER_OP_EQ, subject, NULL) ||
	    !venture_query_add_order(query, "observed-at", VENTURE_SORT_DESCENDING, NULL))
		return FALSE;

	last = venture_database_find_one(w->database, query, NULL);

	if (NULL == last)
		return FALSE;

	g_object_get(last, "observed-at", &at, NULL);
	last_at = (NULL != at) ? g_date_time_to_unix(at) : 0;
	cooldown = alerts_int(rule, "cooldown-minutes");

	return (observed_at <= last_at) || ((cooldown > 0) && (observed_at - last_at < cooldown * 60));
}

/* A candidate's price member as money text, or NULL. */
static gchar *
alerts_price_text(
	JsonObject	*candidate,
	const gchar	*member
){
	g_autofree gchar *currency_member = NULL;
	g_autoptr(VentureMoney) money = NULL;
	const gchar *currency;
	gint64 amount;

	if (!json_object_has_member(candidate, member))
		return NULL;

	currency_member = g_strconcat(member, "_currency", NULL);
	amount = json_object_get_int_member(candidate, member);
	currency = json_object_get_string_member_with_default(candidate, currency_member, "");

	if (venture_currency_is_valid(currency))
		money = venture_money_new_for_currency(amount, currency);

	if (NULL == money)
		return g_strdup_printf("%" G_GINT64_FORMAT " %s", amount, currency);

	return venture_money_to_string(money);
}

static VentureMoney *
alerts_price_money(
	JsonObject	*candidate,
	const gchar	*member
){
	g_autofree gchar *currency_member = NULL;
	const gchar *currency;

	if (!json_object_has_member(candidate, member))
		return NULL;

	currency_member = g_strconcat(member, "_currency", NULL);
	currency = json_object_get_string_member_with_default(candidate, currency_member, "");

	if (!venture_currency_is_valid(currency))
		return NULL;

	return venture_money_new_for_currency(json_object_get_int_member(candidate, member), currency);
}

/* A span of time as a person says it: "45 min", "2 h", "2 h 30 min",
 * "9 days". */
static gchar *
alerts_span(gint64 seconds)
{
	gint64 minutes;

	if (seconds < 60)
		return g_strdup("under a minute");

	if (seconds < 3600)
		return g_strdup_printf("%" G_GINT64_FORMAT " min", seconds / 60);

	if (seconds < 2 * 86400)
	{
		minutes = (seconds % 3600) / 60;

		if (0 == minutes)
			return g_strdup_printf("%" G_GINT64_FORMAT " h", seconds / 3600);

		return g_strdup_printf("%" G_GINT64_FORMAT " h %" G_GINT64_FORMAT " min", seconds / 3600,
		                       minutes);
	}

	return g_strdup_printf("%" G_GINT64_FORMAT " days", seconds / 86400);
}

/* "1 mail", "3 mails". */
static gchar *
alerts_count(
	gint64		 count,
	const gchar	*one,
	const gchar	*many
){
	return g_strdup_printf("%" G_GINT64_FORMAT " %s", count, (1 == count) ? one : many);
}

/* An account kind's sentence. */
static gchar *
alerts_describe_account(
	JsonObject		*candidate,
	VentureAlertKind	 kind,
	const gchar		*what
){
	const gchar *label;
	gint64 observed_at;

	label = json_object_get_string_member_with_default(candidate, "account_label", "");
	observed_at = json_object_get_int_member(candidate, "observed_at");

	switch (kind)
	{
	case VENTURE_ALERT_KIND_POSITION_EXPIRING:
	{
		g_autofree gchar *left = NULL;

		left = alerts_span(json_object_get_int_member(candidate, "expires_at") - observed_at);

		return g_strdup_printf("%s: %" G_GINT64_FORMAT " x %s expires in %s", label,
		                       json_object_get_int_member(candidate, "quantity"), what, left);
	}

	case VENTURE_ALERT_KIND_INBOUND_EXPIRING:
	{
		g_autoptr(GString) text = g_string_new(NULL);
		g_autoptr(GPtrArray) parts = g_ptr_array_new_with_free_func(g_free);
		g_autofree gchar *left = NULL;
		g_autofree gchar *money = alerts_price_text(candidate, "observed");
		g_autofree gchar *cod = alerts_price_text(candidate, "cod");
		const gchar *sender;
		const gchar *subject;
		guint i;

		sender = json_object_get_string_member_with_default(candidate, "sender", NULL);
		subject = json_object_get_string_member_with_default(candidate, "mail_subject", NULL);
		left = alerts_span(json_object_get_int_member(candidate, "expires_at") - observed_at);

		g_string_append_printf(text, "%s: %s", label,
		                       json_object_get_boolean_member_with_default(candidate, "returned", FALSE)
		                       ? "returned mail" : "mail");

		if (!venture_string_is_empty(sender))
			g_string_append_printf(text, " from %s", sender);

		if (!venture_string_is_empty(subject))
			g_string_append_printf(text, " \"%s\"", subject);

		if (NULL != money)
			g_ptr_array_add(parts, g_steal_pointer(&money));

		if (!venture_string_is_empty(json_object_get_string_member_with_default(candidate,
		                                                                        "instrument_key", "")))
			g_ptr_array_add(parts, g_strdup_printf("%" G_GINT64_FORMAT " x %s",
				json_object_get_int_member_with_default(candidate, "quantity", 1), what));

		if (NULL != cod)
			g_ptr_array_add(parts, g_strdup_printf("cash on delivery %s", cod));

		for (i = 0; i < parts->len; i++)
			g_string_append_printf(text, "%s%s", (0 == i) ? " (" : ", ",
			                       (const gchar *)g_ptr_array_index(parts, i));

		if (parts->len > 0)
			g_string_append_c(text, ')');

		g_string_append_printf(text, " expires in %s", left);

		return g_string_free(g_steal_pointer(&text), FALSE);
	}

	case VENTURE_ALERT_KIND_ACCOUNT_STALE:
	{
		g_autoptr(GDateTime) seen = NULL;
		g_autofree gchar *since = NULL;
		g_autofree gchar *span = NULL;

		seen = g_date_time_new_from_unix_utc(observed_at);
		since = (NULL != seen) ? g_date_time_format(seen, "%Y-%m-%d") : g_strdup("?");
		span = alerts_span(json_object_get_int_member(candidate, "judged_at") - observed_at);

		return g_strdup_printf("%s has not been seen for %s (since %s)", label, span, since);
	}

	case VENTURE_ALERT_KIND_COLLECT_READY:
	default:
	{
		g_autoptr(GString) text = g_string_new(NULL);
		g_autoptr(GPtrArray) parts = g_ptr_array_new_with_free_func(g_free);
		JsonArray *money;
		gint64 mails;
		gint64 items;
		gint64 expired;
		guint i;

		mails = json_object_get_int_member_with_default(candidate, "mails", 0);
		items = json_object_get_int_member_with_default(candidate, "items", 0);
		expired = json_object_get_int_member_with_default(candidate, "expired_positions", 0);
		money = json_object_has_member(candidate, "money")
		      ? json_object_get_array_member(candidate, "money") : NULL;

		g_string_append_printf(text, "%s:", label);

		if (mails > 0)
		{
			g_autofree gchar *count = alerts_count(mails, "mail", "mails");

			g_string_append_printf(text, " %s to collect", count);

			for (i = 0; (NULL != money) && (i < json_array_get_length(money)); i++)
			{
				JsonObject *each = json_array_get_object_element(money, i);
				const gchar *currency = json_object_get_string_member(each, "currency");
				gint64 amount = json_object_get_int_member(each, "amount");
				g_autoptr(VentureMoney) value = NULL;

				if (venture_currency_is_valid(currency))
					value = venture_money_new_for_currency(amount, currency);

				g_ptr_array_add(parts, (NULL != value) ? venture_money_to_string(value)
				                       : g_strdup_printf("%" G_GINT64_FORMAT " %s", amount, currency));
			}

			if (items > 0)
				g_ptr_array_add(parts, alerts_count(items, "item", "items"));

			for (i = 0; i < parts->len; i++)
				g_string_append_printf(text, "%s%s", (0 == i) ? " (" : ", ",
				                       (const gchar *)g_ptr_array_index(parts, i));

			if (parts->len > 0)
				g_string_append_c(text, ')');
		}

		if (expired > 0)
		{
			g_autofree gchar *count = alerts_count(expired, "listing", "listings");

			g_string_append_printf(text, "%s %s expired, awaiting login", (mails > 0) ? ";" : "",
			                       count);
		}

		return g_string_free(g_steal_pointer(&text), FALSE);
	}
	}
}

/* What a candidate says, in one sentence: prices formatted here, on the
 * main thread, where the currency registry is. */
static gchar *
alerts_describe(JsonObject *candidate)
{
	g_autofree gchar *observed = alerts_price_text(candidate, "observed");
	g_autofree gchar *reference = alerts_price_text(candidate, "reference");
	const gchar *name;
	const gchar *what;
	const gchar *venue;
	VentureAlertKind kind;
	VentureMarketdataBasis basis;

	kind = (VentureAlertKind)json_object_get_int_member(candidate, "kind");
	basis = (VentureMarketdataBasis)json_object_get_int_member(candidate, "basis");
	name = json_object_get_string_member_with_default(candidate, "instrument_name", "");
	what = !venture_string_is_empty(name) ? name
	     : json_object_get_string_member_with_default(candidate, "instrument_key", "");
	/* The realm as a person knows it, its key when the store has no name. */
	venue = json_object_get_string_member_with_default(candidate, "venue_name", "");

	if (venture_string_is_empty(venue))
		venue = json_object_get_string_member_with_default(candidate, "venue_key", "");

	switch (kind)
	{
	case VENTURE_ALERT_KIND_BELOW:
	case VENTURE_ALERT_KIND_ABOVE:
		return g_strdup_printf("%s at %s: %s, at or %s %s", what, venue, observed,
		                       (VENTURE_ALERT_KIND_BELOW == kind) ? "below" : "above", reference);

	case VENTURE_ALERT_KIND_PCT_VS_REFERENCE:
		return g_strdup_printf("%s at %s: %s is %.1f%% of its %s (%s), at or below %g%%", what,
		                       venue, observed,
		                       json_object_get_double_member(candidate, "observed_number"),
		                       alerts_basis_nick(basis), reference,
		                       json_object_get_double_member(candidate, "reference_number"));

	case VENTURE_ALERT_KIND_SPREAD:
	{
		g_autofree gchar *spread = alerts_price_text(candidate, "spread");
		g_autofree gchar *threshold = alerts_price_text(candidate, "threshold");

		const gchar *other;

		other = json_object_get_string_member_with_default(candidate, "other_venue_name", NULL);

		if (venture_string_is_empty(other))
			other = json_object_get_string_member_with_default(candidate, "other_venue_key",
			                                                   "the group");

		return g_strdup_printf("%s: buy at %s for %s, sell at %s for %s (%s): %s apart, at "
		                       "least %s", what, venue, observed, other, reference,
		                       alerts_basis_nick(basis), spread, threshold);
	}

	case VENTURE_ALERT_KIND_DEAL:
	{
		g_autofree gchar *profit = alerts_price_text(candidate, "profit");
		const gchar *other;

		other = json_object_get_string_member_with_default(candidate, "other_venue_name", NULL);

		if (venture_string_is_empty(other))
			other = json_object_get_string_member_with_default(candidate, "other_venue_key", "");

		/* Short, because its first line is the label a webhook's push
		 * carries; the cut is the Deals page's, said in the docs. */
		return g_strdup_printf("%s: buy at %s for %s, sell at %s for %s, %s profit (%.1f%%)",
		                       what, venue, observed, other, reference,
		                       (NULL != profit) ? profit : "?",
		                       json_object_get_double_member(candidate, "observed_number"));
	}

	case VENTURE_ALERT_KIND_OUT_OF_STOCK:
		return g_strdup_printf("%s is out of stock at %s", what, venue);

	case VENTURE_ALERT_KIND_BACK_IN_STOCK:
		return g_strdup_printf("%s is back in stock at %s: %" G_GINT64_FORMAT " on offer%s%s",
		                       what, venue, json_object_get_int_member(candidate, "quantity"),
		                       (NULL != observed) ? " from " : "",
		                       (NULL != observed) ? observed : "");

	case VENTURE_ALERT_KIND_SHORTAGE:
		return g_strdup_printf("%s at %s: %" G_GINT64_FORMAT " on offer, fewer than %g", what,
		                       venue, json_object_get_int_member(candidate, "quantity"),
		                       json_object_get_double_member(candidate, "reference_number"));

	case VENTURE_ALERT_KIND_SPIKE:
	{
		gdouble change = json_object_get_double_member(candidate, "observed_number");
		g_autofree gchar *from = NULL;
		g_autofree gchar *to = NULL;

		if (VENTURE_MARKETDATA_BASIS_QUANTITY == basis)
		{
			from = g_strdup_printf("%" G_GINT64_FORMAT,
			                       json_object_get_int_member(candidate, "then_quantity"));
			to = g_strdup_printf("%" G_GINT64_FORMAT,
			                     json_object_get_int_member(candidate, "quantity"));
		}
		else
		{
			from = g_strdup(reference);
			to = g_strdup(observed);
		}

		return g_strdup_printf("%s at %s: %s %s %.1f%% in %" G_GINT64_FORMAT " hours (%s to %s)",
		                       what, venue, alerts_basis_nick(basis),
		                       (change >= 0.0) ? "rose" : "fell", fabs(change),
		                       json_object_get_int_member(candidate, "window_hours"), from, to);
	}

	case VENTURE_ALERT_KIND_UNDERCUT:
		return g_strdup_printf("Listing #%" G_GINT64_FORMAT " (%s at %s) asks %s; %s is on offer "
		                       "there now", json_object_get_int_member(candidate, "listing_id"),
		                       what, venue, reference, observed);

	case VENTURE_ALERT_KIND_POSITION_EXPIRING:
	case VENTURE_ALERT_KIND_INBOUND_EXPIRING:
	case VENTURE_ALERT_KIND_ACCOUNT_STALE:
	case VENTURE_ALERT_KIND_COLLECT_READY:
		return alerts_describe_account(candidate, kind, what);

	case VENTURE_ALERT_KIND_ENTRY_MATCH:
	default:
	{
		const gchar *url = json_object_get_string_member_with_default(candidate, "url", NULL);

		return g_strdup_printf("%s%s%s",
		                       json_object_get_string_member_with_default(candidate, "title", ""),
		                       (NULL != url) ? " " : "", (NULL != url) ? url : "");
	}
	}
}

/* The listing a candidate names, when it is still there to point at:
 * a reference to a deleted row is refused at the save. */
static gint64
alerts_live_listing(
	VentureDatabase	*database,
	gint64		 listing_id
){
	g_autoptr(VentureEntity) listing = NULL;

	if (listing_id <= 0)
		return 0;

	listing = venture_database_get(database, VENTURE_TYPE_LISTING, listing_id, NULL);

	return ((NULL != listing) && !venture_entity_is_deleted(listing)) ? listing_id : 0;
}

/* The live listing the mirror keeps for a position of this source, or 0:
 * its external id is "<source uuid>:<position key>". A position nobody
 * mirrored -- mirroring off, no product -- has none, and the hit still
 * names it by its keys. */
static gint64
alerts_position_listing(
	AlertsWrite	*w,
	const gchar	*position_key
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) listing = NULL;
	g_autofree gchar *external_id = NULL;

	if (venture_string_is_empty(position_key) || (NULL == w->source_uuid))
		return 0;

	external_id = g_strdup_printf("%s:%s", w->source_uuid, position_key);
	query = venture_query_new(VENTURE_TYPE_LISTING);
	venture_query_set_organization(query, w->organization_id);
	venture_query_set_limit(query, 1);

	if (!venture_query_add_filter_string(query, "external-id", VENTURE_FILTER_OP_EQ, external_id, NULL))
		return 0;

	listing = venture_database_find_one(w->database, query, NULL);

	return (NULL != listing) ? venture_entity_get_id(listing) : 0;
}

/* One hit, saved as the system, then told to its recipient. */
static gboolean
alerts_write_hit(
	AlertsWrite	 *w,
	VentureEntity	 *rule,
	JsonObject	 *candidate,
	const gchar	 *message,
	GError		**error
){
	g_autoptr(VentureAlertHit) hit = NULL;
	g_autoptr(GDateTime) observed_at = NULL;
	g_autoptr(VentureMoney) observed = NULL;
	g_autoptr(VentureMoney) reference = NULL;
	g_autofree gchar *rule_name = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *label = NULL;
	g_autoptr(GError) notify_error = NULL;
	const gchar *instrument_key;
	const gchar *venue_key;
	VentureActor actor;
	gint64 user_id;
	gint64 listing_id;

	listing_id = alerts_live_listing(w->database,
		json_object_get_int_member_with_default(candidate, "listing_id", 0));

	if (0 == listing_id)
		listing_id = alerts_position_listing(w,
			json_object_get_string_member_with_default(candidate, "position_key", NULL));

	instrument_key = json_object_get_string_member_with_default(candidate, "instrument_key", "");
	venue_key = json_object_get_string_member_with_default(candidate, "venue_key", "");
	observed_at = g_date_time_new_from_unix_utc(json_object_get_int_member(candidate, "observed_at"));
	observed = alerts_price_money(candidate, "observed");
	reference = alerts_price_money(candidate, "reference");

	hit = venture_alert_hit_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(hit), w->organization_id);
	g_object_set(hit,
	             "rule-id", venture_entity_get_id(rule),
	             "kind", (VentureAlertKind)json_object_get_int_member(candidate, "kind"),
	             "observed-at", observed_at,
	             "message", message,
	             "data-source-id", w->source_id,
	             "instrument-key", instrument_key,
	             "instrument-id", alerts_write_record_id(w, VENTURE_TYPE_INSTRUMENT, w->instruments,
	                                                     instrument_key),
	             "venue-key", venue_key,
	             "venue-id", alerts_write_record_id(w, VENTURE_TYPE_VENUE, w->venues, venue_key),
	             "account-key", json_object_get_string_member_with_default(candidate, "account_key", NULL),
	             "listing-id", listing_id,
	             "entry-key", json_object_get_string_member_with_default(candidate, "entry_key", NULL),
	             "url", json_object_get_string_member_with_default(candidate, "url", NULL),
	             "observed", observed,
	             "reference", reference,
	             "observed-number",
	             json_object_get_double_member_with_default(candidate, "observed_number", 0.0),
	             "reference-number",
	             json_object_get_double_member_with_default(candidate, "reference_number", 0.0),
	             "subject", json_object_get_string_member(candidate, "subject"),
	             NULL);

	if (!venture_database_save(w->database, VENTURE_ENTITY(hit), alerts_system_actor(&actor), error))
		return FALSE;

	w->written++;
	json_array_add_int_element(w->hits, venture_entity_get_id(VENTURE_ENTITY(hit)));

	/* The inbox. A failure here is logged, not the hit's: the hit is
	 * written and already published. */
	user_id = alerts_write_recipient(w, rule);
	g_object_get(rule, "name", &rule_name, NULL);
	title = g_strdup_printf("%s: %s", rule_name, message);

	if (g_utf8_strlen(title, -1) > ALERTS_TITLE_CHARS)
	{
		g_autofree gchar *cut = g_utf8_substring(title, 0, ALERTS_TITLE_CHARS - 1);

		g_free(title);
		title = g_strconcat(cut, "\xe2\x80\xa6", NULL);
	}

	label = venture_entity_get_display_name(VENTURE_ENTITY(hit));

	if (!venture_notify_send(w->context, user_id, VENTURE_NOTIFICATION_KIND_ALERT, title, message,
	                         "alert_hit", venture_entity_get_id(VENTURE_ENTITY(hit)), label,
	                         "alerts", &notify_error))
		g_message("alerts: hit %" G_GINT64_FORMAT " was not put in an inbox: %s",
		          venture_entity_get_id(VENTURE_ENTITY(hit)), notify_error->message);

	return TRUE;
}

/*
 * Every candidate of one source, in the order found: its rule as it is
 * now, its message, then -- when writing -- the cooldown, the cap and the
 * hit. A hit that fails to save is logged and counted; the others still
 * go.
 */
static void
alerts_write_candidates(
	AlertsWrite	*w,
	JsonObject	*candidates
){
	g_autoptr(GList) members = NULL;
	GList *l;

	members = json_object_get_members(candidates);

	for (l = members; NULL != l; l = l->next)
	{
		JsonObject *candidate = json_object_get_object_member(candidates, l->data);
		g_autofree gchar *message = NULL;
		g_autoptr(GError) error = NULL;
		VentureEntity *rule;
		const gchar *subject;
		gint64 observed_at;

		rule = alerts_write_rule(w, json_object_get_int_member(candidate, "rule_id"));

		if (NULL == rule)
			continue;

		w->candidates++;
		message = alerts_describe(candidate);
		subject = json_object_get_string_member(candidate, "subject");
		observed_at = json_object_get_int_member(candidate, "observed_at");

		if (json_array_get_length(w->matches) < ALERTS_REPORT_MATCHES)
		{
			JsonObject *match = json_object_new();

			json_object_set_string_member(match, "message", message);
			json_object_set_string_member(match, "subject", subject);
			json_object_set_string_member(match, "venue_key",
				json_object_get_string_member_with_default(candidate, "venue_key", ""));
			json_object_set_string_member(match, "instrument_key",
				json_object_get_string_member_with_default(candidate, "instrument_key", ""));
			json_object_set_string_member(match, "account_key",
				json_object_get_string_member_with_default(candidate, "account_key", ""));
			json_object_set_int_member(match, "observed_at", observed_at);
			json_object_set_int_member(match, "data_source_id", w->source_id);
			json_array_add_object_element(w->matches, match);
		}

		if (!w->record)
			continue;

		if (alerts_write_cooling(w, rule, subject, observed_at))
		{
			w->cooled++;
			continue;
		}

		if (w->written >= VENTURE_ALERTS_MAX_HITS_PER_RUN)
		{
			w->over_cap++;
			continue;
		}

		if (!alerts_write_hit(w, rule, candidate, message, &error))
		{
			w->failed++;
			g_message("alerts: a hit of rule %" G_GINT64_FORMAT " was not written: %s",
			          venture_entity_get_id(rule),
			          (NULL != error) ? error->message : "refused");
		}
	}
}

/* Notes a run's dropped candidates and capped hits on its record. */
static void
alerts_note_run(
	AlertsWrite	*w,
	VentureEntity	*run_record,
	gint64		 dropped
){
	g_autoptr(GString) notes = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *existing = NULL;
	VentureActor actor;

	if ((0 == w->over_cap) && (dropped <= 0))
		return;

	if (NULL == run_record)
	{
		g_message("alerts: %u hits past the cap and %" G_GINT64_FORMAT " candidates past theirs "
		          "were not written", w->over_cap, dropped);
		return;
	}

	g_object_get(run_record, "notes", &existing, NULL);
	notes = g_string_new(existing);

	if (w->over_cap > 0)
		g_string_append_printf(notes, "%salerts: %u hits not written: a run writes at most %d",
		                       (notes->len > 0) ? "\n" : "", w->over_cap,
		                       VENTURE_ALERTS_MAX_HITS_PER_RUN);

	if (dropped > 0)
		g_string_append_printf(notes, "%salerts: %" G_GINT64_FORMAT " candidates not considered: "
		                       "a run hands back at most %d", (notes->len > 0) ? "\n" : "",
		                       dropped, VENTURE_ALERTS_MAX_CANDIDATES);

	g_object_set(run_record, "notes", notes->str, NULL);

	if (!venture_database_save(w->database, run_record, alerts_system_actor(&actor), &error))
		g_message("alerts: the run's note was not written: %s", error->message);
}

/* A run's payload, written: its candidates as hits, its caps as notes. */
static void
alerts_deliver(
	VentureContext	*context,
	VentureFeedRun	*run,
	VentureEntity	*run_record
){
	g_autoptr(VentureAccessScope) internal = NULL;
	JsonNode *payload;
	JsonObject *root;
	AlertsWrite w;

	payload = venture_feed_run_get_payload(run, VENTURE_ALERTS_HOOK);

	if ((NULL == payload) || !venture_context_module_enabled(context, "marketdata"))
		return;

	root = json_node_get_object(payload);
	internal = venture_access_policy_enter(
		venture_database_get_access_policy(venture_context_get_database(context)), NULL);

	alerts_write_init(&w, context, venture_feed_run_get_organization_id(run), TRUE, TRUE);
	alerts_write_set_source(&w, venture_feed_run_get_source_id(run));
	alerts_write_candidates(&w, json_object_get_object_member(root, "candidates"));
	alerts_note_run(&w, run_record, json_object_get_int_member(root, "over"));
	alerts_write_clear(&w);
}

typedef struct
{
	GWeakRef	 context;
	VentureFeedRun	*run;
	gint64		 run_record_id;
} AlertsDeferred;

static void
alerts_deferred_free(gpointer data)
{
	AlertsDeferred *deferred = data;

	g_weak_ref_clear(&deferred->context);
	venture_feed_run_unref(deferred->run);
	g_free(deferred);
}

static gboolean
alerts_deferred_fire(gpointer data)
{
	AlertsDeferred *deferred = data;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureEntity) record = NULL;

	context = g_weak_ref_get(&deferred->context);

	if (NULL == context)
		return G_SOURCE_REMOVE;

	if (alerts_must_wait(context))
		return G_SOURCE_CONTINUE;

	if (deferred->run_record_id > 0)
		record = venture_database_get(venture_context_get_database(context),
		                              VENTURE_TYPE_DATA_SOURCE_RUN, deferred->run_record_id, NULL);

	alerts_deliver(context, deferred->run, record);

	return G_SOURCE_REMOVE;
}

/*
 * On the main thread after the run record is written. The feeds service
 * already waits out transactions; an automation handler running a nested
 * main loop is the case left, and a hit written inside one would raise no
 * on_created -- so the run waits for the handler to return.
 */
static void
alerts_hook_run(
	VentureContext	*context,
	VentureFeedRun	*run,
	VentureEntity	*run_record,
	gpointer	 user_data
){
	(void)user_data;

	if (NULL == venture_feed_run_get_payload(run, VENTURE_ALERTS_HOOK))
		return;

	if (alerts_must_wait(context))
	{
		AlertsDeferred *deferred = g_new0(AlertsDeferred, 1);

		g_weak_ref_init(&deferred->context, context);
		deferred->run = venture_feed_run_ref(run);
		deferred->run_record_id = (NULL != run_record) ? venture_entity_get_id(run_record) : 0;
		g_timeout_add_full(G_PRIORITY_DEFAULT, ALERTS_RETRY_MS, alerts_deferred_fire, deferred,
		                   alerts_deferred_free);
		return;
	}

	alerts_deliver(context, run, run_record);
}

/* --- Listeners: what a rule's frozen copy is made of --------------------------- */

static void
alerts_entity_changed(
	VentureContext	*context,
	VentureEntity	*entity
){
	/* Instruments are the feeds module's own to watch. */
	if (VENTURE_IS_ALERT_RULE(entity) || VENTURE_IS_WATCHLIST(entity) ||
	    VENTURE_IS_WATCHLIST_ENTRY(entity) || VENTURE_IS_VENUE(entity) ||
	    VENTURE_IS_LISTING(entity) || VENTURE_IS_CATEGORY(entity))
		venture_feeds_queue_refresh(context);
}

static void
alerts_on_saved(
	VentureDatabase	*database,
	VentureEntity	*entity,
	gboolean	 created,
	gpointer	 user_data
){
	(void)database;
	(void)created;

	alerts_entity_changed(VENTURE_CONTEXT(user_data), entity);
}

static void
alerts_on_deleted(
	VentureDatabase	*database,
	VentureEntity	*entity,
	gpointer	 user_data
){
	(void)database;

	alerts_entity_changed(VENTURE_CONTEXT(user_data), entity);
}

#endif /* VENTURE_HAVE_SQLITE */

/* ==========================================================================
 * Evaluating by hand
 * ========================================================================== */

gboolean
venture_marketdata_alerts_evaluate(
	VentureContext	 *context,
	VentureEntity	 *rule,
	gboolean	  record,
	JsonNode	**out_report,
	GError		**error
){
	return venture_marketdata_alerts_evaluate_at(context, rule, record,
	                                             g_get_real_time() / G_USEC_PER_SEC, out_report,
	                                             error);
}

gboolean
venture_marketdata_alerts_evaluate_at(
	VentureContext	 *context,
	VentureEntity	 *rule,
	gboolean	  record,
	gint64		  now,
	JsonNode	**out_report,
	GError		**error
){
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(VENTURE_IS_ALERT_RULE(rule), FALSE);

	if (NULL != out_report)
		*out_report = NULL;

	if (venture_automation_is_dispatching(venture_context_get_automation(context)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		                    "Alert rules are not evaluated from inside an automation handler: "
		                    "a hit written there would raise no event anybody hears");
		return FALSE;
	}

	if (!venture_context_module_enabled(context, "marketdata"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Alert rules belong to the marketdata module, which is off");
		return FALSE;
	}

#ifndef VENTURE_HAVE_SQLITE
	(void)record;
	(void)now;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
	                    "Alert rules read a series store, which this build (without SQLite) "
	                    "does not have");
	return FALSE;
#else
	{
		g_autoptr(VentureAccessScope) internal = NULL;
		g_autoptr(GPtrArray) sources = NULL;
		g_autoptr(JsonBuilder) builder = NULL;
		g_autoptr(JsonArray) notes = NULL;
		VentureFeedsService *service;
		VentureDatabase *database;
		AlertsWrite w;
		gint64 source_id;
		guint i;

		service = venture_context_get_feeds_service(context);

		if (NULL == service)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			                    "Market data feeds are off (feeds.enabled), so there is no store "
			                    "to evaluate a rule against");
			return FALSE;
		}

		database = venture_context_get_database(context);
		internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
		source_id = alerts_int(rule, "data-source-id");

		/* The rule's source, else every source of its organization. */
		if (source_id > 0)
		{
			VentureEntity *source;

			source = venture_database_get(database, VENTURE_TYPE_DATA_SOURCE, source_id, NULL);

			if ((NULL == source) || venture_entity_is_deleted(source))
			{
				g_clear_object(&source);
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
				            "The rule reads data source #%" G_GINT64_FORMAT ", which is gone",
				            source_id);
				return FALSE;
			}

			sources = g_ptr_array_new_with_free_func(g_object_unref);
			g_ptr_array_add(sources, source);
		}
		else
		{
			g_autoptr(VentureQuery) query = NULL;

			query = venture_query_new(VENTURE_TYPE_DATA_SOURCE);
			venture_query_set_organization(query, venture_entity_get_organization_id(rule));
			venture_query_set_limit(query, 100);

			if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
				return FALSE;

			sources = venture_database_find(database, query, error);

			if (NULL == sources)
				return FALSE;
		}

		notes = json_array_new();
		alerts_write_init(&w, context, venture_entity_get_organization_id(rule), record, FALSE);

		for (i = 0; i < sources->len; i++)
		{
			VentureEntity *source = g_ptr_array_index(sources, i);
			g_autoptr(VentureSeriesStore) reader = NULL;
			g_autoptr(GPtrArray) venues = NULL;
			g_autoptr(GPtrArray) entries = NULL;
			g_autoptr(GPtrArray) keys = NULL;
			g_autoptr(JsonObject) candidates = NULL;
			g_autoptr(GError) source_error = NULL;
			AlertsFrozen *frozen;
			AlertsSink sink;
			guint v;
			gboolean ok;

			frozen = alerts_freeze(context, source, rule);

			if (NULL == frozen)
			{
				json_array_add_string_element(notes, "the rule names nothing in a source");
				continue;
			}

			reader = venture_feeds_service_open_reader(service, venture_entity_get_id(source),
			                                           &source_error);

			if (NULL == reader)
			{
				alerts_frozen_free(frozen);

				if (g_error_matches(source_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
				{
					json_array_add_string_element(notes, source_error->message);
					continue;
				}

				alerts_write_clear(&w);
				g_propagate_error(error, g_steal_pointer(&source_error));
				return FALSE;
			}

			venues = venture_series_store_list_venues(reader, &source_error);
			keys = g_ptr_array_new();

			for (v = 0; (NULL != venues) && (v < venues->len); v++)
				g_ptr_array_add(keys, ((VentureSeriesVenueRow *)g_ptr_array_index(venues, v))->key);

			g_ptr_array_add(keys, NULL);

			if ((NULL != venues) && alerts_frozen_has_kind(frozen, VENTURE_ALERT_KIND_ENTRY_MATCH))
				entries = venture_series_store_list_new_entries(reader,
					now - VENTURE_ALERTS_ENTRY_WINDOW, VENTURE_SERIES_MAX_PAGE, &source_error);

			candidates = json_object_new();
			sink.candidates = candidates;
			sink.over = 0;
			sink.notes = g_ptr_array_new_with_free_func(g_free);
			ok = (NULL != venues) && (NULL == source_error) &&
			     alerts_evaluate(reader, frozen, (const gchar *const *)keys->pdata, entries,
			                     NULL, now, now, &sink, &source_error);

			if (frozen->listings_capped)
			{
				g_autofree gchar *note = g_strdup_printf("undercut compared only the newest %d open "
				                                         "listings", VENTURE_ALERTS_MAX_LISTINGS);

				json_array_add_string_element(notes, note);
			}

			alerts_frozen_free(frozen);

			for (v = 0; v < sink.notes->len; v++)
				json_array_add_string_element(notes, g_ptr_array_index(sink.notes, v));

			g_ptr_array_unref(sink.notes);

			if (!ok)
			{
				alerts_write_clear(&w);
				g_propagate_error(error, g_steal_pointer(&source_error));
				return FALSE;
			}

			if (sink.over > 0)
			{
				g_autofree gchar *note = g_strdup_printf("%u candidates past %d were not considered",
				                                         sink.over, VENTURE_ALERTS_MAX_CANDIDATES);

				json_array_add_string_element(notes, note);
			}

			alerts_write_set_source(&w, venture_entity_get_id(source));
			alerts_write_candidates(&w, candidates);
		}

		if (w.over_cap > 0)
		{
			g_autofree gchar *note = g_strdup_printf("%u hits not written: an evaluation writes "
			                                         "at most %d", w.over_cap,
			                                         VENTURE_ALERTS_MAX_HITS_PER_RUN);

			json_array_add_string_element(notes, note);
		}

		if (NULL != out_report)
		{
			builder = json_builder_new();
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "candidates");
			json_builder_add_int_value(builder, w.candidates);
			json_builder_set_member_name(builder, "written");
			json_builder_add_int_value(builder, w.written);
			json_builder_set_member_name(builder, "cooled");
			json_builder_add_int_value(builder, w.cooled);
			json_builder_set_member_name(builder, "over_cap");
			json_builder_add_int_value(builder, w.over_cap);
			json_builder_set_member_name(builder, "failed");
			json_builder_add_int_value(builder, w.failed);
			json_builder_set_member_name(builder, "sources");
			json_builder_add_int_value(builder, sources->len);
			json_builder_set_member_name(builder, "matches");
			json_builder_add_value(builder, json_node_init_array(json_node_alloc(), w.matches));
			json_builder_set_member_name(builder, "hits");
			json_builder_add_value(builder, json_node_init_array(json_node_alloc(), w.hits));
			json_builder_set_member_name(builder, "notes");
			json_builder_add_value(builder, json_node_init_array(json_node_alloc(), notes));
			json_builder_end_object(builder);
			*out_report = json_builder_get_root(builder);
		}

		alerts_write_clear(&w);

		return TRUE;
	}
#endif
}

/* --- The action ------------------------------------------------------------------ */

static gboolean
alerts_action_allowed(
	VentureAction		 *action,
	VentureEntity		 *entity,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureContext) context = NULL;

	(void)actor;

	if (venture_entity_is_deleted(entity))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
		                    "A deleted alert rule is not evaluated");
		return FALSE;
	}

	context = alerts_context_for(VENTURE_DATABASE(venture_action_get_data(action)));

	if (NULL == context)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Alert rules need a running server to be evaluated");
		return FALSE;
	}

	return TRUE;
}

/* The report as one line for the record's `result`. */
static gchar *
alerts_report_text(
	JsonNode	*report,
	gboolean	 record
){
	g_autoptr(GString) text = g_string_new(NULL);
	JsonObject *object;
	JsonArray *matches;
	JsonArray *notes;
	guint i;

	object = json_node_get_object(report);
	matches = json_object_get_array_member(object, "matches");
	notes = json_object_get_array_member(object, "notes");

	if (record)
		g_string_append_printf(text, "%" G_GINT64_FORMAT " matched: %" G_GINT64_FORMAT
		                       " written, %" G_GINT64_FORMAT " within the cooldown",
		                       json_object_get_int_member(object, "candidates"),
		                       json_object_get_int_member(object, "written"),
		                       json_object_get_int_member(object, "cooled"));
	else
		g_string_append_printf(text, "%" G_GINT64_FORMAT " would fire",
		                       json_object_get_int_member(object, "candidates"));

	for (i = 0; (i < json_array_get_length(matches)) && (i < 10); i++)
		g_string_append_printf(text, "%s%s", (0 == i) ? ": " : "; ",
		                       json_object_get_string_member(
		                       	json_array_get_object_element(matches, i), "message"));

	if (json_array_get_length(matches) > 10)
		g_string_append(text, "; ...");

	for (i = 0; i < json_array_get_length(notes); i++)
		g_string_append_printf(text, " (%s)", json_array_get_string_element(notes, i));

	return g_string_free(g_steal_pointer(&text), FALSE);
}

static VentureEntity *
alerts_action_evaluate(
	VentureAction		 *action,
	VentureEntity		 *entity,
	GHashTable		 *params,
	const VentureActor	 *actor,
	GError			**error
){
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(JsonNode) report = NULL;
	g_autofree gchar *text = NULL;
	JsonNode *record_node;
	gboolean record;

	(void)actor;

	context = alerts_context_for(VENTURE_DATABASE(venture_action_get_data(action)));

	if (NULL == context)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Alert rules need a running server to be evaluated");
		return NULL;
	}

	record_node = (NULL != params) ? g_hash_table_lookup(params, "record") : NULL;
	record = (NULL != record_node) && JSON_NODE_HOLDS_VALUE(record_node) &&
	         (G_TYPE_BOOLEAN == json_node_get_value_type(record_node)) &&
	         json_node_get_boolean(record_node);

	if (!venture_marketdata_alerts_evaluate(context, entity, record, &report, error))
		return NULL;

	text = alerts_report_text(report, record);
	g_object_set(entity, "result", text, NULL);

	return g_object_ref(entity);
}

static void
alerts_register_action(VentureDatabase *database)
{
	g_autoptr(GPtrArray) parameters = NULL;
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;
	VentureFieldSpec *field;

	parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	field = venture_field_spec_new("record", "Record hits", VENTURE_FIELD_KIND_BOOLEAN);
	field->help = g_strdup("Write what fires as alert hits, under the cooldown, as a feed run "
	                       "would; otherwise only say what would fire");
	g_ptr_array_add(parameters, field);

	/*
	 * A record action on the rule, judged in its organization. Not
	 * stageable: what it writes are hits, which are the system's, and a
	 * dry run writes nothing to approve.
	 */
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT,
	                      "type-name", "alert_rule", "name", "evaluate",
	                      "label", "Evaluate now",
	                      "description", "Hold the rule against each venue's newest snapshot now "
	                                     "and say what fires",
	                      "parameters", parameters, "stageable", FALSE,
	                      "roles", VENTURE_USER_ROLE_EDITOR, NULL);

	if (!venture_action_registry_register(venture_database_get_action_registry(database),
	                                      action, alerts_action_allowed, alerts_action_evaluate,
	                                      database, NULL, &error))
		g_error("Alert action registration: %s", error->message);
}

/* --- Installation ------------------------------------------------------------------ */

void
venture_marketdata_alerts_install(VentureContext *context)
{
	VentureDatabase *database;
	GWeakRef *ref;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The last context over a database is the one its action asks. */
	ref = g_new0(GWeakRef, 1);
	g_weak_ref_init(ref, context);
	g_object_set_data_full(G_OBJECT(database), ALERTS_CONTEXT_KEY, ref, alerts_weak_ref_free);

	/* Validators and actions are per database; the tests build several
	 * contexts over one. */
	if (NULL == g_object_get_data(G_OBJECT(database), ALERTS_INSTALLED_KEY))
	{
		g_object_set_data(G_OBJECT(database), ALERTS_INSTALLED_KEY, GINT_TO_POINTER(1));
		venture_database_add_save_validator(database, VENTURE_TYPE_ALERT_RULE,
		                                    alerts_validate_rule, NULL, NULL);
		venture_database_add_save_validator(database, VENTURE_TYPE_ALERT_HIT,
		                                    alerts_validate_hit, NULL, NULL);
		alerts_register_action(database);
	}

#ifdef VENTURE_HAVE_SQLITE
	venture_feeds_add_hook(context, VENTURE_ALERTS_HOOK, alerts_hook_freeze, alerts_hook_commit,
	                       alerts_hook_run, NULL, NULL);
	g_signal_connect_object(database, "entity-saved", G_CALLBACK(alerts_on_saved), context, 0);
	g_signal_connect_object(database, "entity-deleted", G_CALLBACK(alerts_on_deleted), context, 0);
#endif
}
