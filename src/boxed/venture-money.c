/*
 * venture-money.c - Exact monetary amounts
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The invariants this file maintains:
 *
 *   - a value is always an exact integer count of minor units; no operation
 *     ever routes through a double
 *   - every arithmetic operation checks for signed overflow and reports it
 *     as an error instead of wrapping
 *   - rounding, when it is unavoidable, is half to even, which is the
 *     convention that does not bias a long series of roundings upward
 *   - splitting an amount always preserves the total exactly
 *   - operations across currencies fail loudly
 */

#include "venture.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>

/*
 * The process-wide default currency. Written once during startup from the
 * configuration and read from many places afterwards, so it is guarded by
 * g_once and a mutex rather than being a plain global.
 */
static gchar venture_default_currency[VENTURE_MONEY_CURRENCY_LEN] = "USD";
static GMutex venture_default_currency_lock;

/*
 * Currencies whose minor unit is not two decimal digits. Anything absent
 * from this table is assumed to use two, which is correct for the
 * overwhelming majority and is a safe default for a code we do not know.
 */
typedef struct
{
	const gchar	*code;
	guint8		 exponent;
	const gchar	*symbol;
} VentureCurrencyInfo;

static const VentureCurrencyInfo venture_currency_table[] = {
	{ "USD", 2, "$" },
	{ "EUR", 2, "\xe2\x82\xac" },      /* euro sign */
	{ "GBP", 2, "\xc2\xa3" },          /* pound sign */
	{ "CAD", 2, "CA$" },
	{ "AUD", 2, "A$" },
	{ "CHF", 2, "CHF" },
	{ "CNY", 2, "\xc2\xa5" },          /* yen/yuan sign */
	{ "INR", 2, "\xe2\x82\xb9" },      /* rupee sign */
	{ "MXN", 2, "MX$" },
	{ "BRL", 2, "R$" },
	{ "SEK", 2, "kr" },
	{ "NOK", 2, "kr" },
	{ "DKK", 2, "kr" },
	{ "PLN", 2, "z\xc5\x82" },
	{ "NZD", 2, "NZ$" },
	{ "ZAR", 2, "R" },
	{ "SGD", 2, "S$" },
	{ "HKD", 2, "HK$" },
	/* Zero-decimal currencies: a JPY amount of 100 is one hundred yen,
	 * not one yen. Treating these as two-decimal silently inflates every
	 * figure by a factor of a hundred. */
	{ "JPY", 0, "\xc2\xa5" },
	{ "KRW", 0, "\xe2\x82\xa9" },
	{ "VND", 0, "\xe2\x82\xab" },
	{ "CLP", 0, "CLP$" },
	{ "ISK", 0, "kr" },
	{ "UGX", 0, "USh" },
	{ "PYG", 0, "\xe2\x82\xb2" },
	{ "RWF", 0, "FRw" },
	{ "XAF", 0, "FCFA" },
	{ "XOF", 0, "CFA" },
	/* Three-decimal currencies. */
	{ "BHD", 3, ".\xd8\xaf.\xd8\xa8" },
	{ "IQD", 3, "IQD" },
	{ "JOD", 3, "JOD" },
	{ "KWD", 3, "KWD" },
	{ "LYD", 3, "LYD" },
	{ "OMR", 3, "OMR" },
	{ "TND", 3, "TND" },
	{ NULL, 0, NULL }
};

/* --- Internal helpers ---------------------------------------------------- */

/*
 * Ten raised to a small non-negative power, as an exact integer. The table
 * stops at VENTURE_MONEY_MAX_EXPONENT because nothing here ever needs more,
 * and bounding it keeps every scaling operation provably within gint64.
 */
static gint64
venture_money_pow10(guint8 exponent)
{
	static const gint64 powers[] = { 1, 10, 100, 1000, 10000 };

	if (exponent > VENTURE_MONEY_MAX_EXPONENT)
		exponent = VENTURE_MONEY_MAX_EXPONENT;

	return powers[exponent];
}

/*
 * Integer division rounding half to even, also called banker's rounding.
 *
 * The naive alternative -- always rounding a half away from zero -- biases a
 * long series of roundings upward, which over a year of sales tax lines
 * shows up as real money. Half to even splits ties between rounding up and
 * rounding down, so the bias cancels.
 *
 * The tie is detected without computing 2*remainder, which could overflow:
 * an exact half is only possible when the divisor is even and the remainder
 * is exactly half of it.
 */
static gint64
venture_money_div_round_half_even(
	gint64	numerator,
	gint64	denominator
){
	gint64 quotient;
	gint64 remainder;
	gint64 abs_num;
	gint64 abs_den;
	gint64 half;
	gboolean negative;
	gboolean round_away;

	g_assert(0 != denominator);

	negative = ((numerator < 0) != (denominator < 0));

	/* Work with magnitudes so the rounding decision does not have to
	 * reason about the sign of a C integer division, which truncates
	 * toward zero. G_MININT64 has no positive counterpart, so it is
	 * handled by promoting through guint64 arithmetic. */
	abs_num = (numerator < 0) ? -(numerator + 1) + 1 : numerator;
	abs_den = (denominator < 0) ? -(denominator + 1) + 1 : denominator;

	quotient = abs_num / abs_den;
	remainder = abs_num % abs_den;

	if (0 == remainder)
		return negative ? -quotient : quotient;

	half = abs_den / 2;

	if (remainder > half)
	{
		/* Strictly more than half: always round away from zero. */
		round_away = TRUE;
	}
	else if ((remainder == half) && (0 == (abs_den % 2)))
	{
		/* Exactly half: round to the even quotient. */
		round_away = (0 != (quotient % 2));
	}
	else
	{
		round_away = FALSE;
	}

	if (round_away)
		quotient += 1;

	return negative ? -quotient : quotient;
}

/*
 * Scales a minor-unit amount from one exponent to another, rounding half to
 * even when precision is being dropped.
 */
static gboolean
venture_money_scale_amount(
	gint64	  amount,
	guint8	  from_exponent,
	guint8	  to_exponent,
	gint64	 *out_amount,
	GError	**error
){
	gint64 factor;
	gint64 scaled;

	if (from_exponent == to_exponent)
	{
		*out_amount = amount;
		return TRUE;
	}

	if (to_exponent > from_exponent)
	{
		/* Gaining precision is exact but can overflow. */
		factor = venture_money_pow10((guint8)(to_exponent - from_exponent));

		if (__builtin_mul_overflow(amount, factor, &scaled))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "Rescaling %" G_GINT64_FORMAT " from exponent %u to %u overflows",
			            amount, (guint)from_exponent, (guint)to_exponent);
			return FALSE;
		}

		*out_amount = scaled;
		return TRUE;
	}

	/* Losing precision: divide and round. */
	factor = venture_money_pow10((guint8)(from_exponent - to_exponent));
	*out_amount = venture_money_div_round_half_even(amount, factor);

	return TRUE;
}

/*
 * Normalises two operands onto a common exponent, which is always the larger
 * of the two so that no precision is discarded by the act of adding.
 * Verifies that the currencies match.
 */
static gboolean
venture_money_align(
	const VentureMoney	 *a,
	const VentureMoney	 *b,
	gint64			 *out_a,
	gint64			 *out_b,
	guint8			 *out_exponent,
	GError			**error
){
	guint8 exponent;

	if (0 != g_ascii_strcasecmp(a->currency, b->currency))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "Cannot combine %s and %s: no exchange rate is available "
		            "and guessing one would be wrong",
		            a->currency, b->currency);
		return FALSE;
	}

	exponent = MAX(a->exponent, b->exponent);

	if (!venture_money_scale_amount(a->amount, a->exponent, exponent, out_a, error))
		return FALSE;

	if (!venture_money_scale_amount(b->amount, b->exponent, exponent, out_b, error))
		return FALSE;

	*out_exponent = exponent;

	return TRUE;
}

/*
 * Copies a currency code into a fixed buffer, uppercasing it and falling
 * back to the process default when the caller passed nothing usable.
 */
static void
venture_money_store_currency(
	gchar		*dest,
	const gchar	*currency
){
	const gchar *source;
	gsize i;

	source = currency;

	if ((NULL == source) || ('\0' == source[0]))
		source = venture_money_get_default_currency();

	for (i = 0; (i < VENTURE_MONEY_CURRENCY_LEN - 1) && ('\0' != source[i]); i++)
		dest[i] = g_ascii_toupper(source[i]);

	dest[i] = '\0';
}

static const VentureCurrencyInfo *
venture_currency_lookup(const gchar *currency)
{
	gsize i;

	if (NULL == currency)
		return NULL;

	for (i = 0; NULL != venture_currency_table[i].code; i++)
	{
		if (0 == g_ascii_strcasecmp(venture_currency_table[i].code, currency))
			return &venture_currency_table[i];
	}

	return NULL;
}

/* --- Boxed type ---------------------------------------------------------- */

G_DEFINE_BOXED_TYPE(VentureMoney, venture_money,
                    venture_money_copy, venture_money_free)

/* --- Construction -------------------------------------------------------- */

VentureMoney *
venture_money_new(
	gint64		 amount,
	const gchar	*currency,
	guint8		 exponent
){
	VentureMoney *self;

	self = g_new0(VentureMoney, 1);
	self->amount = amount;
	venture_money_store_currency(self->currency, currency);

	/* An out-of-range exponent means the caller did not have one to give;
	 * fall back to whatever the currency naturally uses rather than
	 * silently clamping to a wrong precision. */
	if (exponent > VENTURE_MONEY_MAX_EXPONENT)
		self->exponent = venture_currency_get_exponent(self->currency);
	else
		self->exponent = exponent;

	return self;
}

VentureMoney *
venture_money_new_for_currency(
	gint64		 amount,
	const gchar	*currency
){
	gchar normalised[VENTURE_MONEY_CURRENCY_LEN];

	venture_money_store_currency(normalised, currency);

	return venture_money_new(amount, normalised,
	                         venture_currency_get_exponent(normalised));
}

VentureMoney *
venture_money_new_zero(const gchar *currency)
{
	return venture_money_new_for_currency(0, currency);
}

VentureMoney *
venture_money_copy(const VentureMoney *self)
{
	VentureMoney *copy;

	if (NULL == self)
		return NULL;

	/* The struct holds no pointers, so a shallow copy is a full copy. */
	copy = g_new0(VentureMoney, 1);
	*copy = *self;

	return copy;
}

void
venture_money_free(VentureMoney *self)
{
	g_free(self);
}

/* --- Accessors ----------------------------------------------------------- */

gint64
venture_money_get_amount(const VentureMoney *self)
{
	g_return_val_if_fail(NULL != self, 0);

	return self->amount;
}

const gchar *
venture_money_get_currency(const VentureMoney *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->currency;
}

guint8
venture_money_get_exponent(const VentureMoney *self)
{
	g_return_val_if_fail(NULL != self, 0);

	return self->exponent;
}

gboolean
venture_money_is_zero(const VentureMoney *self)
{
	/* A missing amount is treated as zero: a report that sums a column
	 * where some rows have no value should not have to special-case it. */
	if (NULL == self)
		return TRUE;

	return (0 == self->amount);
}

gboolean
venture_money_is_negative(const VentureMoney *self)
{
	if (NULL == self)
		return FALSE;

	return (self->amount < 0);
}

/* --- Arithmetic ---------------------------------------------------------- */

VentureMoney *
venture_money_add(
	const VentureMoney	 *a,
	const VentureMoney	 *b,
	GError			**error
){
	gint64 left;
	gint64 right;
	gint64 total;
	guint8 exponent;

	g_return_val_if_fail(NULL != a, NULL);
	g_return_val_if_fail(NULL != b, NULL);

	if (!venture_money_align(a, b, &left, &right, &exponent, error))
		return NULL;

	if (__builtin_add_overflow(left, right, &total))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Monetary addition overflows a 64-bit minor-unit amount");
		return NULL;
	}

	return venture_money_new(total, a->currency, exponent);
}

VentureMoney *
venture_money_subtract(
	const VentureMoney	 *a,
	const VentureMoney	 *b,
	GError			**error
){
	gint64 left;
	gint64 right;
	gint64 difference;
	guint8 exponent;

	g_return_val_if_fail(NULL != a, NULL);
	g_return_val_if_fail(NULL != b, NULL);

	if (!venture_money_align(a, b, &left, &right, &exponent, error))
		return NULL;

	if (__builtin_sub_overflow(left, right, &difference))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Monetary subtraction overflows a 64-bit minor-unit amount");
		return NULL;
	}

	return venture_money_new(difference, a->currency, exponent);
}

VentureMoney *
venture_money_negate(const VentureMoney *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	/* G_MININT64 cannot be negated in two's complement. Clamping would be
	 * a silent lie, but this value cannot arise from any real bookkeeping
	 * figure, so warn and saturate rather than fail a void-returning API. */
	if (G_MININT64 == self->amount)
	{
		g_warning("Negating the minimum representable monetary amount; "
		          "saturating at the maximum");
		return venture_money_new(G_MAXINT64, self->currency, self->exponent);
	}

	return venture_money_new(-self->amount, self->currency, self->exponent);
}

VentureMoney *
venture_money_abs(const VentureMoney *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	if (self->amount >= 0)
		return venture_money_copy(self);

	return venture_money_negate(self);
}

VentureMoney *
venture_money_multiply_int(
	const VentureMoney	 *self,
	gint64			  factor,
	GError			**error
){
	gint64 product;

	g_return_val_if_fail(NULL != self, NULL);

	if (__builtin_mul_overflow(self->amount, factor, &product))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "Multiplying %" G_GINT64_FORMAT " by %" G_GINT64_FORMAT
		            " overflows a 64-bit minor-unit amount",
		            self->amount, factor);
		return NULL;
	}

	return venture_money_new(product, self->currency, self->exponent);
}

VentureMoney *
venture_money_multiply_rational(
	const VentureMoney	 *self,
	gint64			  numerator,
	gint64			  denominator,
	GError			**error
){
	gint64 product;
	gint64 result;

	g_return_val_if_fail(NULL != self, NULL);

	if (0 == denominator)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Monetary multiplier has a zero denominator");
		return NULL;
	}

	/* Multiply first, then divide once. Doing it the other way round --
	 * computing the ratio and then scaling -- would round twice and drift.
	 * The intermediate product is the only place this can overflow, and a
	 * genuine amount times a genuine rate never comes close. */
	if (__builtin_mul_overflow(self->amount, numerator, &product))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "Multiplying %" G_GINT64_FORMAT " by %" G_GINT64_FORMAT
		            "/%" G_GINT64_FORMAT " overflows an intermediate value",
		            self->amount, numerator, denominator);
		return NULL;
	}

	result = venture_money_div_round_half_even(product, denominator);

	return venture_money_new(result, self->currency, self->exponent);
}

VentureMoney *
venture_money_multiply_percent(
	const VentureMoney	 *self,
	gint64			  percent_basis_points,
	GError			**error
){
	/* Basis points are hundredths of a percent, so the denominator is
	 * 100 percent * 100 basis points. */
	return venture_money_multiply_rational(self, percent_basis_points,
	                                       10000, error);
}

VentureMoney *
venture_money_convert_at_rate(const VentureMoney *self, gint64 numerator, gint64 denominator,
	const gchar *currency, GError **error)
{
	__int128 product, divisor, quotient, remainder;
	guint8 dest_exp;
	g_return_val_if_fail(NULL != self, NULL);
	if (!venture_currency_is_valid(currency) || numerator <= 0 || denominator <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"A conversion needs a valid destination currency and a positive rate");
		return NULL;
	}
	if (g_strcmp0(self->currency, currency) == 0)
		return venture_money_copy(self);
	dest_exp = venture_currency_get_exponent(currency);
	/* Two 64-bit factors fit here. If scaling overflows this wider
	 * intermediate, even division by the largest rate denominator cannot
	 * bring the result back into the representable minor-unit range. */
	product = (__int128)self->amount * numerator;
	divisor = denominator;
	if (dest_exp > self->exponent)
	{
		if (__builtin_mul_overflow(product,
			(__int128)venture_money_pow10(dest_exp - self->exponent), &product))
			goto overflow;
	}
	else
		divisor *= venture_money_pow10(self->exponent - dest_exp);
	quotient = product / divisor;
	remainder = product % divisor;
	if (remainder < 0)
		remainder = -remainder;
	if (remainder * 2 > divisor || (remainder * 2 == divisor && quotient % 2 != 0))
		quotient += product < 0 ? -1 : 1;
	if (quotient < G_MININT64 || quotient > G_MAXINT64)
		goto overflow;
	return venture_money_new((gint64)quotient, currency, dest_exp);
overflow:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		"Currency conversion exceeds the minor-unit range");
	return NULL;
}

/*
 * Sort helper for the allocation remainder pass: orders indices by
 * descending ratio so that the largest shares absorb the leftover minor
 * units. Ties fall back to the original index, which keeps the result
 * deterministic -- important, because these amounts get written to a ledger
 * and must not change between runs.
 */
typedef struct
{
	gsize	index;
	gint64	ratio;
} VentureAllocationSlot;

static gint
venture_allocation_slot_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const VentureAllocationSlot *slot_a = a;
	const VentureAllocationSlot *slot_b = b;

	if (slot_a->ratio != slot_b->ratio)
		return (slot_a->ratio > slot_b->ratio) ? -1 : 1;

	if (slot_a->index != slot_b->index)
		return (slot_a->index < slot_b->index) ? -1 : 1;

	return 0;
}

GPtrArray *
venture_money_allocate(
	const VentureMoney	 *self,
	const gint64		 *ratios,
	gsize			  n_ratios,
	GError			**error
){
	g_autoptr(GPtrArray) parts = NULL;
	g_autofree gint64 *shares = NULL;
	g_autofree VentureAllocationSlot *slots = NULL;
	gint64 total_ratio;
	gint64 allocated;
	gint64 remainder;
	gsize i;

	g_return_val_if_fail(NULL != self, NULL);
	g_return_val_if_fail(NULL != ratios, NULL);

	if (0 == n_ratios)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Cannot allocate a monetary amount into zero parts");
		return NULL;
	}

	total_ratio = 0;

	for (i = 0; i < n_ratios; i++)
	{
		if (ratios[i] < 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "Allocation ratio %" G_GSIZE_FORMAT " is negative",
			            i);
			return NULL;
		}

		if (__builtin_add_overflow(total_ratio, ratios[i], &total_ratio))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "Allocation ratios sum to more than a 64-bit integer");
			return NULL;
		}
	}

	if (0 == total_ratio)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Allocation ratios sum to zero");
		return NULL;
	}

	shares = g_new0(gint64, n_ratios);
	allocated = 0;

	/* First pass: give each part the floor of its exact share. Truncating
	 * toward zero here (rather than rounding) guarantees the sum of the
	 * shares never exceeds the total, so the remainder is always
	 * non-negative for a positive amount and can simply be handed out. */
	for (i = 0; i < n_ratios; i++)
	{
		gint64 product;

		if (__builtin_mul_overflow(self->amount, ratios[i], &product))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "Allocation overflows an intermediate value");
			return NULL;
		}

		shares[i] = product / total_ratio;
		allocated += shares[i];
	}

	remainder = self->amount - allocated;

	/* Second pass: distribute the leftover minor units, one each, to the
	 * largest ratios first. This is what makes the parts sum back to the
	 * original exactly -- the property that matters when the parts are
	 * about to become ledger lines. */
	slots = g_new0(VentureAllocationSlot, n_ratios);

	for (i = 0; i < n_ratios; i++)
	{
		slots[i].index = i;
		slots[i].ratio = ratios[i];
	}

	qsort(slots, n_ratios, sizeof(VentureAllocationSlot),
	      venture_allocation_slot_compare);

	i = 0;

	while (0 != remainder)
	{
		gsize target;

		target = slots[i % n_ratios].index;

		if (remainder > 0)
		{
			shares[target] += 1;
			remainder -= 1;
		}
		else
		{
			shares[target] -= 1;
			remainder += 1;
		}

		i++;
	}

	parts = g_ptr_array_new_with_free_func((GDestroyNotify)venture_money_free);

	for (i = 0; i < n_ratios; i++)
	{
		g_ptr_array_add(parts, venture_money_new(shares[i], self->currency,
		                                         self->exponent));
	}

	return g_steal_pointer(&parts);
}

GPtrArray *
venture_money_allocate_evenly(
	const VentureMoney	 *self,
	gsize			  n_parts,
	GError			**error
){
	g_autofree gint64 *ratios = NULL;
	gsize i;

	g_return_val_if_fail(NULL != self, NULL);

	if (0 == n_parts)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Cannot allocate a monetary amount into zero parts");
		return NULL;
	}

	ratios = g_new0(gint64, n_parts);

	for (i = 0; i < n_parts; i++)
		ratios[i] = 1;

	return venture_money_allocate(self, ratios, n_parts, error);
}

VentureMoney *
venture_money_sum(
	GPtrArray		 *amounts,
	const gchar		 *fallback_currency,
	GError			**error
){
	g_autoptr(VentureMoney) total = NULL;
	guint i;

	/* An empty set totals to zero, not to an error: a monthly report for a
	 * month with no sales should print 0.00, not fail. */
	if ((NULL == amounts) || (0 == amounts->len))
		return venture_money_new_zero(fallback_currency);

	total = venture_money_copy(g_ptr_array_index(amounts, 0));

	for (i = 1; i < amounts->len; i++)
	{
		g_autoptr(VentureMoney) next = NULL;

		next = venture_money_add(total, g_ptr_array_index(amounts, i), error);

		if (NULL == next)
			return NULL;

		g_clear_pointer(&total, venture_money_free);
		total = g_steal_pointer(&next);
	}

	return g_steal_pointer(&total);
}

/* --- Comparison ---------------------------------------------------------- */

gint
venture_money_compare(
	const VentureMoney	*a,
	const VentureMoney	*b
){
	g_autoptr(GError) local_error = NULL;
	gint64 left;
	gint64 right;
	guint8 exponent;

	g_return_val_if_fail(NULL != a, 0);
	g_return_val_if_fail(NULL != b, 0);

	if (!venture_money_align(a, b, &left, &right, &exponent, &local_error))
	{
		/* Sorting a mixed-currency column is a caller bug rather than a
		 * user error, so it warns and compares equal instead of
		 * inventing an order. */
		g_warning("venture_money_compare: %s", local_error->message);
		return 0;
	}

	if (left < right)
		return -1;

	if (left > right)
		return 1;

	return 0;
}

gboolean
venture_money_equal(
	const VentureMoney	*a,
	const VentureMoney	*b
){
	if (a == b)
		return TRUE;

	if ((NULL == a) || (NULL == b))
		return FALSE;

	if (0 != g_ascii_strcasecmp(a->currency, b->currency))
		return FALSE;

	return (0 == venture_money_compare(a, b));
}

guint
venture_money_hash(const VentureMoney *self)
{
	g_autoptr(GError) local_error = NULL;
	gint64 normalised;
	guint hash;

	g_return_val_if_fail(NULL != self, 0);

	/* Hash on a canonical exponent so two values that compare equal --
	 * 1.50 at exponent 2 and 1.5000 at exponent 4 -- also hash equal. */
	if (!venture_money_scale_amount(self->amount, self->exponent,
	                                VENTURE_MONEY_MAX_EXPONENT,
	                                &normalised, &local_error))
	{
		normalised = self->amount;
	}

	hash = g_int64_hash(&normalised);
	hash ^= g_str_hash(self->currency);

	return hash;
}

/* --- Conversion ---------------------------------------------------------- */

/*
 * Renders the magnitude of an amount as "integer.fraction" with exactly
 * `exponent` fraction digits, optionally grouping the integer part.
 */
static gchar *
venture_money_format_magnitude(
	gint64		amount,
	guint8		exponent,
	gboolean	with_grouping
){
	g_autofree gchar *integer_text = NULL;
	guint64 magnitude;
	guint64 divisor;
	guint64 integer_part;
	guint64 fraction_part;

	/* Go through guint64 so that G_MININT64 -- whose magnitude has no
	 * gint64 representation -- formats correctly rather than wrapping. */
	if (amount < 0)
		magnitude = (guint64)(-(amount + 1)) + 1;
	else
		magnitude = (guint64)amount;

	divisor = (guint64)venture_money_pow10(exponent);
	integer_part = magnitude / divisor;
	fraction_part = magnitude % divisor;

	integer_text = g_strdup_printf("%" G_GUINT64_FORMAT, integer_part);

	if (with_grouping)
	{
		g_autoptr(GString) grouped = NULL;
		gsize length;
		gsize i;

		grouped = g_string_new(NULL);
		length = strlen(integer_text);

		for (i = 0; i < length; i++)
		{
			/* Insert a separator before every group of three that is
			 * not at the start of the number. */
			if ((i > 0) && (0 == ((length - i) % 3)))
				g_string_append_c(grouped, ',');

			g_string_append_c(grouped, integer_text[i]);
		}

		g_free(integer_text);
		integer_text = g_string_free(g_steal_pointer(&grouped), FALSE);
	}

	if (0 == exponent)
		return g_steal_pointer(&integer_text);

	return g_strdup_printf("%s.%0*" G_GUINT64_FORMAT,
	                       integer_text, (int)exponent, fraction_part);
}

gchar *
venture_money_to_string(const VentureMoney *self)
{
	g_autofree gchar *magnitude = NULL;

	g_return_val_if_fail(NULL != self, NULL);

	magnitude = venture_money_format_magnitude(self->amount, self->exponent, FALSE);

	return g_strdup_printf("%s%s %s",
	                       (self->amount < 0) ? "-" : "",
	                       magnitude,
	                       self->currency);
}

gchar *
venture_money_to_display_string(
	const VentureMoney	*self,
	gboolean		 with_grouping
){
	g_autofree gchar *magnitude = NULL;
	const gchar *symbol;

	g_return_val_if_fail(NULL != self, NULL);

	magnitude = venture_money_format_magnitude(self->amount, self->exponent,
	                                           with_grouping);
	symbol = venture_currency_get_symbol(self->currency);

	/* A negative amount reads better with the sign outside the symbol
	 * ("-$5.00") than inside it ("$-5.00"). */
	return g_strdup_printf("%s%s%s",
	                       (self->amount < 0) ? "-" : "",
	                       symbol,
	                       magnitude);
}

/*
 * Refuses an amount that could be read as more than one number, naming
 * why. Returns %NULL so a parser can return it directly.
 */
static VentureMoney *
venture_money_refuse_ambiguous(
	GError		**error,
	const gchar	 *text,
	const gchar	 *why
){
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "\"%s\" is not an unambiguous amount: %s", text, why);
	return NULL;
}

VentureMoney *
venture_money_from_string(
	const gchar	 *text,
	const gchar	 *default_currency,
	GError		**error
){
	g_autofree gchar *working = NULL;
	g_autoptr(GString) digits = NULL;
	gchar currency[VENTURE_MONEY_CURRENCY_LEN];
	gboolean negative;
	gboolean seen_point;
	gboolean have_currency;
	gint64 amount;
	gsize length;
	gsize fraction_digits;
	gsize leading_digits;
	gint group;
	gsize i;

	g_return_val_if_fail(NULL != text, NULL);

	working = g_strstrip(g_strdup(text));
	length = strlen(working);

	if (0 == length)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Cannot parse an empty string as a monetary amount");
		return NULL;
	}

	negative = FALSE;
	have_currency = FALSE;
	currency[0] = '\0';

	/* Accounting notation wraps negatives in parentheses: (12.34). Strip
	 * them first so the sign handling below sees a plain number. */
	if (('(' == working[0]) && (')' == working[length - 1]))
	{
		negative = TRUE;
		working[length - 1] = '\0';
		memmove(working, working + 1, length - 1);
		g_strstrip(working);
		length = strlen(working);
	}

	/* A trailing ISO code: "12.34 USD". Recognised before scanning digits
	 * so its letters are not mistaken for anything else. */
	if (length > 4)
	{
		const gchar *tail;

		tail = working + length - 3;

		if ((' ' == working[length - 4]) &&
		    g_ascii_isalpha(tail[0]) &&
		    g_ascii_isalpha(tail[1]) &&
		    g_ascii_isalpha(tail[2]))
		{
			venture_money_store_currency(currency, tail);
			have_currency = TRUE;
			working[length - 4] = '\0';
			g_strstrip(working);
			length = strlen(working);
		}
	}

	/* A leading ISO code: "USD 12.34". */
	if (!have_currency && (length > 4) && (' ' == working[3]) &&
	    g_ascii_isalpha(working[0]) &&
	    g_ascii_isalpha(working[1]) &&
	    g_ascii_isalpha(working[2]))
	{
		gchar candidate[VENTURE_MONEY_CURRENCY_LEN];

		candidate[0] = working[0];
		candidate[1] = working[1];
		candidate[2] = working[2];
		candidate[3] = '\0';

		venture_money_store_currency(currency, candidate);
		have_currency = TRUE;
		memmove(working, working + 4, length - 3);
		g_strstrip(working);
		length = strlen(working);
	}

	/* Whatever is left should be a signed decimal, possibly decorated
	 * with a currency symbol and thousands separators. Collect the digits
	 * and count how many fell after the decimal point; everything else
	 * that is not a sign or a digit is discarded as decoration. */
	digits = g_string_new(NULL);
	seen_point = FALSE;
	fraction_digits = 0;
	leading_digits = 0;
	/* Digits since the last thousands separator, or -1 before the first. */
	group = -1;

	for (i = 0; i < length; i++)
	{
		gchar c;

		c = working[i];

		if (('-' == c) && (0 == digits->len))
		{
			negative = TRUE;
			continue;
		}

		if (('+' == c) && (0 == digits->len))
			continue;

		if (g_ascii_isdigit(c))
		{
			g_string_append_c(digits, c);

			if (seen_point)
				fraction_digits++;
			else if (group >= 0)
				group++;
			else
				leading_digits++;

			continue;
		}

		/*
		 * Everything below refuses a form that used to be read as some
		 * other number without a word. Each was found in an import:
		 * "1,50 EUR" (a decimal comma) became 150.00, "1.2.3" became
		 * 1.23 and "100.00 CR" became a positive hundred. An amount that
		 * cannot be read one way only is an error, never a guess.
		 */
		if ('.' == c)
		{
			if (seen_point)
				return venture_money_refuse_ambiguous(error, text,
					"it has more than one decimal point");

			if ((group >= 0) && (3 != group))
				return venture_money_refuse_ambiguous(error, text,
					"a thousands separator must be followed by three digits");

			seen_point = TRUE;
			continue;
		}

		/* A comma is a thousands separator in the notation VENTURE
		 * emits, so it may only sit between groups of three digits
		 * before the decimal point. */
		if (',' == c)
		{
			if (seen_point)
				return venture_money_refuse_ambiguous(error, text,
					"a comma follows the decimal point");

			if ((0 == digits->len) || ((group < 0) && (leading_digits > 3)) ||
			    ((group >= 0) && (3 != group)))
				return venture_money_refuse_ambiguous(error, text,
					"a comma that is not a thousands separator may be a decimal comma");

			group = 0;
			continue;
		}

		/* Letters after the number has started are a suffix like "CR"
		 * or "DR" that changes its meaning, or a typo inside it. Before
		 * the first digit they are part of a symbol such as "US$". */
		if (g_ascii_isalpha(c) && (0 != digits->len))
			return venture_money_refuse_ambiguous(error, text,
				"letters follow the digits");

		/* Any other character is a currency symbol or stray
		 * punctuation. Skip it rather than failing: this text comes
		 * from spreadsheets and marketplace exports. */
	}

	if (!seen_point && (group >= 0) && (3 != group))
		return venture_money_refuse_ambiguous(error, text,
			"a comma that is not a thousands separator may be a decimal comma");

	if (0 == digits->len)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "No digits found in \"%s\"", text);
		return NULL;
	}

	if (fraction_digits > VENTURE_MONEY_MAX_EXPONENT)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" has %" G_GSIZE_FORMAT " decimal places; at most %d are supported",
		            text, fraction_digits, VENTURE_MONEY_MAX_EXPONENT);
		return NULL;
	}

	if (!have_currency)
	{
		if ((NULL == default_currency) || ('\0' == default_currency[0]))
		{
			/* Falling back to the process default is right for
			 * interactive use, where the operator has one currency
			 * and never says so. */
			venture_money_store_currency(currency, NULL);
		}
		else
		{
			venture_money_store_currency(currency, default_currency);
		}
	}

	{
		gchar *end = NULL;

		errno = 0;
		amount = g_ascii_strtoll(digits->str, &end, 10);

		if ((NULL != end) && ('\0' != *end))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "\"%s\" is not a valid monetary amount", text);
			return NULL;
		}

		if (0 != errno)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "\"%s\" is out of range for a monetary amount", text);
			return NULL;
		}
	}

	if (negative)
		amount = -amount;

	/* The exponent is whatever the text actually carried, but never less
	 * than the currency's natural precision -- "5 USD" is five dollars
	 * and should be stored as 500 cents so it adds cleanly to 1.23 USD. */
	{
		guint8 natural;
		guint8 exponent;

		natural = venture_currency_get_exponent(currency);
		exponent = (guint8)fraction_digits;

		if (exponent < natural)
		{
			gint64 scaled;

			if (!venture_money_scale_amount(amount, exponent, natural,
			                                &scaled, error))
				return NULL;

			amount = scaled;
			exponent = natural;
		}

		return venture_money_new(amount, currency, exponent);
	}
}

gdouble
venture_money_to_double(const VentureMoney *self)
{
	g_return_val_if_fail(NULL != self, 0.0);

	return (gdouble)self->amount / (gdouble)venture_money_pow10(self->exponent);
}

VentureMoney *
venture_money_rescale(
	const VentureMoney	 *self,
	guint8			  exponent,
	GError			**error
){
	gint64 scaled;

	g_return_val_if_fail(NULL != self, NULL);

	if (exponent > VENTURE_MONEY_MAX_EXPONENT)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "Exponent %u exceeds the maximum of %d",
		            (guint)exponent, VENTURE_MONEY_MAX_EXPONENT);
		return NULL;
	}

	if (!venture_money_scale_amount(self->amount, self->exponent, exponent,
	                                &scaled, error))
		return NULL;

	return venture_money_new(scaled, self->currency, exponent);
}

JsonNode *
venture_money_to_json(const VentureMoney *self)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autofree gchar *formatted = NULL;

	g_return_val_if_fail(NULL != self, NULL);

	formatted = venture_money_to_string(self);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "amount");
	json_builder_add_int_value(builder, self->amount);

	json_builder_set_member_name(builder, "currency");
	json_builder_add_string_value(builder, self->currency);

	json_builder_set_member_name(builder, "exponent");
	json_builder_add_int_value(builder, (gint64)self->exponent);

	/* The formatted form is redundant for a client that understands the
	 * encoding, and essential for one that does not -- notably an AI
	 * reading a tool result, which would otherwise report "amount 123" as
	 * one hundred and twenty-three dollars. */
	json_builder_set_member_name(builder, "formatted");
	json_builder_add_string_value(builder, formatted);

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

VentureMoney *
venture_money_from_json(
	JsonNode	 *node,
	const gchar	 *default_currency,
	GError		**error
){
	JsonObject *object;
	const gchar *currency;
	gint64 amount;
	gint64 exponent;

	g_return_val_if_fail(NULL != node, NULL);

	/* A bare string is the human form; reuse the tolerant parser. */
	if (JSON_NODE_HOLDS_VALUE(node) &&
	    (G_TYPE_STRING == json_node_get_value_type(node)))
	{
		return venture_money_from_string(json_node_get_string(node),
		                                 default_currency, error);
	}

	/* A bare integer is minor units of the default currency. This is what
	 * an AI tends to emit when it has seen the encoding once. */
	if (JSON_NODE_HOLDS_VALUE(node) &&
	    (G_TYPE_INT64 == json_node_get_value_type(node)))
	{
		return venture_money_new_for_currency(json_node_get_int(node),
		                                      default_currency);
	}

	if (!JSON_NODE_HOLDS_OBJECT(node))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		                    "A monetary amount must be an object, a string or an integer");
		return NULL;
	}

	object = json_node_get_object(node);

	if (!json_object_has_member(object, "amount"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		                    "A monetary object requires an \"amount\" member");
		return NULL;
	}

	amount = json_object_get_int_member(object, "amount");

	currency = json_object_has_member(object, "currency")
		? json_object_get_string_member(object, "currency")
		: default_currency;

	if (json_object_has_member(object, "exponent"))
	{
		exponent = json_object_get_int_member(object, "exponent");

		if ((exponent < 0) || (exponent > VENTURE_MONEY_MAX_EXPONENT))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
			            "Exponent %" G_GINT64_FORMAT " is out of range",
			            exponent);
			return NULL;
		}

		return venture_money_new(amount, currency, (guint8)exponent);
	}

	return venture_money_new_for_currency(amount, currency);
}

/* --- Currency metadata --------------------------------------------------- */

guint8
venture_currency_get_exponent(const gchar *currency)
{
	const VentureCurrencyInfo *info;

	info = venture_currency_lookup(currency);

	if (NULL != info)
		return info->exponent;

	/* Two decimal places is right for the vast majority of currencies and
	 * is the least surprising assumption for one we have never seen. */
	return 2;
}

const gchar *
venture_currency_get_symbol(const gchar *currency)
{
	const VentureCurrencyInfo *info;

	info = venture_currency_lookup(currency);

	if (NULL != info)
		return info->symbol;

	/* With no known symbol, the code itself is the clearest label. */
	return (NULL != currency) ? currency : "";
}

gboolean
venture_currency_is_valid(const gchar *currency)
{
	gsize i;

	if (NULL == currency)
		return FALSE;

	for (i = 0; i < 3; i++)
	{
		if (!g_ascii_isalpha(currency[i]))
			return FALSE;
	}

	return ('\0' == currency[3]);
}

const gchar *
venture_money_get_default_currency(void)
{
	/* The buffer is only ever written by the setter below, under the
	 * lock, and only with a NUL-terminated three-letter code. Readers can
	 * therefore take the pointer without locking: the worst a concurrent
	 * write can do is hand back the previous or the next code, both of
	 * which are valid. */
	return venture_default_currency;
}

void
venture_money_set_default_currency(const gchar *currency)
{
	g_return_if_fail(venture_currency_is_valid(currency));

	g_mutex_lock(&venture_default_currency_lock);
	venture_money_store_currency(venture_default_currency, currency);
	g_mutex_unlock(&venture_default_currency_lock);
}
