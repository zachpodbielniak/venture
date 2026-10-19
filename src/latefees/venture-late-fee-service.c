/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "latefees/venture-late-fee-private.h"
#include <string.h>

/*
 * The late-fee engine. A schedule is an ordered list of tiers; a tier's
 * occurrences are days late on which it would charge. Nothing here runs on
 * its own: the dunning sweep calls the late-fee stage for each invoice it
 * visits, the reminder it renders asks for the advance-notice sentence, and
 * a fee reaches the balance only through VentureSettlementService. The
 * identity never written twice is (invoice, schedule, trigger, day), pinned
 * by late_fee.occurrence_key.
 *
 * Money is integer minor units throughout. A percent is parsed from its
 * decimal text into parts per million of the base (1.5% is 15000), so a
 * fee is base * ppm / 1,000,000, rounded once by the tier's rule.
 */

struct _VentureLateFeeService
{
	GObject parent_instance;
	VentureDatabase *database;
	VentureEntity *permit;
	gboolean actions;
};

G_DEFINE_FINAL_TYPE(VentureLateFeeService, venture_late_fee_service, G_TYPE_OBJECT)

#define LF_MAX_TIERS (20)
#define LF_MAX_DAY (365)
#define LF_PPM (1000000)

#define ST_SCHEDULED "scheduled"
#define ST_AWAITING "awaiting_notice"
#define ST_PROPOSED "proposed"
#define ST_APPLIED "applied"
#define ST_SKIPPED "skipped"
#define ST_WAIVED "waived"
#define ST_CANCELLED "cancelled"
#define ST_CAPPED "capped"

typedef enum { FEE_FLAT, FEE_PERCENT_OPEN, FEE_PERCENT_ORIGINAL, FEE_MARKUP } FeeType;
typedef enum { ROUND_HALF_EVEN, ROUND_UP, ROUND_DOWN, ROUND_WHOLE } RoundRule;

static const gchar *const fee_type_names[] = { "flat", "percent_open", "percent_original", "markup", NULL };
static const gchar *const round_names[] = { "half_even", "up", "down", "whole", NULL };

typedef struct
{
	guint position;
	gint64 day;
	gint64 every;
	gint64 times;
	FeeType type;
	VentureMoney *amount;
	gint64 ppm;
	VentureMoney *cap;
	RoundRule round;
	VentureMoney *balance_above;
	VentureMoney *balance_below;
	gchar **tags;
	gchar **invoice_types;
	GArray *product_ids;
	gchar *currency;
	gint64 min_prior_late;
	gboolean skip_if_promise;
} Tier;

static void
tier_free(gpointer data)
{
	Tier *t = data;
	g_clear_pointer(&t->amount, venture_money_free);
	g_clear_pointer(&t->cap, venture_money_free);
	g_clear_pointer(&t->balance_above, venture_money_free);
	g_clear_pointer(&t->balance_below, venture_money_free);
	g_clear_pointer(&t->tags, g_strfreev);
	g_clear_pointer(&t->invoice_types, g_strfreev);
	g_clear_pointer(&t->product_ids, g_array_unref);
	g_free(t->currency);
	g_free(t);
}

static gboolean
refuse(GError **error, const gchar *message)
{
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Late fees: %s", message);
	return FALSE;
}

static gint64
number(VentureEntity *e, const gchar *field)
{
	gint64 value = 0;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gboolean
flag(VentureEntity *e, const gchar *field)
{
	gboolean value = FALSE;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gchar *
text(VentureEntity *e, const gchar *field)
{
	gchar *value = NULL;
	g_object_get(e, field, &value, NULL);
	return value;
}

static GDateTime *
when(VentureEntity *e, const gchar *field)
{
	GDateTime *value = NULL;
	g_object_get(e, field, &value, NULL);
	return value;
}

static VentureMoney *
money(VentureEntity *e, const gchar *field)
{
	VentureMoney *value = NULL;
	g_object_get(e, field, &value, NULL);
	return value;
}

static gboolean
module_on(void)
{
	return venture_entity_registry_lookup(venture_entity_registry_get_default(), "late_fee") != G_TYPE_INVALID;
}

/* Calendar days from @due to @day, in UTC like the dunning offsets. */
static gint
calendar_days(GDateTime *day, GDateTime *due)
{
	GDate a, b;
	g_date_clear(&a, 1);
	g_date_clear(&b, 1);
	g_date_set_dmy(&a, (GDateDay)g_date_time_get_day_of_month(day), (GDateMonth)g_date_time_get_month(day), (GDateYear)g_date_time_get_year(day));
	g_date_set_dmy(&b, (GDateDay)g_date_time_get_day_of_month(due), (GDateMonth)g_date_time_get_month(due), (GDateYear)g_date_time_get_year(due));
	return g_date_days_between(&b, &a);
}

static GDateTime *
day_after(GDateTime *base, gint64 days)
{
	g_autoptr(GDateTime) midnight = g_date_time_new_utc(g_date_time_get_year(base), g_date_time_get_month(base),
		g_date_time_get_day_of_month(base), 0, 0, 0);
	return g_date_time_add_days(midnight, (gint)days);
}

static gchar *
date_text(GDateTime *at)
{
	return at != NULL ? g_date_time_format(at, "%Y-%m-%d") : g_strdup("");
}

/* --- Parsing -------------------------------------------------------------- */

/*
 * A percent as decimal text ("1.5", "10", "0.125"), at most four decimal
 * places, into parts per million. A JSON fraction is refused rather than
 * read through a double: 1.5 as a double is fine, 1.15 is not, and nobody
 * should have to know which.
 */
static gboolean
parse_percent_text(const gchar *raw, gint64 *ppm, GError **error)
{
	g_autofree gchar *copy = NULL;
	const gchar *p;
	gint64 whole = 0, frac = 0;
	gint decimals = 0;
	gboolean digits = FALSE;
	if (raw == NULL)
		return refuse(error, "a percent is required");
	copy = g_strstrip(g_strdup(raw));
	if (g_str_has_suffix(copy, "%"))
		copy[strlen(copy) - 1] = '\0';
	g_strstrip(copy);
	for (p = copy; *p >= '0' && *p <= '9'; p++)
	{
		whole = whole * 10 + (*p - '0');
		digits = TRUE;
		if (whole > 100)
			return refuse(error, "a percent cannot exceed 100");
	}
	if (*p == '.')
	{
		for (p++; *p >= '0' && *p <= '9'; p++)
		{
			if (++decimals > 4)
				return refuse(error, "a percent has at most four decimal places");
			frac = frac * 10 + (*p - '0');
			digits = TRUE;
		}
	}
	if (!digits || *p != '\0')
		return refuse(error, "a percent is written as decimal text, for example \"1.5\"");
	while (decimals++ < 4)
		frac *= 10;
	*ppm = whole * 10000 + frac;
	if (*ppm > LF_PPM)
		return refuse(error, "a percent cannot exceed 100");
	return TRUE;
}

static gboolean
parse_percent_node(JsonNode *node, gint64 *ppm, GError **error)
{
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
		return refuse(error, "a percent tier needs \"percent\"");
	if (json_node_get_value_type(node) == G_TYPE_INT64)
	{
		gint64 v = json_node_get_int(node);
		if (v < 0 || v > 100)
			return refuse(error, "a percent must be between 0 and 100");
		*ppm = v * 10000;
		return TRUE;
	}
	if (json_node_get_value_type(node) != G_TYPE_STRING)
		return refuse(error, "a fractional percent is written as text, for example \"1.5\", never a JSON fraction");
	return parse_percent_text(json_node_get_string(node), ppm, error);
}

static VentureMoney *
parse_money_member(JsonObject *object, const gchar *member, GError **error)
{
	JsonNode *node = json_object_get_member(object, member);
	VentureMoney *value;
	if (node == NULL || JSON_NODE_HOLDS_NULL(node))
		return NULL;
	if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Late fees: %s is money written with its currency, for example \"25.00 USD\"", member);
		return NULL;
	}
	value = venture_money_from_string(json_node_get_string(node), NULL, error);
	if (value != NULL && venture_money_is_negative(value))
	{
		venture_money_free(value);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Late fees: %s cannot be negative", member);
		return NULL;
	}
	return value;
}

static gboolean
json_int(JsonObject *object, const gchar *member, gint64 *value)
{
	JsonNode *node = json_object_get_member(object, member);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64)
		return FALSE;
	*value = json_node_get_int(node);
	return TRUE;
}

static gchar **
string_list(JsonObject *object, const gchar *member, GError **error)
{
	JsonNode *node = json_object_get_member(object, member);
	GPtrArray *out;
	JsonArray *array;
	guint i;
	if (node == NULL || JSON_NODE_HOLDS_NULL(node))
		return NULL;
	if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
	{
		gchar **parts = g_strsplit(json_node_get_string(node), ",", -1);
		for (i = 0; parts[i] != NULL; i++)
			g_strstrip(parts[i]);
		return parts;
	}
	if (!JSON_NODE_HOLDS_ARRAY(node))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Late fees: %s is a list of names", member);
		return NULL;
	}
	array = json_node_get_array(node);
	out = g_ptr_array_new();
	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonNode *element = json_array_get_element(array, i);
		if (!JSON_NODE_HOLDS_VALUE(element) || json_node_get_value_type(element) != G_TYPE_STRING)
		{
			g_ptr_array_free(out, TRUE);
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Late fees: %s is a list of names", member);
			return NULL;
		}
		g_ptr_array_add(out, g_strstrip(g_strdup(json_node_get_string(element))));
	}
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

static gint
name_index(const gchar *const *names, const gchar *name)
{
	gint i;
	for (i = 0; names[i] != NULL; i++)
		if (g_strcmp0(names[i], name) == 0)
			return i;
	return -1;
}

static gboolean
parse_conditions(Tier *t, JsonNode *node, GError **error)
{
	JsonObject *c;
	GError *local = NULL;
	t->min_prior_late = -1;
	if (node == NULL || JSON_NODE_HOLDS_NULL(node))
		return TRUE;
	if (!JSON_NODE_HOLDS_OBJECT(node))
		return refuse(error, "conditions is an object");
	c = json_node_get_object(node);
	t->balance_above = parse_money_member(c, "balance_above", &local);
	if (local == NULL)
		t->balance_below = parse_money_member(c, "balance_below", &local);
	if (local == NULL)
		t->tags = string_list(c, "customer_tags", &local);
	if (local == NULL && t->tags == NULL)
		t->tags = string_list(c, "customer_groups", &local);
	if (local == NULL)
		t->invoice_types = string_list(c, "invoice_types", &local);
	if (local != NULL)
	{
		g_propagate_error(error, local);
		return FALSE;
	}
	if (json_object_has_member(c, "product_ids"))
	{
		JsonNode *ids = json_object_get_member(c, "product_ids");
		guint i;
		if (!JSON_NODE_HOLDS_ARRAY(ids))
			return refuse(error, "product_ids is a list of product ids");
		t->product_ids = g_array_new(FALSE, FALSE, sizeof(gint64));
		for (i = 0; i < json_array_get_length(json_node_get_array(ids)); i++)
		{
			JsonNode *e = json_array_get_element(json_node_get_array(ids), i);
			gint64 id;
			if (!JSON_NODE_HOLDS_VALUE(e) || json_node_get_value_type(e) != G_TYPE_INT64)
				return refuse(error, "product_ids is a list of product ids");
			id = json_node_get_int(e);
			g_array_append_val(t->product_ids, id);
		}
	}
	if (json_object_has_member(c, "currency"))
	{
		JsonNode *cur = json_object_get_member(c, "currency");
		if (!JSON_NODE_HOLDS_VALUE(cur) || json_node_get_value_type(cur) != G_TYPE_STRING ||
			!venture_currency_is_valid(json_node_get_string(cur)))
			return refuse(error, "currency is a currency code");
		t->currency = g_strdup(json_node_get_string(cur));
	}
	if (json_object_has_member(c, "min_prior_late") &&
		(!json_int(c, "min_prior_late", &t->min_prior_late) || t->min_prior_late < 0))
		return refuse(error, "min_prior_late is a whole number of invoices");
	if (json_object_has_member(c, "skip_if_promise"))
	{
		JsonNode *b = json_object_get_member(c, "skip_if_promise");
		if (!JSON_NODE_HOLDS_VALUE(b) || json_node_get_value_type(b) != G_TYPE_BOOLEAN)
			return refuse(error, "skip_if_promise is true or false");
		t->skip_if_promise = json_node_get_boolean(b);
	}
	return TRUE;
}

static GPtrArray *
parse_tiers(const gchar *json, GError **error)
{
	g_autoptr(GPtrArray) tiers = g_ptr_array_new_with_free_func(tier_free);
	g_autoptr(JsonNode) root = NULL;
	JsonArray *array;
	guint i;
	if (venture_string_is_empty(json))
		return g_steal_pointer(&tiers);
	root = json_from_string(json, error);
	if (root == NULL)
		return NULL;
	if (!JSON_NODE_HOLDS_ARRAY(root))
	{
		refuse(error, "tiers must be a JSON array");
		return NULL;
	}
	array = json_node_get_array(root);
	if (json_array_get_length(array) > LF_MAX_TIERS)
	{
		refuse(error, "a schedule has at most 20 tiers");
		return NULL;
	}
	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonNode *node = json_array_get_element(array, i);
		JsonObject *o;
		Tier *t;
		gint index;
		const gchar *name;
		gint64 after = -1;
		GError *local = NULL;
		if (!JSON_NODE_HOLDS_OBJECT(node))
		{
			refuse(error, "each tier is an object");
			return NULL;
		}
		o = json_node_get_object(node);
		t = g_new0(Tier, 1);
		g_ptr_array_add(tiers, t);
		t->position = i + 1;
		t->day = -1;
		if (json_object_has_member(o, "day") && !json_int(o, "day", &t->day))
		{
			refuse(error, "day is a whole number of days late, written as a JSON integer");
			return NULL;
		}
		if (json_object_has_member(o, "every") && (!json_int(o, "every", &t->every) || t->every < 1))
		{
			refuse(error, "every is a whole number of days, at least 1");
			return NULL;
		}
		if (json_object_has_member(o, "after") && (!json_int(o, "after", &after) || after < 0))
		{
			refuse(error, "after is a whole number of days late");
			return NULL;
		}
		if (json_object_has_member(o, "times") && (!json_int(o, "times", &t->times) || t->times < 1))
		{
			refuse(error, "times is how many occurrences, at least 1");
			return NULL;
		}
		/* "every N days after day M": the first occurrence is M + N. */
		if (t->day < 0 && after >= 0 && t->every > 0)
			t->day = after + t->every;
		if (t->day < 1 || t->day > LF_MAX_DAY)
		{
			refuse(error, "each tier needs a day between 1 and 365 (or after and every)");
			return NULL;
		}
		name = json_object_get_string_member_with_default(o, "type", "flat");
		index = name_index(fee_type_names, name);
		if (index < 0)
		{
			refuse(error, "type is flat, percent_open, percent_original or markup");
			return NULL;
		}
		t->type = (FeeType)index;
		name = json_object_get_string_member_with_default(o, "round", "half_even");
		index = name_index(round_names, name);
		if (index < 0)
		{
			refuse(error, "round is half_even, up, down or whole");
			return NULL;
		}
		t->round = (RoundRule)index;
		if (t->type == FEE_FLAT)
		{
			t->amount = parse_money_member(o, "amount", &local);
			if (local == NULL && (t->amount == NULL || venture_money_is_zero(t->amount)))
				refuse(&local, "a flat tier needs a positive amount with its currency");
		}
		else
			parse_percent_node(json_object_get_member(o, "percent"), &t->ppm, &local);
		if (local == NULL)
			t->cap = parse_money_member(o, "cap", &local);
		if (local == NULL)
			parse_conditions(t, json_object_get_member(o, "conditions"), &local);
		if (local != NULL)
		{
			g_propagate_error(error, local);
			return NULL;
		}
	}
	return g_steal_pointer(&tiers);
}

/* --- Money ---------------------------------------------------------------- */

/* @base * @ppm / 1,000,000 by @rule, exactly; NULL on overflow. */
static VentureMoney *
percent_of(const VentureMoney *base, gint64 ppm, RoundRule rule, GError **error)
{
	gint64 amount = venture_money_get_amount(base);
	gint64 product, q, r;
	if (rule == ROUND_HALF_EVEN)
		return venture_money_multiply_rational(base, ppm, LF_PPM, error);
	if (rule == ROUND_WHOLE)
	{
		gint64 unit = 1;
		guint8 e;
		g_autoptr(VentureMoney) whole = NULL;
		for (e = 0; e < venture_money_get_exponent(base); e++)
			unit *= 10;
		whole = venture_money_multiply_rational(base, ppm, (gint64)LF_PPM * unit, error);
		return whole != NULL ? venture_money_multiply_int(whole, unit, error) : NULL;
	}
	if (ppm != 0 && (amount > G_MAXINT64 / ppm || amount < -(G_MAXINT64 / ppm)))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Late fees: the amount is too large");
		return NULL;
	}
	product = amount * ppm;
	q = product / LF_PPM;
	r = product % LF_PPM;
	if (rule == ROUND_UP && r > 0)
		q++;
	return venture_money_new(q, venture_money_get_currency(base), venture_money_get_exponent(base));
}

static VentureMoney *
zero_like(const VentureMoney *m)
{
	return venture_money_new(0, venture_money_get_currency(m), venture_money_get_exponent(m));
}

static gint64
minor_of(const VentureMoney *m)
{
	return m != NULL ? venture_money_get_amount(m) : 0;
}

static gboolean
same_currency(const VentureMoney *a, const VentureMoney *b)
{
	return a != NULL && b != NULL && g_strcmp0(venture_money_get_currency(a), venture_money_get_currency(b)) == 0;
}

/* --- Validators ----------------------------------------------------------- */

static gboolean
schedule_validate(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	g_autofree gchar *json = text(entity, "tiers");
	g_autofree gchar *percent = text(entity, "max-percent");
	g_autoptr(GPtrArray) tiers = NULL;
	gint64 ppm = 0;
	(void)db;
	(void)previous;
	(void)data;
	tiers = parse_tiers(json, error);
	if (tiers == NULL)
		return FALSE;
	if (!venture_string_is_empty(percent) && !parse_percent_text(percent, &ppm, error))
		return FALSE;
	if (number(entity, "grace-days") < 0 || number(entity, "grace-days") > LF_MAX_DAY)
		return refuse(error, "grace_days is between 0 and 365");
	return TRUE;
}

static gboolean
setting_validate(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_LATE_FEE_SETTING);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) enabled_at = when(entity, "enabled-at");
	guint i;
	(void)data;
	venture_query_set_organization(query, venture_entity_get_organization_id(entity));
	venture_query_set_limit(query, 0);
	rows = venture_database_find(db, query, error);
	if (rows == NULL)
		return FALSE;
	for (i = 0; i < rows->len; i++)
		if (venture_entity_get_id(g_ptr_array_index(rows, i)) != venture_entity_get_id(entity))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"Late fees: late_fee_setting #%" G_GINT64_FORMAT " already holds this organization's settings; edit it",
				venture_entity_get_id(g_ptr_array_index(rows, i)));
			return FALSE;
		}
	/* Switching late fees on over an existing ledger must not charge every
	 * overdue invoice for every occurrence it ever passed: the day it was
	 * switched on is when charging began. */
	if (flag(entity, "enabled") && enabled_at == NULL && (previous == NULL || !flag(previous, "enabled")))
	{
		g_autoptr(GDateTime) now = venture_time_now();
		g_autoptr(GDateTime) today = day_after(now, 0);
		g_object_set(entity, "enabled-at", today, NULL);
	}
	return TRUE;
}

static gboolean
fee_validate(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	VentureLateFeeService *self = data;
	(void)db;
	(void)previous;
	if (self->permit == entity)
		return TRUE;
	return refuse(error, "a late_fee is written by the dunning sweep and its approve, skip, skip_notice and waive actions");
}

static gboolean
save_fee(VentureLateFeeService *self, VentureEntity *fee, const VentureActor *actor, GError **error)
{
	gboolean ok;
	self->permit = fee;
	ok = venture_database_save(self->database, fee, actor, error);
	self->permit = NULL;
	return ok;
}

gboolean
venture_late_fee_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error)
{
	(void)database;
	if (!removal || record == NULL || !VENTURE_IS_LATE_FEE(record))
		return TRUE;
	return refuse(error, "late fee history cannot be removed; it is what stops a fee being charged twice");
}

/* --- One invoice's case --------------------------------------------------- */

typedef struct
{
	VentureEntity *invoice;
	VentureEntity *company;
	VentureEntity *settings;
	VentureEntity *schedule;
	GPtrArray *tiers;
	GDateTime *due;
	VentureMoney *original;
	VentureMoney *balance;
	VentureMoney *principal;
	VentureMoney *charged;
	GPtrArray *fees;
	GHashTable *by_key;
	gint start_day;
	gint days;
	gboolean exempt;
	const gchar *pause_scope;
	GDateTime *paused_until;
	gboolean paused;
	gboolean promise;
	gint64 prior_late;
	gboolean prior_known;
} Case;

static void
case_clear(Case *c)
{
	g_clear_object(&c->invoice);
	g_clear_object(&c->company);
	g_clear_object(&c->settings);
	g_clear_object(&c->schedule);
	g_clear_pointer(&c->tiers, g_ptr_array_unref);
	g_clear_pointer(&c->due, g_date_time_unref);
	g_clear_pointer(&c->original, venture_money_free);
	g_clear_pointer(&c->balance, venture_money_free);
	g_clear_pointer(&c->principal, venture_money_free);
	g_clear_pointer(&c->charged, venture_money_free);
	g_clear_pointer(&c->fees, g_ptr_array_unref);
	g_clear_pointer(&c->by_key, g_hash_table_unref);
	g_clear_pointer(&c->paused_until, g_date_time_unref);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(Case, case_clear)

static VentureEntity *
load(VentureLateFeeService *self, GType type, gint64 id, gint64 org)
{
	g_autoptr(VentureEntity) row = NULL;
	if (id <= 0)
		return NULL;
	row = venture_database_get(self->database, type, id, NULL);
	if (row == NULL || venture_entity_is_deleted(row) || venture_entity_get_organization_id(row) != org)
		return NULL;
	return g_steal_pointer(&row);
}

static VentureEntity *
settings_for(VentureLateFeeService *self, gint64 org)
{
	g_autoptr(VentureQuery) query = NULL;
	if (!module_on())
		return NULL;
	query = venture_query_new(VENTURE_TYPE_LATE_FEE_SETTING);
	venture_query_set_organization(query, org);
	return venture_database_find_one(self->database, query, NULL);
}

static gboolean
contains_name(gchar **list, const gchar *wanted)
{
	guint i;
	for (i = 0; list != NULL && list[i] != NULL; i++)
		if (list[i][0] != '\0' && wanted != NULL && g_ascii_strcasecmp(list[i], wanted) == 0)
			return TRUE;
	return FALSE;
}

/* The invoice's own schedule, its customer's, the one that names its
 * invoice type, then the organization default. */
static VentureEntity *
resolve_schedule(VentureLateFeeService *self, VentureEntity *invoice, VentureEntity *company, VentureEntity *settings)
{
	gint64 org = venture_entity_get_organization_id(invoice);
	g_autofree gchar *type = text(invoice, "invoice-type");
	VentureEntity *found;
	if ((found = load(self, VENTURE_TYPE_LATE_FEE_SCHEDULE, number(invoice, "late-fee-schedule-id"), org)) != NULL)
		return found;
	if (company != NULL && (found = load(self, VENTURE_TYPE_LATE_FEE_SCHEDULE, number(company, "late-fee-schedule-id"), org)) != NULL)
		return found;
	if (!venture_string_is_empty(type))
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_LATE_FEE_SCHEDULE);
		g_autoptr(GPtrArray) rows = NULL;
		guint i;
		venture_query_set_organization(query, org);
		venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
		venture_query_set_limit(query, 0);
		rows = venture_database_find(self->database, query, NULL);
		for (i = 0; rows != NULL && i < rows->len; i++)
		{
			g_autofree gchar *names = text(g_ptr_array_index(rows, i), "invoice-types");
			g_auto(GStrv) list = names != NULL ? g_strsplit(names, ",", -1) : NULL;
			guint j;
			for (j = 0; list != NULL && list[j] != NULL; j++)
				g_strstrip(list[j]);
			if (contains_name(list, type))
				return g_object_ref(g_ptr_array_index(rows, i));
		}
	}
	return settings != NULL ? load(self, VENTURE_TYPE_LATE_FEE_SCHEDULE, number(settings, "default-schedule-id"), org) : NULL;
}

/* Whether @row's pause holds on @at: paused, and either no end or an end
 * after @at. */
static gboolean
pause_holds(VentureEntity *row, const gchar *flag_field, const gchar *until_field, GDateTime *at, GDateTime **until)
{
	g_autoptr(GDateTime) end = NULL;
	if (row == NULL || !flag(row, flag_field))
		return FALSE;
	end = when(row, until_field);
	if (end != NULL && calendar_days(end, at) <= 0)
		return FALSE;
	if (until != NULL)
		*until = end != NULL ? g_date_time_ref(end) : NULL;
	return TRUE;
}

static void
case_pause(Case *c, GDateTime *at)
{
	if (pause_holds(c->invoice, "late-fees-paused", "late-fees-paused-until", at, &c->paused_until))
		c->pause_scope = "invoice";
	else if (pause_holds(c->company, "late-fees-paused", "late-fees-paused-until", at, &c->paused_until))
		c->pause_scope = "company";
	else if (pause_holds(c->settings, "paused", "paused-until", at, &c->paused_until))
		c->pause_scope = "organization";
	c->paused = c->pause_scope != NULL;
}

static GPtrArray *
fees_of(VentureLateFeeService *self, gint64 invoice_id, gint64 org)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_LATE_FEE);
	venture_query_set_organization(query, org);
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, invoice_id, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 0);
	return venture_database_find(self->database, query, NULL);
}

static gboolean
status_is(VentureEntity *fee, const gchar *status)
{
	g_autofree gchar *s = text(fee, "status");
	return g_strcmp0(s, status) == 0;
}

/* Rows that hold cap room: charged, or on their way to it. */
static gboolean
commits(VentureEntity *fee)
{
	return status_is(fee, ST_APPLIED) || status_is(fee, ST_PROPOSED);
}

static gboolean
pending(VentureEntity *fee)
{
	return status_is(fee, ST_SCHEDULED) || status_is(fee, ST_AWAITING) || status_is(fee, ST_PROPOSED);
}

static gint64
start_day(Case *c)
{
	g_autoptr(GDateTime) enabled = c->settings != NULL ? when(c->settings, "enabled-at") : NULL;
	g_autoptr(GDateTime) adopted = c->schedule != NULL ? when(c->schedule, "adopted-at") : NULL;
	gint64 day = G_MININT;
	guint i;
	if (enabled == NULL && c->settings != NULL && venture_entity_get_created_at(c->settings) != NULL)
		enabled = g_date_time_ref(venture_entity_get_created_at(c->settings));
	if (adopted == NULL && c->schedule != NULL && venture_entity_get_created_at(c->schedule) != NULL)
		adopted = g_date_time_ref(venture_entity_get_created_at(c->schedule));
	if (enabled != NULL)
		day = MAX(day, calendar_days(enabled, c->due));
	if (adopted != NULL)
		day = MAX(day, calendar_days(adopted, c->due));
	/* A new schedule takes over after the last day the old one decided: what
	 * was charged stays, and the new schedule's past never comes due. */
	for (i = 0; c->fees != NULL && c->schedule != NULL && i < c->fees->len; i++)
	{
		VentureEntity *fee = g_ptr_array_index(c->fees, i);
		if (number(fee, "schedule-id") != venture_entity_get_id(c->schedule) && !status_is(fee, ST_CANCELLED))
			day = MAX(day, number(fee, "trigger-day") + 1);
	}
	return day;
}

static gboolean
case_open(VentureLateFeeService *self, Case *c, VentureEntity *invoice, GDateTime *at, GError **error)
{
	gint64 org = venture_entity_get_organization_id(invoice);
	VentureSettlementService *settlement = venture_settlement_service_get(self->database);
	g_autoptr(GPtrArray) events = NULL;
	guint i;
	memset(c, 0, sizeof(*c));
	c->invoice = g_object_ref(invoice);
	c->company = load(self, VENTURE_TYPE_COMPANY, number(invoice, "company-id"), org);
	c->settings = settings_for(self, org);
	c->due = when(invoice, "due-at");
	c->exempt = flag(invoice, "late-fee-exempt") || (c->company != NULL && flag(c->company, "late-fee-exempt"));
	case_pause(c, at);
	if (c->due == NULL)
		return TRUE;
	c->days = calendar_days(at, c->due);
	c->schedule = resolve_schedule(self, invoice, c->company, c->settings);
	if (c->schedule != NULL)
	{
		g_autofree gchar *json = text(c->schedule, "tiers");
		c->tiers = parse_tiers(json, error);
		if (c->tiers == NULL)
			return FALSE;
	}
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE_EVENT);
		venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(invoice), NULL);
		venture_query_set_include_deleted(query, TRUE);
		venture_query_set_limit(query, 0);
		events = venture_database_find(self->database, query, error);
		if (events == NULL)
			return FALSE;
	}
	for (i = 0; i < events->len; i++)
	{
		g_autofree gchar *kind = text(g_ptr_array_index(events, i), "kind");
		if (g_strcmp0(kind, "issue") == 0)
			c->original = money(g_ptr_array_index(events, i), "amount");
	}
	if (c->original == NULL)
		return TRUE;
	c->balance = venture_settlement_service_invoice_balance(settlement, venture_entity_get_id(invoice), NULL, error);
	if (c->balance == NULL)
		return FALSE;
	c->fees = fees_of(self, venture_entity_get_id(invoice), org);
	c->by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	c->charged = zero_like(c->original);
	for (i = 0; c->fees != NULL && i < c->fees->len; i++)
	{
		VentureEntity *fee = g_ptr_array_index(c->fees, i);
		g_hash_table_insert(c->by_key, text(fee, "occurrence-key"), g_object_ref(fee));
		if (status_is(fee, ST_APPLIED))
		{
			g_autoptr(VentureMoney) amount = money(fee, "amount");
			VentureMoney *next = same_currency(amount, c->charged) ? venture_money_add(c->charged, amount, NULL) : NULL;
			if (next != NULL)
			{
				venture_money_free(c->charged);
				c->charged = next;
			}
		}
	}
	/* What is still owed on the invoice itself: the balance less fees
	 * charged and not waived (a waiver's credit already left the balance). */
	{
		gint64 principal = minor_of(c->balance) - minor_of(c->charged);
		if (principal < 0)
			principal = 0;
		c->principal = venture_money_new(principal, venture_money_get_currency(c->original), venture_money_get_exponent(c->original));
	}
	c->start_day = (gint)CLAMP(start_day(c), G_MININT / 2, G_MAXINT / 2);
	{
		const gchar *hold = venture_dunning_service_hold_reason(venture_dunning_service_get(self->database), invoice, at);
		c->promise = hold != NULL;
	}
	return TRUE;
}

/* Other invoices of the customer, due before this one, that were paid
 * after their due date or are still open past it. */
static gint64
prior_late(VentureLateFeeService *self, Case *c, GDateTime *at)
{
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	gint64 n = 0;
	if (c->prior_known)
		return c->prior_late;
	c->prior_known = TRUE;
	if (c->company == NULL)
		return 0;
	query = venture_query_new(VENTURE_TYPE_INVOICE);
	venture_query_set_organization(query, venture_entity_get_organization_id(c->invoice));
	venture_query_add_filter_int(query, "company-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(c->company), NULL);
	venture_query_set_limit(query, 1000);
	rows = venture_database_find(self->database, query, NULL);
	for (i = 0; rows != NULL && i < rows->len; i++)
	{
		VentureEntity *other = g_ptr_array_index(rows, i);
		g_autoptr(GDateTime) due = when(other, "due-at");
		g_autoptr(GDateTime) paid = when(other, "paid-at");
		gint status = 0;
		if (venture_entity_get_id(other) == venture_entity_get_id(c->invoice) || due == NULL ||
			calendar_days(due, c->due) >= 0)
			continue;
		g_object_get(other, "status", &status, NULL);
		if (paid != NULL && calendar_days(paid, due) > 0)
			n++;
		else if (paid == NULL && (status == VENTURE_INVOICE_STATUS_SENT || status == VENTURE_INVOICE_STATUS_PARTIALLY_PAID) &&
			calendar_days(at, due) > 0)
			n++;
	}
	c->prior_late = n;
	return n;
}

static gboolean
invoice_has_product(VentureLateFeeService *self, Case *c, GArray *ids)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INVOICE_LINE);
	g_autoptr(GPtrArray) lines = NULL;
	guint i, j;
	venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(c->invoice), NULL);
	venture_query_set_limit(query, 0);
	lines = venture_database_find(self->database, query, NULL);
	for (i = 0; lines != NULL && i < lines->len; i++)
		for (j = 0; j < ids->len; j++)
			if (number(g_ptr_array_index(lines, i), "product-id") == g_array_index(ids, gint64, j))
				return TRUE;
	return FALSE;
}

/* NULL when every condition holds, else why not. */
static const gchar *
condition_fails(VentureLateFeeService *self, Case *c, const Tier *t, GDateTime *at)
{
	const gchar *currency = venture_money_get_currency(c->original);
	if (t->type == FEE_FLAT && !same_currency(t->amount, c->original))
		return "the tier's amount is in another currency";
	if (t->cap != NULL && !same_currency(t->cap, c->original))
		return "the tier's cap is in another currency";
	if (t->currency != NULL && g_ascii_strcasecmp(t->currency, currency) != 0)
		return "currency";
	if (t->balance_above != NULL && (!same_currency(t->balance_above, c->principal) ||
		minor_of(c->principal) <= minor_of(t->balance_above)))
		return "balance is not above the tier's amount";
	if (t->balance_below != NULL && (!same_currency(t->balance_below, c->principal) ||
		minor_of(c->principal) >= minor_of(t->balance_below)))
		return "balance is not below the tier's amount";
	if (t->tags != NULL)
	{
		g_autofree gchar *raw = c->company != NULL ? text(c->company, "tags") : NULL;
		g_auto(GStrv) tags = raw != NULL ? g_strsplit(raw, ",", -1) : NULL;
		gboolean any = FALSE;
		guint i;
		for (i = 0; tags != NULL && tags[i] != NULL; i++)
		{
			g_strstrip(tags[i]);
			if (contains_name(t->tags, tags[i]))
				any = TRUE;
		}
		if (!any)
			return "customer tag";
	}
	if (t->invoice_types != NULL)
	{
		g_autofree gchar *type = text(c->invoice, "invoice-type");
		if (!contains_name(t->invoice_types, type))
			return "invoice type";
	}
	if (t->product_ids != NULL && t->product_ids->len > 0 && !invoice_has_product(self, c, t->product_ids))
		return "service line";
	if (t->min_prior_late >= 0 && prior_late(self, c, at) < t->min_prior_late)
		return "too few prior late invoices";
	if (t->skip_if_promise && c->promise)
		return "a payment plan or promise to pay is active";
	return NULL;
}

/* --- Occurrences ---------------------------------------------------------- */

typedef struct
{
	const Tier *tier;
	gint64 day;
	gchar *key;
} Occurrence;

static void
occurrence_clear(gpointer data)
{
	Occurrence *o = data;
	g_free(o->key);
}

static gint
occurrence_compare(gconstpointer a, gconstpointer b)
{
	const Occurrence *x = a, *y = b;
	if (x->day != y->day)
		return x->day < y->day ? -1 : 1;
	return x->tier->position < y->tier->position ? -1 : (x->tier->position > y->tier->position ? 1 : 0);
}

/* The key names the trigger, not the tier's position: inserting a tier
 * before it must not make an occurrence chargeable twice. */
static gchar *
occurrence_key(Case *c, const Tier *t, gint64 day)
{
	return g_strdup_printf("latefee:inv:%" G_GINT64_FORMAT ":schedule:%" G_GINT64_FORMAT ":trigger:%" G_GINT64_FORMAT
		"/%" G_GINT64_FORMAT ":%s:day:%" G_GINT64_FORMAT, venture_entity_get_id(c->invoice),
		venture_entity_get_id(c->schedule), t->day, t->every, fee_type_names[t->type], day);
}

static GArray *
occurrences(Case *c)
{
	GArray *out = g_array_new(FALSE, FALSE, sizeof(Occurrence));
	gint64 grace = c->schedule != NULL ? number(c->schedule, "grace-days") : 0;
	guint i;
	g_array_set_clear_func(out, occurrence_clear);
	for (i = 0; c->tiers != NULL && i < c->tiers->len; i++)
	{
		const Tier *t = g_ptr_array_index(c->tiers, i);
		gint64 day;
		gint64 n = 0;
		for (day = t->day; day <= LF_MAX_DAY; day += t->every)
		{
			Occurrence o;
			if (t->times > 0 && n >= t->times)
				break;
			n++;
			if (day > grace)
			{
				o.tier = t;
				o.day = day;
				o.key = occurrence_key(c, t, day);
				g_array_append_val(out, o);
			}
			if (t->every <= 0)
				break;
		}
	}
	g_array_sort(out, occurrence_compare);
	return out;
}

static gint64
minor(VentureEntity *fee)
{
	g_autoptr(VentureMoney) amount = money(fee, "amount");
	return minor_of(amount);
}

/* Running totals while walking the timeline: everything committed so far
 * (charged, proposed, or in a preview, assumed), and per trigger. */
typedef struct
{
	VentureMoney *committed;
	GHashTable *per_tier;
} Running;

static void
running_init(Running *r, Case *c)
{
	guint i;
	r->committed = zero_like(c->original);
	r->per_tier = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
	for (i = 0; c->fees != NULL && i < c->fees->len; i++)
	{
		VentureEntity *fee = g_ptr_array_index(c->fees, i);
		g_autoptr(VentureMoney) amount = money(fee, "amount");
		if (commits(fee) && same_currency(amount, r->committed))
			r->committed->amount += minor_of(amount);
	}
}

static void
running_clear(Running *r)
{
	g_clear_pointer(&r->committed, venture_money_free);
	g_clear_pointer(&r->per_tier, g_hash_table_unref);
}

static gint64 *
tier_slot(Running *r, Case *c, const Tier *t)
{
	gint64 *slot = g_hash_table_lookup(r->per_tier, t);
	guint i;
	/* Recorded fees of the same trigger, read once into the table. */
	if (slot == NULL)
	{
		g_autofree gchar *prefix = g_strdup_printf("latefee:inv:%" G_GINT64_FORMAT ":schedule:%" G_GINT64_FORMAT ":trigger:%" G_GINT64_FORMAT
			"/%" G_GINT64_FORMAT ":%s:", venture_entity_get_id(c->invoice), venture_entity_get_id(c->schedule), t->day, t->every,
			fee_type_names[t->type]);
		slot = g_new0(gint64, 1);
		for (i = 0; c->fees != NULL && i < c->fees->len; i++)
		{
			VentureEntity *fee = g_ptr_array_index(c->fees, i);
			g_autofree gchar *key = text(fee, "occurrence-key");
			if (commits(fee) && g_str_has_prefix(key, prefix))
				*slot += minor(fee);
		}
		g_hash_table_insert(r->per_tier, (gpointer)t, slot);
	}
	return slot;
}

static gint64
tier_total(Running *r, Case *c, const Tier *t)
{
	return *tier_slot(r, c, t);
}

static void
running_add(Running *r, Case *c, const Tier *t, gint64 amount)
{
	*tier_slot(r, c, t) += amount;
	r->committed->amount += amount;
}

/* "1.5", "10", "0.125": parts per million as a person writes a percent. */
static gchar *
percent_display(gint64 ppm)
{
	gint64 frac = ppm % 10000;
	gchar digits[8];
	gint end;
	if (frac == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT, ppm / 10000);
	g_snprintf(digits, sizeof(digits), "%04" G_GINT64_FORMAT, frac);
	for (end = 3; end > 0 && digits[end] == '0'; end--)
		digits[end] = '\0';
	return g_strdup_printf("%" G_GINT64_FORMAT ".%s", ppm / 10000, digits);
}

/* How a tier reaches its amount, the same words wherever it is shown. */
static gchar *
tier_basis(const Tier *t)
{
	g_autofree gchar *percent = NULL;
	if (t->type == FEE_FLAT)
	{
		g_autofree gchar *shown = venture_money_to_display_string(t->amount, TRUE);
		return g_strdup_printf("flat %s", shown);
	}
	percent = percent_display(t->ppm);
	return g_strdup_printf("%s%% of %s", percent,
		t->type == FEE_PERCENT_ORIGINAL ? "the original invoice" : (t->type == FEE_MARKUP ? "everything outstanding" : "the open balance"));
}

/*
 * What @o would charge with @r committed before it: the raw amount, then
 * the tier cap, the schedule's absolute cap and its percent-of-original
 * cap, each cutting the fee to the room left. 0 is a capped occurrence.
 */
static gint64
occurrence_amount(Case *c, Running *r, const Occurrence *o, gchar **basis, GError **error)
{
	const Tier *t = o->tier;
	g_autoptr(VentureMoney) raw = NULL;
	g_autoptr(VentureMoney) max_total = c->schedule != NULL ? money(c->schedule, "max-total") : NULL;
	g_autofree gchar *max_percent = c->schedule != NULL ? text(c->schedule, "max-percent") : NULL;
	gint64 fee;
	switch (t->type)
	{
	case FEE_FLAT:
		raw = venture_money_copy(t->amount);
		break;
	case FEE_PERCENT_OPEN:
		raw = percent_of(c->principal, t->ppm, t->round, error);
		break;
	case FEE_PERCENT_ORIGINAL:
		raw = percent_of(c->original, t->ppm, t->round, error);
		break;
	case FEE_MARKUP:
	default:
		{
			g_autoptr(VentureMoney) outstanding = venture_money_new(minor_of(c->principal) + minor_of(r->committed),
				venture_money_get_currency(c->principal), venture_money_get_exponent(c->principal));
			raw = percent_of(outstanding, t->ppm, t->round, error);
		}
		break;
	}
	if (raw == NULL)
		return -1;
	fee = minor_of(raw);
	if (basis != NULL)
		*basis = tier_basis(t);
	if (t->cap != NULL)
		fee = MIN(fee, MAX(0, minor_of(t->cap) - tier_total(r, c, t)));
	if (max_total != NULL && same_currency(max_total, c->original))
		fee = MIN(fee, MAX(0, minor_of(max_total) - minor_of(r->committed)));
	if (!venture_string_is_empty(max_percent))
	{
		gint64 ppm = 0;
		if (parse_percent_text(max_percent, &ppm, NULL))
		{
			g_autoptr(VentureMoney) limit = venture_money_multiply_rational(c->original, ppm, LF_PPM, NULL);
			if (limit != NULL)
				fee = MIN(fee, MAX(0, minor_of(limit) - minor_of(r->committed)));
		}
	}
	return MAX(fee, 0);
}

static VentureMoney *
fee_money(Case *c, gint64 amount)
{
	return venture_money_new(amount, venture_money_get_currency(c->original), venture_money_get_exponent(c->original));
}

/* Whether the occurrence's advance notice was sent before its day. */
static const gchar *
notice_state(VentureLateFeeService *self, VentureEntity *fee, gint64 due_day, Case *c)
{
	g_autoptr(VentureEntity) event = NULL;
	g_autofree gchar *status = NULL;
	g_autoptr(GDateTime) sent_at = NULL;
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "dunning_event");
	if (fee == NULL)
		return "none";
	if (flag(fee, "notice-skipped"))
		return "skipped";
	if (type == G_TYPE_INVALID || number(fee, "notice-event-id") <= 0)
		return "none";
	event = load(self, type, number(fee, "notice-event-id"), venture_entity_get_organization_id(fee));
	if (event == NULL)
		return "none";
	status = text(event, "delivery-status");
	if (g_strcmp0(status, "sent") != 0)
		return g_strcmp0(status, "queued") == 0 ? "queued" : "none";
	/* Sent on or after the fee's own day is not advance notice. */
	sent_at = when(event, "sent-at");
	if (sent_at != NULL && c->due != NULL && calendar_days(sent_at, c->due) >= due_day)
		return "late";
	return "sent";
}

static gboolean
fees_enabled(Case *c)
{
	return c->settings != NULL && flag(c->settings, "enabled");
}

static gboolean
invoice_open(Case *c)
{
	gint status = 0;
	g_autofree gchar *workflow = NULL;
	g_object_get(c->invoice, "status", &status, "workflow-state", &workflow, NULL);
	return (status == VENTURE_INVOICE_STATUS_SENT || status == VENTURE_INVOICE_STATUS_PARTIALLY_PAID) &&
		g_strcmp0(workflow, "disputed") != 0 && c->balance != NULL && minor_of(c->balance) > 0;
}

/* --- Preview -------------------------------------------------------------- */

JsonNode *
venture_late_fee_service_preview(VentureLateFeeService *self, gint64 invoice_id, GDateTime *as_of, GError **error)
{
	g_auto(Case) c;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GArray) occ = NULL;
	g_autoptr(JsonBuilder) b = json_builder_new();
	GDateTime *at = as_of != NULL ? as_of : now;
	Running r;
	guint i;
	memset(&c, 0, sizeof(c));
	g_return_val_if_fail(VENTURE_IS_LATE_FEE_SERVICE(self), NULL);
	if (!module_on())
	{
		refuse(error, "the late_fees module is disabled");
		return NULL;
	}
	invoice = venture_database_get(self->database, VENTURE_TYPE_INVOICE, invoice_id, error);
	if (invoice == NULL)
	{
		if (error != NULL && *error == NULL)
			refuse(error, "no such invoice");
		return NULL;
	}
	if (!case_open(self, &c, invoice, at, error))
		return NULL;
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "invoice_id");
	json_builder_add_int_value(b, invoice_id);
	json_builder_set_member_name(b, "enabled");
	json_builder_add_boolean_value(b, fees_enabled(&c));
	json_builder_set_member_name(b, "auto_apply");
	json_builder_add_boolean_value(b, c.settings != NULL && flag(c.settings, "auto-apply"));
	json_builder_set_member_name(b, "schedule_id");
	json_builder_add_int_value(b, c.schedule != NULL ? venture_entity_get_id(c.schedule) : 0);
	json_builder_set_member_name(b, "schedule_name");
	{
		g_autofree gchar *name = c.schedule != NULL ? text(c.schedule, "name") : NULL;
		json_builder_add_string_value(b, name != NULL ? name : "");
	}
	json_builder_set_member_name(b, "exempt");
	json_builder_add_boolean_value(b, c.exempt);
	json_builder_set_member_name(b, "paused");
	json_builder_add_boolean_value(b, c.paused);
	if (c.paused)
	{
		g_autofree gchar *until = date_text(c.paused_until);
		json_builder_set_member_name(b, "pause_scope");
		json_builder_add_string_value(b, c.pause_scope);
		json_builder_set_member_name(b, "paused_until");
		json_builder_add_string_value(b, until);
	}
	json_builder_set_member_name(b, "days_late");
	json_builder_add_int_value(b, c.days);
	if (c.original != NULL)
	{
		g_autofree gchar *charged = venture_money_to_string(c.charged);
		g_autofree gchar *original = venture_money_to_string(c.original);
		json_builder_set_member_name(b, "currency");
		json_builder_add_string_value(b, venture_money_get_currency(c.original));
		json_builder_set_member_name(b, "original");
		json_builder_add_string_value(b, original);
		json_builder_set_member_name(b, "charged");
		json_builder_add_string_value(b, charged);
	}
	json_builder_set_member_name(b, "entries");
	json_builder_begin_array(b);
	if (c.original != NULL && c.schedule != NULL)
	{
		g_autoptr(VentureMoney) minimum = money(c.schedule, "minimum-balance");
		occ = occurrences(&c);
		running_init(&r, &c);
		for (i = 0; i < occ->len; i++)
		{
			const Occurrence *o = &g_array_index(occ, Occurrence, i);
			VentureEntity *fee = g_hash_table_lookup(c.by_key, o->key);
			g_autoptr(GDateTime) date = day_after(c.due, o->day);
			g_autofree gchar *day_text = date_text(date);
			g_autofree gchar *basis = NULL;
			g_autoptr(VentureMoney) amount = NULL;
			g_autofree gchar *exact = NULL;
			g_autofree gchar *display = NULL;
			g_autofree gchar *status = NULL;
			const gchar *reason = NULL;
			gint64 fee_minor;
			if (fee != NULL)
			{
				status = text(fee, "status");
				amount = money(fee, "amount");
				basis = text(fee, "basis");
				reason = NULL;
				if (venture_string_is_empty(basis))
				{
					g_free(basis);
					basis = tier_basis(o->tier);
				}
				/* A recorded fee still to come holds its room under the caps
				 * exactly as it will when charged; applied and proposed ones
				 * are already in the running total. Leaving it out is how a
				 * preview showed 115.00 of fees under a 100.00 cap. */
				if ((status_is(fee, ST_SCHEDULED) || status_is(fee, ST_AWAITING)) && amount != NULL)
					running_add(&r, &c, o->tier, minor_of(amount));
			}
			else if (o->day < c.start_day)
			{
				status = g_strdup("before_start");
				reason = "before late fees governed this invoice";
			}
			else if ((reason = condition_fails(self, &c, o->tier, at)) != NULL)
				status = g_strdup("skipped_condition");
			else if (minimum != NULL && same_currency(minimum, c.principal) && minor_of(c.principal) < minor_of(minimum))
			{
				status = g_strdup("skipped_condition");
				reason = "balance is below the schedule's minimum";
			}
			else
			{
				fee_minor = occurrence_amount(&c, &r, o, &basis, error);
				if (fee_minor < 0)
					return NULL;
				amount = fee_money(&c, fee_minor);
				if (fee_minor == 0)
				{
					status = g_strdup("capped");
					reason = "a cap is reached";
				}
				else
				{
					status = g_strdup(o->day <= c.days ? "due" : "upcoming");
					running_add(&r, &c, o->tier, fee_minor);
				}
			}
			json_builder_begin_object(b);
			json_builder_set_member_name(b, "day");
			json_builder_add_int_value(b, o->day);
			json_builder_set_member_name(b, "date");
			json_builder_add_string_value(b, day_text);
			json_builder_set_member_name(b, "tier");
			json_builder_add_int_value(b, o->tier->position);
			json_builder_set_member_name(b, "type");
			json_builder_add_string_value(b, fee_type_names[o->tier->type]);
			if (amount != NULL)
			{
				exact = venture_money_to_string(amount);
				display = venture_money_to_display_string(amount, TRUE);
				json_builder_set_member_name(b, "amount");
				json_builder_add_string_value(b, exact);
				json_builder_set_member_name(b, "display");
				json_builder_add_string_value(b, display);
			}
			json_builder_set_member_name(b, "status");
			json_builder_add_string_value(b, status);
			json_builder_set_member_name(b, "recurring");
			json_builder_add_boolean_value(b, o->tier->every > 0);
			if (basis != NULL)
			{
				json_builder_set_member_name(b, "basis");
				json_builder_add_string_value(b, basis);
			}
			if (reason != NULL)
			{
				json_builder_set_member_name(b, "reason");
				json_builder_add_string_value(b, reason);
			}
			json_builder_set_member_name(b, "notice");
			json_builder_add_string_value(b, notice_state(self, fee, o->day, &c));
			if (fee != NULL)
			{
				json_builder_set_member_name(b, "late_fee_id");
				json_builder_add_int_value(b, venture_entity_get_id(fee));
			}
			json_builder_end_object(b);
		}
		running_clear(&r);
	}
	json_builder_end_array(b);
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

/* --- Notice --------------------------------------------------------------- */

/* The next chargeable occurrence strictly after today, with its amount. */
static gboolean
next_fee(VentureLateFeeService *self, Case *c, GDateTime *at, const Occurrence **found, GArray **keep, gint64 *amount)
{
	g_autoptr(VentureMoney) minimum = NULL;
	Running r;
	guint i;
	*found = NULL;
	if (!fees_enabled(c) || c->paused || c->exempt || c->schedule == NULL || c->original == NULL || !invoice_open(c))
		return FALSE;
	minimum = money(c->schedule, "minimum-balance");
	if (minimum != NULL && same_currency(minimum, c->principal) && minor_of(c->principal) < minor_of(minimum))
		return FALSE;
	*keep = occurrences(c);
	running_init(&r, c);
	for (i = 0; i < (*keep)->len; i++)
	{
		const Occurrence *o = &g_array_index(*keep, Occurrence, i);
		VentureEntity *fee = g_hash_table_lookup(c->by_key, o->key);
		gint64 value;
		if (o->day <= c->days || o->day < c->start_day)
			continue;
		if (fee != NULL && !status_is(fee, ST_SCHEDULED) && !status_is(fee, ST_AWAITING))
			continue;
		if (condition_fails(self, c, o->tier, at) != NULL)
			continue;
		value = occurrence_amount(c, &r, o, NULL, NULL);
		if (value <= 0)
			continue;
		*found = o;
		*amount = value;
		break;
	}
	running_clear(&r);
	return *found != NULL;
}

gchar *
venture_late_fee_service_notice_text(VentureLateFeeService *self, VentureEntity *invoice, GDateTime *as_of)
{
	g_auto(Case) c;
	g_autoptr(GArray) keep = NULL;
	const Occurrence *o = NULL;
	gint64 amount = 0;
	memset(&c, 0, sizeof(c));
	g_return_val_if_fail(VENTURE_IS_LATE_FEE_SERVICE(self), g_strdup(""));
	if (!module_on() || invoice == NULL || !case_open(self, &c, invoice, as_of, NULL) ||
		!next_fee(self, &c, as_of, &o, &keep, &amount))
		return g_strdup("");
	{
		g_autoptr(VentureMoney) m = fee_money(&c, amount);
		g_autofree gchar *shown = venture_money_to_display_string(m, TRUE);
		g_autoptr(GDateTime) date = day_after(c.due, o->day);
		g_autofree gchar *day = date_text(date);
		return g_strdup_printf("A %s late fee will be added on %s.", shown, day);
	}
}

static VentureEntity *
new_fee(Case *c, const Occurrence *o, const gchar *status)
{
	VentureEntity *fee = g_object_new(VENTURE_TYPE_LATE_FEE, NULL);
	g_autoptr(GDateTime) due_on = day_after(c->due, o->day);
	venture_entity_set_organization_id(fee, venture_entity_get_organization_id(c->invoice));
	g_object_set(fee, "invoice-id", venture_entity_get_id(c->invoice),
		"company-id", c->company != NULL ? venture_entity_get_id(c->company) : (gint64)0,
		"schedule-id", venture_entity_get_id(c->schedule), "tier", (gint64)o->tier->position,
		"trigger-day", o->day, "occurrence-key", o->key, "due-on", due_on, "status", status, NULL);
	return fee;
}

gboolean
venture_late_fee_service_record_notice(VentureLateFeeService *self, VentureEntity *invoice, VentureEntity *event,
	GDateTime *as_of, const VentureActor *actor, GError **error)
{
	g_auto(Case) c;
	g_autoptr(GArray) keep = NULL;
	g_autoptr(VentureEntity) fee = NULL;
	g_autoptr(VentureMoney) m = NULL;
	const Occurrence *o = NULL;
	gint64 amount = 0;
	memset(&c, 0, sizeof(c));
	if (!module_on() || invoice == NULL || event == NULL)
		return TRUE;
	if (!case_open(self, &c, invoice, as_of, error))
		return FALSE;
	if (!next_fee(self, &c, as_of, &o, &keep, &amount))
		return TRUE;
	fee = g_hash_table_lookup(c.by_key, o->key);
	if (fee != NULL)
	{
		const gchar *state = notice_state(self, fee, o->day, &c);
		g_object_ref(fee);
		/* A notice already sent stands; a later reminder repeats it. */
		if (g_strcmp0(state, "sent") == 0 || g_strcmp0(state, "skipped") == 0)
			return TRUE;
	}
	else
		fee = new_fee(&c, o, ST_SCHEDULED);
	m = fee_money(&c, amount);
	{
		g_autofree gchar *basis = tier_basis(o->tier);
		g_object_set(fee, "notice-event-id", venture_entity_get_id(event), "noticed-amount", m, "amount", m,
			"status", ST_SCHEDULED, "basis", basis, NULL);
	}
	return save_fee(self, fee, actor, error);
}

/* --- Decisions ------------------------------------------------------------ */

static const gchar *
actor_name(const VentureActor *actor)
{
	return actor != NULL && actor->name != NULL ? actor->name : "system";
}

static void
stamp(VentureEntity *fee, const VentureActor *actor)
{
	g_autoptr(GDateTime) now = venture_time_now();
	g_object_set(fee, "decided-by", actor_name(actor), "decided-at", now, NULL);
}

/* The latest dated event on the invoice: a fee approved late must not be
 * dated before a payment already recorded after its day. */
static GDateTime *
adjustment_date(VentureLateFeeService *self, VentureEntity *fee)
{
	g_autoptr(GDateTime) due_on = when(fee, "due-on");
	GType types[2];
	GDateTime *best = due_on != NULL ? g_date_time_ref(due_on) : venture_time_now();
	guint t, i;
	types[0] = VENTURE_TYPE_INVOICE_EVENT;
	types[1] = VENTURE_TYPE_PAYMENT_ALLOCATION;
	for (t = 0; t < 2; t++)
	{
		g_autoptr(VentureQuery) query = venture_query_new(types[t]);
		g_autoptr(GPtrArray) rows = NULL;
		venture_query_add_filter_int(query, "invoice-id", VENTURE_FILTER_OP_EQ, number(fee, "invoice-id"), NULL);
		venture_query_set_include_deleted(query, TRUE);
		venture_query_set_limit(query, 0);
		rows = venture_database_find(self->database, query, NULL);
		for (i = 0; rows != NULL && i < rows->len; i++)
		{
			g_autoptr(GDateTime) date = when(g_ptr_array_index(rows, i), "date");
			if (date != NULL && g_date_time_compare(date, best) > 0)
			{
				g_date_time_unref(best);
				best = g_date_time_ref(date);
			}
		}
	}
	return best;
}

static VentureEntity *
fresh(VentureLateFeeService *self, VentureEntity *fee, GError **error)
{
	VentureEntity *row;
	if (fee == NULL || !VENTURE_IS_LATE_FEE(fee) || venture_entity_get_id(fee) <= 0)
	{
		refuse(error, "a saved late_fee is required");
		return NULL;
	}
	row = venture_database_get(self->database, VENTURE_TYPE_LATE_FEE, venture_entity_get_id(fee), error);
	if (row == NULL && error != NULL && *error == NULL)
		refuse(error, "no such late fee");
	return row;
}

static gboolean
apply_fee(VentureLateFeeService *self, VentureEntity *fee, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureMoney) amount = money(fee, "amount");
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(GDateTime) chosen = date != NULL ? g_date_time_ref(date) : adjustment_date(self, fee);
	if (!venture_database_begin(self->database, error))
		return FALSE;
	event = venture_settlement_service_add_late_fee(venture_settlement_service_get(self->database),
		number(fee, "invoice-id"), amount, chosen, actor, error);
	if (event != NULL)
	{
		g_object_set(fee, "status", ST_APPLIED, "invoice-event-id", venture_entity_get_id(event), NULL);
		stamp(fee, actor);
	}
	if (event == NULL || !save_fee(self, fee, actor, error))
	{
		venture_database_rollback(self->database);
		return FALSE;
	}
	return venture_database_commit(self->database, error);
}

/* Whether this fee may still be added today: late fees on and unpaused,
 * the invoice open, the customer not exempt. */
static gboolean
may_charge(VentureLateFeeService *self, VentureEntity *fee, GError **error)
{
	g_auto(Case) c;
	g_autoptr(VentureEntity) invoice = venture_database_get(self->database, VENTURE_TYPE_INVOICE, number(fee, "invoice-id"), error);
	g_autoptr(GDateTime) now = venture_time_now();
	memset(&c, 0, sizeof(c));
	if (invoice == NULL || !case_open(self, &c, invoice, now, error))
		return FALSE;
	if (!fees_enabled(&c))
		return refuse(error, "late fees are switched off for this organization");
	if (c.paused)
		return refuse(error, "late fees are paused for this invoice");
	if (c.exempt)
		return refuse(error, "this invoice or customer is exempt from late fees");
	if (!invoice_open(&c))
		return refuse(error, "the invoice has no open balance");
	return TRUE;
}

gboolean
venture_late_fee_service_approve(VentureLateFeeService *self, VentureEntity *fee, GDateTime *date,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	g_return_val_if_fail(VENTURE_IS_LATE_FEE_SERVICE(self), FALSE);
	row = fresh(self, fee, error);
	if (row == NULL)
		return FALSE;
	if (!status_is(row, ST_PROPOSED))
		return refuse(error, "only a proposed fee can be approved");
	if (!may_charge(self, row, error) || !apply_fee(self, row, date, actor, error))
		return FALSE;
	if (fee != row)
		venture_entity_copy_properties_from(fee, row, FALSE);
	return TRUE;
}

gboolean
venture_late_fee_service_skip(VentureLateFeeService *self, VentureEntity *fee, const gchar *reason,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	g_return_val_if_fail(VENTURE_IS_LATE_FEE_SERVICE(self), FALSE);
	row = fresh(self, fee, error);
	if (row == NULL)
		return FALSE;
	if (!pending(row))
		return refuse(error, "only a fee not yet added can be skipped; waive an applied one");
	g_object_set(row, "status", ST_SKIPPED, "reason", venture_string_is_empty(reason) ? "skipped by the owner" : reason, NULL);
	stamp(row, actor);
	if (!save_fee(self, row, actor, error))
		return FALSE;
	if (fee != row)
		venture_entity_copy_properties_from(fee, row, FALSE);
	return TRUE;
}

gboolean
venture_late_fee_service_skip_notice(VentureLateFeeService *self, VentureEntity *fee, const gchar *reason,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	g_return_val_if_fail(VENTURE_IS_LATE_FEE_SERVICE(self), FALSE);
	row = fresh(self, fee, error);
	if (row == NULL)
		return FALSE;
	if (!status_is(row, ST_SCHEDULED) && !status_is(row, ST_AWAITING))
		return refuse(error, "only a fee still waiting for its notice can skip it");
	g_object_set(row, "notice-skipped", TRUE, "reason", venture_string_is_empty(reason) ? "notice skipped by the owner" : reason, NULL);
	stamp(row, actor);
	if (!save_fee(self, row, actor, error))
		return FALSE;
	if (fee != row)
		venture_entity_copy_properties_from(fee, row, FALSE);
	return TRUE;
}

static gchar *
recipient(VentureLateFeeService *self, VentureEntity *invoice)
{
	gint64 org = venture_entity_get_organization_id(invoice);
	g_autoptr(VentureEntity) contact = load(self, VENTURE_TYPE_CONTACT, number(invoice, "contact-id"), org);
	g_autoptr(VentureEntity) company = load(self, VENTURE_TYPE_COMPANY, number(invoice, "company-id"), org);
	g_autofree gchar *to = contact != NULL ? text(contact, "email") : NULL;
	if (venture_string_is_empty(to) && company != NULL)
	{
		g_free(to);
		to = text(company, "email");
	}
	return venture_string_is_empty(to) ? NULL : g_steal_pointer(&to);
}

gboolean
venture_late_fee_service_waive(VentureLateFeeService *self, VentureEntity *fee, const gchar *reason,
	const gchar *note, GDateTime *date, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(GDateTime) today = NULL;
	g_return_val_if_fail(VENTURE_IS_LATE_FEE_SERVICE(self), FALSE);
	if (venture_string_is_empty(reason))
		return refuse(error, "a waiver needs a reason");
	row = fresh(self, fee, error);
	if (row == NULL)
		return FALSE;
	if (!status_is(row, ST_APPLIED) || number(row, "invoice-event-id") <= 0)
		return refuse(error, "only an applied fee can be waived");
	today = date != NULL ? g_date_time_ref(date) : venture_settlement_service_today(venture_settlement_service_get(self->database));
	if (!venture_database_begin(self->database, error))
		return FALSE;
	credit = venture_settlement_service_waive_late_fee(venture_settlement_service_get(self->database),
		number(row, "invoice-event-id"), today, reason, actor, error);
	if (credit != NULL)
	{
		g_object_set(row, "status", ST_WAIVED, "reason", reason, "waiver-credit-id", venture_entity_get_id(credit),
			"note", venture_string_is_empty(note) ? NULL : note, NULL);
		stamp(row, actor);
	}
	if (credit == NULL || !save_fee(self, row, actor, error))
	{
		venture_database_rollback(self->database);
		return FALSE;
	}
	/* The note goes through the outbox, keyed by the fee: approving the
	 * waiver twice cannot mail the customer twice. */
	if (!venture_string_is_empty(note) &&
		venture_entity_registry_lookup(venture_entity_registry_get_default(), "mail_message") != G_TYPE_INVALID)
	{
		g_autoptr(VentureEntity) invoice = venture_database_get(self->database, VENTURE_TYPE_INVOICE, number(row, "invoice-id"), NULL);
		g_autofree gchar *to = invoice != NULL ? recipient(self, invoice) : NULL;
		if (to != NULL)
		{
			g_autoptr(VentureMailMessage) message = venture_mail_message_new();
			g_autoptr(VentureMailMessage) queued = NULL;
			g_autofree gchar *number_text = text(invoice, "number");
			g_autofree gchar *subject = g_strdup_printf("Late fee removed from invoice %s", number_text ? number_text : "");
			g_autofree gchar *key = g_strdup_printf("late-fee-waiver:%s", venture_entity_get_uuid(row));
			venture_entity_set_organization_id(VENTURE_ENTITY(message), venture_entity_get_organization_id(row));
			g_object_set(message, "to", to, "subject", subject, "text-body", note, "idempotency-key", key,
				"related-type", "late_fee", "related-id", venture_entity_get_id(row), NULL);
			queued = venture_mail_outbox_enqueue(venture_database_get_mail_outbox(self->database), message, actor, error);
			if (queued == NULL)
			{
				venture_database_rollback(self->database);
				return FALSE;
			}
		}
	}
	if (!venture_database_commit(self->database, error))
		return FALSE;
	if (fee != row)
		venture_entity_copy_properties_from(fee, row, FALSE);
	return TRUE;
}

JsonNode *
venture_late_fee_service_decide(VentureLateFeeService *self, gint64 organization_id, const gint64 *ids, gsize n_ids,
	gboolean approve, const gchar *reason, const VentureActor *actor, GError **error)
{
	g_autoptr(JsonBuilder) b = json_builder_new();
	gsize i;
	gint64 decided = 0, failed = 0;
	g_return_val_if_fail(VENTURE_IS_LATE_FEE_SERVICE(self), NULL);
	if (organization_id <= 0 || n_ids == 0 || n_ids > 500)
	{
		refuse(error, "name an organization and between 1 and 500 fees");
		return NULL;
	}
	for (i = 0; i < n_ids; i++)
	{
		g_autoptr(VentureEntity) fee = venture_database_get(self->database, VENTURE_TYPE_LATE_FEE, ids[i], NULL);
		if (fee == NULL || venture_entity_get_organization_id(fee) != organization_id)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "Late fees: late_fee #%" G_GINT64_FORMAT " is not in this organization", ids[i]);
			return NULL;
		}
	}
	json_builder_begin_object(b);
	json_builder_set_member_name(b, "errors");
	json_builder_begin_array(b);
	for (i = 0; i < n_ids; i++)
	{
		g_autoptr(VentureEntity) fee = venture_database_get(self->database, VENTURE_TYPE_LATE_FEE, ids[i], NULL);
		g_autoptr(GError) local = NULL;
		gboolean ok = approve ? venture_late_fee_service_approve(self, fee, NULL, actor, &local) :
			venture_late_fee_service_skip(self, fee, reason, actor, &local);
		if (ok)
			decided++;
		else
		{
			g_autofree gchar *line = g_strdup_printf("#%" G_GINT64_FORMAT ": %s", ids[i], local != NULL ? local->message : "refused");
			failed++;
			json_builder_add_string_value(b, line);
		}
	}
	json_builder_end_array(b);
	json_builder_set_member_name(b, "decided");
	json_builder_add_int_value(b, decided);
	json_builder_set_member_name(b, "failed");
	json_builder_add_int_value(b, failed);
	json_builder_end_object(b);
	return json_builder_get_root(b);
}

/* --- Pauses --------------------------------------------------------------- */

static VentureEntity *
pause_target(VentureLateFeeService *self, const gchar *scope, gint64 id, const gchar **flag_field,
	const gchar **until_field, const gchar **reason_field, GError **error)
{
	VentureEntity *row = NULL;
	if (g_strcmp0(scope, "invoice") == 0 || g_strcmp0(scope, "company") == 0)
	{
		row = venture_database_get(self->database, g_strcmp0(scope, "invoice") == 0 ? VENTURE_TYPE_INVOICE : VENTURE_TYPE_COMPANY, id, error);
		*flag_field = "late-fees-paused";
		*until_field = "late-fees-paused-until";
		*reason_field = "late-fees-pause-reason";
	}
	else if (g_strcmp0(scope, "organization") == 0)
	{
		row = settings_for(self, id);
		if (row == NULL && module_on())
		{
			row = g_object_new(VENTURE_TYPE_LATE_FEE_SETTING, NULL);
			venture_entity_set_organization_id(row, id);
		}
		*flag_field = "paused";
		*until_field = "paused-until";
		*reason_field = "pause-reason";
	}
	else
		refuse(error, "scope is invoice, company or organization");
	if (row == NULL && error != NULL && *error == NULL)
		refuse(error, "nothing to pause");
	return row;
}

gboolean
venture_late_fee_service_pause(VentureLateFeeService *self, const gchar *scope, gint64 id, GDateTime *until,
	const gchar *reason, const VentureActor *actor, GError **error)
{
	const gchar *f = NULL, *u = NULL, *r = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_return_val_if_fail(VENTURE_IS_LATE_FEE_SERVICE(self), FALSE);
	if (!module_on())
		return refuse(error, "the late_fees module is disabled");
	row = pause_target(self, scope, id, &f, &u, &r, error);
	if (row == NULL)
		return FALSE;
	g_object_set(row, f, TRUE, u, until, r, reason, NULL);
	return venture_database_save(self->database, row, actor, error);
}

gboolean
venture_late_fee_service_resume(VentureLateFeeService *self, const gchar *scope, gint64 id,
	const VentureActor *actor, GError **error)
{
	const gchar *f = NULL, *u = NULL, *r = NULL;
	g_autoptr(VentureEntity) row = NULL;
	g_return_val_if_fail(VENTURE_IS_LATE_FEE_SERVICE(self), FALSE);
	if (!module_on())
		return refuse(error, "the late_fees module is disabled");
	row = pause_target(self, scope, id, &f, &u, &r, error);
	if (row == NULL)
		return FALSE;
	g_object_set(row, f, FALSE, u, NULL, NULL);
	return venture_database_save(self->database, row, actor, error);
}

gchar *
venture_late_fee_service_pause_state(VentureLateFeeService *self, VentureEntity *invoice, GDateTime *at)
{
	g_auto(Case) c;
	memset(&c, 0, sizeof(c));
	g_return_val_if_fail(VENTURE_IS_LATE_FEE_SERVICE(self), NULL);
	if (!module_on() || invoice == NULL)
		return NULL;
	c.invoice = g_object_ref(invoice);
	c.company = load(self, VENTURE_TYPE_COMPANY, number(invoice, "company-id"), venture_entity_get_organization_id(invoice));
	c.settings = settings_for(self, venture_entity_get_organization_id(invoice));
	case_pause(&c, at);
	if (!c.paused)
		return NULL;
	{
		const gchar *whose = g_strcmp0(c.pause_scope, "company") == 0 ? "this customer" :
			(g_strcmp0(c.pause_scope, "invoice") == 0 ? "this invoice" : "the organization");
		g_autofree gchar *day = c.paused_until != NULL ? date_text(c.paused_until) : NULL;
		return day != NULL ? g_strdup_printf("Late fees paused for %s until %s", whose, day) :
			g_strdup_printf("Late fees paused for %s indefinitely", whose);
	}
}

/* Which scope holds the pause on @invoice at @at, or NULL. */
const gchar *
venture_late_fee_service_pause_scope(VentureLateFeeService *self, VentureEntity *invoice, GDateTime *at)
{
	g_auto(Case) c;
	memset(&c, 0, sizeof(c));
	g_return_val_if_fail(VENTURE_IS_LATE_FEE_SERVICE(self), NULL);
	if (!module_on() || invoice == NULL)
		return NULL;
	c.invoice = g_object_ref(invoice);
	c.company = load(self, VENTURE_TYPE_COMPANY, number(invoice, "company-id"), venture_entity_get_organization_id(invoice));
	c.settings = settings_for(self, venture_entity_get_organization_id(invoice));
	case_pause(&c, at);
	return c.pause_scope;
}

/* --- The sweep stage ------------------------------------------------------ */

gboolean
venture_late_fee_service_sweep_needed(VentureLateFeeService *self, gint64 organization_id)
{
	g_autoptr(VentureEntity) settings = NULL;
	g_return_val_if_fail(VENTURE_IS_LATE_FEE_SERVICE(self), FALSE);
	settings = settings_for(self, organization_id);
	return settings != NULL && flag(settings, "enabled");
}

static void
plan_entry(JsonArray *plan, Case *c, gint64 day, const gchar *outcome, gint64 amount, const gchar *reason)
{
	JsonObject *entry;
	g_autofree gchar *number_text = NULL;
	if (plan == NULL)
		return;
	number_text = text(c->invoice, "number");
	entry = json_object_new();
	json_object_set_int_member(entry, "invoice_id", venture_entity_get_id(c->invoice));
	json_object_set_string_member(entry, "invoice_number", number_text ? number_text : "");
	json_object_set_string_member(entry, "stage", "late_fee");
	json_object_set_int_member(entry, "day", day);
	json_object_set_string_member(entry, "outcome", outcome);
	if (amount > 0)
	{
		g_autoptr(VentureMoney) m = fee_money(c, amount);
		g_autofree gchar *s = venture_money_to_string(m);
		json_object_set_string_member(entry, "amount", s);
	}
	if (reason != NULL)
		json_object_set_string_member(entry, "reason", reason);
	json_array_add_object_element(plan, entry);
}

/* Cancels every pending fee of the invoice (or, with @other_than, of other
 * schedules), saying why. */
static gboolean
cancel_pending(VentureLateFeeService *self, Case *c, gint64 other_than, const gchar *why, gboolean dry_run,
	JsonArray *plan, VentureLateFeeSweepTally *tally, const VentureActor *actor, gboolean *wrote, GError **error)
{
	guint i;
	for (i = 0; c->fees != NULL && i < c->fees->len; i++)
	{
		VentureEntity *fee = g_ptr_array_index(c->fees, i);
		if (!pending(fee) || (other_than > 0 && number(fee, "schedule-id") == other_than))
			continue;
		*wrote = TRUE;
		tally->cancelled++;
		if (dry_run)
		{
			plan_entry(plan, c, number(fee, "trigger-day"), "cancel", 0, why);
			continue;
		}
		g_object_set(fee, "status", ST_CANCELLED, "reason", why, NULL);
		if (!save_fee(self, fee, actor, error))
			return FALSE;
	}
	return TRUE;
}

gboolean
venture_late_fee_service_sweep_invoice(VentureLateFeeService *self, VentureEntity *invoice, GDateTime *as_of,
	gboolean dry_run, JsonArray *plan, VentureLateFeeSweepTally *tally, const VentureActor *actor, GError **error)
{
	g_auto(Case) c;
	g_autoptr(GArray) occ = NULL;
	g_autoptr(GPtrArray) to_apply = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(VentureMoney) minimum = NULL;
	gboolean wrote = FALSE;
	gboolean auto_apply;
	Running r;
	guint i;
	memset(&c, 0, sizeof(c));
	g_return_val_if_fail(VENTURE_IS_LATE_FEE_SERVICE(self), FALSE);
	if (!module_on() || !case_open(self, &c, invoice, as_of, error))
		return FALSE;
	if (!fees_enabled(&c) || c.original == NULL || c.fees == NULL)
		return FALSE;
	/* A pause holds everything where it is: nothing is proposed or added. */
	if (c.paused)
		return FALSE;
	if (!venture_database_begin(self->database, error))
		return FALSE;
	if (!invoice_open(&c) || c.exempt || c.schedule == NULL)
	{
		const gchar *why = !invoice_open(&c) ? "settled" : (c.exempt ? "exempt" : "no schedule");
		gboolean ok = cancel_pending(self, &c, 0, why, dry_run, plan, tally, actor, &wrote, error);
		if (!ok)
		{
			venture_database_rollback(self->database);
			return FALSE;
		}
		return venture_database_commit(self->database, error) && wrote;
	}
	/* Moved to another schedule: what was charged stays, what was still
	 * pending under the old one is cancelled, audited by its own save. */
	if (!cancel_pending(self, &c, venture_entity_get_id(c.schedule), "schedule changed", dry_run, plan, tally, actor, &wrote, error))
	{
		venture_database_rollback(self->database);
		return FALSE;
	}
	auto_apply = flag(c.settings, "auto-apply");
	minimum = money(c.schedule, "minimum-balance");
	occ = occurrences(&c);
	running_init(&r, &c);
	for (i = 0; i < occ->len; i++)
	{
		const Occurrence *o = &g_array_index(occ, Occurrence, i);
		VentureEntity *fee = g_hash_table_lookup(c.by_key, o->key);
		g_autoptr(VentureEntity) row = NULL;
		g_autofree gchar *basis = NULL;
		g_autoptr(VentureMoney) noticed = NULL;
		const gchar *notice;
		const gchar *fails;
		gint64 amount;
		if (o->day > c.days)
			break;
		if (o->day < c.start_day)
			continue;
		if (fee != NULL && !status_is(fee, ST_SCHEDULED) && !status_is(fee, ST_AWAITING))
			continue;
		fails = condition_fails(self, &c, o->tier, as_of);
		if (fails == NULL && minimum != NULL && same_currency(minimum, c.principal) && minor_of(c.principal) < minor_of(minimum))
			fails = "balance is below the schedule's minimum";
		if (fails != NULL)
		{
			if (fee != NULL)
			{
				wrote = TRUE;
				tally->cancelled++;
				plan_entry(plan, &c, o->day, "cancel", 0, fails);
				if (!dry_run)
				{
					g_object_set(fee, "status", ST_CANCELLED, "reason", fails, NULL);
					if (!save_fee(self, fee, actor, error))
						goto fail;
				}
			}
			continue;
		}
		amount = occurrence_amount(&c, &r, o, &basis, error);
		if (amount < 0)
			goto fail;
		row = fee != NULL ? g_object_ref(fee) : new_fee(&c, o, ST_AWAITING);
		/* The customer was told an amount; they are never charged more. */
		noticed = money(row, "noticed-amount");
		if (noticed != NULL && same_currency(noticed, c.original) && minor_of(noticed) < amount)
			amount = minor_of(noticed);
		if (amount == 0)
		{
			g_autoptr(VentureMoney) zero = fee_money(&c, 0);
			wrote = TRUE;
			tally->capped++;
			plan_entry(plan, &c, o->day, "capped", 0, "a cap is reached");
			if (!dry_run)
			{
				g_object_set(row, "status", ST_CAPPED, "amount", zero, "basis", basis, "reason", "a cap is reached", NULL);
				if (!save_fee(self, row, actor, error))
					goto fail;
			}
			continue;
		}
		notice = notice_state(self, fee, o->day, &c);
		if (g_strcmp0(notice, "sent") != 0 && g_strcmp0(notice, "skipped") != 0)
		{
			/* No fee without the notice: it waits, visibly, for a reminder
			 * that states it or the owner's decision to skip the notice. */
			if (status_is(row, ST_AWAITING) && venture_entity_is_persisted(row))
				continue;
			{
				g_autoptr(VentureMoney) m = fee_money(&c, amount);
				wrote = TRUE;
				tally->awaiting_notice++;
				plan_entry(plan, &c, o->day, "await_notice", amount, notice);
				if (!dry_run)
				{
					g_object_set(row, "status", ST_AWAITING, "amount", m, "basis", basis, NULL);
					if (!save_fee(self, row, actor, error))
						goto fail;
				}
			}
			continue;
		}
		{
			g_autoptr(VentureMoney) m = fee_money(&c, amount);
			wrote = TRUE;
			running_add(&r, &c, o->tier, amount);
			if (auto_apply)
				tally->applied++;
			else
				tally->proposed++;
			plan_entry(plan, &c, o->day, auto_apply ? "apply" : "propose", amount, NULL);
			if (!dry_run)
			{
				g_object_set(row, "status", ST_PROPOSED, "amount", m, "basis", basis, NULL);
				if (!save_fee(self, row, actor, error))
					goto fail;
				if (auto_apply)
					g_ptr_array_add(to_apply, g_object_ref(row));
			}
		}
	}
	running_clear(&r);
	if (dry_run)
	{
		venture_database_rollback(self->database);
		return wrote;
	}
	if (!venture_database_commit(self->database, error))
		return FALSE;
	/* Auto-apply is approval by the system, one fee per transaction, after
	 * the proposal committed: a refused adjustment leaves it proposed. */
	for (i = 0; i < to_apply->len; i++)
	{
		g_autoptr(GError) local = NULL;
		if (!apply_fee(self, g_ptr_array_index(to_apply, i), NULL, actor, &local))
		{
			tally->applied--;
			tally->proposed++;
			tally->failed++;
			g_warning("late fees: invoice #%" G_GINT64_FORMAT " fee stays proposed: %s",
				venture_entity_get_id(invoice), local != NULL ? local->message : "refused");
		}
	}
	return wrote;
fail:
	running_clear(&r);
	venture_database_rollback(self->database);
	return FALSE;
}

/* Fees still pending on invoices the sweep no longer visits (paid, void,
 * disputed) are cancelled: an announced fee on a paid invoice is never
 * charged. */
gboolean
venture_late_fee_service_sweep_settled(VentureLateFeeService *self, gint64 organization_id, GDateTime *as_of,
	gboolean dry_run, JsonArray *plan, VentureLateFeeSweepTally *tally, const VentureActor *actor, GError **error)
{
	static const gchar *const states[] = { ST_SCHEDULED, ST_AWAITING, ST_PROPOSED, NULL };
	g_autoptr(GHashTable) seen = g_hash_table_new(g_int64_hash, g_int64_equal);
	g_autoptr(GPtrArray) keep = g_ptr_array_new_with_free_func(g_free);
	guint s, i;
	if (!module_on())
		return TRUE;
	for (s = 0; states[s] != NULL; s++)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_LATE_FEE);
		g_autoptr(GPtrArray) rows = NULL;
		venture_query_set_organization(query, organization_id);
		venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, states[s], NULL);
		venture_query_set_limit(query, 1000);
		rows = venture_database_find(self->database, query, error);
		if (rows == NULL)
			return FALSE;
		for (i = 0; i < rows->len; i++)
		{
			gint64 id = number(g_ptr_array_index(rows, i), "invoice-id");
			gint64 *stored;
			g_autoptr(VentureEntity) invoice = NULL;
			gint status = 0;
			if (g_hash_table_contains(seen, &id))
				continue;
			stored = g_new(gint64, 1);
			*stored = id;
			g_ptr_array_add(keep, stored);
			g_hash_table_add(seen, stored);
			invoice = venture_database_get(self->database, VENTURE_TYPE_INVOICE, id, NULL);
			if (invoice == NULL)
				continue;
			g_object_get(invoice, "status", &status, NULL);
			if (status == VENTURE_INVOICE_STATUS_SENT || status == VENTURE_INVOICE_STATUS_PARTIALLY_PAID)
				continue;
			{
				g_auto(Case) c;
				gboolean wrote = FALSE;
				memset(&c, 0, sizeof(c));
				if (!case_open(self, &c, invoice, as_of, error))
					return FALSE;
				if (c.fees == NULL)
					c.fees = fees_of(self, id, organization_id);
				if (!venture_database_begin(self->database, error))
					return FALSE;
				if (!cancel_pending(self, &c, 0, "settled", dry_run, plan, tally, actor, &wrote, error))
				{
					venture_database_rollback(self->database);
					return FALSE;
				}
				if (dry_run)
					venture_database_rollback(self->database);
				else if (!venture_database_commit(self->database, error))
					return FALSE;
			}
		}
	}
	return TRUE;
}

/* --- GObject -------------------------------------------------------------- */

static void
finalize(GObject *object)
{
	VentureLateFeeService *self = VENTURE_LATE_FEE_SERVICE(object);
	if (self->database != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->database), (gpointer *)&self->database);
	G_OBJECT_CLASS(venture_late_fee_service_parent_class)->finalize(object);
}

static void
venture_late_fee_service_class_init(VentureLateFeeServiceClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = finalize;
}

static void
venture_late_fee_service_init(VentureLateFeeService *self)
{
	(void)self;
}

VentureDatabase *
venture_late_fee_service_get_database(VentureLateFeeService *self)
{
	g_return_val_if_fail(VENTURE_IS_LATE_FEE_SERVICE(self), NULL);
	return self->database;
}

gboolean
venture_late_fee_service_claim_actions(VentureLateFeeService *self)
{
	gboolean first = !self->actions;
	self->actions = TRUE;
	return first;
}

VentureLateFeeService *
venture_late_fee_service_get(VentureDatabase *database)
{
	VentureLateFeeService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-late-fee-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_LATE_FEE_SERVICE, NULL);
		self->database = database;
		g_object_add_weak_pointer(G_OBJECT(database), (gpointer *)&self->database);
		g_object_set_data_full(G_OBJECT(database), "venture-late-fee-service", self, g_object_unref);
		venture_database_add_save_validator(database, VENTURE_TYPE_LATE_FEE_SCHEDULE, schedule_validate, self, NULL);
		venture_database_add_save_validator(database, VENTURE_TYPE_LATE_FEE_SETTING, setting_validate, self, NULL);
		venture_database_add_save_validator(database, VENTURE_TYPE_LATE_FEE, fee_validate, self, NULL);
		venture_late_fee_actions_register(database);
	}
	return self;
}
