/*
 * venture-marketdata-records.c - Venues, instruments and watchlists
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

/* ==========================================================================
 * Venues
 *
 * A place things are priced and traded: one realm's auction house, a
 * bookmaker, an exchange, a supplier's price list, your own shop. The
 * series store keeps a venue as a key and a group (a realm and its
 * region); a venue *record* is that venue promoted into the books, with
 * what the store cannot know: where money moves through when you trade
 * there (a location's holding, else an account), what it charges and what
 * it costs to move goods to it.
 *
 * `namespace` and `key` name the store's row, and `external-ref` is the
 * two joined -- "eu-realm:3678" -- derived at the save and unique in the
 * organization, deleted rows included, so promoting the same venue twice
 * finds the first record instead of making a second. A venue typed in by
 * hand with no key has no reference and no uniqueness to keep.
 *
 * The fee model is a name stored as text for now: the registry that
 * gives the names meaning arrives with the arbitrage engine, and its
 * parameters are YAML the model reads.
 * ========================================================================== */

static const VentureFieldDecl venture_venue_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "Which venue: Argent Dawn auction house, Pinnacle, a supplier"),
	VENTURE_FIELD_ENUM("kind", "Kind",
	                   "marketplace, auction_house, bookmaker, exchange, supplier, store or other",
	                   venture_venue_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("namespace", "Namespace",
	              "Optional: tells this key from another source's same key -- eu-realm, bookmaker",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("key", "Key",
	              "Optional: the venue's key in its data source's store",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("group-key", "Group",
	              "Optional: the group it is compared within -- a region, a country",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("external-ref", "Reference",
	              "Namespace and key, joined; derived when saved and unique in the organization",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD_REF("data-source-id", "Data source",
	                  "Optional: the source whose store has this venue's prices",
	                  "data_source", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("location-id", "Location",
	                  "Optional: the location whose holding money moves through when you trade here",
	                  "location", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("account-id", "Account",
	                  "Optional: the account money moves through when the venue has no location",
	                  "account", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("currency", "Currency",
	              "Optional: what the venue prices in",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("fee-model", "Fee model",
	              "Optional: the name of what the venue charges -- a cut, a deposit, a commission",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("fee-params", "Fee parameters",
	              "Optional: the fee model's parameters, as YAML",
	              VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("transfer-cost", "Transfer cost",
	                    "Optional: what moving one lot to or from this venue costs"),
	VENTURE_FIELD("transfer-hours", "Transfer hours",
	              "Optional: how long moving goods or money here takes; 0 when nobody said",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureVenue, venture_venue, venture_venue_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Venue", NULL);)

/* ==========================================================================
 * Instruments
 *
 * A thing that is priced: an item, an outcome of an event, a share, a
 * supplier's SKU. Like a venue it may name its row in a data source's
 * store (`data-source-id` and `key`, joined with the namespace into the
 * unique `external-ref`), and it may name the `product` it is -- which is
 * what joins outside prices to recipes, stock and goals: the price oracle
 * answers for a product through the instruments that name it.
 *
 * `parent-id` nests instruments: an event over its outcomes. The tree is
 * held to the category rules -- no loop, one organization, bounded depth
 * -- by venture_category_check_tree_node(), the one definition of a loop.
 * ========================================================================== */

static const VentureFieldDecl venture_instrument_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What is priced: Copper Ore, Arsenal to win, SKU 4411"),
	VENTURE_FIELD_ENUM("kind", "Kind", "item, outcome, event, asset, sku or other",
	                   venture_instrument_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("namespace", "Namespace",
	              "Optional: tells this key from another source's same key -- wow-item, sku",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("key", "Key",
	              "Optional: the instrument's key in its data source's store",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("external-ref", "Reference",
	              "Namespace and key, joined; derived when saved and unique in the organization",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_TECHNICAL),
	VENTURE_FIELD_REF("data-source-id", "Data source",
	                  "Optional: the source whose store has this instrument's prices",
	                  "data_source", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("product-id", "Product",
	                  "Optional: the product it is, so recipes, stock and goals can be priced from it",
	                  "product", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("category-id", "Category", "Optional: where it is filed",
	                  "category", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("parent-id", "Part of",
	                  "Optional: what it belongs to -- an outcome's event",
	                  "instrument", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("attrs", "Attributes",
	              "Optional: what the source said about it, as YAML or JSON",
	              VENTURE_FIELD_KIND_TEXT, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureInstrument, venture_instrument, venture_instrument_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Instrument", NULL);)

/* ==========================================================================
 * Watchlists
 *
 * A named list of instruments somebody is keeping an eye on, optionally
 * in one venue group. Shared in the organization rather than owned by a
 * person: alerts read them, and a record with a personal owner is one the
 * webhooks and automation are kept quiet about.
 * ========================================================================== */

static const VentureFieldDecl venture_watchlist_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What the list is for: herbs to flip, Saturday's matches"),
	VENTURE_FIELD("group-key", "Group",
	              "Optional: the venue group its prices are read in -- a region",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("venture-id", "Venture", "Optional: the venture it is for",
	                  "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureWatchlist, venture_watchlist, venture_watchlist_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Watchlist", NULL);)

/* ==========================================================================
 * Venue groups
 *
 * A named set of venues to look at together -- the realms a person keeps
 * characters and banks on, say -- picked on the browse, deals and
 * instrument pages in place of one venue or a whole region. Entries are
 * venue keys or venue names, and a name also matches one part of a venue
 * named by several ("Thrall" in "Thrall, Area 52"), so a group can be
 * written the way a person says it. Like a watchlist it is shared in the
 * organization rather than owned.
 * ========================================================================== */

static const VentureFieldDecl venture_venue_group_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What the venues have in common: bank realms, flip targets"),
	VENTURE_FIELD("venues", "Venues",
	              "Venue keys or names, comma separated: 3676, Thrall, Moon Guard",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureVenueGroup, venture_venue_group, venture_venue_group_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Venue group", NULL);)

/* ==========================================================================
 * Ignored accounts
 *
 * An account, or every account on one realm, the organization does not
 * play: the characters a person keeps on a realm they never visit. An
 * ignored account drops out of the Accounts pages (behind a "show
 * ignored" switch), their totals, and "My characters". It is matched by
 * its key -- a character's account key, or a realm's name or venue key,
 * ignoring case -- so it outlives the store it was set from, and like a
 * venue group it is shared in the organization rather than owned.
 * ========================================================================== */

static const VentureFieldDecl venture_account_ignore_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What is ignored, as it is shown: a character, a realm"),
	VENTURE_FIELD("kind", "Kind", "character (one account) or realm (every account on it)",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("key", "Key", "The account's key, or the realm's name or venue key",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureAccountIgnore, venture_account_ignore, venture_account_ignore_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Ignored account", NULL);)

/* ==========================================================================
 * Watchlist entries
 *
 * One instrument on a watchlist, with the prices that would make it worth
 * acting on: buy at or under one, sell at or over the other. Either may be
 * left empty; both are in whatever currency the venue prices in, and the
 * validator holds them to one currency and to nothing negative.
 * ========================================================================== */

static const VentureFieldDecl venture_watchlist_entry_fields[] = {
	VENTURE_FIELD_REF("watchlist-id", "Watchlist", "The list it is on",
	                  "watchlist", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_REF("instrument-id", "Instrument", "What is watched",
	                  "instrument", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_MONEY("target-buy", "Buy at",
	                    "Optional: worth buying at or below this"),
	VENTURE_FIELD_MONEY("target-sell", "Sell at",
	                    "Optional: worth selling at or above this"),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureWatchlistEntry, venture_watchlist_entry,
	venture_watchlist_entry_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Watchlist entry", NULL);)

/* ==========================================================================
 * Alert rules
 *
 * What somebody wants to be told about the market data: a price at or
 * under a line, a venue running out, a listing of theirs undercut, a news
 * entry naming something. A rule watches a scope -- a watchlist's
 * instruments, one instrument, or a category of them -- optionally at one
 * venue or within one venue group, and one kind of thing; the fields a
 * kind reads are held to it by the save validator in
 * venture-marketdata-alerts.c, and the ones it does not read must be
 * empty, so a threshold nobody's rule looks at is never left looking
 * meaningful.
 *
 * Shared in the organization, with no personal owner: a record with one is
 * kept from webhooks and automation, and a hit is exactly what those are
 * for. Whose inbox a hit reaches is `notify-username`, or, left empty,
 * whoever created the rule (read back from the audit log; an API token
 * stands for its owner).
 *
 * `enabled` starts TRUE on a new object, as a data source's does; the
 * column's zero value, FALSE, is the safe reading of a row nobody wrote it
 * on. `cooldown-minutes` starts at 60 the same way.
 *
 * Category references are judged as an instrument's would be (the
 * qdata below): a rule names the category its instruments are filed
 * under, which is a tree that applies to instruments, not to alert rules.
 * ========================================================================== */

static const VentureFieldDecl venture_alert_rule_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What it watches for: cheap copper ore, my listings undercut"),
	VENTURE_FIELD_ENUM("kind", "Kind",
	                   "below, above, pct_vs_reference, spread, out_of_stock, back_in_stock, "
	                   "shortage, spike, undercut, entry_match, position_expiring, "
	                   "inbound_expiring, account_stale, collect_ready or deal",
	                   venture_alert_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("enabled", "Enabled",
	              "Evaluated after every feed run; switched off, it keeps its hits and fires no more",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("watchlist-id", "Watchlist",
	                  "The instruments on this watchlist (its group too, when the rule names none)",
	                  "watchlist", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("instrument-id", "Instrument", "This one instrument",
	                  "instrument", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("category-id", "Category",
	                  "Every instrument filed under this category or beneath it",
	                  "category", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("data-source-id", "Data source",
	                  "Optional: only this source's data; empty for every source of the organization",
	                  "data_source", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("venue-id", "Venue",
	                  "Optional: only at this venue (an account kind: the position's venue, else the account's)",
	                  "venue", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("group-key", "Group",
	              "Optional: only at venues in this group -- a region; an account kind: accounts in this group",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("threshold", "Threshold price",
	                    "below and above: the line the cheapest unit crosses; spread: the least gap worth hearing about; "
	                    "deal: the least profit after the cut (optional with a threshold number)"),
	VENTURE_FIELD("threshold-number", "Threshold number",
	              "pct_vs_reference: a percent of the reference (80 is 20% under it); shortage: units; "
	              "spike: a percent change, negative for a drop; position_expiring and "
	              "inbound_expiring: hours ahead, up to 720; account_stale: days unseen, up to 365; "
	              "deal: the least return, a percent of the buy price (optional with a threshold price)",
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("pattern", "Pattern",
	              "entry_match: text an entry's title or summary contains, ignoring case",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("basis", "Basis",
	                   "pct_vs_reference and spread: the reference price; spike: min, market or quantity",
	                   venture_marketdata_basis_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("window-hours", "Window hours",
	              "spike: compare with the hourly figure this many hours earlier, 1 to 336",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("cooldown-minutes", "Cooldown minutes",
	              "Quiet time after a hit, per instrument and venue; 0 tells you about every snapshot",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("notify-username", "Tell",
	              "Optional: whose inbox a hit reaches; empty is whoever created the rule",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL),
	VENTURE_FIELD("result", "Result", "What the last evaluation said; shown once, never stored",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_TRANSIENT)
};

static void venture_alert_rule_constructed(GObject *object);

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureAlertRule, venture_alert_rule, venture_alert_rule_fields,
	G_OBJECT_CLASS(klass)->constructed = venture_alert_rule_constructed;
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Alert rule", NULL);
	g_type_set_qdata(G_TYPE_FROM_CLASS(klass),
		g_quark_from_static_string(VENTURE_CATEGORY_APPLIES_AS_QDATA), (gpointer)"instrument");)

/* On, and an hour's quiet between hits about the same thing: what
 * somebody adding a rule means. A row read back overwrites both. */
static void
venture_alert_rule_constructed(GObject *object)
{
	if (NULL != G_OBJECT_CLASS(venture_alert_rule_parent_class)->constructed)
		G_OBJECT_CLASS(venture_alert_rule_parent_class)->constructed(object);

	g_object_set(object, "enabled", TRUE, "cooldown-minutes", (gint64)60, NULL);
}

/* ==========================================================================
 * Alert hits
 *
 * One time a rule fired: what was seen, against what, where and when. Written
 * once, by the system, on the main thread after a feed run (or an explicit
 * evaluation), and never edited -- it is evidence, like a data source run,
 * and the generic write routes refuse it. Its creation is the event:
 * webhooks publish `alert_hit.created` and automation hears `on_created`
 * for it, with no separate alert event to keep in step.
 *
 * Prices are money in the store's currency; percents and quantities are
 * the numbers. An instrument or venue the store saw but nobody promoted
 * has its key and no record. `subject` is what the cooldown is kept per.
 * ========================================================================== */

static const VentureFieldDecl venture_alert_hit_fields[] = {
	VENTURE_FIELD_REF("rule-id", "Rule", "The rule that fired",
	                  "alert_rule", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_ENUM("kind", "Kind", "What the rule watched for when it fired",
	                   venture_alert_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("observed-at", "Observed", "When the data was seen: the snapshot or the entry's arrival",
	              VENTURE_FIELD_KIND_DATETIME,
	              VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("message", "Message", "What happened, in a sentence"),
	VENTURE_FIELD_REF("data-source-id", "Data source", "Whose store it was seen in",
	                  "data_source", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("instrument-id", "Instrument", "The instrument, when it has a record",
	                  "instrument", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("instrument-key", "Instrument key", "The instrument's key in the store",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("venue-id", "Venue", "The venue, when it has a record",
	                  "venue", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("venue-key", "Venue key", "The venue's key in the store",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("account-key", "Account key",
	              "The operator's account in the store, for a kind about one: position_expiring, "
	              "inbound_expiring, account_stale, collect_ready",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("listing-id", "Listing",
	                  "undercut: the listing that was undercut; position_expiring: the position's listing",
	                  "listing", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("entry-key", "Entry", "entry_match: the entry's key in the store",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("url", "Link", "entry_match: the entry's link",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("observed", "Observed price", "The price seen, for a kind that reads one"),
	VENTURE_FIELD_MONEY("reference", "Reference price",
	                    "What it was held against: the threshold, the reference, the listing, the earlier price"),
	VENTURE_FIELD("observed-number", "Observed number",
	              "The percent, change or quantity seen, for a kind that reads one",
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("reference-number", "Reference number",
	              "The rule's threshold number it was held against",
	              VENTURE_FIELD_KIND_DOUBLE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("subject", "Subject",
	              "What the cooldown is kept per: the venue, the instrument, and the listing or entry; "
	              "or the position, inbound row or account",
	              VENTURE_FIELD_KIND_STRING,
	              VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_TECHNICAL)
};

/*
 * The hit's own sentence, cut to a line: it is the label a webhook body
 * and an automation's {event->label} carry, so a rule forwarding hits to a
 * phone can say what happened without reading the record back. "Alert #12"
 * for a hit with no message.
 */
static gchar *
venture_alert_hit_display_name(VentureEntity *self)
{
	g_autofree gchar *message = NULL;

	g_object_get(self, "message", &message, NULL);

	if ((NULL == message) || ('\0' == message[0]) || !g_utf8_validate(message, -1, NULL))
		return g_strdup_printf("Alert #%" G_GINT64_FORMAT, venture_entity_get_id(self));

	if (g_utf8_strlen(message, -1) > 120)
	{
		g_autofree gchar *cut = g_utf8_substring(message, 0, 119);

		return g_strconcat(cut, "\xe2\x80\xa6", NULL);
	}

	return g_steal_pointer(&message);
}

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureAlertHit, venture_alert_hit, venture_alert_hit_fields,
	VENTURE_ENTITY_CLASS(klass)->get_display_name = venture_alert_hit_display_name;
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Alert hit", NULL);)
