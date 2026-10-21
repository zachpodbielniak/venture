# Household finance — children 1–6 of epic #201

This is a partial delivery. Shared/personal account settlement, deductible
exports and warranty reminders, a dedicated phone dashboard, and the full-month
acceptance walkthrough (children 7–10) remain open. The responsive walkthrough
and its repairs cover the existing finance pages, not those remaining children.
Refer to #201; do not describe this branch as completing or closing the epic.

## Choose the books before the view

Create a venture with `venture_type=household` (inspect `describe venture`).
The browser's **Venture view** selector offers household navigation: Money,
Bills, Budgets, Goals, Documents and Reports. `/view/venture/ID` selects it;
`/view/venture/all` restores the full view. Both are authenticated browser
routes, not API commands. Disabled modules stay hidden.

The selector changes navigation and words only. Reports and records still
belong to the selected entity (`organization_id`). A household and business
in the same entity share books and access. Use separate entities when their
money must be separate; the view is not an access boundary.

## Accounts, statements and transfers

`describe bank_account` exposes `kind`: checking, savings, card,
cash, loan, investment (old records start unspecified). Link asset ledger
accounts for checking/savings/cash/investments and liability accounts for
cards/loans. These are book balances, not investment lots or market prices.

Use the bank account's existing import actions for CSV/OFX/QFX; optional
`bankfeed` imports provider evidence. Imports alone do not post spending.
A statement line's `correct` action reverses and replaces its bank expense
atomically and creates a disabled merchant rule for that account. Review the
preview and explicitly enable the rule. Ambiguous matches remain for review.

Use the bank account's `transfer` action for movements between owned accounts,
including card payments and loan principal. Link both imported statement lines
when present, so they cannot also become spending. Fees and interest are
separate spending. Different currencies are refused, not guessed.

## Envelopes and bills

Create an active `budget` with monthly `budget_line` envelopes. Set the first
month, amount and per-category rollover. Overspending rolls too; without
rollover each month starts afresh. `budget_envelopes` shows left to spend,
80%-used warnings, used-up and over-budget states. Editing today's settings
recalculates earlier months; these are not locked snapshots.

The money calendar combines outgoing bills/schedules with tentative repeat
charge suggestions. Three equal consecutive charges at an exact weekly,
monthly or yearly cadence are required; missed or changed charges may hide a
real subscription. Confirm one using a `recurring_schedule`. Set subscription
and **Forecast only** for charges already imported from a bank: no duplicate
expense or payment is created. Pausing a matching schedule suppresses its
suggestion; it does not cancel the vendor's contract.

`household_subscriptions` lists confirmed and possible subscriptions separately
with their cost over the twelve months from the period start. Reminders are
opt-in inbox notices (1–28 days, named authorized recipient), swept when the
calendar/inbox opens or by `venturectl calendar remind`. No email, timer or
automatic payment is implied.

## Wealth and savings reports

All use the ordinary `venturectl report NAME PERIOD` and
`GET /api/v1/reports/NAME?period=PERIOD` surfaces. Discover enabled reports
before calling; currencies are kept separate. Web equivalents are
`/reports/NAME`. These reports create no transfers or payments.

| Report | Period and options | Meaning and bounds |
|---|---|---|
| `net_worth` | e.g. `2026`; `organization_id`, `currency`, `as_of` | Monthly posted assets minus liabilities, max 600 months; current classifications apply to old cutoffs. |
| `debt_payoff` | e.g. `2026-01`; same options | Selected bank liabilities, minimum-only/snowball/avalanche, max 200 debts and 600 months. |
| `savings_goals` | exactly one month; `organization_id`, `currency`, `goal_id`; no `as_of` | Linked goals, direct posted asset balance or rollover envelope left; forecast max 1200 months. |
| `household_subscriptions` | e.g. `2026-01` | Calendar occurrences over the following twelve months, not posted spending. |
| `budget_envelopes` | exactly one month | Allocation, carry, spending and left to spend by category. |

For debt planning edit `bank_account`'s `payoff_enabled`, `payoff_apr_bps`
(100 = 1%), `payoff_minimum` and `payoff_extra`. Selected extras form one pool
per currency; snowball/avalanche recycle retired minimums. Projections assume
fixed rates and monthly payments, no new charges/fees; incomplete projections
have no payoff date or interest-saved claim. They are not lender quotes.

For a `goal`, set exactly one `savings_account_id` (asset ledger account) or
`savings_envelope_id` (active rollover budget line), a positive `savings_target`,
optional `savings_monthly`, and optional `due_on`. Do not fill the manual numeric
progress fields. The goal detail and `savings_goals` use the same money report.
Missing contribution means no future forecast; missing due date means no
monthly-needed value. Current settings apply at historical month-end cutoffs.

Each goal reads its whole source: shared sources are not reserved or amounts
to add together. Envelopes represent allocations, not bank cash. Forecasts
assume the configured net contribution with no interest, fees or withdrawals.
The savings query applies target presence and currency before counting its
row bound; ordinary and other-currency goals cannot exhaust a narrowed report.
Too many matching goals still fail: narrow by `goal_id`.

Full setup, calculation rules and migration notes: `docs/household.org`.
