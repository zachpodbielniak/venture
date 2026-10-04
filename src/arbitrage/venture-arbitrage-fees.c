/*
 * venture-arbitrage-fees.c - What a venue charges: the fee model registry
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A venue names a fee model in `fee-model` and gives it parameters in
 * `fee-params` (YAML). The built-in models:
 *
 *  - `percent`: a marketplace or an auction house. cut_percent of the
 *    sale, fixed_per_unit and fixed_per_order on top, at least min_fee;
 *    deposit_percent of the listing price (deposit_basis: price) or of a
 *    reference (deposit_basis: reference), refunded on a sale unless
 *    deposit_refundable is false. A `buy:` mapping with the same keys
 *    prices the buying side; without one, buying is free.
 *  - `commission`: an exchange. rate_percent of net winnings.
 *  - `none`: nothing.
 *
 * A model's parameters are judged when a venue writes its model or its
 * parameters (the save validator below); a name kept is never judged
 * again, so a venue saved under a plugin's model stays editable after the
 * plugin is gone -- its rows in a scan say the model is missing instead.
 */

#include "venture.h"
#include "arbitrage/venture-arbitrage-engine-private.h"

#include <yaml-glib.h>
#include <math.h>
#include <string.h>

/* The longest fee-params text a venue may hold. */
#define ARB_FEES_MAX_PARAMS (16384)

#define ARB_FEES_CONTEXT_KEY "venture-arbitrage-fee-models"

/* ==========================================================================
 * Shared readers
 * ========================================================================== */

gboolean
venture_arbitrage_name_check(
	const gchar	 *what,
	const gchar	 *name,
	GError		**error
){
	const gchar *cursor;

	if ((NULL == name) || !g_ascii_islower(name[0]) || (strlen(name) > VENTURE_ARBITRAGE_NAME_MAX))
		goto refuse;

	for (cursor = name; '\0' != *cursor; cursor++)
	{
		if (!g_ascii_islower(*cursor) && !g_ascii_isdigit(*cursor) && ('_' != *cursor))
			goto refuse;
	}

	return TRUE;

refuse:
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "\"%s\" is not a %s name: lower case letters, digits and underscores, "
	            "starting with a letter, at most %d bytes",
	            (NULL != name) ? name : "", what, VENTURE_ARBITRAGE_NAME_MAX);
	return FALSE;
}

gchar *
venture_arbitrage_node_text(JsonNode *node)
{
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
	GType type;

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
		return NULL;

	type = json_node_get_value_type(node);

	if (G_TYPE_STRING == type)
		return g_strdup(json_node_get_string(node));

	if (G_TYPE_INT64 == type)
		return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));

	if (G_TYPE_DOUBLE == type)
		/* Fifteen significant digits: YAML's 0.10 is the double nearest
		 * a tenth, and it should read back as "0.1", not seventeen
		 * places of binary noise. */
		return g_strdup(g_ascii_formatd(buffer, sizeof(buffer), "%.15g", json_node_get_double(node)));

	if (G_TYPE_BOOLEAN == type)
		return g_strdup(json_node_get_boolean(node) ? "true" : "false");

	return NULL;
}

gboolean
venture_arbitrage_member_percent(
	JsonObject	 *object,
	const gchar	 *member,
	gint64		  fallback,
	gboolean	  allow_negative,
	gint64		 *out_ppm,
	GError		**error
){
	g_autofree gchar *text = NULL;

	*out_ppm = fallback;

	if ((NULL == object) || !json_object_has_member(object, member))
		return TRUE;

	text = venture_arbitrage_node_text(json_object_get_member(object, member));

	if (NULL == text)
	{
		if (JSON_NODE_HOLDS_NULL(json_object_get_member(object, member)))
			return TRUE;

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s is a percent, e.g. 5 or 2.5", member);
		return FALSE;
	}

	if (!venture_arbitrage_parse_percent(text, allow_negative, out_ppm, error))
	{
		g_prefix_error(error, "%s: ", member);
		return FALSE;
	}

	return TRUE;
}

gboolean
venture_arbitrage_member_money(
	JsonObject	 *object,
	const gchar	 *member,
	const gchar	 *currency,
	VentureMoney	**out,
	GError		**error
){
	g_autofree gchar *text = NULL;
	g_autoptr(VentureMoney) money = NULL;
	JsonNode *node;

	*out = NULL;

	if ((NULL == object) || !json_object_has_member(object, member))
		return TRUE;

	node = json_object_get_member(object, member);

	if (JSON_NODE_HOLDS_NULL(node))
		return TRUE;

	text = venture_arbitrage_node_text(node);

	if (NULL == text)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s is an amount, e.g. 0.05 or \"0.05 GOLD\"", member);
		return FALSE;
	}

	money = venture_money_from_string(text, currency, error);

	if (NULL == money)
	{
		g_prefix_error(error, "%s: ", member);
		return FALSE;
	}

	if (venture_money_is_negative(money))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s cannot be negative", member);
		return FALSE;
	}

	/* A fee in one currency on a sale in another is a configuration
	 * mistake, not a conversion to make quietly. */
	if ((NULL != currency) && (0 != g_strcmp0(venture_money_get_currency(money), currency)))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s is in %s, and the amount it applies to is in %s", member,
		            venture_money_get_currency(money), currency);
		return FALSE;
	}

	*out = g_steal_pointer(&money);

	return TRUE;
}

gboolean
venture_arbitrage_member_bool(
	JsonObject	 *object,
	const gchar	 *member,
	gboolean	  fallback,
	gboolean	 *out,
	GError		**error
){
	g_autofree gchar *text = NULL;

	*out = fallback;

	if ((NULL == object) || !json_object_has_member(object, member) ||
	    JSON_NODE_HOLDS_NULL(json_object_get_member(object, member)))
		return TRUE;

	text = venture_arbitrage_node_text(json_object_get_member(object, member));

	if ((NULL != text) &&
	    ((0 == g_ascii_strcasecmp(text, "true")) || (0 == g_ascii_strcasecmp(text, "yes")) ||
	     (0 == g_ascii_strcasecmp(text, "on")) || (0 == g_strcmp0(text, "1"))))
	{
		*out = TRUE;
		return TRUE;
	}

	if ((NULL != text) &&
	    ((0 == g_ascii_strcasecmp(text, "false")) || (0 == g_ascii_strcasecmp(text, "no")) ||
	     (0 == g_ascii_strcasecmp(text, "off")) || (0 == g_strcmp0(text, "0"))))
	{
		*out = FALSE;
		return TRUE;
	}

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
	            "%s is true or false", member);
	return FALSE;
}

void
venture_arbitrage_set_money(
	JsonObject		*object,
	const gchar		*member,
	const VentureMoney	*money
){
	if (NULL == money)
		json_object_set_null_member(object, member);
	else
		json_object_set_member(object, member, venture_money_to_json(money));
}

void
venture_arbitrage_set_ratio(
	JsonObject	*object,
	const gchar	*member,
	gdouble		 value
){
	if (!isfinite(value))
		json_object_set_null_member(object, member);
	else
		json_object_set_double_member(object, member, value);
}

VentureMoney *
venture_arbitrage_get_money(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
		return NULL;

	return venture_money_from_json(node, NULL, NULL);
}

gdouble
venture_arbitrage_get_ratio(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
		return NAN;

	return json_node_get_double(node);
}

/* ==========================================================================
 * Quotes
 * ========================================================================== */

void
venture_fee_quote_clear(VentureFeeQuote *quote)
{
	if (NULL == quote)
		return;

	g_clear_pointer(&quote->fee, venture_money_free);
	g_clear_pointer(&quote->deposit, venture_money_free);
	memset(quote, 0, sizeof(*quote));
}

static VentureMoney *
arb_fees_zero(const VentureMoney *like)
{
	return venture_money_new(0, venture_money_get_currency(like), venture_money_get_exponent(like));
}

/* ==========================================================================
 * The built-in models
 * ========================================================================== */

static const gchar *const arb_percent_keys[] = {
	"cut_percent", "fixed_per_unit", "fixed_per_order", "min_fee", "deposit_percent",
	"deposit_basis", "deposit_refundable", "buy", NULL
};

/* One side's parameters: the keys, the ranges, the basis word. */
static gboolean
arb_percent_check_side(
	JsonObject	 *params,
	gboolean	  nested,
	GError		**error
){
	g_autoptr(GList) members = NULL;
	g_autoptr(VentureMoney) per_unit = NULL;
	g_autoptr(VentureMoney) per_order = NULL;
	g_autoptr(VentureMoney) min_fee = NULL;
	const gchar *basis;
	GList *member;
	gint64 ppm;
	gboolean flag;

	members = json_object_get_members(params);

	for (member = members; NULL != member; member = member->next)
	{
		if (!g_strv_contains(arb_percent_keys, member->data) ||
		    (nested && (0 == g_strcmp0(member->data, "buy"))))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "The percent fee model takes cut_percent, fixed_per_unit, "
			            "fixed_per_order, min_fee, deposit_percent, deposit_basis, "
			            "deposit_refundable%s; %s is not one of them",
			            nested ? "" : " and buy", (const gchar *)member->data);
			return FALSE;
		}
	}

	if (!venture_arbitrage_member_percent(params, "cut_percent", 0, FALSE, &ppm, error))
		return FALSE;

	if (ppm > 1000000)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "cut_percent is at most 100");
		return FALSE;
	}

	if (!venture_arbitrage_member_percent(params, "deposit_percent", 0, FALSE, &ppm, error) ||
	    !venture_arbitrage_member_money(params, "fixed_per_unit", NULL, &per_unit, error) ||
	    !venture_arbitrage_member_money(params, "fixed_per_order", NULL, &per_order, error) ||
	    !venture_arbitrage_member_money(params, "min_fee", NULL, &min_fee, error) ||
	    !venture_arbitrage_member_bool(params, "deposit_refundable", TRUE, &flag, error))
		return FALSE;

	basis = venture_json_object_get_string(params, "deposit_basis", NULL);

	if (json_object_has_member(params, "deposit_basis") &&
	    (0 != g_strcmp0(basis, "price")) && (0 != g_strcmp0(basis, "reference")))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "deposit_basis is price or reference");
		return FALSE;
	}

	return TRUE;
}

static gboolean
arb_percent_validate(
	JsonObject	 *params,
	gpointer	  user_data,
	GError		**error
){
	JsonNode *buy;

	(void)user_data;

	if (!arb_percent_check_side(params, FALSE, error))
		return FALSE;

	buy = json_object_get_member(params, "buy");

	if (NULL == buy)
		return TRUE;

	if (!JSON_NODE_HOLDS_OBJECT(buy))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "buy is a mapping of the buying side's fees");
		return FALSE;
	}

	if (!arb_percent_check_side(json_node_get_object(buy), TRUE, error))
	{
		g_prefix_error(error, "buy: ");
		return FALSE;
	}

	return TRUE;
}

/* @amount x @ppm / 1e6, rounded half to even. */
static VentureMoney *
arb_fees_share(
	const VentureMoney	 *amount,
	gint64			  ppm,
	GError			**error
){
	return venture_money_multiply_rational(amount, ppm, 1000000, error);
}

static gboolean
arb_fees_add(
	VentureMoney		**total,
	const VentureMoney	 *part,
	GError			**error
){
	VentureMoney *next;

	if (NULL == part)
		return TRUE;

	next = venture_money_add(*total, part, error);

	if (NULL == next)
		return FALSE;

	venture_money_free(*total);
	*total = next;

	return TRUE;
}

static gboolean
arb_percent_compute(
	JsonObject		 *params,
	VentureFeeSide		  side,
	const VentureMoney	 *amount,
	gint64			  units,
	const VentureMoney	 *reference,
	JsonObject		 *attrs,
	VentureFeeQuote		 *out,
	gpointer		  user_data,
	GError			**error
){
	g_autoptr(VentureMoney) fee = NULL;
	g_autoptr(VentureMoney) per_unit = NULL;
	g_autoptr(VentureMoney) per_order = NULL;
	g_autoptr(VentureMoney) min_fee = NULL;
	g_autoptr(VentureMoney) units_fee = NULL;
	const gchar *currency;
	const gchar *basis;
	JsonObject *side_params;
	JsonNode *buy;
	gint64 cut;
	gint64 deposit;
	gboolean refundable;

	(void)attrs;
	(void)user_data;

	currency = venture_money_get_currency(amount);
	side_params = params;

	/* The buying side has fees only when the venue says so. */
	if (VENTURE_FEE_SIDE_BUY == side)
	{
		buy = (NULL != params) ? json_object_get_member(params, "buy") : NULL;

		if ((NULL == buy) || !JSON_NODE_HOLDS_OBJECT(buy))
		{
			out->fee = arb_fees_zero(amount);
			return TRUE;
		}

		side_params = json_node_get_object(buy);
	}

	if (!venture_arbitrage_member_percent(side_params, "cut_percent", 0, FALSE, &cut, error) ||
	    !venture_arbitrage_member_percent(side_params, "deposit_percent", 0, FALSE, &deposit, error) ||
	    !venture_arbitrage_member_money(side_params, "fixed_per_unit", currency, &per_unit, error) ||
	    !venture_arbitrage_member_money(side_params, "fixed_per_order", currency, &per_order, error) ||
	    !venture_arbitrage_member_money(side_params, "min_fee", currency, &min_fee, error) ||
	    !venture_arbitrage_member_bool(side_params, "deposit_refundable", TRUE, &refundable, error))
		return FALSE;

	/* The cut, then the fixed parts, then the floor. */
	fee = arb_fees_share(amount, cut, error);

	if (NULL == fee)
		return FALSE;

	if (NULL != per_unit)
	{
		units_fee = venture_money_multiply_int(per_unit, units, error);

		if (NULL == units_fee)
			return FALSE;
	}

	if (!arb_fees_add(&fee, units_fee, error) || !arb_fees_add(&fee, per_order, error))
		return FALSE;

	if ((NULL != min_fee) && (venture_money_compare(fee, min_fee) < 0))
	{
		venture_money_free(fee);
		fee = venture_money_copy(min_fee);
	}

	out->fee = g_steal_pointer(&fee);
	out->deposit_refundable = refundable;

	if (deposit > 0)
	{
		const VentureMoney *base;

		basis = venture_json_object_get_string(side_params, "deposit_basis", "price");
		base = (0 == g_strcmp0(basis, "reference")) ? reference : amount;

		/* A deposit on a reference nobody gave is unknown, not nothing. */
		if (NULL == base)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "The deposit is a share of a reference price, and none is known");
			venture_fee_quote_clear(out);
			return FALSE;
		}

		if (0 != g_strcmp0(venture_money_get_currency(base), currency))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "The deposit's reference is in %s and the sale in %s",
			            venture_money_get_currency(base), currency);
			venture_fee_quote_clear(out);
			return FALSE;
		}

		out->deposit = arb_fees_share(base, deposit, error);

		if (NULL == out->deposit)
		{
			venture_fee_quote_clear(out);
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean
arb_commission_validate(
	JsonObject	 *params,
	gpointer	  user_data,
	GError		**error
){
	g_autoptr(GList) members = NULL;
	GList *member;
	gint64 ppm;

	(void)user_data;

	members = json_object_get_members(params);

	for (member = members; NULL != member; member = member->next)
	{
		if (0 != g_strcmp0(member->data, "rate_percent"))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "The commission fee model takes rate_percent; %s is not it",
			            (const gchar *)member->data);
			return FALSE;
		}
	}

	if (!venture_arbitrage_member_percent(params, "rate_percent", 0, FALSE, &ppm, error))
		return FALSE;

	if (ppm >= 1000000)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "rate_percent is under 100: an exchange that kept every winning "
		                    "would be no exchange");
		return FALSE;
	}

	return TRUE;
}

static gboolean
arb_commission_compute(
	JsonObject		 *params,
	VentureFeeSide		  side,
	const VentureMoney	 *amount,
	gint64			  units,
	const VentureMoney	 *reference,
	JsonObject		 *attrs,
	VentureFeeQuote		 *out,
	gpointer		  user_data,
	GError			**error
){
	gint64 ppm;

	(void)units;
	(void)reference;
	(void)attrs;
	(void)user_data;

	if (!venture_arbitrage_member_percent(params, "rate_percent", 0, FALSE, &ppm, error))
		return FALSE;

	out->commission = (gdouble)ppm / 1000000.0;

	/* Only winnings pay commission: a stake, or a losing result, none. */
	if ((VENTURE_FEE_SIDE_SELL == side) && (venture_money_get_amount(amount) > 0))
	{
		out->fee = arb_fees_share(amount, ppm, error);
		return NULL != out->fee;
	}

	out->fee = arb_fees_zero(amount);

	return TRUE;
}

static gboolean
arb_none_compute(
	JsonObject		 *params,
	VentureFeeSide		  side,
	const VentureMoney	 *amount,
	gint64			  units,
	const VentureMoney	 *reference,
	JsonObject		 *attrs,
	VentureFeeQuote		 *out,
	gpointer		  user_data,
	GError			**error
){
	(void)params;
	(void)side;
	(void)units;
	(void)reference;
	(void)attrs;
	(void)user_data;
	(void)error;

	out->fee = arb_fees_zero(amount);

	return TRUE;
}

/* ==========================================================================
 * The registry
 * ========================================================================== */

typedef struct
{
	gchar				*name;
	gchar				*description;
	VentureFeeModelComputeFunc	 compute;
	VentureFeeModelValidateFunc	 validate;
	gpointer			 user_data;
	GDestroyNotify			 destroy;
} ArbFeeEntry;

struct _VentureFeeModelRegistry
{
	GObject		 parent_instance;

	GPtrArray	*entries;	/* ArbFeeEntry, registration order */
};

G_DEFINE_FINAL_TYPE(VentureFeeModelRegistry, venture_fee_model_registry, G_TYPE_OBJECT)

static void
arb_fee_entry_free(gpointer data)
{
	ArbFeeEntry *entry;

	entry = data;

	if (NULL != entry->destroy)
		entry->destroy(entry->user_data);

	g_free(entry->name);
	g_free(entry->description);
	g_free(entry);
}

static void
venture_fee_model_registry_finalize(GObject *object)
{
	VentureFeeModelRegistry *self;

	self = VENTURE_FEE_MODEL_REGISTRY(object);
	g_ptr_array_unref(self->entries);

	G_OBJECT_CLASS(venture_fee_model_registry_parent_class)->finalize(object);
}

static void
venture_fee_model_registry_class_init(VentureFeeModelRegistryClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = venture_fee_model_registry_finalize;
}

static void
venture_fee_model_registry_init(VentureFeeModelRegistry *self)
{
	self->entries = g_ptr_array_new_with_free_func(arb_fee_entry_free);
}

static ArbFeeEntry *
arb_fee_lookup(
	VentureFeeModelRegistry	*self,
	const gchar		*name
){
	guint i;

	for (i = 0; (NULL != name) && (i < self->entries->len); i++)
	{
		ArbFeeEntry *entry = g_ptr_array_index(self->entries, i);

		if (0 == g_strcmp0(entry->name, name))
			return entry;
	}

	return NULL;
}

VentureFeeModelRegistry *
venture_fee_model_registry_new(void)
{
	VentureFeeModelRegistry *self;

	self = g_object_new(VENTURE_TYPE_FEE_MODEL_REGISTRY, NULL);

	venture_fee_model_registry_add(self, "percent",
		"A share of the sale (cut_percent), fixed fees per unit and per order, a minimum, "
		"and a listing deposit refunded on a sale; a buy: mapping prices buying",
		arb_percent_compute, arb_percent_validate, NULL, NULL, NULL);
	venture_fee_model_registry_add(self, "commission",
		"An exchange's share of net winnings (rate_percent)",
		arb_commission_compute, arb_commission_validate, NULL, NULL, NULL);
	venture_fee_model_registry_add(self, "none", "Charges nothing",
		arb_none_compute, NULL, NULL, NULL, NULL);

	return self;
}

gboolean
venture_fee_model_registry_add(
	VentureFeeModelRegistry		 *self,
	const gchar			 *name,
	const gchar			 *description,
	VentureFeeModelComputeFunc	  compute,
	VentureFeeModelValidateFunc	  validate,
	gpointer			  user_data,
	GDestroyNotify			  destroy,
	GError				**error
){
	ArbFeeEntry *entry;

	g_return_val_if_fail(VENTURE_IS_FEE_MODEL_REGISTRY(self), FALSE);
	g_return_val_if_fail(NULL != compute, FALSE);

	if (!venture_arbitrage_name_check("fee model", name, error))
		return FALSE;

	if (NULL != arb_fee_lookup(self, name))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS,
		            "A fee model named %s is already registered", name);
		return FALSE;
	}

	entry = g_new0(ArbFeeEntry, 1);
	entry->name = g_strdup(name);
	entry->description = g_strdup((NULL != description) ? description : "");
	entry->compute = compute;
	entry->validate = validate;
	entry->user_data = user_data;
	entry->destroy = destroy;
	g_ptr_array_add(self->entries, entry);

	return TRUE;
}

gboolean
venture_fee_model_registry_has(
	VentureFeeModelRegistry	*self,
	const gchar		*name
){
	g_return_val_if_fail(VENTURE_IS_FEE_MODEL_REGISTRY(self), FALSE);

	return NULL != arb_fee_lookup(self, name);
}

gchar **
venture_fee_model_registry_dup_names(VentureFeeModelRegistry *self)
{
	GStrvBuilder *builder;
	gchar **names;
	guint i;

	g_return_val_if_fail(VENTURE_IS_FEE_MODEL_REGISTRY(self), NULL);

	builder = g_strv_builder_new();

	for (i = 0; i < self->entries->len; i++)
		g_strv_builder_add(builder, ((ArbFeeEntry *)g_ptr_array_index(self->entries, i))->name);

	names = g_strv_builder_end(builder);
	g_strv_builder_unref(builder);

	return names;
}

const gchar *
venture_fee_model_registry_get_description(
	VentureFeeModelRegistry	*self,
	const gchar		*name
){
	ArbFeeEntry *entry;

	g_return_val_if_fail(VENTURE_IS_FEE_MODEL_REGISTRY(self), NULL);

	entry = arb_fee_lookup(self, name);

	return (NULL != entry) ? entry->description : NULL;
}

JsonObject *
venture_arbitrage_parse_mapping(
	const gchar	 *text,
	const gchar	 *what,
	const gchar	 *example,
	GError		**error
){
	g_autoptr(YamlParser) parser = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autoptr(JsonNode) root = NULL;
	YamlNode *yaml_root;

	if (venture_string_is_empty(text))
		return json_object_new();

	if (strlen(text) > ARB_FEES_MAX_PARAMS)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s are at most %d bytes", what, ARB_FEES_MAX_PARAMS);
		return NULL;
	}

	/* YAML is a superset of JSON, so one parser reads both. */
	parser = yaml_parser_new();

	if (!yaml_parser_load_from_data(parser, text, -1, &local_error))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s are not valid YAML: %s", what, local_error->message);
		return NULL;
	}

	yaml_root = yaml_parser_get_root(parser);

	if (NULL == yaml_root)
		return json_object_new();

	root = yaml_node_to_json_node(yaml_root);

	if ((NULL == root) || !JSON_NODE_HOLDS_OBJECT(root))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s are a mapping of names to values, e.g. \"%s\"", what, example);
		return NULL;
	}

	return json_object_ref(json_node_get_object(root));
}

JsonObject *
venture_fee_model_parse_params(
	const gchar	 *text,
	GError		**error
){
	return venture_arbitrage_parse_mapping(text, "Fee parameters", "cut_percent: 5", error);
}

gboolean
venture_fee_model_registry_validate(
	VentureFeeModelRegistry	 *self,
	const gchar		 *name,
	const gchar		 *params_text,
	GError			**error
){
	g_autoptr(JsonObject) params = NULL;
	ArbFeeEntry *entry;

	g_return_val_if_fail(VENTURE_IS_FEE_MODEL_REGISTRY(self), FALSE);

	params = venture_fee_model_parse_params(params_text, error);

	if (NULL == params)
		return FALSE;

	if (venture_string_is_empty(name))
	{
		if (json_object_get_size(params) > 0)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "Fee parameters need a fee model to belong to");
			return FALSE;
		}

		return TRUE;
	}

	entry = arb_fee_lookup(self, name);

	if (NULL == entry)
	{
		g_auto(GStrv) names = venture_fee_model_registry_dup_names(self);
		g_autofree gchar *list = g_strjoinv(", ", names);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "\"%s\" is not a registered fee model; registered: %s", name, list);
		return FALSE;
	}

	if (NULL == entry->validate)
	{
		if (json_object_get_size(params) > 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "The %s fee model takes no parameters", name);
			return FALSE;
		}

		return TRUE;
	}

	return entry->validate(params, entry->user_data, error);
}

gboolean
venture_fee_model_registry_compute(
	VentureFeeModelRegistry	 *self,
	const gchar		 *name,
	JsonObject		 *params,
	VentureFeeSide		  side,
	const VentureMoney	 *amount,
	gint64			  units,
	const VentureMoney	 *reference,
	JsonObject		 *attrs,
	VentureFeeQuote		 *out,
	GError			**error
){
	g_autoptr(JsonObject) empty = NULL;
	ArbFeeEntry *entry;

	g_return_val_if_fail(VENTURE_IS_FEE_MODEL_REGISTRY(self), FALSE);
	g_return_val_if_fail(NULL != amount, FALSE);
	g_return_val_if_fail(NULL != out, FALSE);

	memset(out, 0, sizeof(*out));

	if (venture_string_is_empty(name))
		name = "none";

	entry = arb_fee_lookup(self, name);

	if (NULL == entry)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "The fee model %s is not loaded", name);
		return FALSE;
	}

	if (NULL == params)
		params = empty = json_object_new();

	if (!entry->compute(params, side, amount, MAX(units, (gint64)1), reference, attrs, out,
	                    entry->user_data, error))
	{
		venture_fee_quote_clear(out);
		return FALSE;
	}

	/* A model that forgot to say is a model that charged nothing. */
	if (NULL == out->fee)
		out->fee = arb_fees_zero(amount);

	return TRUE;
}

VentureFeeModelRegistry *
venture_context_get_fee_models(VentureContext *context)
{
	VentureFeeModelRegistry *registry;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	registry = g_object_get_data(G_OBJECT(context), ARB_FEES_CONTEXT_KEY);

	if (NULL == registry)
	{
		registry = venture_fee_model_registry_new();
		g_object_set_data_full(G_OBJECT(context), ARB_FEES_CONTEXT_KEY, registry, g_object_unref);
	}

	return registry;
}

/* ==========================================================================
 * The venue validator
 * ========================================================================== */

/*
 * A venue's fee model and parameters, judged when written: the model must
 * be registered and take the parameters. Only while the arbitrage module
 * is on -- with it off nothing reads a fee model, and nothing loaded is
 * not evidence that a name is wrong. A name kept from before is never
 * judged again, so removing a plugin does not make its venues uneditable.
 */
gboolean
venture_arbitrage_fees_validate_venue(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	gpointer	  user_data,
	GError		**error
){
	VentureFeeModelRegistry *registry;
	g_autofree gchar *model = NULL;
	g_autofree gchar *params = NULL;
	g_autofree gchar *old_model = NULL;
	g_autofree gchar *old_params = NULL;

	(void)user_data;

	registry = g_object_get_data(G_OBJECT(database), VENTURE_ARBITRAGE_FEES_KEY);

	if ((NULL == registry) ||
	    !venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(),
	                                             "arbitrage_trade"))
		return TRUE;

	g_object_get(entity, "fee-model", &model, "fee-params", &params, NULL);

	if (NULL != previous)
		g_object_get(previous, "fee-model", &old_model, "fee-params", &old_params, NULL);

	if ((NULL != previous) && (0 == g_strcmp0(model, old_model)) &&
	    (0 == g_strcmp0(params, old_params)))
		return TRUE;

	if (!venture_fee_model_registry_validate(registry, model, params, error))
	{
		g_prefix_error(error, "Fee model: ");
		return FALSE;
	}

	return TRUE;
}
