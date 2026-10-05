# The books: journals, posting, statements, periods, close

Read this for anything that touches the general ledger: posting, reversing,
balances, statements, fiscal periods, the close, second-person consent and
automatic journals. Housekeeping (assets, cutover, setup, custom values,
report packs, budgets, equity, consolidation) is in
[ledger-operations.md](ledger-operations.md); currencies and holdings in
[currencies.md](currencies.md). Sources: `docs/ledger.org`,
`docs/statements.org`, `docs/periods.org`, `docs/close.org`,
`docs/autojournal.org`, `docs/money.org`.

## The model

- **Journals are immutable posted evidence.** A `journal` is draft, then
  posted, and a correction is a *reversing* journal plus a replacement --
  never an edit. `journal` and `journal_line` can be edited only while
  draft; generic updates cannot advance `state`.
- **`ledger_entry` rows are read-only projections** of posted lines;
  generic writes to them (staged ones too) are refused.
- **Posting happens on save** through one posting service: saving a sale
  with `gross` or an expense with an `amount` posts it (it needs an
  organization and a posting profile). Correcting financial values creates
  reversal and replacement journals; a re-save that changes nothing
  financial posts nothing. **Deleting a source record does not erase its
  journal** -- deletion is not a financial correction.
- Every automatic journal goes through the posting service's currency route
  (book, valued, separate book, memo) -- see [currencies.md](currencies.md).
- The fiscal-period guard runs inside posting: closed or locked dates
  refuse financial writes.

## Posting and reversing by hand

```bash
venturectl journal post 42                               # post a draft (editors propose)
venturectl --stage journal post 42                       # always propose
venturectl act journal 42 reverse occurred_at=2026-09-13 memo="Correction"
venturectl act journal 0 create_and_post 'journal={"organization_id":1,"source_type":"organization","source_id":1,"currency":"USD","occurred_at":"2026-09-13","lines":[{"account_id":1,"side":"debit","amount":"10 USD"},{"account_id":2,"side":"credit","amount":"10 USD"}]}'
```

- `POST /api/v1/journals/post` takes the same header plus `lines`
  (`?stage=1` too); it dispatches the type-level `create_and_post` action.
  Use real source and account ids from the same organization. Invalid lines
  leave no draft behind; closed periods and repeat reversals are refused.
- A manual journal may mix currencies only through `create_and_post`:
  kept-apart currencies each get a journal balanced through the "Currency
  clearing" equity account. `act journal ID post` on a saved draft cannot
  split, and refuses a line the rule keeps apart. A memo-currency line is
  refused.
- An organization **editor**'s `journal post` returns a *confirmation*
  (202), not a posting: treat it as pending until finance approves.
  Approval rechecks the draft header, version and line fingerprint.
  Generated assistant and MCP action tools always stage.

## Second-person consent (accounting approval rules)

When an organization enables a post/pay second-actor rule
(`accounting_approval_rule`), an operation that posts or pays first returns
**permission denied after saving a pending proposal**. That is not a
success and is separate from `--stage`/202. A *different* authorized
account must repeat the same business command with the same inputs; two
tokens of one account do not qualify. One consent covers its generated
invoices, allocations and journals and is consumed only when the whole
operation succeeds. Changed inputs or records need a fresh proposal;
consent expires after 24 hours and after a server restart or posting-rule
replacement. Drafts and read-only previews stay available. Use an explicit
date for reproducible posting commands, and re-read records after a failed
operation.

## Automatic journals and backfill

`posting_profile` (generic CRUD; `describe posting_profile`) maps a sale's
and an expense's legs to accounts. `post backfill [organization_id=ID]
[--dry-run]` is an editor action that posts missing sale/expense versions
in date order:

```bash
venturectl report unposted all
venturectl post backfill --dry-run organization_id=1     # validates, keeps nothing
venturectl post backfill organization_id=1
```

The answer carries `candidates`, `posted`, `skipped`, `dry_run`. A period
refusal aborts the whole batch.

## Balances and statements

- `report trial_balance PERIOD` -- posted balances at the period's end, one
  section per book currency, the organization's first, each row labelled
  in `books` (`Book currency: GOLD`, `Separate book: TICKET`, `Own book:
  EUR (no rate to GOLD)`). The operational `pnl` reads sales and expenses;
  it is not the authoritative journal balance.
- `report balance_sheet|income_statement|cash_flow|general_ledger|
  account_balances|pnl_reconciliation` read posted evidence per exact
  organization and currency, book currency first, with a `books` label (a
  separate book or a rate-less currency is its own section; memo never
  appears). `compare_to=2026-07` after the period adds prior/delta
  columns; `general_ledger` takes `account_id=ID`; all take `basis=cash|
  accrual`, `dimension`, `as_of`.

```bash
venturectl -f csv report balance_sheet 2026-08 organization_id=1 currency=USD compare_to=2026-07
```

Synthetic totals have no account id; real account/journal ids link to
their pages. Cash-flow classes follow the chart codes in
`docs/statements.org` (`cash_equivalent`, `cash_flow_class` on `account`).
`report year_end_pack` is a zip of CSVs (`/books/year-end-pack.zip`).

## Fiscal periods

Fiscal calendars are ordinary `fiscal_year` and `fiscal_period` records;
creating a year generates its monthly or quarterly periods. Closed or
locked dates refuse financial writes. Reopening needs `periods.reopen`,
held by active administrators and owners; locked periods cannot reopen.
`report snapshot_vs_live PERIOD` compares totals preserved at close with
live reports; `as_of=` on any financial report gives the historical view.

## The close

```bash
venturectl close open fiscal_period_id=1 currency=USD    # or: act fiscal_period ID open_close [currency=USD]
venturectl list close_task workspace_id=1
venturectl act close_task 7 complete 'notes=Finding'     # or: waive 'notes=Reason'
venturectl act close_discrepancy 3 explain 'explanation=Evidence'
venturectl close run 1; venturectl close sign 1 role=preparer
venturectl close sign 1 role=reviewer                    # a different account
venturectl close pack 1; venturectl close complete 1     # close reopen 1 to undo
venturectl report close_workspace 2026-01 organization_id=1
```

`act close_workspace ID run_checks|sign|complete|reopen` is the same
service. Review must come from a different authenticated account. These
actions cannot be staged and need organization finance, owner or
administrator; signed/closed task evidence must be reopened before
completion or waiver can change it. A close ties out one currency (the
workspace's; the book currency when omitted). An unmatched bank line in a
separate-book, rate-less or memo currency is left out and named in the
`bank_recon` task's notes; one in a currency with a rate still refuses.

## Daily accounting home

`venturectl accounting` (`GET /api/v1/accounting/home`) lists the books'
next actions, each with `kind`, `count`, `href` and `reason`: unmatched
bank lines, overdue invoices, bills to pay, open close-checklist tasks, the
capture inbox. Start there when asked "what needs doing in the books".
