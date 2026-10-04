/*
 * venture-marketdata-oracle.c - What something is worth, from market data
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The oracle maps a question onto a series store's precomputed figures:
 * an instrument record names its store (data source) and its key there;
 * a venue record, or a key, names the venue; a group names the venues
 * compared together. Every figure keeps the currency it was stored in.
 * See docs/market-data.org, "The price oracle".
 */

#include "venture.h"

#include <math.h>
#include <string.h>

/* How far back the history is read for a question about an earlier
 * moment: the hourly table's retention, then the 60 days every daily
 * figure is compared over. Older than that, there is no answer. */
#define ORACLE_HOURLY_SECONDS ((gint64)VENTURE_SERIES_EWMA_DAYS * 86400)
#define ORACLE_DAILY_SECONDS ((gint64)VENTURE_SERIES_HISTORICAL_DAYS * 86400)

/* The most instruments one product is asked through: a product named by
 * more instruments than this is a data problem, not a question. */
#define ORACLE_MAX_INSTRUMENTS (100)

/* ==========================================================================
 * Questions and evidence
 * ========================================================================== */

void
venture_marketdata_question_init(VentureMarketdataQuestion *question)
{
	g_return_if_fail(NULL != question);

	question->organization_id = 0;
	question->instrument_id = 0;
	question->product_id = 0;
	question->venue_id = 0;
	question->where = NULL;
	question->basis = VENTURE_MARKETDATA_BASIS_MARKET;
	question->at = NULL;
	question->currency = NULL;
	question->prefer_currency = NULL;
	question->allow_fallback = FALSE;
	question->fallback_source = NULL;
}

VentureMarketdataEvidence *
venture_marketdata_evidence_copy(const VentureMarketdataEvidence *evidence)
{
	VentureMarketdataEvidence *copy;

	if (NULL == evidence)
		return NULL;

	copy = g_new0(VentureMarketdataEvidence, 1);
	copy->basis = evidence->basis;
	copy->origin = g_strdup(evidence->origin);
	copy->data_source_id = evidence->data_source_id;
	copy->instrument_id = evidence->instrument_id;
	copy->instrument_key = g_strdup(evidence->instrument_key);
	copy->venue_key = g_strdup(evidence->venue_key);
	copy->group_key = g_strdup(evidence->group_key);
	copy->taken_at = evidence->taken_at;
	copy->observation_id = evidence->observation_id;
	copy->observation_source = g_strdup(evidence->observation_source);
	copy->note = g_strdup(evidence->note);

	return copy;
}

void
venture_marketdata_evidence_free(VentureMarketdataEvidence *evidence)
{
	if (NULL == evidence)
		return;

	g_free(evidence->origin);
	g_free(evidence->instrument_key);
	g_free(evidence->venue_key);
	g_free(evidence->group_key);
	g_free(evidence->observation_source);
	g_free(evidence->note);
	g_free(evidence);
}

G_DEFINE_BOXED_TYPE(VentureMarketdataEvidence, venture_marketdata_evidence,
                    venture_marketdata_evidence_copy, venture_marketdata_evidence_free)

/* A member that is set, or a JSON null. */
static void
oracle_json_string(
	JsonBuilder	*builder,
	const gchar	*name,
	const gchar	*value
){
	json_builder_set_member_name(builder, name);

	if (NULL == value)
		json_builder_add_null_value(builder);
	else
		json_builder_add_string_value(builder, value);
}

static void
oracle_json_id(
	JsonBuilder	*builder,
	const gchar	*name,
	gint64		 value
){
	json_builder_set_member_name(builder, name);

	if (value <= 0)
		json_builder_add_null_value(builder);
	else
		json_builder_add_int_value(builder, value);
}

JsonNode *
venture_marketdata_evidence_to_json(const VentureMarketdataEvidence *evidence)
{
	g_autoptr(JsonBuilder) builder = NULL;

	g_return_val_if_fail(NULL != evidence, NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	oracle_json_string(builder, "basis",
	                   venture_enum_to_nick(VENTURE_TYPE_MARKETDATA_BASIS, (gint)evidence->basis));
	oracle_json_string(builder, "origin", evidence->origin);
	oracle_json_id(builder, "data_source_id", evidence->data_source_id);
	oracle_json_id(builder, "instrument_id", evidence->instrument_id);
	oracle_json_string(builder, "instrument_key", evidence->instrument_key);
	oracle_json_string(builder, "venue_key", evidence->venue_key);
	oracle_json_string(builder, "group_key", evidence->group_key);

	json_builder_set_member_name(builder, "taken_at");

	if (evidence->taken_at > 0)
	{
		g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(evidence->taken_at);
		g_autofree gchar *text = venture_time_to_string(when);

		json_builder_add_string_value(builder, text);
	}
	else
		json_builder_add_null_value(builder);

	oracle_json_id(builder, "observation_id", evidence->observation_id);
	oracle_json_string(builder, "observation_source", evidence->observation_source);
	oracle_json_string(builder, "note", evidence->note);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* ==========================================================================
 * The oracle object
 * ========================================================================== */

struct _VentureMarketdataOracle
{
	GObject		 parent_instance;

	VentureContext	*context;

	/* Data source id -> read handle, or NULL for "no store": a source
	 * with nothing stored is asked once per oracle, not once per
	 * product. */
	GHashTable	*readers;
};

G_DEFINE_FINAL_TYPE(VentureMarketdataOracle, venture_marketdata_oracle, G_TYPE_OBJECT)

static void
oracle_reader_free(gpointer data)
{
	if (NULL != data)
		g_object_unref(data);
}

static void
venture_marketdata_oracle_finalize(GObject *object)
{
	VentureMarketdataOracle *self = VENTURE_MARKETDATA_ORACLE(object);

	g_clear_pointer(&self->readers, g_hash_table_unref);
	g_clear_object(&self->context);

	G_OBJECT_CLASS(venture_marketdata_oracle_parent_class)->finalize(object);
}

static void
venture_marketdata_oracle_class_init(VentureMarketdataOracleClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_marketdata_oracle_finalize;
}

static void
venture_marketdata_oracle_init(VentureMarketdataOracle *self)
{
	self->readers = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
	                                      oracle_reader_free);
}

VentureMarketdataOracle *
venture_marketdata_oracle_new(VentureContext *context)
{
	VentureMarketdataOracle *self;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	self = g_object_new(VENTURE_TYPE_MARKETDATA_ORACLE, NULL);
	self->context = g_object_ref(context);

	return self;
}

/* ==========================================================================
 * One answer
 * ========================================================================== */

typedef struct
{
	gboolean	 found;
	gint64		 amount;	/* minor units of @currency, or units */
	gdouble		 number;	/* for the rates */
	gchar		 currency[VENTURE_MONEY_CURRENCY_LEN];
	gint64		 taken_at;
	gint64		 data_source_id;
	gint64		 instrument_id;
	gchar		*instrument_key;
	gchar		*venue_key;
	gchar		*group_key;
	gchar		*note;
} OracleAnswer;

static void
oracle_answer_clear(OracleAnswer *answer)
{
	g_clear_pointer(&answer->instrument_key, g_free);
	g_clear_pointer(&answer->venue_key, g_free);
	g_clear_pointer(&answer->group_key, g_free);
	g_clear_pointer(&answer->note, g_free);
	memset(answer, 0, sizeof(*answer));
	answer->number = NAN;
}

/* Moves @from into @to, leaving @from empty. */
static void
oracle_answer_move(
	OracleAnswer	*to,
	OracleAnswer	*from
){
	oracle_answer_clear(to);
	*to = *from;
	memset(from, 0, sizeof(*from));
	from->number = NAN;
}

/* Records why nothing answered; the first reason is kept, being the one
 * about the first place asked. */
static void
oracle_note(
	OracleAnswer	*answer,
	const gchar	*format,
	...
) G_GNUC_PRINTF(2, 3);

static void
oracle_note(
	OracleAnswer	*answer,
	const gchar	*format,
	...
){
	va_list args;

	if (NULL != answer->note)
		return;

	va_start(args, format);
	answer->note = g_strdup_vprintf(format, args);
	va_end(args);
}

#ifdef VENTURE_HAVE_SQLITE

/* Only the store's readers name a currency, so only they need this. */
static void
oracle_set_currency(
	OracleAnswer	*answer,
	const gchar	*currency
){
	g_strlcpy(answer->currency, (NULL != currency) ? currency : "", sizeof(answer->currency));
}

/*
 * A read handle on a source's store, opened once per oracle. A store that
 * does not exist yet, or feeds being off, is no store and a note -- never
 * an error: nothing observed is an answer. A store that cannot be read
 * (a newer schema) is an error, because the answer would be a lie.
 */
static gboolean
oracle_reader(
	VentureMarketdataOracle	 *self,
	gint64			  data_source_id,
	VentureSeriesStore	**out_store,
	OracleAnswer		 *answer,
	GError			**error
){
	VentureFeedsService *service;
	g_autoptr(GError) open_error = NULL;
	VentureSeriesStore *store;
	gpointer cached;

	*out_store = NULL;

	if (g_hash_table_lookup_extended(self->readers, &data_source_id, NULL, &cached))
	{
		*out_store = cached;

		if (NULL == cached)
			oracle_note(answer, "data source #%" G_GINT64_FORMAT " has stored nothing yet",
			            data_source_id);
		return TRUE;
	}

	service = venture_context_get_feeds_service(self->context);

	if (NULL == service)
	{
		oracle_note(answer, "the feeds module is off, so no store is read");
		return TRUE;
	}

	store = venture_feeds_service_open_reader(service, data_source_id, &open_error);

	if ((NULL == store) && !g_error_matches(open_error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND))
	{
		g_propagate_error(error, g_steal_pointer(&open_error));
		return FALSE;
	}

	g_hash_table_insert(self->readers, g_memdup2(&data_source_id, sizeof(data_source_id)), store);

	if (NULL == store)
		oracle_note(answer, "data source #%" G_GINT64_FORMAT " has stored nothing yet",
		            data_source_id);

	*out_store = store;

	return TRUE;
}

/*
 * The group a group-wide figure is read in when the question named none:
 * the store's only group. A store whose venues sit in several groups
 * cannot answer "the region" without being told which, and guessing would
 * price an EU recipe from US realms; that is refused, naming the groups.
 */
static gboolean
oracle_only_group(
	VentureSeriesStore	 *store,
	gint64			  data_source_id,
	gchar			**out_group,
	OracleAnswer		 *answer,
	GError			**error
){
	g_autoptr(GPtrArray) venues = NULL;
	g_autoptr(GPtrArray) groups = NULL;
	guint i;

	*out_group = NULL;
	venues = venture_series_store_list_venues(store, error);

	if (NULL == venues)
		return FALSE;

	groups = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < venues->len; i++)
	{
		VentureSeriesVenueRow *row = g_ptr_array_index(venues, i);
		const gchar *group = (NULL != row->group_key) ? row->group_key : "";

		if (!g_ptr_array_find_with_equal_func(groups, group, g_str_equal, NULL))
			g_ptr_array_add(groups, g_strdup(group));
	}

	if (0 == groups->len)
	{
		oracle_note(answer, "data source #%" G_GINT64_FORMAT " has no venues yet", data_source_id);
		return TRUE;
	}

	if (groups->len > 1)
	{
		g_autoptr(GString) list = g_string_new(NULL);

		for (i = 0; (i < groups->len) && (i < 5); i++)
			g_string_append_printf(list, "%s\"%s\"", (i > 0) ? ", " : "",
			                       (const gchar *)g_ptr_array_index(groups, i));

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "Data source #%" G_GINT64_FORMAT " has venues in %u groups (%s%s); name "
		            "the group or a venue to read a group's figure in, e.g. series:<basis>@<group>",
		            data_source_id, groups->len, list->str, (groups->len > 5) ? ", ..." : "");
		return FALSE;
	}

	*out_group = g_strdup(g_ptr_array_index(groups, 0));

	return TRUE;
}

/* The figure a basis reads from a current row, an hourly point or a day. */
static gint64
oracle_row_value(
	const VentureSeriesRow	*row,
	VentureMarketdataBasis	 basis
){
	switch (basis)
	{
	case VENTURE_MARKETDATA_BASIS_MIN:
		return row->min_price;
	case VENTURE_MARKETDATA_BASIS_QUANTITY:
		return row->quantity;
	case VENTURE_MARKETDATA_BASIS_MARKET:
	default:
		return row->market_value;
	}
}

static gint64
oracle_point_value(
	const VentureSeriesPoint	*point,
	VentureMarketdataBasis		 basis
){
	switch (basis)
	{
	case VENTURE_MARKETDATA_BASIS_MIN:
		return point->min_price;
	case VENTURE_MARKETDATA_BASIS_QUANTITY:
		return point->quantity;
	case VENTURE_MARKETDATA_BASIS_MARKET:
	default:
		return point->market_value;
	}
}

static gint64
oracle_day_value(
	const VentureSeriesDay	*day,
	VentureMarketdataBasis	 basis
){
	switch (basis)
	{
	case VENTURE_MARKETDATA_BASIS_MIN:
		return day->min_price;
	case VENTURE_MARKETDATA_BASIS_QUANTITY:
		return day->max_quantity;
	case VENTURE_MARKETDATA_BASIS_MARKET:
	default:
		return day->market_value;
	}
}

/*
 * One venue's min, market or quantity: the current row when it was taken
 * at or before the moment asked about; otherwise the newest hourly point,
 * then the newest day, at or before it. A day's figure is the whole day's
 * (its largest snapshot's), which is the history the store keeps.
 */
static gboolean
oracle_venue_figure(
	VentureSeriesStore		 *store,
	const VentureMarketdataQuestion	 *question,
	const gchar			 *venue_key,
	const gchar			 *instrument_key,
	gint64				  at_unix,
	OracleAnswer			 *answer,
	GError				**error
){
	g_autoptr(VentureSeriesRow) row = NULL;
	gint64 value;
	guint i;

	if (!venture_series_store_get_current(store, venue_key, instrument_key, &row, error))
		return FALSE;

	value = VENTURE_SERIES_NONE;

	if ((NULL != row) && (row->taken_at <= at_unix))
	{
		value = oracle_row_value(row, question->basis);
		oracle_set_currency(answer, row->currency);
		answer->taken_at = row->taken_at;
	}
	else if (NULL != question->at)
	{
		g_autoptr(GArray) points = NULL;

		points = venture_series_store_hourly(store, venue_key, instrument_key,
		                                     at_unix - ORACLE_HOURLY_SECONDS, error);

		if (NULL == points)
			return FALSE;

		for (i = points->len; i > 0; i--)
		{
			VentureSeriesPoint *point = &g_array_index(points, VentureSeriesPoint, i - 1);

			if (point->at <= at_unix)
			{
				value = oracle_point_value(point, question->basis);
				oracle_set_currency(answer, point->currency);
				answer->taken_at = point->at;
				break;
			}
		}

		if (0 == answer->taken_at)
		{
			g_autoptr(GArray) days = NULL;

			days = venture_series_store_daily(store, venue_key, instrument_key,
			                                  at_unix - ORACLE_DAILY_SECONDS, error);

			if (NULL == days)
				return FALSE;

			for (i = days->len; i > 0; i--)
			{
				VentureSeriesDay *day = &g_array_index(days, VentureSeriesDay, i - 1);

				if (day->day_start <= at_unix)
				{
					value = oracle_day_value(day, question->basis);
					oracle_set_currency(answer, day->currency);
					answer->taken_at = day->day_start;
					break;
				}
			}
		}
	}

	answer->venue_key = g_strdup(venue_key);

	if (0 == answer->taken_at)
	{
		oracle_note(answer, "venue %s has no figures for %s%s", venue_key, instrument_key,
		            (NULL != question->at) ? " at or before the moment asked about" : "");
		return TRUE;
	}

	if (VENTURE_SERIES_NONE == value)
	{
		oracle_note(answer, "%s is not on offer at venue %s", instrument_key, venue_key);
		return TRUE;
	}

	answer->found = TRUE;
	answer->amount = value;
	answer->number = (gdouble)value;

	return TRUE;
}

/*
 * The cheapest minimum, or the units on offer, across a group's venues
 * (every venue, with no group): what buying it anywhere in the group
 * costs, and how much of it there is. Only current rows taken at or
 * before the moment count. The cheapest is in one currency: the one asked
 * for, else the preferred one when a venue offers in it, else the
 * cheapest venue's.
 */
static gboolean
oracle_across_venues(
	VentureSeriesStore		 *store,
	const VentureMarketdataQuestion	 *question,
	const gchar			 *group,
	const gchar			 *instrument_key,
	gint64				  at_unix,
	OracleAnswer			 *answer,
	GError				**error
){
	g_autoptr(GPtrArray) rows = NULL;
	const gchar *target;
	VentureSeriesRow *best;
	gint64 units;
	guint counted;
	guint i;

	rows = venture_series_store_other_venues(store, instrument_key, group, error);

	if (NULL == rows)
		return FALSE;

	answer->group_key = g_strdup(group);

	if (VENTURE_MARKETDATA_BASIS_QUANTITY == question->basis)
	{
		units = 0;
		counted = 0;

		/* A venue with none on offer is a venue that answered: 0 units. */
		for (i = 0; i < rows->len; i++)
		{
			VentureSeriesRow *row = g_ptr_array_index(rows, i);

			if (row->taken_at > at_unix)
				continue;

			counted++;
			answer->taken_at = MAX(answer->taken_at, row->taken_at);

			if ((row->quantity > 0) && __builtin_add_overflow(units, row->quantity, &units))
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				                    "More units are on offer than can be counted");
				return FALSE;
			}
		}

		if (0 == counted)
		{
			oracle_note(answer, "no venue has figures for %s", instrument_key);
			return TRUE;
		}

		answer->found = TRUE;
		answer->amount = units;
		answer->number = (gdouble)units;
		return TRUE;
	}

	/* Which currency the cheapest is looked for in. */
	target = question->currency;

	for (i = 0; (NULL == target) && (NULL != question->prefer_currency) && (i < rows->len); i++)
	{
		VentureSeriesRow *row = g_ptr_array_index(rows, i);

		if ((row->taken_at <= at_unix) && (VENTURE_SERIES_NONE != row->min_price) &&
		    (0 == g_ascii_strcasecmp(row->currency, question->prefer_currency)))
			target = question->prefer_currency;
	}

	best = NULL;

	for (i = 0; i < rows->len; i++)
	{
		VentureSeriesRow *row = g_ptr_array_index(rows, i);

		if ((row->taken_at > at_unix) || (VENTURE_SERIES_NONE == row->min_price))
			continue;

		if (NULL == target)
			target = row->currency;

		if (0 != g_ascii_strcasecmp(row->currency, target))
			continue;

		if ((NULL == best) || (row->min_price < best->min_price))
			best = row;
	}

	if (NULL == best)
	{
		oracle_note(answer, "%s is not on offer at any venue%s%s%s", instrument_key,
		            (NULL != group) ? " in group " : "", (NULL != group) ? group : "",
		            (NULL != question->currency) ? " in the currency asked for" : "");
		return TRUE;
	}

	answer->found = TRUE;
	answer->amount = best->min_price;
	answer->number = (gdouble)best->min_price;
	answer->taken_at = best->taken_at;
	answer->venue_key = g_strdup(best->venue_key);
	oracle_set_currency(answer, best->currency);

	return TRUE;
}

/*
 * A group's region figure: the median or 33rd percentile of its venues'
 * cheapest prices, or the mean of their market values, as the store's
 * periodic recompute left them. Current only: a figure computed after the
 * moment asked about does not answer for it.
 */
static gboolean
oracle_region_figure(
	VentureSeriesStore		 *store,
	const VentureMarketdataQuestion	 *question,
	const gchar			 *group,
	const gchar			 *instrument_key,
	gint64				  at_unix,
	OracleAnswer			 *answer,
	GError				**error
){
	VentureSeriesRegion region;
	gint64 value;

	memset(&region, 0, sizeof(region));
	answer->group_key = g_strdup(group);

	if (NULL != question->currency)
	{
		g_autofree gchar *code = g_ascii_strup(question->currency, -1);

		if (!venture_series_store_get_region(store, group, instrument_key, code, &region, error))
			return FALSE;
	}
	else
	{
		if ((NULL != question->prefer_currency) &&
		    !venture_series_store_get_region(store, group, instrument_key,
		                                     question->prefer_currency, &region, error))
			return FALSE;

		if (!region.found &&
		    !venture_series_store_get_region(store, group, instrument_key, NULL, &region, error))
			return FALSE;
	}

	if (!region.found)
	{
		oracle_note(answer, "no region figures for %s in group \"%s\" yet; they are "
		            "recomputed after a run, every 30 minutes", instrument_key, group);
		return TRUE;
	}

	if (region.computed_at > at_unix)
	{
		oracle_note(answer, "region figures are current only, and these were computed after "
		            "the moment asked about");
		return TRUE;
	}

	switch (question->basis)
	{
	case VENTURE_MARKETDATA_BASIS_REGION_MEDIAN:
		value = region.median_min;
		break;
	case VENTURE_MARKETDATA_BASIS_REGION_P33:
		value = region.p33;
		break;
	case VENTURE_MARKETDATA_BASIS_REGION_MARKET_AVG:
	case VENTURE_MARKETDATA_BASIS_MARKET:
	default:
		value = region.market_avg;
		break;
	}

	if (VENTURE_SERIES_NONE == value)
	{
		oracle_note(answer, "group \"%s\" has no such figure for %s", group, instrument_key);
		return TRUE;
	}

	answer->found = TRUE;
	answer->amount = value;
	answer->number = (gdouble)value;
	answer->taken_at = region.computed_at;
	oracle_set_currency(answer, region.currency);

	return TRUE;
}

/*
 * The daily-history figures: the 14-day and 60-day market values, the
 * average sale price, the sale rate and the units sold a day, for a venue
 * or a group (the mean of its venues per day, sales summed), as of the day
 * the moment asked about falls in.
 */
static gboolean
oracle_reference_figure(
	VentureSeriesStore		 *store,
	const VentureMarketdataQuestion	 *question,
	const gchar			 *venue_key,
	const gchar			 *group,
	const gchar			 *instrument_key,
	gint64				  now_unix,
	OracleAnswer			 *answer,
	GError				**error
){
	VentureSeriesReference reference;
	gint64 value;
	gint64 days;

	memset(&reference, 0, sizeof(reference));

	if (!venture_series_store_reference(store, venue_key, (NULL != venue_key) ? NULL : group,
	                                    instrument_key, now_unix, &reference, error))
		return FALSE;

	answer->venue_key = g_strdup(venue_key);
	answer->group_key = (NULL != venue_key) ? NULL : g_strdup(group);
	days = (VENTURE_MARKETDATA_BASIS_HISTORICAL_60D == question->basis) ? reference.days_60
	                                                                    : reference.days_14;

	switch (question->basis)
	{
	case VENTURE_MARKETDATA_BASIS_SALE_RATE:
	case VENTURE_MARKETDATA_BASIS_SOLD_PER_DAY:
		answer->number = (VENTURE_MARKETDATA_BASIS_SALE_RATE == question->basis)
			? reference.sale_rate : reference.sold_per_day;

		if (isnan(answer->number))
		{
			oracle_note(answer, "no sales history for %s", instrument_key);
			return TRUE;
		}

		answer->found = TRUE;
		g_free(answer->note);

		/* The store's own estimate wins; the source's figure only fills
		 * a gap, and the evidence says which one answered. */
		if (((VENTURE_MARKETDATA_BASIS_SALE_RATE == question->basis) &&
		     reference.sale_rate_from_source) ||
		    ((VENTURE_MARKETDATA_BASIS_SOLD_PER_DAY == question->basis) &&
		     reference.sold_per_day_from_source))
			answer->note = g_strdup("the source's own figure: no sales history of its own");
		else
			answer->note = g_strdup_printf("from %" G_GINT64_FORMAT " days of history", days);
		return TRUE;
	case VENTURE_MARKETDATA_BASIS_MARKET_14D:
		value = reference.market_14d;
		break;
	case VENTURE_MARKETDATA_BASIS_HISTORICAL_60D:
		value = reference.historical_60d;
		break;
	case VENTURE_MARKETDATA_BASIS_SALE_AVG:
	default:
		value = reference.sale_avg;
		break;
	}

	if (VENTURE_SERIES_NONE == value)
	{
		oracle_note(answer, "no daily history for %s", instrument_key);
		return TRUE;
	}

	answer->found = TRUE;
	answer->amount = value;
	answer->number = (gdouble)value;
	oracle_set_currency(answer, reference.currency);
	g_free(answer->note);

	if ((VENTURE_MARKETDATA_BASIS_HISTORICAL_60D == question->basis) &&
	    reference.historical_from_source)
		answer->note = g_strdup("the source's own historical price: no daily history of its own");
	else
		answer->note = g_strdup_printf("from %" G_GINT64_FORMAT " days of history", days);

	return TRUE;
}

#endif /* VENTURE_HAVE_SQLITE */

/*
 * Asks one instrument's store. @venue is the venue record named, if any.
 * Every "cannot answer" is a note on @answer; only a failed read is an
 * error.
 */
static gboolean
oracle_ask_instrument(
	VentureMarketdataOracle		 *self,
	const VentureMarketdataQuestion	 *question,
	VentureEntity			 *instrument,
	VentureEntity			 *venue,
	OracleAnswer			 *answer,
	GError				**error
){
	g_autofree gchar *key = NULL;
	gint64 data_source_id;

	g_object_get(instrument, "data-source-id", &data_source_id, "key", &key, NULL);
	answer->instrument_id = venture_entity_get_id(instrument);

	if ((data_source_id <= 0) || venture_string_is_empty(key))
	{
		oracle_note(answer, "instrument #%" G_GINT64_FORMAT " names no data source and key",
		            venture_entity_get_id(instrument));
		return TRUE;
	}

	answer->data_source_id = data_source_id;
	answer->instrument_key = g_strdup(key);

#ifndef VENTURE_HAVE_SQLITE
	(void)self;
	(void)question;
	(void)venue;
	(void)error;
	oracle_note(answer, "this build has no series store");
	return TRUE;
#else
	{
		g_autofree gchar *venue_key = NULL;
		g_autofree gchar *group = NULL;
		VentureSeriesStore *store;
		gint64 at_unix;
		gint64 now_unix;

		/* A venue record answers for its own store only. */
		if (NULL != venue)
		{
			gint64 venue_source;

			g_object_get(venue, "data-source-id", &venue_source, "key", &venue_key, NULL);

			if (venture_string_is_empty(venue_key))
			{
				oracle_note(answer, "venue #%" G_GINT64_FORMAT " has no key in a store",
				            venture_entity_get_id(venue));
				return TRUE;
			}

			if ((venue_source > 0) && (venue_source != data_source_id))
			{
				oracle_note(answer, "venue #%" G_GINT64_FORMAT " is in data source #%"
				            G_GINT64_FORMAT ", the instrument in #%" G_GINT64_FORMAT,
				            venture_entity_get_id(venue), venue_source, data_source_id);
				return TRUE;
			}
		}

		if (!oracle_reader(self, data_source_id, &store, answer, error))
			return FALSE;

		if (NULL == store)
			return TRUE;

		/* A place named by text is a venue when the store knows a venue by
		 * that key, and a group otherwise. */
		if (!venture_string_is_empty(question->where))
		{
			VentureSeriesVenueState state;

			memset(&state, 0, sizeof(state));

			if (!venture_series_store_get_venue_state(store, question->where, &state, error))
				return FALSE;

			if (state.found)
				venue_key = g_strdup(question->where);
			else
				group = g_strdup(question->where);
		}

		at_unix = (NULL != question->at) ? g_date_time_to_unix(question->at) : G_MAXINT64;
		now_unix = (NULL != question->at) ? at_unix : (g_get_real_time() / G_USEC_PER_SEC);

		switch (question->basis)
		{
		case VENTURE_MARKETDATA_BASIS_MIN:
		case VENTURE_MARKETDATA_BASIS_QUANTITY:
			if (NULL != venue_key)
				return oracle_venue_figure(store, question, venue_key, key, at_unix, answer, error);

			return oracle_across_venues(store, question, group, key, at_unix, answer, error);

		case VENTURE_MARKETDATA_BASIS_MARKET:
			if (NULL != venue_key)
				return oracle_venue_figure(store, question, venue_key, key, at_unix, answer, error);

			/* A group's market value is the mean of its venues': the
			 * region figure, read the same way. */
			/* fall through */
		case VENTURE_MARKETDATA_BASIS_REGION_MEDIAN:
		case VENTURE_MARKETDATA_BASIS_REGION_P33:
		case VENTURE_MARKETDATA_BASIS_REGION_MARKET_AVG:
			if ((NULL == group) && (NULL != venue_key))
			{
				g_autoptr(VentureSeriesRow) row = NULL;

				/* A venue's region is its group's. */
				if (!venture_series_store_get_current(store, venue_key, key, &row, error))
					return FALSE;

				if (NULL != row)
					group = g_strdup(row->group_key);
			}

			if ((NULL == group) && !oracle_only_group(store, data_source_id, &group, answer, error))
				return FALSE;

			if (NULL == group)
				return TRUE;

			return oracle_region_figure(store, question, group, key, at_unix, answer, error);

		case VENTURE_MARKETDATA_BASIS_MARKET_14D:
		case VENTURE_MARKETDATA_BASIS_HISTORICAL_60D:
		case VENTURE_MARKETDATA_BASIS_SALE_AVG:
		case VENTURE_MARKETDATA_BASIS_SALE_RATE:
		case VENTURE_MARKETDATA_BASIS_SOLD_PER_DAY:
		default:
			if ((NULL == venue_key) && (NULL == group) &&
			    !oracle_only_group(store, data_source_id, &group, answer, error))
				return FALSE;

			if ((NULL == venue_key) && (NULL == group))
				return TRUE;

			return oracle_reference_figure(store, question, venue_key, group, key, now_unix,
			                               answer, error);
		}
	}
#endif
}

/* A record of the question's organization, or NOT_FOUND. */
static VentureEntity *
oracle_get(
	VentureDatabase	 *database,
	GType		  type,
	gint64		  id,
	gint64		  organization_id,
	const gchar	 *noun,
	GError		**error
){
	g_autoptr(VentureEntity) record = NULL;

	record = venture_database_get(database, type, id, NULL);

	if ((NULL == record) || venture_entity_is_deleted(record) ||
	    (venture_entity_get_organization_id(record) != organization_id))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No %s #%" G_GINT64_FORMAT " in this organization", noun, id);
		return NULL;
	}

	return g_steal_pointer(&record);
}

/*
 * The whole question: the instruments to ask (one, or every one naming the
 * product, oldest first), each asked in turn until one answers -- in the
 * preferred currency when one is preferred -- the currency held to what
 * was asked for, and then, for a price and only when allowed, the newest
 * observation.
 */
static gboolean
oracle_ask(
	VentureMarketdataOracle			 *self,
	const VentureMarketdataQuestion		 *question,
	gboolean				  want_number,
	OracleAnswer				 *answer,
	VentureMoney				**out_price,
	VentureMarketdataEvidence		 *evidence,
	GError					**error
){
	g_autoptr(VentureEntity) venue = NULL;
	g_autoptr(GPtrArray) instruments = NULL;
	VentureDatabase *database;
	OracleAnswer candidate;
	gint64 product_id;
	guint i;

	database = venture_context_get_database(self->context);
	memset(&candidate, 0, sizeof(candidate));
	candidate.number = NAN;

	/* --- The question itself --- */

	if (want_number != venture_marketdata_basis_is_number(question->basis))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%s is %s", venture_enum_to_nick(VENTURE_TYPE_MARKETDATA_BASIS,
		                                             (gint)question->basis),
		            want_number ? "a price, not a number" : "a number, not a price");
		return FALSE;
	}

	if ((question->venue_id > 0) && !venture_string_is_empty(question->where))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Name a venue record or a venue or group key, not both");
		return FALSE;
	}

	if ((question->instrument_id <= 0) && (question->product_id <= 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Name an instrument or a product to price");
		return FALSE;
	}

	if (NULL != question->currency)
	{
		g_autofree gchar *code = g_ascii_strup(question->currency, -1);

		if (!venture_currency_is_valid(code))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "currency \"%s\" is not a currency code", question->currency);
			return FALSE;
		}
	}

	product_id = question->product_id;

	/* --- The instruments, while the module is on --- */

	if (!venture_context_module_enabled(self->context, "marketdata"))
		oracle_note(answer, "the marketdata module is off, so no instrument is read");
	else
	{
		if (question->venue_id > 0)
		{
			venue = oracle_get(database, VENTURE_TYPE_VENUE, question->venue_id,
			                   question->organization_id, "venue", error);

			if (NULL == venue)
				return FALSE;
		}

		instruments = g_ptr_array_new_with_free_func(g_object_unref);

		if (question->instrument_id > 0)
		{
			VentureEntity *instrument;

			instrument = oracle_get(database, VENTURE_TYPE_INSTRUMENT, question->instrument_id,
			                        question->organization_id, "instrument", error);

			if (NULL == instrument)
				return FALSE;

			g_object_get(instrument, "product-id", &product_id, NULL);
			g_ptr_array_add(instruments, instrument);
		}
		else
		{
			g_autoptr(VentureQuery) query = NULL;
			g_autoptr(GPtrArray) found = NULL;

			query = venture_query_new(VENTURE_TYPE_INSTRUMENT);
			venture_query_set_organization(query, question->organization_id);
			venture_query_set_limit(query, ORACLE_MAX_INSTRUMENTS);

			if (!venture_query_add_filter_int(query, "product-id", VENTURE_FILTER_OP_EQ,
			                                  question->product_id, error) ||
			    !venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
				return FALSE;

			found = venture_database_find(database, query, error);

			if (NULL == found)
				return FALSE;

			for (i = 0; i < found->len; i++)
				g_ptr_array_add(instruments, g_object_ref(g_ptr_array_index(found, i)));

			if (0 == instruments->len)
				oracle_note(answer, "no instrument names product #%" G_GINT64_FORMAT,
				            question->product_id);
		}

		for (i = 0; i < instruments->len; i++)
		{
			OracleAnswer one;

			memset(&one, 0, sizeof(one));
			one.number = NAN;

			if (!oracle_ask_instrument(self, question, g_ptr_array_index(instruments, i), venue,
			                           &one, error))
			{
				oracle_answer_clear(&one);
				oracle_answer_clear(&candidate);
				return FALSE;
			}

			/* Never converted: a figure in another currency than the one
			 * asked for is no answer, and the note says what it was. */
			if (one.found && !want_number && (NULL != question->currency) &&
			    (0 != g_ascii_strcasecmp(one.currency, question->currency)))
			{
				g_autofree gchar *why = g_strdup_printf(
					"%s is priced in %s, not %s; nothing is converted",
					(NULL != one.instrument_key) ? one.instrument_key : "the instrument",
					one.currency, question->currency);

				one.found = FALSE;
				g_clear_pointer(&one.note, g_free);
				one.note = g_steal_pointer(&why);
			}

			if (!one.found)
			{
				/* The first reason anybody gave is the one reported. */
				if ((NULL == answer->note) && (NULL != one.note))
					answer->note = g_strdup(one.note);

				oracle_answer_clear(&one);
				continue;
			}

			/* Preferred: the first in that currency wins; another is
			 * kept in case none is. */
			if (!want_number && (NULL != question->prefer_currency) &&
			    (0 != g_ascii_strcasecmp(one.currency, question->prefer_currency)))
			{
				if (!candidate.found)
					oracle_answer_move(&candidate, &one);
				else
					oracle_answer_clear(&one);

				continue;
			}

			g_clear_pointer(&answer->note, g_free);
			oracle_answer_move(answer, &one);
			break;
		}

		if (!answer->found && candidate.found)
		{
			g_clear_pointer(&answer->note, g_free);
			oracle_answer_move(answer, &candidate);
		}

		oracle_answer_clear(&candidate);
	}

	/* --- The answer, from a store --- */

	if (answer->found)
	{
		evidence->origin = g_strdup("series");
		evidence->data_source_id = answer->data_source_id;
		evidence->instrument_id = answer->instrument_id;
		evidence->instrument_key = g_strdup(answer->instrument_key);
		evidence->venue_key = g_strdup(answer->venue_key);
		evidence->group_key = g_strdup(answer->group_key);
		evidence->taken_at = answer->taken_at;
		evidence->note = g_strdup(answer->note);

		if (!want_number && (NULL != out_price))
			*out_price = venture_money_new_for_currency(answer->amount, answer->currency);

		return TRUE;
	}

	/* --- Or from an observation, when the caller allows it --- */

	if (!want_number && question->allow_fallback && (product_id > 0) &&
	    venture_context_module_enabled(self->context, "market"))
	{
		g_autoptr(VentureMoney) price = NULL;
		g_autoptr(VentureEntity) observation = NULL;
		gboolean read;

		if (NULL != question->currency)
		{
			g_autofree gchar *code = g_ascii_strup(question->currency, -1);

			read = venture_market_latest_price(database, question->organization_id, product_id,
			                                   question->fallback_source, code, question->at,
			                                   &price, &observation, error);
		}
		else
			read = venture_market_price_preferring(database, question->organization_id,
			                                       product_id, question->fallback_source,
			                                       question->prefer_currency, question->at,
			                                       &price, &observation, error);

		if (!read)
			return FALSE;

		if (NULL != observation)
		{
			g_autoptr(GDateTime) observed = NULL;

			g_object_get(observation, "observed-at", &observed, NULL);
			evidence->origin = g_strdup("observation");
			evidence->observation_id = venture_entity_get_id(observation);
			g_object_get(observation, "source", &evidence->observation_source, NULL);
			evidence->taken_at = (NULL != observed) ? g_date_time_to_unix(observed) : 0;
			evidence->note = g_strdup_printf("no market data answered%s%s; the newest price "
			                                 "observation did",
			                                 (NULL != answer->note) ? ": " : "",
			                                 (NULL != answer->note) ? answer->note : "");
			answer->found = TRUE;

			if (NULL != out_price)
				*out_price = g_steal_pointer(&price);

			return TRUE;
		}
	}

	evidence->note = g_strdup((NULL != answer->note) ? answer->note : "nothing answered");

	return TRUE;
}

/* Fills the common outs around oracle_ask(). */
static gboolean
oracle_run(
	VentureMarketdataOracle			 *self,
	const VentureMarketdataQuestion		 *question,
	gboolean				  want_number,
	VentureMoney				**out_price,
	gdouble					 *out_value,
	VentureMarketdataEvidence		**out_evidence,
	GError					**error
){
	g_autoptr(VentureMarketdataEvidence) evidence = NULL;
	g_autoptr(VentureMoney) price = NULL;
	OracleAnswer answer;
	gboolean ok;

	memset(&answer, 0, sizeof(answer));
	answer.number = NAN;
	evidence = g_new0(VentureMarketdataEvidence, 1);
	evidence->basis = question->basis;

	ok = oracle_ask(self, question, want_number, &answer, &price, evidence, error);

	if (ok && (NULL != out_value))
		*out_value = answer.found ? answer.number : NAN;

	oracle_answer_clear(&answer);

	if (!ok)
		return FALSE;

	if (NULL != out_price)
		*out_price = g_steal_pointer(&price);

	if (NULL != out_evidence)
		*out_evidence = g_steal_pointer(&evidence);

	return TRUE;
}

gboolean
venture_marketdata_oracle_price(
	VentureMarketdataOracle			 *self,
	const VentureMarketdataQuestion		 *question,
	VentureMoney				**out_price,
	VentureMarketdataEvidence		**out_evidence,
	GError					**error
){
	g_return_val_if_fail(VENTURE_IS_MARKETDATA_ORACLE(self), FALSE);
	g_return_val_if_fail(NULL != question, FALSE);

	if (NULL != out_price)
		*out_price = NULL;

	if (NULL != out_evidence)
		*out_evidence = NULL;

	return oracle_run(self, question, FALSE, out_price, NULL, out_evidence, error);
}

gboolean
venture_marketdata_oracle_number(
	VentureMarketdataOracle			 *self,
	const VentureMarketdataQuestion		 *question,
	gdouble					 *out_value,
	VentureMarketdataEvidence		**out_evidence,
	GError					**error
){
	g_return_val_if_fail(VENTURE_IS_MARKETDATA_ORACLE(self), FALSE);
	g_return_val_if_fail(NULL != question, FALSE);
	g_return_val_if_fail(NULL != out_value, FALSE);

	*out_value = NAN;

	if (NULL != out_evidence)
		*out_evidence = NULL;

	return oracle_run(self, question, TRUE, NULL, out_value, out_evidence, error);
}

gboolean
venture_marketdata_reference_price(
	VentureContext				 *context,
	const VentureMarketdataQuestion		 *question,
	VentureMoney				**out_price,
	VentureMarketdataEvidence		**out_evidence,
	GError					**error
){
	g_autoptr(VentureMarketdataOracle) oracle = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	oracle = venture_marketdata_oracle_new(context);

	return venture_marketdata_oracle_price(oracle, question, out_price, out_evidence, error);
}

gboolean
venture_marketdata_reference_number(
	VentureContext				 *context,
	const VentureMarketdataQuestion		 *question,
	gdouble					 *out_value,
	VentureMarketdataEvidence		**out_evidence,
	GError					**error
){
	g_autoptr(VentureMarketdataOracle) oracle = NULL;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	oracle = venture_marketdata_oracle_new(context);

	return venture_marketdata_oracle_number(oracle, question, out_value, out_evidence, error);
}

gboolean
venture_marketdata_oracle_source_price(
	VentureMarketdataOracle	 *self,
	gint64			  organization_id,
	gint64			  product_id,
	VentureMarketdataBasis	  basis,
	const gchar		 *where,
	const gchar		 *currency,
	gboolean		  strict,
	GDateTime		 *at,
	VentureMoney		**out_price,
	GError			**error
){
	VentureMarketdataQuestion question;

	g_return_val_if_fail(VENTURE_IS_MARKETDATA_ORACLE(self), FALSE);
	g_return_val_if_fail(NULL != out_price, FALSE);

	venture_marketdata_question_init(&question);
	question.organization_id = organization_id;
	question.product_id = product_id;
	question.basis = basis;
	question.where = where;
	question.at = at;

	/* Named: only that currency; otherwise preferred, as the observation
	 * path does with the book currency. Never a fallback: see the
	 * header. */
	if (strict)
		question.currency = currency;
	else
		question.prefer_currency = currency;

	return venture_marketdata_oracle_price(self, &question, out_price, NULL, error);
}
