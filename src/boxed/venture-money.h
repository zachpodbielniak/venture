/*
 * venture-money.h - Exact monetary amounts
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Money in VENTURE is never a double. Binary floating point cannot represent
 * 0.10, and a bookkeeping system that cannot represent a dime is not a
 * bookkeeping system. #VentureMoney stores an exact integer count of minor
 * units (cents, pence, yen) alongside the currency code and the number of
 * minor-unit digits that currency uses.
 *
 * A currency is either one of the ISO 4217 codes this file knows, or one an
 * operator defined: points, a commodity, the gold of a game economy. Those
 * are registered at startup from the `currency` records (see
 * venture_currency_register()) and carry their own exponent, symbol and,
 * optionally, denominations -- "12g 34s 56c" rather than "12.3456".
 *
 * Because the exponent travels with the value, the same type handles USD
 * (two digits), JPY (zero digits) and the four-digit exponents used for unit
 * prices without any global configuration.
 *
 * Arithmetic between different currencies is refused rather than silently
 * coerced: there is no exchange rate in this type, and guessing one would be
 * worse than failing.
 */

#ifndef VENTURE_MONEY_H
#define VENTURE_MONEY_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define VENTURE_TYPE_MONEY (venture_money_get_type())

/**
 * VENTURE_MONEY_MAX_EXPONENT:
 *
 * The largest number of minor-unit digits a #VentureMoney may carry. Six
 * covers every ISO 4217 currency (the maximum in use is three), the extra
 * digits per-unit pricing sometimes needs, and a user-defined currency whose
 * smallest coin is a millionth of its largest. An exponent passed to
 * venture_money_new() above this value is not clamped: it means "use the
 * currency's natural exponent".
 */
#define VENTURE_MONEY_MAX_EXPONENT (6)

/**
 * VENTURE_MONEY_CURRENCY_LEN:
 *
 * The size of the currency-code buffer: the longest code the grammar allows
 * (fifteen characters, see venture_currency_is_valid()) plus a terminator.
 */
#define VENTURE_MONEY_CURRENCY_LEN (16)

/**
 * VENTURE_CURRENCY_MAX_DENOMINATIONS:
 *
 * The most denominations one currency may declare. Eight is far beyond any
 * coinage in use and keeps a registry entry a fixed size.
 */
#define VENTURE_CURRENCY_MAX_DENOMINATIONS (8)

/**
 * VentureMoney:
 * @amount: the value as a signed count of minor units
 * @currency: the currency code, uppercased and NUL terminated
 * @exponent: how many decimal digits of minor unit @amount is expressed in
 *
 * An exact monetary amount. A US dollar and twenty-three cents is
 * `{ .amount = 123, .currency = "USD", .exponent = 2 }`.
 *
 * The struct is public and copyable by value; it holds no pointers, so a
 * shallow copy is always correct.
 */
struct _VentureMoney
{
	gint64	amount;
	gchar	currency[VENTURE_MONEY_CURRENCY_LEN];
	guint8	exponent;
};

GType
venture_money_get_type(void) G_GNUC_CONST;

/**
 * venture_money_new:
 * @amount: the value in minor units
 * @currency: (nullable): a currency code; %NULL means the default currency
 * @exponent: minor-unit digits, or the currency's natural exponent if the
 *   value is greater than %VENTURE_MONEY_MAX_EXPONENT
 *
 * Creates a new monetary amount from an exact minor-unit count. A code
 * longer than the buffer holds is a programming error and returns %NULL
 * with a critical warning rather than being truncated into a different
 * currency; input from outside goes through venture_money_from_string() or
 * venture_money_from_json(), which report it as an error.
 *
 * Returns: (transfer full): a new #VentureMoney. Free with
 *   venture_money_free().
 */
VentureMoney *
venture_money_new(
	gint64		 amount,
	const gchar	*currency,
	guint8		 exponent
);

/**
 * venture_money_new_for_currency:
 * @amount: the value in minor units
 * @currency: (nullable): a currency code; %NULL means the default currency
 *
 * Creates a new monetary amount using the natural exponent of @currency, so
 * a caller does not need to remember that JPY has no minor unit.
 *
 * Returns: (transfer full): a new #VentureMoney
 */
VentureMoney *
venture_money_new_for_currency(
	gint64		 amount,
	const gchar	*currency
);

/**
 * venture_money_new_zero:
 * @currency: (nullable): a currency code; %NULL means the default currency
 *
 * Creates a zero amount in @currency. Useful as the identity element when
 * summing a column that might be empty.
 *
 * Returns: (transfer full): a new #VentureMoney
 */
VentureMoney *
venture_money_new_zero(const gchar *currency);

/**
 * venture_money_copy:
 * @self: (nullable): a #VentureMoney
 *
 * Returns: (transfer full) (nullable): a copy of @self, or %NULL if @self
 *   was %NULL
 */
VentureMoney *
venture_money_copy(const VentureMoney *self);

/**
 * venture_money_free:
 * @self: (nullable): a #VentureMoney
 *
 * Frees @self. Safe to call with %NULL.
 */
void
venture_money_free(VentureMoney *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureMoney, venture_money_free)

/* --- Accessors ----------------------------------------------------------- */

/**
 * venture_money_get_amount:
 * @self: a #VentureMoney
 *
 * Returns: the exact value in minor units
 */
gint64
venture_money_get_amount(const VentureMoney *self);

/**
 * venture_money_get_currency:
 * @self: a #VentureMoney
 *
 * Returns: (transfer none): the currency code
 */
const gchar *
venture_money_get_currency(const VentureMoney *self);

/**
 * venture_money_get_exponent:
 * @self: a #VentureMoney
 *
 * Returns: the number of minor-unit digits
 */
guint8
venture_money_get_exponent(const VentureMoney *self);

/**
 * venture_money_is_zero:
 * @self: (nullable): a #VentureMoney
 *
 * Returns: %TRUE if @self is %NULL or has a zero amount
 */
gboolean
venture_money_is_zero(const VentureMoney *self);

/**
 * venture_money_is_negative:
 * @self: (nullable): a #VentureMoney
 *
 * Returns: %TRUE if @self is a negative amount
 */
gboolean
venture_money_is_negative(const VentureMoney *self);

/* --- Arithmetic ---------------------------------------------------------- */

/**
 * venture_money_add:
 * @a: the first operand
 * @b: the second operand
 * @error: (out) (optional): return location for a #GError
 *
 * Adds two amounts. The operands must be in the same currency; if their
 * exponents differ, the result uses the larger exponent so no precision is
 * lost. Overflow is detected and reported rather than wrapping.
 *
 * Returns: (transfer full) (nullable): the sum, or %NULL on error
 */
VentureMoney *
venture_money_add(
	const VentureMoney	 *a,
	const VentureMoney	 *b,
	GError			**error
);

/**
 * venture_money_subtract:
 * @a: the amount to subtract from
 * @b: the amount to subtract
 * @error: (out) (optional): return location for a #GError
 *
 * Subtracts @b from @a under the same rules as venture_money_add().
 *
 * Returns: (transfer full) (nullable): the difference, or %NULL on error
 */
VentureMoney *
venture_money_subtract(
	const VentureMoney	 *a,
	const VentureMoney	 *b,
	GError			**error
);

/**
 * venture_money_negate:
 * @self: a #VentureMoney
 *
 * Returns: (transfer full): @self with the sign flipped
 */
VentureMoney *
venture_money_negate(const VentureMoney *self);

/**
 * venture_money_abs:
 * @self: a #VentureMoney
 *
 * Returns: (transfer full): the absolute value of @self
 */
VentureMoney *
venture_money_abs(const VentureMoney *self);

/**
 * venture_money_multiply_int:
 * @self: a #VentureMoney
 * @factor: an integer multiplier, for example a quantity
 * @error: (out) (optional): return location for a #GError
 *
 * Multiplies an amount by a whole number. This is exact: a unit price times
 * a quantity never needs rounding.
 *
 * Returns: (transfer full) (nullable): the product, or %NULL on overflow
 */
VentureMoney *
venture_money_multiply_int(
	const VentureMoney	 *self,
	gint64			  factor,
	GError			**error
);

/**
 * venture_money_multiply_rational:
 * @self: a #VentureMoney
 * @numerator: the numerator of the multiplier
 * @denominator: the denominator of the multiplier; must not be zero
 * @error: (out) (optional): return location for a #GError
 *
 * Multiplies by the exact rational @numerator/@denominator, rounding the
 * result half to even. Use this for tax rates, commissions, discounts and
 * percentage splits rather than converting to a double first: 7% of $1.15 is
 * computed as 115 * 7 / 100 and rounded once, so the answer does not drift.
 *
 * Returns: (transfer full) (nullable): the product, or %NULL on error
 */
VentureMoney *
venture_money_multiply_rational(
	const VentureMoney	 *self,
	gint64			  numerator,
	gint64			  denominator,
	GError			**error
);

/**
 * venture_money_multiply_percent:
 * @self: a #VentureMoney
 * @percent_basis_points: the rate in basis points, so 7.25% is 725
 * @error: (out) (optional): return location for a #GError
 *
 * Convenience wrapper over venture_money_multiply_rational() for a
 * percentage expressed in basis points, which is how tax and commission
 * rates are stored throughout VENTURE.
 *
 * Returns: (transfer full) (nullable): the product, or %NULL on error
 */
VentureMoney *
venture_money_multiply_percent(
	const VentureMoney	 *self,
	gint64			  percent_basis_points,
	GError			**error
);

/**
 * venture_money_convert_at_rate:
 * @self: source amount
 * @numerator: rate numerator
 * @denominator: rate denominator
 * @currency: destination currency code
 * @error: (out) (optional): overflow or a zero denominator
 *
 * Converts @self at @numerator/@denominator, rounding once at the
 * destination currency exponent (half to even).
 *
 * Returns: (transfer full) (nullable): the converted amount
 */
VentureMoney *
venture_money_convert_at_rate(const VentureMoney *self, gint64 numerator, gint64 denominator,
	const gchar *currency, GError **error);

/**
 * venture_money_allocate:
 * @self: the amount to split
 * @ratios: (array length=n_ratios): the relative share of each part
 * @n_ratios: the number of parts
 * @error: (out) (optional): return location for a #GError
 *
 * Splits an amount into @n_ratios parts in the given proportions such that
 * the parts sum back to exactly @self. Remainder minor units are distributed
 * one at a time to the largest ratios first, so splitting $0.05 three ways
 * gives 2c, 2c, 1c rather than losing a cent to rounding.
 *
 * This is the operation to use whenever money is divided -- allocating a
 * shipping charge across line items, splitting a payment across ventures,
 * apportioning a shared expense between entities.
 *
 * Returns: (transfer full) (element-type VentureMoney) (nullable): an array
 *   of @n_ratios amounts, or %NULL on error. Free with g_ptr_array_unref().
 */
GPtrArray *
venture_money_allocate(
	const VentureMoney	 *self,
	const gint64		 *ratios,
	gsize			  n_ratios,
	GError			**error
);

/**
 * venture_money_allocate_evenly:
 * @self: the amount to split
 * @n_parts: how many parts to split into; must be greater than zero
 * @error: (out) (optional): return location for a #GError
 *
 * Splits an amount into @n_parts equal parts, distributing any remainder
 * minor units to the earlier parts so the total is preserved exactly.
 *
 * Returns: (transfer full) (element-type VentureMoney) (nullable): the
 *   parts, or %NULL on error
 */
GPtrArray *
venture_money_allocate_evenly(
	const VentureMoney	 *self,
	gsize			  n_parts,
	GError			**error
);

/**
 * venture_money_sum_dominant:
 * @amounts: (element-type VentureMoney): amounts, possibly in several
 *   currencies
 * @out_skipped: (out) (optional): how many were left out
 *
 * Totals the amounts that are in the currency most of them are in, and
 * counts the rest. For a figure that must survive one stray currency --
 * what the coding runs cost, when one provider bills in euros -- where
 * venture_money_sum() would refuse the lot and adding across currencies
 * would need a rate nobody gave. A tie goes to the default currency when
 * it is one of those tied, else to whichever was met first.
 *
 * Returns: (transfer full): the total; zero in the default currency when
 *   @amounts is empty
 */
VentureMoney *
venture_money_sum_dominant(
	GPtrArray	*amounts,
	guint		*out_skipped
);

/**
 * venture_money_sum:
 * @amounts: (element-type VentureMoney): the amounts to total
 * @fallback_currency: (nullable): the currency of the result when @amounts
 *   is empty
 * @error: (out) (optional): return location for a #GError
 *
 * Totals a list of amounts. An empty list yields zero in
 * @fallback_currency rather than an error, which is what a report summing an
 * empty period wants.
 *
 * Returns: (transfer full) (nullable): the total, or %NULL on error
 */
VentureMoney *
venture_money_sum(
	GPtrArray		 *amounts,
	const gchar		 *fallback_currency,
	GError			**error
);

/* --- Totals per currency ------------------------------------------------- */

/**
 * venture_money_totals_new:
 *
 * An empty set of totals, one #VentureMoney per currency. Where
 * venture_money_sum() refuses a second currency and
 * venture_money_sum_dominant() drops it, a set of totals keeps every
 * currency apart and loses nothing: what FIFO cost layers bought in two
 * currencies cost is two amounts, not one.
 *
 * Returns: (transfer full) (element-type VentureMoney): an empty array that
 *   owns its amounts
 */
GPtrArray *
venture_money_totals_new(void);

/**
 * venture_money_totals_add:
 * @totals: (element-type VentureMoney): totals from venture_money_totals_new()
 * @amount: (nullable): the amount to add; %NULL adds nothing
 * @error: (out) (optional): overflow
 *
 * Adds @amount into its currency's total, starting one when @amount is the
 * first in that currency. A zero amount starts a zero total too, so a
 * caller can tell "nothing consumed" from "consumed at no cost"; it never
 * clashes with another currency, because nothing here adds across two.
 *
 * Returns: %TRUE on success, %FALSE on overflow
 */
gboolean
venture_money_totals_add(
	GPtrArray		 *totals,
	const VentureMoney	 *amount,
	GError			**error
);

/**
 * venture_money_totals_lookup:
 * @totals: (element-type VentureMoney): a set of totals
 * @currency: the code wanted
 *
 * Returns: (transfer none) (nullable): the total in @currency, or %NULL
 *   when nothing in that currency was added
 */
const VentureMoney *
venture_money_totals_lookup(
	GPtrArray	*totals,
	const gchar	*currency
);

/**
 * venture_money_totals_sort:
 * @totals: (element-type VentureMoney): a set of totals
 * @first: (nullable): a code to put first, usually the book currency
 *
 * Orders the totals by currency code, with @first (when present) ahead of
 * the rest, so reports and tests read them in a stable order.
 */
void
venture_money_totals_sort(
	GPtrArray	*totals,
	const gchar	*first
);

/**
 * venture_money_totals_has_value:
 * @totals: (element-type VentureMoney) (nullable): a set of totals
 *
 * Returns: %TRUE when any total is not zero
 */
gboolean
venture_money_totals_has_value(GPtrArray *totals);

/* --- Comparison ---------------------------------------------------------- */

/**
 * venture_money_compare:
 * @a: the first amount
 * @b: the second amount
 *
 * Compares two amounts of the same currency. Differing exponents are
 * reconciled before comparison. Comparing different currencies is a
 * programming error and returns 0 after a warning.
 *
 * Returns: a negative value if @a is less than @b, zero if they are equal,
 *   a positive value otherwise
 */
gint
venture_money_compare(
	const VentureMoney	*a,
	const VentureMoney	*b
);

/**
 * venture_money_equal:
 * @a: (nullable): the first amount
 * @b: (nullable): the second amount
 *
 * Returns: %TRUE if both are %NULL, or both represent the same value in the
 *   same currency
 */
gboolean
venture_money_equal(
	const VentureMoney	*a,
	const VentureMoney	*b
);

/**
 * venture_money_hash:
 * @self: a #VentureMoney
 *
 * Returns: a hash suitable for use with #GHashTable
 */
guint
venture_money_hash(const VentureMoney *self);

/* --- Conversion ---------------------------------------------------------- */

/**
 * venture_money_to_string:
 * @self: a #VentureMoney
 *
 * Formats the amount as a plain decimal with its currency code, for example
 * "1234.56 USD". This is the canonical round-trippable text form: it is what
 * venture_money_from_string() parses and what the CSV and org exporters
 * emit.
 *
 * Returns: (transfer full): the formatted string. Free with g_free().
 */
gchar *
venture_money_to_string(const VentureMoney *self);

/**
 * venture_money_to_display_string:
 * @self: a #VentureMoney
 * @with_grouping: whether to insert thousands separators
 *
 * Formats the amount for a human, with a currency symbol where one is known
 * and optional grouping, for example "$1,234.56". A registered currency
 * with a suffix symbol reads "1,234.56 pts"; one with denominations reads
 * "12g 34s 56c" (zero parts left out, "0c" for nothing, never grouped). Not
 * round-trippable; use venture_money_to_string() for anything that will be
 * parsed again.
 *
 * Returns: (transfer full): the formatted string
 */
gchar *
venture_money_to_display_string(
	const VentureMoney	*self,
	gboolean		 with_grouping
);

/**
 * venture_money_from_string:
 * @text: the text to parse
 * @default_currency: (nullable): the currency to assume when @text does not
 *   name one
 * @error: (out) (optional): return location for a #GError
 *
 * Parses a monetary amount. The parser is deliberately forgiving because
 * this text arrives from CSV imports, web forms, the CLI and AI tool calls:
 * a leading or trailing code, a currency symbol, thousands separators,
 * parentheses for negation and a leading sign are all accepted. A leading or
 * trailing word is read as a code when it is three letters (the ISO shape)
 * or names a registered currency, so "100.00 CR" is still refused rather
 * than read as a hundred of something.
 *
 * A currency with denominations is also accepted in its display form,
 * "12g 34s 56c", optionally with its code before or after. Without a code
 * the suffixes pick the currency: @default_currency if its denominations
 * cover them, otherwise the one registered currency whose denominations do.
 * Two candidates is ambiguous and refused.
 *
 * What it will not do is guess a currency when none is available and no
 * default was supplied.
 *
 * Returns: (transfer full) (nullable): the parsed amount, or %NULL on error
 */
VentureMoney *
venture_money_from_string(
	const gchar	 *text,
	const gchar	 *default_currency,
	GError		**error
);

/**
 * venture_money_to_double:
 * @self: a #VentureMoney
 *
 * Converts to a double. This loses exactness and must only be used for
 * charting, ratios and other display-side arithmetic -- never to store a
 * value or to compute one that will be stored.
 *
 * Returns: the approximate value in major units
 */
gdouble
venture_money_to_double(const VentureMoney *self);

/**
 * venture_money_rescale:
 * @self: a #VentureMoney
 * @exponent: the target number of minor-unit digits
 * @error: (out) (optional): return location for a #GError
 *
 * Returns @self expressed with a different exponent, rounding half to even
 * when the target exponent is smaller.
 *
 * Returns: (transfer full) (nullable): the rescaled amount, or %NULL on
 *   error
 */
VentureMoney *
venture_money_rescale(
	const VentureMoney	 *self,
	guint8			  exponent,
	GError			**error
);

/**
 * venture_money_to_json:
 * @self: a #VentureMoney
 *
 * Serialises to a JSON object of the form
 * `{"amount": 123, "currency": "USD", "exponent": 2, "formatted": "1.23 USD"}`.
 * The integer minor-unit amount is the authoritative field; "formatted" is
 * present so that a consumer which does not understand the encoding -- an AI
 * reading a tool result, for instance -- still sees a correct value.
 *
 * Returns: (transfer full): a new #JsonNode
 */
JsonNode *
venture_money_to_json(const VentureMoney *self);

/**
 * venture_money_from_json:
 * @node: a #JsonNode
 * @default_currency: (nullable): currency to assume if the node omits one
 * @error: (out) (optional): return location for a #GError
 *
 * Parses a #VentureMoney from JSON. Accepts the object form produced by
 * venture_money_to_json(), a bare string in the form
 * venture_money_from_string() understands, and a bare integer interpreted as
 * minor units of @default_currency.
 *
 * Returns: (transfer full) (nullable): the parsed amount, or %NULL on error
 */
VentureMoney *
venture_money_from_json(
	JsonNode	 *node,
	const gchar	 *default_currency,
	GError		**error
);

/* --- Currency metadata --------------------------------------------------- */

/**
 * venture_currency_get_exponent:
 * @currency: a currency code
 *
 * Retrieves the number of minor-unit digits a currency uses: 2 for most, 0
 * for JPY and similar, 3 for a handful, and whatever a registered currency
 * declared. Unknown codes are assumed to use 2.
 *
 * Returns: the natural exponent
 */
guint8
venture_currency_get_exponent(const gchar *currency);

/**
 * venture_currency_get_symbol:
 * @currency: a currency code
 *
 * Retrieves the display symbol for a currency, for example "$" for USD.
 * Currencies with no known symbol return their own code. Whether the symbol
 * goes before or after the figure is venture_money_to_display_string()'s
 * business.
 *
 * Returns: (transfer none): the symbol; never freed, so safe to keep
 */
const gchar *
venture_currency_get_symbol(const gchar *currency);

/**
 * venture_currency_is_valid:
 * @currency: (nullable): a candidate currency code
 *
 * Checks that @currency is well formed: a letter followed by one to
 * fourteen letters, digits or underscores (`[A-Z][A-Z0-9_]{1,14}`, compared
 * without regard to case, since codes are uppercased when stored). VENTURE
 * does not maintain a closed list of valid codes, so that a user can track
 * something the list would not have -- but the shape is enforced.
 *
 * Returns: %TRUE if @currency is well formed
 */
gboolean
venture_currency_is_valid(const gchar *currency);

/**
 * venture_currency_is_normalised:
 * @currency: (nullable): a candidate currency code
 *
 * Like venture_currency_is_valid(), but also requires the code to be in
 * its stored spelling already (uppercase). For callers that compare codes
 * byte for byte and must not accept "usd" as a different currency.
 *
 * Returns: %TRUE if @currency is valid and uppercase
 */
gboolean
venture_currency_is_normalised(const gchar *currency);

/**
 * venture_currency_is_builtin:
 * @currency: (nullable): a currency code
 *
 * Returns: %TRUE if @currency is one of the ISO 4217 codes this build
 *   carries metadata for (the table that knows JPY has no minor unit)
 */
gboolean
venture_currency_is_builtin(const gchar *currency);

/**
 * venture_currency_is_iso:
 * @currency: (nullable): a currency code
 *
 * Whether @currency can be an ISO 4217 code: three letters, and not a
 * currency somebody registered. This is what the doors to the outside world
 * ask -- a card processor, a tax form, a storefront import -- because none
 * of them can settle in a game's gold. It is deliberately wider than
 * venture_currency_is_builtin(): the built-in table only lists codes whose
 * metadata differs from the default, and a Thai baht is still a currency
 * Stripe will take.
 *
 * Returns: %TRUE if @currency is ISO-shaped and not user-defined
 */
gboolean
venture_currency_is_iso(const gchar *currency);

/**
 * venture_currency_is_registered:
 * @currency: (nullable): a currency code
 *
 * Returns: %TRUE if @currency is a user-defined currency in the registry
 */
gboolean
venture_currency_is_registered(const gchar *currency);

/**
 * venture_currency_check_denominations:
 * @denominations: (nullable): a JSON array, or %NULL or empty for none
 * @error: (out) (optional): what is wrong with it
 *
 * Validates a denomination list: a JSON array of up to
 * %VENTURE_CURRENCY_MAX_DENOMINATIONS objects
 * `{"suffix": "g", "units": 10000}`, where @units counts minor units. The
 * units must be positive and strictly descending, each must divide the one
 * before it, and the last must be 1 so that every amount can be written
 * exactly. A suffix must be non-empty, at most eight bytes, unique within
 * the list (ignoring case), and contain no digit, space, sign, parenthesis,
 * point or comma -- otherwise "12g" could not be told from a number.
 *
 * Returns: %TRUE if @denominations is empty or well formed
 */
gboolean
venture_currency_check_denominations(
	const gchar	 *denominations,
	GError		**error
);

/**
 * venture_currency_register:
 * @currency: the code, which must satisfy venture_currency_is_valid()
 * @exponent: minor-unit digits, 0 to %VENTURE_MONEY_MAX_EXPONENT
 * @symbol: (nullable): the display symbol; %NULL or empty to show the code
 * @symbol_suffix: whether the symbol follows the figure ("12 pts")
 * @denominations: (nullable): see venture_currency_check_denominations()
 * @error: (out) (optional): why the currency was refused
 *
 * Adds or replaces a user-defined currency in the process-wide registry,
 * which venture_currency_get_exponent(), venture_currency_get_symbol() and
 * the display and parsing functions consult before the built-in table. A
 * built-in ISO code is refused: redefining how USD displays would change
 * every figure in the books without touching a row.
 *
 * The registry is guarded by a reader-writer lock because a coding run's
 * thread formats amounts while the main thread may be reloading it.
 *
 * Returns: %TRUE if the currency was registered
 */
gboolean
venture_currency_register(
	const gchar	 *currency,
	guint8		  exponent,
	const gchar	 *symbol,
	gboolean	  symbol_suffix,
	const gchar	 *denominations,
	GError		**error
);

/**
 * venture_currency_get_denominations_json:
 *
 * Every registered currency written in coins, as a JSON object from code
 * to its denominations largest first: `{"GOLD":[["g",10000],["s",100],
 * ["c",1]]}`, codes in order. A page carries it so a browser can tell
 * "12g 5s" from text that merely ends in a letter, and so a chart can
 * label its axis in the same coins. Every character of a suffix outside
 * [A-Za-z] that is ASCII is written as a \u escape, so the text is safe
 * inside a script element.
 *
 * Returns: (transfer full) (nullable): the JSON text, or %NULL when no
 *   registered currency has denominations
 */
gchar *
venture_currency_get_denominations_json(void);

/**
 * venture_currency_set_book_treatment:
 * @currency: a registered currency's code
 * @treatment: how the ledger treats amounts in it
 *
 * Records a registered currency's book treatment, read back by
 * venture_currency_get_book_treatment(). The `currency` records set it when
 * they are loaded; a replaced registration keeps the treatment it had, so a
 * reload never shows a currency as valued for a moment.
 *
 * Returns: %TRUE if @currency is registered
 */
gboolean
venture_currency_set_book_treatment(
	const gchar		*currency,
	VentureBookTreatment	 treatment
);

/**
 * venture_currency_get_book_treatment:
 * @currency: (nullable): a currency code
 *
 * How the ledger treats amounts in @currency: a user-defined currency
 * answers what its record says; every built-in ISO code, and any code the
 * registry does not hold, is %VENTURE_BOOK_TREATMENT_VALUED. Whether a
 * valued amount is actually converted also depends on an exchange rate
 * existing -- ask venture_posting_service_route_currency() for that.
 *
 * Returns: the currency's book treatment
 */
VentureBookTreatment
venture_currency_get_book_treatment(const gchar *currency);

/**
 * venture_currency_retain_registered:
 * @keep: (nullable) (array zero-terminated=1): the codes to keep
 *
 * Removes every registered currency whose code is not in @keep. A reload
 * registers the rows it read and then calls this, so a currency that still
 * exists is never missing from the registry, even for a moment.
 */
void
venture_currency_retain_registered(const gchar *const *keep);

/**
 * venture_currency_clear_registered:
 *
 * Removes every user-defined currency, leaving the built-in table. For
 * tests, and for a context that is loading a different database.
 */
void
venture_currency_clear_registered(void);

/**
 * venture_money_get_default_currency:
 *
 * Retrieves the process-wide default currency, used when an amount is
 * created without one. Defaults to "USD" and is set from configuration at
 * startup.
 *
 * Returns: (transfer none): the default currency code
 */
const gchar *
venture_money_get_default_currency(void);

/**
 * venture_money_set_default_currency:
 * @currency: a currency code satisfying venture_currency_is_valid()
 *
 * Sets the process-wide default currency. Called once during startup from
 * the loaded configuration.
 */
void
venture_money_set_default_currency(const gchar *currency);

G_END_DECLS

#endif /* VENTURE_MONEY_H */
