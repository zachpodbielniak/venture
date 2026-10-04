/*
 * venture-marketdata.c - The marketdata module's rules, promotion and grammar
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

#define VENTURE_MARKETDATA_STATE_KEY "venture-marketdata-installed"

/* ==========================================================================
 * Shared
 * ========================================================================== */

gboolean
venture_marketdata_basis_is_number(VentureMarketdataBasis basis)
{
	return (VENTURE_MARKETDATA_BASIS_SALE_RATE == basis) ||
	       (VENTURE_MARKETDATA_BASIS_SOLD_PER_DAY == basis) ||
	       (VENTURE_MARKETDATA_BASIS_QUANTITY == basis);
}

gchar *
venture_marketdata_external_ref(
	const gchar	*namespace_,
	const gchar	*key
){
	if (venture_string_is_empty(key))
		return NULL;

	return g_strdup_printf("%s:%s", (NULL != namespace_) ? namespace_ : "", key);
}

/* An integer property's value, 0 on a missing @entity. */
static gint64
marketdata_int(
	VentureEntity	*entity,
	const gchar	*property
){
	gint64 value;

	value = 0;

	if (NULL != entity)
		g_object_get(entity, property, &value, NULL);

	return value;
}

/*
 * A namespace, key or group: at most the store's key length, valid UTF-8
 * and no control characters. These end up in a store lookup, a page and
 * an export; a newline in one is never what somebody meant.
 */
static gboolean
marketdata_check_text(
	const gchar	 *value,
	const gchar	 *label,
	GError		**error
){
	const gchar *p;

	if (venture_string_is_empty(value))
		return TRUE;

	if ((strlen(value) > VENTURE_MARKETDATA_MAX_KEY_LENGTH) || !g_utf8_validate(value, -1, NULL))
	{
		venture_set_error_validation(error, label,
			"must be valid text of at most %d bytes", VENTURE_MARKETDATA_MAX_KEY_LENGTH);
		return FALSE;
	}

	for (p = value; '\0' != *p; p++)
	{
		if (g_ascii_iscntrl(*p))
		{
			venture_set_error_validation(error, label, "cannot contain control characters");
			return FALSE;
		}
	}

	return TRUE;
}

/*
 * Refuses a reference being written to a record in another organization:
 * the generic check only asks whether the target exists for the writer,
 * and somebody in two organizations can see both. Only a value being
 * written is judged, so a row pointing somewhere since moved stays
 * editable.
 */
static gboolean
marketdata_same_organization(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	const gchar	 *property,
	GType		  target_type,
	const gchar	 *label,
	GError		**error
){
	g_autoptr(VentureEntity) target = NULL;
	gint64 target_id;

	target_id = marketdata_int(entity, property);

	if (target_id <= 0)
		return TRUE;

	if ((NULL != previous) && (marketdata_int(previous, property) == target_id))
		return TRUE;

	target = venture_database_get(database, target_type, target_id, NULL);

	if ((NULL != target) &&
	    (venture_entity_get_organization_id(target) != venture_entity_get_organization_id(entity)))
	{
		venture_set_error_validation(error, label,
			"#%" G_GINT64_FORMAT " belongs to another organization", target_id);
		return FALSE;
	}

	return TRUE;
}

VentureEntity *
venture_marketdata_find_by_ref(
	VentureDatabase	 *database,
	GType		  type,
	gint64		  organization_id,
	const gchar	 *external_ref,
	GError		**error
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;

	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);

	if (venture_string_is_empty(external_ref))
		return NULL;

	query = venture_query_new(type);
	venture_query_set_organization(query, organization_id);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 1);

	if (!venture_query_add_filter_string(query, "external-ref", VENTURE_FILTER_OP_EQ,
	                                     external_ref, error))
		return NULL;

	rows = venture_database_find(database, query, error);

	if ((NULL == rows) || (0 == rows->len))
		return NULL;

	return g_object_ref(g_ptr_array_index(rows, 0));
}

/*
 * Derives `external-ref` from the namespace and the key, and refuses one
 * another record of the organization already carries -- deleted ones
 * included, as the unique index counts them -- with a message that says
 * which, rather than the index's bare constraint failure.
 */
static gboolean
marketdata_derive_ref(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	GType		  type,
	const gchar	 *noun,
	GError		**error
){
	g_autofree gchar *namespace_ = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *ref = NULL;
	g_autofree gchar *was = NULL;
	g_autoptr(VentureEntity) other = NULL;
	g_autoptr(GError) lookup_error = NULL;

	g_object_get(entity, "namespace", &namespace_, "key", &key, NULL);

	if (!marketdata_check_text(namespace_, "Namespace", error) ||
	    !marketdata_check_text(key, "Key", error))
		return FALSE;

	if (!venture_string_is_empty(namespace_) && (NULL != strchr(namespace_, ':')))
	{
		/* A colon would make "a:b" + "c" and "a" + "b:c" one reference. */
		venture_set_error_validation(error, "Namespace", "cannot contain a colon");
		return FALSE;
	}

	ref = venture_marketdata_external_ref(namespace_, key);
	g_object_set(entity, "external-ref", ref, NULL);

	if (NULL == ref)
		return TRUE;

	if (NULL != previous)
		g_object_get(previous, "external-ref", &was, NULL);

	if ((NULL != previous) && (0 == g_strcmp0(was, ref)))
		return TRUE;

	other = venture_marketdata_find_by_ref(database, type,
	                                      venture_entity_get_organization_id(entity), ref,
	                                      &lookup_error);

	if (NULL != lookup_error)
	{
		g_propagate_error(error, g_steal_pointer(&lookup_error));
		return FALSE;
	}

	if ((NULL != other) && (venture_entity_get_id(other) != venture_entity_get_id(entity)))
	{
		venture_set_error_validation(error, "Key",
			"%s #%" G_GINT64_FORMAT " already has the reference %s%s", noun,
			venture_entity_get_id(other), ref,
			venture_entity_is_deleted(other) ? " (it is deleted; restore it instead)" : "");
		return FALSE;
	}

	return TRUE;
}

/* An optional amount that may not be negative. */
static gboolean
marketdata_check_amount(
	VentureEntity	 *entity,
	const gchar	 *property,
	const gchar	 *label,
	GError		**error
){
	g_autoptr(VentureMoney) amount = NULL;

	g_object_get(entity, property, &amount, NULL);

	if ((NULL != amount) && venture_money_is_negative(amount))
	{
		venture_set_error_validation(error, label, "cannot be negative");
		return FALSE;
	}

	return TRUE;
}

/* ==========================================================================
 * Venues
 * ========================================================================== */

/*
 * A venue's rules: a sane namespace, key and group; the derived reference,
 * unique in the organization; a currency that is a currency code, stored
 * in capitals as every currency is; references in its own organization;
 * and a transfer that costs nothing negative and takes no negative time.
 * The fee model is only a name until the registry that gives names meaning
 * exists (the arbitrage engine), so it is not checked against one here.
 */
static gboolean
marketdata_validate_venue(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autofree gchar *group_key = NULL;
	g_autofree gchar *currency = NULL;
	g_autofree gchar *fee_model = NULL;
	gint64 hours;

	(void)user_data;

	g_object_get(entity, "group-key", &group_key, "currency", &currency,
	             "fee-model", &fee_model, "transfer-hours", &hours, NULL);

	if (!marketdata_check_text(group_key, "Group", error) ||
	    !marketdata_check_text(fee_model, "Fee model", error) ||
	    !marketdata_derive_ref(database, entity, previous, VENTURE_TYPE_VENUE, "Venue", error))
		return FALSE;

	if (!venture_string_is_empty(currency))
	{
		g_autofree gchar *code = g_ascii_strup(g_strstrip(currency), -1);

		if (!venture_currency_is_valid(code))
		{
			venture_set_error_validation(error, "Currency", "\"%s\" is not a currency code", currency);
			return FALSE;
		}

		g_object_set(entity, "currency", code, NULL);
	}

	if (!marketdata_check_amount(entity, "transfer-cost", "Transfer cost", error))
		return FALSE;

	if (hours < 0)
	{
		venture_set_error_validation(error, "Transfer hours", "cannot be negative; leave it 0 when nobody said");
		return FALSE;
	}

	return marketdata_same_organization(database, entity, previous, "data-source-id",
	                                    VENTURE_TYPE_DATA_SOURCE, "Data source", error) &&
	       marketdata_same_organization(database, entity, previous, "location-id",
	                                    VENTURE_TYPE_LOCATION, "Location", error) &&
	       marketdata_same_organization(database, entity, previous, "account-id",
	                                    VENTURE_TYPE_ACCOUNT, "Account", error);
}

/* ==========================================================================
 * Instruments
 * ========================================================================== */

/*
 * An instrument's rules: the derived, unique reference; references in its
 * own organization; and a parent that is an instrument of the same
 * organization and not the instrument itself or one beneath it --
 * venture_category_check_tree_node(), the one definition of a loop. That
 * its category groups instruments is the category validator's to say.
 */
static gboolean
marketdata_validate_instrument(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	(void)user_data;

	if (!marketdata_derive_ref(database, entity, previous, VENTURE_TYPE_INSTRUMENT,
	                           "Instrument", error))
		return FALSE;

	if (!marketdata_same_organization(database, entity, previous, "data-source-id",
	                                  VENTURE_TYPE_DATA_SOURCE, "Data source", error) ||
	    !marketdata_same_organization(database, entity, previous, "product-id",
	                                  VENTURE_TYPE_PRODUCT, "Product", error))
		return FALSE;

	return venture_category_check_tree_node(database, entity, error);
}

/* ==========================================================================
 * Watchlists and their entries
 * ========================================================================== */

static gboolean
marketdata_validate_watchlist(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autofree gchar *group_key = NULL;

	(void)user_data;

	g_object_get(entity, "group-key", &group_key, NULL);

	return marketdata_check_text(group_key, "Group", error) &&
	       marketdata_same_organization(database, entity, previous, "venture-id",
	                                    VENTURE_TYPE_VENTURE, "Venture", error);
}

/*
 * An entry names a watchlist and an instrument of its own organization,
 * once per list -- a second entry for the same instrument would be two
 * targets nobody could tell apart -- and targets that are not negative
 * and, when both are given, in one currency: a buy price in gold and a
 * sell price in tickets is not a spread anybody can act on.
 */
static gboolean
marketdata_validate_watchlist_entry(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(VentureMoney) buy = NULL;
	g_autoptr(VentureMoney) sell = NULL;
	gint64 watchlist_id;
	gint64 instrument_id;

	(void)user_data;

	watchlist_id = marketdata_int(entity, "watchlist-id");
	instrument_id = marketdata_int(entity, "instrument-id");

	/* NOT_NULL covers strings and times only; a reference reads 0. */
	if (watchlist_id <= 0)
	{
		venture_set_error_validation(error, "Watchlist", "is required");
		return FALSE;
	}

	if (instrument_id <= 0)
	{
		venture_set_error_validation(error, "Instrument", "is required");
		return FALSE;
	}

	if (!marketdata_same_organization(database, entity, previous, "watchlist-id",
	                                  VENTURE_TYPE_WATCHLIST, "Watchlist", error) ||
	    !marketdata_same_organization(database, entity, previous, "instrument-id",
	                                  VENTURE_TYPE_INSTRUMENT, "Instrument", error) ||
	    !marketdata_check_amount(entity, "target-buy", "Buy at", error) ||
	    !marketdata_check_amount(entity, "target-sell", "Sell at", error))
		return FALSE;

	g_object_get(entity, "target-buy", &buy, "target-sell", &sell, NULL);

	if ((NULL != buy) && (NULL != sell) &&
	    (0 != g_strcmp0(venture_money_get_currency(buy), venture_money_get_currency(sell))))
	{
		venture_set_error_validation(error, "Sell at",
			"is in %s but the buy price is in %s; an entry's targets share one currency",
			venture_money_get_currency(sell), venture_money_get_currency(buy));
		return FALSE;
	}

	/* Once per list, judged when either half is written. */
	if ((NULL == previous) ||
	    (marketdata_int(previous, "watchlist-id") != watchlist_id) ||
	    (marketdata_int(previous, "instrument-id") != instrument_id))
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) rows = NULL;
		guint i;

		query = venture_query_new(VENTURE_TYPE_WATCHLIST_ENTRY);
		venture_query_set_limit(query, 2);

		if (!venture_query_add_filter_int(query, "watchlist-id", VENTURE_FILTER_OP_EQ,
		                                  watchlist_id, error) ||
		    !venture_query_add_filter_int(query, "instrument-id", VENTURE_FILTER_OP_EQ,
		                                  instrument_id, error))
			return FALSE;

		rows = venture_database_find(database, query, error);

		if (NULL == rows)
			return FALSE;

		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *other = g_ptr_array_index(rows, i);

			if (venture_entity_get_id(other) != venture_entity_get_id(entity))
			{
				venture_set_error_validation(error, "Instrument",
					"is already on this watchlist (entry #%" G_GINT64_FORMAT ")",
					venture_entity_get_id(other));
				return FALSE;
			}
		}
	}

	return TRUE;
}

void
venture_marketdata_install(VentureContext *context)
{
	VentureDatabase *database;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));

	database = venture_context_get_database(context);

	/* The tests build several contexts over one database; validators are
	 * per database, so the second one must add nothing. */
	if (NULL != g_object_get_data(G_OBJECT(database), VENTURE_MARKETDATA_STATE_KEY))
		return;

	g_object_set_data(G_OBJECT(database), VENTURE_MARKETDATA_STATE_KEY, GINT_TO_POINTER(1));

	venture_database_add_save_validator(database, VENTURE_TYPE_VENUE,
	                                    marketdata_validate_venue, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_INSTRUMENT,
	                                    marketdata_validate_instrument, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_WATCHLIST,
	                                    marketdata_validate_watchlist, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_WATCHLIST_ENTRY,
	                                    marketdata_validate_watchlist_entry, NULL, NULL);
}

/* ==========================================================================
 * Known instruments, for a source that tracks only those
 * ========================================================================== */

GPtrArray *
venture_marketdata_known_keys(
	VentureContext	*context,
	VentureEntity	*data_source
){
	g_autoptr(VentureAccessScope) internal = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	GPtrArray *keys;
	VentureDatabase *database;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(data_source), NULL);

	keys = g_ptr_array_new_with_free_func(g_free);

	if (!venture_context_module_enabled(context, "marketdata"))
		return keys;

	database = venture_context_get_database(context);
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	query = venture_query_new(VENTURE_TYPE_INSTRUMENT);
	venture_query_set_organization(query, venture_entity_get_organization_id(data_source));
	venture_query_set_limit(query, 0);

	if (!venture_query_add_filter_int(query, "data-source-id", VENTURE_FILTER_OP_EQ,
	                                  venture_entity_get_id(data_source), &error))
	{
		g_message("marketdata: could not ask for known instruments: %s", error->message);
		return keys;
	}

	rows = venture_database_find(database, query, &error);

	if (NULL == rows)
	{
		/* Read by the feeds freeze, which must not fail a source over
		 * it: the settings' own list still applies. */
		g_message("marketdata: could not read known instruments: %s", error->message);
		return keys;
	}

	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *key = NULL;

		g_object_get(g_ptr_array_index(rows, i), "key", &key, NULL);

		if (!venture_string_is_empty(key))
			g_ptr_array_add(keys, g_steal_pointer(&key));
	}

	return keys;
}

/* ==========================================================================
 * Promotion
 * ========================================================================== */

#ifdef VENTURE_HAVE_SQLITE

/* The data source, which must be the organization's and not deleted. */
static VentureEntity *
marketdata_source(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  data_source_id,
	GError		**error
){
	g_autoptr(VentureEntity) source = NULL;

	source = venture_database_get(database, VENTURE_TYPE_DATA_SOURCE, data_source_id, NULL);

	if ((NULL == source) || venture_entity_is_deleted(source) ||
	    (venture_entity_get_organization_id(source) != organization_id))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No data source #%" G_GINT64_FORMAT " in this organization", data_source_id);
		return NULL;
	}

	return g_steal_pointer(&source);
}

/* A read handle on the source's store, or an error that says why not. */
static VentureSeriesStore *
marketdata_reader(
	VentureContext	 *context,
	gint64		  data_source_id,
	GError		**error
){
	VentureFeedsService *service;
	g_autoptr(GError) local_error = NULL;
	VentureSeriesStore *store;

	service = venture_context_get_feeds_service(context);

	if (NULL == service)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "Promotion reads a data source's store, and the feeds module is off");
		return NULL;
	}

	store = venture_feeds_service_open_reader(service, data_source_id, &local_error);

	if (NULL == store)
	{
		if (g_error_matches(local_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Data source #%" G_GINT64_FORMAT " has stored nothing yet", data_source_id);
			return NULL;
		}

		g_propagate_error(error, g_steal_pointer(&local_error));
		return NULL;
	}

	return store;
}

/* The store's free-text kind as a nick: "Auction House" is auction_house,
 * and anything not a kind is other rather than a refusal. */
static gint
marketdata_kind(
	GType		 enum_type,
	const gchar	*text
){
	g_autofree gchar *nick = NULL;
	gchar *p;
	gint value;

	if (venture_string_is_empty(text))
		return 0;

	nick = g_ascii_strdown(text, -1);

	for (p = nick; '\0' != *p; p++)
	{
		if ((' ' == *p) || ('-' == *p))
			*p = '_';
	}

	if (!venture_enum_from_nick(enum_type, nick, &value))
		return 0;

	return value;
}

/*
 * The category whose computed path is the store's "/"-separated one, among
 * the organization's live categories that may group instruments; 0 when
 * there is none. Categories are never made here: a tree is something a
 * person lays out, and a store's taxonomy (a game's item classes) would
 * otherwise grow one branch per distinct string a provider ever sent.
 */
static gint64
marketdata_category_for_path(
	VentureDatabase	*database,
	gint64		 organization_id,
	const gchar	*path
){
	g_auto(GStrv) parts = NULL;
	g_autoptr(GString) wanted = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	const gchar *last;
	guint i;

	if (venture_string_is_empty(path))
		return 0;

	parts = g_strsplit(path, "/", -1);
	wanted = g_string_new(NULL);
	last = NULL;

	for (i = 0; NULL != parts[i]; i++)
	{
		g_strstrip(parts[i]);

		if ('\0' == parts[i][0])
			continue;

		if (wanted->len > 0)
			g_string_append(wanted, VENTURE_CATEGORY_PATH_SEPARATOR);

		g_string_append(wanted, parts[i]);
		last = parts[i];
	}

	if (NULL == last)
		return 0;

	query = venture_query_new(VENTURE_TYPE_CATEGORY);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 50);

	if (!venture_query_add_filter_string(query, "name", VENTURE_FILTER_OP_EQ, last, NULL) ||
	    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL))
		return 0;

	rows = venture_database_find(database, query, NULL);

	for (i = 0; (NULL != rows) && (i < rows->len); i++)
	{
		gint64 id = venture_entity_get_id(g_ptr_array_index(rows, i));
		g_autofree gchar *found = venture_category_path(database, VENTURE_TYPE_CATEGORY, id, NULL);

		if ((0 == g_strcmp0(found, wanted->str)) &&
		    venture_category_check_applies_to(database, id, "instrument", NULL))
			return id;
	}

	return 0;
}

/*
 * The record with @ref, brought back if deleted, re-read so the caller
 * holds the version the restore wrote. *@out is NULL when there is none.
 */
static gboolean
marketdata_existing(
	VentureDatabase		 *database,
	GType			  type,
	gint64			  organization_id,
	const gchar		 *ref,
	const VentureActor	 *actor,
	VentureEntity		**out,
	GError			**error
){
	g_autoptr(VentureEntity) existing = NULL;
	g_autoptr(GError) lookup_error = NULL;

	*out = NULL;
	existing = venture_marketdata_find_by_ref(database, type, organization_id, ref, &lookup_error);

	if (NULL != lookup_error)
	{
		g_propagate_error(error, g_steal_pointer(&lookup_error));
		return FALSE;
	}

	if (NULL == existing)
		return TRUE;

	if (venture_entity_is_deleted(existing))
	{
		gint64 id = venture_entity_get_id(existing);

		if (!venture_database_restore(database, existing, actor, error))
			return FALSE;

		g_clear_object(&existing);
		existing = venture_database_get(database, type, id, error);

		if (NULL == existing)
			return FALSE;
	}

	*out = g_steal_pointer(&existing);

	return TRUE;
}

static gboolean
marketdata_promote_instrument_at(
	VentureContext		 *context,
	VentureSeriesStore	 *store,
	VentureEntity		 *source,
	const gchar		 *key,
	const VentureActor	 *actor,
	guint			  depth,
	VentureEntity		**out,
	GError			**error
){
	g_autoptr(VentureSeriesInstrumentRow) row = NULL;
	g_autoptr(VentureEntity) existing = NULL;
	g_autoptr(VentureInstrument) instrument = NULL;
	g_autofree gchar *source_namespace = NULL;
	g_autofree gchar *ref = NULL;
	VentureDatabase *database;
	const gchar *namespace_;
	gint64 organization_id;
	gint64 parent_id;

	database = venture_context_get_database(context);
	organization_id = venture_entity_get_organization_id(source);

	if (!venture_series_store_get_instrument(store, key, &row, error))
		return FALSE;

	if (NULL == row)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "Data source #%" G_GINT64_FORMAT " has not seen an instrument \"%s\"",
		            venture_entity_get_id(source), key);
		return FALSE;
	}

	g_object_get(source, "instrument-namespace", &source_namespace, NULL);
	namespace_ = !venture_string_is_empty(row->namespace_) ? row->namespace_ : source_namespace;
	ref = venture_marketdata_external_ref(namespace_, key);

	if (!marketdata_existing(database, VENTURE_TYPE_INSTRUMENT, organization_id, ref, actor,
	                         &existing, error))
		return FALSE;

	if (NULL != existing)
	{
		*out = g_steal_pointer(&existing);
		return TRUE;
	}

	/* The parent first, so the child can name it. A parent the store
	 * does not know is left out rather than failing the child; the depth
	 * bound ends a loop the store's data might contain, where each level
	 * would otherwise promote the other forever. */
	parent_id = 0;

	if (!venture_string_is_empty(row->parent_key) && (0 != g_strcmp0(row->parent_key, key)) &&
	    (depth + 1 < VENTURE_CATEGORY_MAX_DEPTH))
	{
		g_autoptr(VentureEntity) parent = NULL;
		g_autoptr(GError) parent_error = NULL;

		if (marketdata_promote_instrument_at(context, store, source, row->parent_key, actor,
		                                     depth + 1, &parent, &parent_error))
			parent_id = venture_entity_get_id(parent);
		else if (!g_error_matches(parent_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
		{
			g_propagate_error(error, g_steal_pointer(&parent_error));
			return FALSE;
		}
	}

	instrument = venture_instrument_new();
	venture_entity_set_organization_id(VENTURE_ENTITY(instrument), organization_id);
	g_object_set(instrument,
	             "name", !venture_string_is_empty(row->name) ? row->name : key,
	             "kind", marketdata_kind(VENTURE_TYPE_INSTRUMENT_KIND, row->kind),
	             "namespace", namespace_, "key", key,
	             "data-source-id", venture_entity_get_id(source),
	             "category-id", marketdata_category_for_path(database, organization_id, row->category),
	             "parent-id", parent_id,
	             "attrs", (!venture_string_is_empty(row->attrs_json) &&
	                       (0 != g_strcmp0(row->attrs_json, "{}"))) ? row->attrs_json : NULL,
	             NULL);

	if (!venture_database_save(database, VENTURE_ENTITY(instrument), actor, error))
		return FALSE;

	*out = VENTURE_ENTITY(g_steal_pointer(&instrument));

	return TRUE;
}

#endif /* VENTURE_HAVE_SQLITE */

/* What promotion needs before it reads anything. */
static gboolean
marketdata_can_promote(
	VentureContext	 *context,
	const gchar	 *key,
	GError		**error
){
	if (!venture_context_module_enabled(context, "marketdata"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
		                    "The marketdata module is off");
		return FALSE;
	}

	if (venture_string_is_empty(key))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Promotion needs the key the store knows it by");
		return FALSE;
	}

#ifndef VENTURE_HAVE_SQLITE
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
	                    "Promotion reads a series store, which this build does not have");
	return FALSE;
#else
	return TRUE;
#endif
}

gboolean
venture_marketdata_promote_instrument(
	VentureContext		 *context,
	gint64			  organization_id,
	gint64			  data_source_id,
	const gchar		 *key,
	const VentureActor	 *actor,
	VentureEntity		**out_instrument,
	GError			**error
){
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	if (NULL != out_instrument)
		*out_instrument = NULL;

	if (!marketdata_can_promote(context, key, error))
		return FALSE;

#ifdef VENTURE_HAVE_SQLITE
	{
		g_autoptr(VentureEntity) source = NULL;
		g_autoptr(VentureSeriesStore) store = NULL;
		g_autoptr(VentureEntity) instrument = NULL;

		source = marketdata_source(venture_context_get_database(context), organization_id,
		                           data_source_id, error);

		if (NULL == source)
			return FALSE;

		store = marketdata_reader(context, data_source_id, error);

		if (NULL == store)
			return FALSE;

		if (!marketdata_promote_instrument_at(context, store, source, key, actor, 0,
		                                      &instrument, error))
			return FALSE;

		if (NULL != out_instrument)
			*out_instrument = g_steal_pointer(&instrument);

		return TRUE;
	}
#else
	(void)organization_id;
	(void)data_source_id;
	(void)actor;
	return FALSE;
#endif
}

gboolean
venture_marketdata_promote_venue(
	VentureContext		 *context,
	gint64			  organization_id,
	gint64			  data_source_id,
	const gchar		 *key,
	const VentureActor	 *actor,
	VentureEntity		**out_venue,
	GError			**error
){
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	if (NULL != out_venue)
		*out_venue = NULL;

	if (!marketdata_can_promote(context, key, error))
		return FALSE;

#ifdef VENTURE_HAVE_SQLITE
	{
		g_autoptr(VentureEntity) source = NULL;
		g_autoptr(VentureSeriesStore) store = NULL;
		g_autoptr(GPtrArray) venues = NULL;
		g_autoptr(VentureEntity) existing = NULL;
		g_autoptr(VentureVenue) venue = NULL;
		g_autofree gchar *source_namespace = NULL;
		g_autofree gchar *ref = NULL;
		VentureSeriesVenueRow *row;
		VentureDatabase *database;
		const gchar *namespace_;
		guint i;

		database = venture_context_get_database(context);
		source = marketdata_source(database, organization_id, data_source_id, error);

		if (NULL == source)
			return FALSE;

		store = marketdata_reader(context, data_source_id, error);

		if (NULL == store)
			return FALSE;

		venues = venture_series_store_list_venues(store, error);

		if (NULL == venues)
			return FALSE;

		row = NULL;

		for (i = 0; (i < venues->len) && (NULL == row); i++)
		{
			if (0 == g_strcmp0(((VentureSeriesVenueRow *)g_ptr_array_index(venues, i))->key, key))
				row = g_ptr_array_index(venues, i);
		}

		if (NULL == row)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "Data source #%" G_GINT64_FORMAT " has not seen a venue \"%s\"",
			            data_source_id, key);
			return FALSE;
		}

		g_object_get(source, "venue-namespace", &source_namespace, NULL);
		namespace_ = !venture_string_is_empty(row->namespace_) ? row->namespace_ : source_namespace;
		ref = venture_marketdata_external_ref(namespace_, key);

		if (!marketdata_existing(database, VENTURE_TYPE_VENUE, organization_id, ref, actor,
		                         &existing, error))
			return FALSE;

		if (NULL != existing)
		{
			if (NULL != out_venue)
				*out_venue = g_steal_pointer(&existing);
			return TRUE;
		}

		venue = venture_venue_new();
		venture_entity_set_organization_id(VENTURE_ENTITY(venue), organization_id);
		g_object_set(venue,
		             "name", !venture_string_is_empty(row->name) ? row->name : key,
		             "kind", marketdata_kind(VENTURE_TYPE_VENUE_KIND, row->kind),
		             "namespace", namespace_, "key", key,
		             "group-key", row->group_key,
		             "currency", row->currency,
		             "data-source-id", data_source_id,
		             NULL);

		if (!venture_database_save(database, VENTURE_ENTITY(venue), actor, error))
			return FALSE;

		if (NULL != out_venue)
			*out_venue = VENTURE_ENTITY(g_steal_pointer(&venue));

		return TRUE;
	}
#else
	(void)organization_id;
	(void)data_source_id;
	(void)actor;
	return FALSE;
#endif
}

/* ==========================================================================
 * The series: price-source grammar
 * ========================================================================== */

gboolean
venture_marketdata_parse_price_source(
	const gchar		 *text,
	gboolean		 *out_series,
	VentureMarketdataBasis	 *out_basis,
	gchar			**out_where,
	GError			**error
){
	g_autofree gchar *rest = NULL;
	const gchar *at;
	gint value;

	g_return_val_if_fail(NULL != out_series, FALSE);

	*out_series = FALSE;

	if (NULL != out_basis)
		*out_basis = VENTURE_MARKETDATA_BASIS_MARKET;

	if (NULL != out_where)
		*out_where = NULL;

	if ((NULL == text) || !g_str_has_prefix(text, VENTURE_MARKETDATA_SERIES_PREFIX))
		return TRUE;

	*out_series = TRUE;
	rest = g_strdup(text + strlen(VENTURE_MARKETDATA_SERIES_PREFIX));
	at = strchr(rest, '@');

	if (NULL != at)
	{
		g_autofree gchar *where = g_strstrip(g_strdup(at + 1));

		rest[at - rest] = '\0';

		if ('\0' == where[0])
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "price_source \"%s\" names no venue or group after @", text);
			return FALSE;
		}

		if (!marketdata_check_text(where, "price_source", error))
			return FALSE;

		if (NULL != out_where)
			*out_where = g_steal_pointer(&where);
	}

	g_strstrip(rest);

	if (('\0' == rest[0]) ||
	    !venture_enum_from_nick(VENTURE_TYPE_MARKETDATA_BASIS, rest, &value))
	{
		g_autofree gchar *nicks = NULL;
		g_auto(GStrv) all = venture_enum_list_nicks(VENTURE_TYPE_MARKETDATA_BASIS);

		nicks = g_strjoinv(", ", all);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "price_source \"%s\" names no basis; series:<basis>[@<venue or group>] "
		            "takes one of %s", text, nicks);

		if (NULL != out_where)
			g_clear_pointer(out_where, g_free);

		return FALSE;
	}

	if (venture_marketdata_basis_is_number((VentureMarketdataBasis)value))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "price_source \"%s\": %s is a number, not a price", text,
		            venture_enum_to_nick(VENTURE_TYPE_MARKETDATA_BASIS, value));

		if (NULL != out_where)
			g_clear_pointer(out_where, g_free);

		return FALSE;
	}

	if (NULL != out_basis)
		*out_basis = (VentureMarketdataBasis)value;

	return TRUE;
}

gboolean
venture_marketdata_series_available(
	VentureContext	 *context,
	GError		**error
){
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	if (!venture_context_module_enabled(context, "marketdata"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "a series: price_source reads the marketdata module's instruments, "
		                    "and the marketdata module is off");
		return FALSE;
	}

#ifndef VENTURE_HAVE_SQLITE
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	                    "a series: price_source reads a series store, which this build "
	                    "(without SQLite) does not have");
	return FALSE;
#else
	if (!venture_context_module_enabled(context, "feeds"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "a series: price_source reads the stores the feeds module "
		                    "keeps, and the feeds module is off");
		return FALSE;
	}

	return TRUE;
#endif
}
