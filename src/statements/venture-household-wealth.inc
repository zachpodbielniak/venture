/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Shares the statement engine's scoped, posted book evidence. */
static VentureReportResult *
household_net_worth(Books *books, VentureDateRange *period, GError **error)
{
	g_autoptr(VentureReportResult) r = venture_report_result_new("Net worth over time", period);
	g_autoptr(GDateTime) month = g_date_time_new(g_date_time_get_timezone(books->start),
		g_date_time_get_year(books->start), g_date_time_get_month(books->start), 1, 0, 0, 0);
	guint count = 0;
	text_column(r, "key", "Month");
	text_column(r, "as_of", "Through");
	text_column(r, "currency", "Currency");
	money_column(r, "assets", "Assets");
	money_column(r, "liabilities", "Liabilities");
	money_column(r, "net_worth", "Net worth");
	while (g_date_time_compare(month, books->end) < 0)
	{
		g_autoptr(GDateTime) next = g_date_time_add_months(month, 1);
		GDateTime *end = next != NULL && g_date_time_compare(next, books->end) < 0 ? next : books->end;
		g_autoptr(GDateTime) through = g_date_time_add(end, -1);
		g_autofree gchar *label = g_date_time_format(month, "%Y-%m");
		g_autofree gchar *date = g_date_time_format_iso8601(through);
		guint c, i;
		if (++count > 600)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Net worth supports at most 600 months per report");
			return NULL;
		}
		for (c = 0; c < books->currencies->len; c++)
		{
			const gchar *currency = g_ptr_array_index(books->currencies, c);
			g_autoptr(VentureMoney) assets = venture_money_new_zero(currency);
			g_autoptr(VentureMoney) liabilities = venture_money_new_zero(currency);
			g_autoptr(VentureMoney) worth = NULL;
			for (i = 0; i < books->entries->len; i++)
			{
				Evidence *e = g_ptr_array_index(books->entries, i);
				VentureEntity *account = find_account(books, e->account_id);
				gint kind;
				if (account == NULL || g_strcmp0(e->amount->currency, currency) != 0 ||
					g_date_time_compare(e->date, end) >= 0)
					continue;
				kind = account_kind(account);
				if (kind == VENTURE_ACCOUNT_KIND_ASSET &&
					!add(&assets, e->amount, e->side == VENTURE_LEDGER_SIDE_CREDIT, error)) return NULL;
				if (kind == VENTURE_ACCOUNT_KIND_LIABILITY &&
					!add(&liabilities, e->amount, e->side == VENTURE_LEDGER_SIDE_DEBIT, error)) return NULL;
			}
			worth = venture_money_subtract(assets, liabilities, error);
			if (worth == NULL) return NULL;
			venture_report_result_begin_row(r);
			venture_report_result_set_text(r, "key", label);
			venture_report_result_set_text(r, "as_of", date);
			venture_report_result_set_text(r, "currency", currency);
			venture_report_result_set_money(r, "assets", assets);
			venture_report_result_set_money(r, "liabilities", liabilities);
			venture_report_result_set_money(r, "net_worth", worth);
		}
		if (next == NULL) break;
		g_date_time_unref(month);
		month = g_steal_pointer(&next);
	}
	venture_report_result_append_note(r, "Posted book values at each month end (or the cutoff shown), not market valuations. Assets minus liabilities; parent accounts are not counted twice. Current account classifications apply to history.");
	note_sections(r, books);
	return g_steal_pointer(&r);
}

typedef struct
{
	gint64 id, apr, minimum, extra, principal;
	gchar *name, *currency;
} HouseholdDebt;

typedef struct
{
	gint64 balance, interest;
	guint months;
} HouseholdPayoff;

static void
household_debt_free(gpointer data)
{
	HouseholdDebt *debt = data;
	g_free(debt->name);
	g_free(debt->currency);
	g_free(debt);
}

/* Integer minor units throughout, including half-even monthly interest. */
static gboolean
household_debt_add(gint64 *amount, gint64 increment, GError **error)
{
	if (increment < 0 || *amount > G_MAXINT64 - increment)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Debt projection exceeds the money range");
		return FALSE;
	}
	*amount += increment;
	return TRUE;
}

static HouseholdPayoff *
household_payoff(GPtrArray *debts, const gchar *currency, guint strategy, GError **error)
{
	g_autofree HouseholdPayoff *state = g_new0(HouseholdPayoff, debts->len);
	gint64 budget = 0;
	guint i, month;
	for (i = 0; i < debts->len; i++)
	{
		HouseholdDebt *d = g_ptr_array_index(debts, i);
		if (g_strcmp0(d->currency, currency) != 0) continue;
		state[i].balance = d->principal;
		if (!household_debt_add(&budget, d->minimum, error) ||
			!household_debt_add(&budget, d->extra, error)) return NULL;
	}
	for (month = 1; month <= 600; month++)
	{
		gint64 available = budget;
		gboolean remaining = FALSE;
		for (i = 0; i < debts->len; i++)
		{
			HouseholdDebt *d = g_ptr_array_index(debts, i);
			g_autoptr(VentureMoney) balance = NULL, interest = NULL;
			gint64 payment;
			if (state[i].balance == 0) continue;
			balance = venture_money_new_for_currency(state[i].balance, currency);
			interest = venture_money_multiply_rational(balance, d->apr, 120000, error);
			if (interest == NULL || !household_debt_add(&state[i].balance, interest->amount, error) ||
				!household_debt_add(&state[i].interest, interest->amount, error)) return NULL;
			payment = MIN(d->minimum, state[i].balance);
			state[i].balance -= payment;
			available -= payment;
		}
		/* Minimum-only drops each retired payment. Strategies retain the full
		 * budget and put the remainder against one priority debt at a time. */
		while (strategy != 0 && available > 0)
		{
			guint chosen = debts->len;
			gint64 payment;
			for (i = 0; i < debts->len; i++)
			{
				HouseholdDebt *d = g_ptr_array_index(debts, i);
				HouseholdDebt *best;
				if (state[i].balance == 0) continue;
				if (chosen == debts->len) { chosen = i; continue; }
				best = g_ptr_array_index(debts, chosen);
				if ((strategy == 1 && state[i].balance < state[chosen].balance) ||
					(strategy == 2 && d->apr > best->apr)) chosen = i;
			}
			if (chosen == debts->len) break;
			payment = MIN(state[chosen].balance, available);
			state[chosen].balance -= payment;
			available -= payment;
		}
		for (i = 0; i < debts->len; i++)
		{
			HouseholdDebt *d = g_ptr_array_index(debts, i);
			if (g_strcmp0(d->currency, currency) != 0) continue;
			if (state[i].balance > 0) remaining = TRUE;
			else if (state[i].months == 0 && d->principal > 0) state[i].months = month;
		}
		if (!remaining) break;
	}
	return g_steal_pointer(&state);
}

static gboolean
household_payoff_row(VentureReportResult *r, const gchar *key, const gchar *name,
	const gchar *strategy, const gchar *currency, gint64 principal, gint64 monthly,
	HouseholdPayoff *payoff, HouseholdPayoff *baseline, GDateTime *start, GError **error)
{
	g_autoptr(VentureMoney) money = venture_money_new_for_currency(principal, currency);
	venture_report_result_begin_row(r);
	venture_report_result_set_text(r, "key", key);
	venture_report_result_set_text(r, "name", name);
	venture_report_result_set_text(r, "strategy", strategy);
	venture_report_result_set_text(r, "currency", currency);
	venture_report_result_set_money(r, "principal", money);
	money->amount = monthly;
	venture_report_result_set_money(r, "monthly", money);
	money->amount = payoff->interest;
	venture_report_result_set_money(r, "interest", money);
	money->amount = payoff->balance;
	venture_report_result_set_money(r, "remaining", money);
	venture_report_result_set_text(r, "status", payoff->balance == 0 ? "Paid off" : "Not paid off within 600 months");
	if (payoff->balance == 0)
	{
		g_autoptr(GDateTime) date = g_date_time_add_months(start, payoff->months);
		g_autofree gchar *label = NULL;
		if (date == NULL)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Payoff date exceeds the supported calendar");
			return FALSE;
		}
		label = g_date_time_format(date, "%Y-%m-%d");
		venture_report_result_set_text(r, "payoff_date", label);
		if (baseline->balance == 0)
		{
			money->amount = baseline->interest - payoff->interest;
			venture_report_result_set_money(r, "interest_saved", money);
		}
	}
	return TRUE;
}

static VentureReportResult *
household_debt_report(Books *books, VentureDateRange *period, GError **error)
{
	g_autoptr(VentureReportResult) r = venture_report_result_new("Debt payoff planner", period);
	g_autoptr(GPtrArray) debts = g_ptr_array_new_with_free_func(household_debt_free);
	g_autoptr(GPtrArray) banks = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GDateTime) start = g_date_time_add(books->end, -1);
	guint i, c, strategy;
	const gchar *strategies[] = { "minimum", "snowball", "avalanche" };
	text_column(r, "key", "Key");
	text_column(r, "name", "Debt");
	text_column(r, "strategy", "Strategy");
	text_column(r, "currency", "Currency");
	money_column(r, "principal", "Starting debt");
	money_column(r, "monthly", "Monthly budget / minimum");
	money_column(r, "interest", "Interest through payoff / horizon");
	money_column(r, "interest_saved", "Interest saved vs minimum only");
	money_column(r, "remaining", "Remaining at horizon");
	text_column(r, "payoff_date", "Estimated payoff date");
	text_column(r, "status", "Status");
	if (venture_entity_registry_lookup(venture_entity_registry_get_default(), "bank_account") == 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "Debt planning requires the banking module");
		return NULL;
	}
	query = venture_query_new(VENTURE_TYPE_BANK_ACCOUNT);
	venture_query_set_organization(query, books->org);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	banks = venture_database_find(books->db, query, error);
	if (banks == NULL) return NULL;
	for (i = 0; i < banks->len; i++)
	{
		VentureEntity *bank = g_ptr_array_index(banks, i), *account;
		gboolean enabled = FALSE;
		gint64 id = 0;
		g_autofree gchar *currency = NULL;
		g_autoptr(VentureMoney) minimum = NULL, extra = NULL;
		g_autoptr(VentureMoney) opening = NULL, debits = NULL, credits = NULL, closing = NULL;
		HouseholdDebt *d;
		guint j;
		g_object_get(bank, "payoff-enabled", &enabled, "currency", &currency, NULL);
		if (!enabled || (books->currency != NULL && g_strcmp0(books->currency, currency) != 0)) continue;
		g_object_get(bank, "account-id", &id, "payoff-minimum", &minimum, "payoff-extra", &extra, NULL);
		account = find_account(books, id);
		if (account == NULL || venture_entity_is_deleted(account) || account_kind(account) != VENTURE_ACCOUNT_KIND_LIABILITY ||
			minimum == NULL || minimum->amount <= 0 || g_strcmp0(minimum->currency, currency) != 0 ||
			(extra != NULL && (extra->amount < 0 || g_strcmp0(extra->currency, currency) != 0)))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Each planned debt needs a live liability account and a positive minimum in its currency");
			return NULL;
		}
		for (j = 0; j < debts->len; j++)
		{
			HouseholdDebt *other = g_ptr_array_index(debts, j);
			if (other->id == id && g_strcmp0(other->currency, currency) == 0)
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Select each liability account only once per currency");
				return NULL;
			}
		}
		if (!measure(books, id, currency, FALSE, &opening, &debits, &credits, &closing, error)) return NULL;
		if (closing->amount == G_MININT64)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Debt balance exceeds the projection range");
			return NULL;
		}
		d = g_new0(HouseholdDebt, 1);
		g_ptr_array_add(debts, d);
		d->id = id;
		d->currency = g_strdup(currency);
		d->minimum = minimum->amount;
		d->extra = extra != NULL ? extra->amount : 0;
		d->principal = MAX(0, -closing->amount);
		g_object_get(bank, "name", &d->name, "payoff-apr-bps", &d->apr, NULL);
		if (d->apr < 0 || d->apr > 100000 || debts->len > 200)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Debt planning supports up to 200 debts and APR from 0 to 100000 basis points");
			return NULL;
		}
		add_currency(books, currency);
	}
	sort_currencies(books);
	for (c = 0; debts->len > 0 && c < books->currencies->len; c++)
	{
		const gchar *currency = g_ptr_array_index(books->currencies, c);
		g_autofree HouseholdPayoff *baseline = household_payoff(debts, currency, 0, error);
		if (baseline == NULL) return NULL;
		for (strategy = 0; strategy < 3; strategy++)
		{
			g_autofree HouseholdPayoff *state = household_payoff(debts, currency, strategy, error);
			HouseholdPayoff total = { 0, 0, 0 }, base = { 0, 0, 0 };
			gint64 principal = 0, budget = 0;
			gboolean found = FALSE;
			if (state == NULL) return NULL;
			for (i = 0; i < debts->len; i++)
			{
				HouseholdDebt *d = g_ptr_array_index(debts, i);
				g_autofree gchar *key = NULL;
				if (g_strcmp0(d->currency, currency) != 0) continue;
				found = TRUE;
				key = g_strdup_printf("%s:%" G_GINT64_FORMAT, strategies[strategy], d->id);
				if (!household_debt_add(&principal, d->principal, error) ||
					!household_debt_add(&budget, d->minimum, error) ||
					(strategy != 0 && !household_debt_add(&budget, d->extra, error)) ||
					!household_debt_add(&total.balance, state[i].balance, error) ||
					!household_debt_add(&total.interest, state[i].interest, error) ||
					!household_debt_add(&base.balance, baseline[i].balance, error) ||
					!household_debt_add(&base.interest, baseline[i].interest, error)) return NULL;
				total.months = MAX(total.months, state[i].months);
				if (!household_payoff_row(r, key, d->name, strategies[strategy], currency, d->principal, d->minimum, &state[i], &baseline[i], start, error)) return NULL;
			}
			if (found && !household_payoff_row(r, strategies[strategy], "All selected debts", strategies[strategy], currency, principal, budget, &total, &base, start, error)) return NULL;
		}
	}
	venture_report_result_append_note(r, "Select debts and set APR (basis points), a fixed monthly minimum and optional pooled extra contribution on Bank accounts. Starting debt is the posted liability balance at the selected cutoff; credit balances count as zero. Only selected accounts are included, with no parent rollup. Payments start one month after the cutoff on its anchored day, clamped in shorter months. Monthly interest is APR/12 rounded half to even; no fees, new charges or rate changes. Minimum-only drops retired payments and uses no extra; snowball pays the smallest remaining balance first, avalanche the highest APR first, after all minimums. Both recycle the fixed initial budget; ties use account record order. Savings are blank if either projection is unfinished after 600 months. These estimates do not create payments.");
	note_sections(r, books);
	return g_steal_pointer(&r);
}
