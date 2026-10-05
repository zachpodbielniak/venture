# Money, currencies and holdings

Read this whenever an amount, a currency, an exchange rate, a wallet/till/
character balance or a multi-currency report is involved. Sources:
`docs/money.org`, `docs/currencies.org`, `docs/ledger.org` (holdings).

## Money

Integer minor units plus a currency code and its exponent; half-to-even
rounding; splits that sum back exactly; arithmetic across currencies
refused rather than guessed. Write `amount=12.34` (read in the record's
currency) or name it: `"12.34 EUR"`, `"150 POINTS"`, `"12g 34s 56c GOLD"`.
Read back `{"amount":1234,"currency":"USD","exponent":2,"formatted":"12.34 USD"}`
-- `.formatted` for display, `.amount` for arithmetic, never a float. A
currency nobody named is the organization's **book currency**
(`default_currency`), never a hard-coded USD. A currency code is 2-15
characters (`[A-Z][A-Z0-9_]{1,14}`), not only ISO 4217.

## User-defined currencies (`currency`, core)

- Fields: `code`, `name`, `kind` (`virtual|points|commodity|other`),
  `exponent` (0-6), `symbol`, `symbol_position` (`prefix|suffix`),
  `denominations` (a JSON **string**), `book_treatment`
  (`valued|separate_book|memo`), `description`. Check with `describe
  currency`.
- Creating, editing or deleting one needs `admin` or `owner`; reading is
  open; an editor's write is 403. `code` and `exponent` cannot change once
  saved; a built-in ISO code (USD, EUR, JPY...) cannot be defined; the code
  is unique install-wide.

```sh
venturectl create currency code=GOLD name=Gold exponent=4 \
  denominations='[{"suffix":"g","units":10000},{"suffix":"s","units":100},{"suffix":"c","units":1}]'
venturectl create sale venture_id=1 gross="12g 34s 56c"
```

- Denominations: largest first, each dividing the one before, the last
  worth exactly 1 minor unit, suffixes without digits/spaces/points.
  `.formatted` stays the canonical decimal (`"12.3456 GOLD"`).
- A coin amount without a code is read in the field's default currency if
  its coins fit, else in the one currency that has those coins; two
  candidates is refused as ambiguous -- add the code. A word beside a
  number is a code only if it is three letters or registered (`100.00 CR`
  is not "CR").
- A deleted currency still displays its amounts the same way; restore it to
  edit it. Its code stays taken.
- **Doors that settle real money take ISO only**: Stripe checkout, tax
  filing and Shopify import refuse a user-defined currency, even a
  registered three-letter one.
- Register a game currency (GOLD at exponent 4 for the Blizzard plugin)
  *before* the first feed sync; a provider never creates one.

## Exchange rates and book treatment

Value one currency in another with an ordinary `exchange_rate`
(`from_currency=GOLD to_currency=USD rate_numerator=15
rate_denominator=1000 effective_at=2026-03-01`). Nothing invents a rate;
nothing is ever inverted.

`book_treatment` says what the ledger does with a currency:

- `valued` (default): converted into the organization's book currency when
  an `exchange_rate` to it exists on the date (one book-currency journal;
  each line keeps `amount` and gets `book_amount`); with no rate on the
  date it posts as its own balanced journal.
- `separate_book`: always its own journal, never converted.
- `memo`: never posted; tracked as holding movements only. An
  organization's `default_currency` can never be memo (refused on both the
  currency and the organization).

The treatment may change; only later postings follow it -- a re-save with
the same original amounts is not reposted because a rate or treatment
changed since. This applies to every currency, ISO included: once a
`EUR`->`USD` rate is recorded, a EUR expense in a USD organization posts
one USD journal whose lines keep the EUR amount. `report trial_balance`
still shows each separate book as its own balanced section. A document
spanning several journals balances through currency clearing
(`currency_clearing`, else `<org>:3900`).

## Several currencies in one organization

- Operational reports (`pnl`, `ventures`, `monthly`) and the inventory,
  holdings, listing, session and arbitrage reports keep **one figure per
  currency**, book currency first, plain metric keys for the book currency
  and `_<CODE>` for others (`revenue_TICKET`, `valuation_GOLD`). Never add
  rows of different currencies ([reports.md](reports.md)).
- Stock bought in several currencies keeps them apart: an issue posts one
  cost-of-goods pair per currency its FIFO layers cost (each by its
  treatment; memo posts nothing); `report inventory_valuation` and `report
  inventory` give a row per currency (`books` says how it reaches the
  books). A unit made from GOLD and TICKET inputs carries both shares as one
  lot.
- `report inventory` shows each location's path and the average unit cost
  of the units carrying each currency; its metrics are `value` (book
  currency) and `value_<CODE>`, the valuation's `valuation` and
  `valuation_<CODE>`.
- A purchase order line and a vendor bill line must be in their document's
  `currency`. **Stock priced in another currency comes in through a
  purchase order** -- `create purchase_order status=draft currency=TICKET
  vendor_id=N` (a supplier company) and its `purchase_order_line
  inventory_item_id=N quantity=2 unit_price="4 TICKET"`, then `purchase
  approve|send|receive` ([payables.md](payables.md)):
  the receipt is a FIFO cost layer in the order's currency, journalled
  Inventory against GRNI by its treatment (memo: none). A purchase order is
  not a payment: a memo currency paid out of a purse is a `holding_txn`
  `kind=spend` (negative); a posted one is the vendor bill.

## Holdings: what each wallet, till or character holds (module `ledger`)

A **holding** is an `account` with `location_id` set (a purse, a till, a
petty-cash tin, a game character). It holds every currency. What moves it:

- a `sale` or `expense` whose `cash_account_id` is the holding account;
- `act session ID post`: each money yield goes into the holding at the
  session's `location_id` (credit `session_income`, else `<org>:4900`);
  no location, or the ledger off, leaves the money yield unposted
  (skipped, not refused);
- `act location FROM transfer to_location_id=TO amount="5 TICKET"
  [occurred_at=...] [notes=...]` -- one currency per transfer, a bare
  number in the book currency, both locations in one organization;
- an arbitrage leg (through the venue's location, or the leg's
  `location_id`), and an external ledger posted to the books
  ([accounts.md](accounts.md));
- a `holding_txn` by hand -- **memo currencies only** (`amount` signed;
  `kind` `adjust` default, `earn` positive, `spend` negative; `transfer`
  refused by hand).

Posted currencies (book, valued, separate book) are held as the account's
journal lines; memo currencies as `holding_txn` rows. **Never create a
`holding_txn` in a posted currency** (refused) and never edit or delete
one whose `source_type` is set (change the sale/expense/session instead).
**A holding cannot go below zero at any moment** -- the document that would
do it is refused whole ("... holds 12 TICKET on 2026-03-02 and this takes
50 TICKET ...") unless the account has `allow_negative=true`. It is the
balance *on the document's date*: a spend back-dated before the takings it
needs is refused, so record (or date) what came in first. Accounts with no
location are never judged. A location with two holding accounts makes
`transfer` and `session post` refuse -- name the account. One is made on
first use (`<org>:holding:<location>`) when there is none. Deleting a
document keeps its holding movements, as it keeps its journal.

```sh
venturectl create account code=EVM-1101 name="Aria's purse" kind=asset location_id=12 organization_id=2
venturectl create expense venture_id=3 amount="20 TICKET" cash_account_id=40 description=Prize organization_id=2
venturectl act location 12 transfer to_location_id=13 amount="5 TICKET"
venturectl report holdings all organization_id=2 location_id=12 currency=TICKET
```

`report holdings`: one row per location path **and currency**, book
currency first: `location`, `currency`, `books` (e.g. `Memo: TICKET
(counted here, never posted)`), `earned`, `spent`, `transfers` (net),
`net`, `balance`; metrics `held` (book currency) and `held_<CODE>`.
`location_id` includes every location beneath it; `as_of=` dates it.
`holdings` is the ledger's report -- the operator's in-game inventory is
`account_holdings` ([accounts.md](accounts.md)).
