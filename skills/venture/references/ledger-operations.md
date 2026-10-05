# Books housekeeping: assets, setup, cutover, custom values, packs, budgets, equity, group

Read this for the accounting modules around the journal: fixed assets and
deferrals, guided setup and control accounts, opening balances from another
ledger, custom values and scheduled report packs, budgets, owner equity and
consolidation. The journal itself is in [ledger.md](ledger.md). Sources:
`docs/assets.org`, `docs/setup.org`, `docs/cutover.org`,
`docs/custom-fields.org`, `docs/backup.org`, `docs/budgets.org`,
`docs/equity.org`, `docs/group.org`.

## Fixed assets and deferrals (module `assets`)

```bash
venturectl asset place 12                                  # in_service_at= optional
venturectl asset dispose 12 'proceeds=500 USD' disposed_at=2026-07-15
venturectl asset write-off 12 disposed_at=2026-07-15
venturectl assets run-period 2026-01 organization_id=1 dry_run=true   # preview
venturectl assets run-period 2026-01 organization_id=1                # post
```

- Configure ordinary profile fields (cost, salvage, life, method,
  convention, the four accounts) on the *draft* first; status changes only
  through the actions (`POST /api/v1/fixed_assets/:id/place-in-service|
  dispose|write-off`, all stageable with `?stage=1`).
- A period run posts the whole organization-month or nothing; retries post
  once; a closed period is refused. **Pass `dry_run=true`, not
  `--dry-run`**: the global flag is refused for `assets` (exit 2) even
  though the usage line mentions it. `assets run-tax-period` posts the tax
  book (`tax_book` journals statements ignore).
- `create deferral` builds a prepayment/accrual schedule; stage
  `operation=settle` with a settlement account after an accrual's releases
  complete. Never set derived status or edit schedule rows
  (`depreciation_entry`, `deferral_entry`) directly.
- `report fixed_assets` and `report deferrals` accept the ordinary period
  and `as_of`.

## Guided setup and control accounts (module `setup`)

`accounting_setup` is a resumable checklist (legal entity, book currency,
fiscal year, basis, tax profile, chart template, banks, opening cash):
`act accounting_setup ID preview` stores the answers, `act accounting_setup
ID complete` applies them -- updates the organization, seeds a chart when
the entity has none, maps control accounts, generates the fiscal calendar,
creates an operating bank and posts opening cash against retained earnings.
Generic writes cannot mark setup complete.

`accounting_control_map` names a classification and an account in the same
entity: `cash`, `receivables`, `payables`, `tax`, `retained_earnings`,
`clearing`, `inventory`, `income`, `expense`, `currency_clearing` (must be
equity; made on first use as `<org>:3900`), `session_income` (`<org>:4900`),
`arbitrage_positions`/`_gains`/`_fees` (`<org>:1460`/`4960`/`6960`),
`trading_sales`, `trading_purchases`, `trading_income`,
`trading_expenses`, `trading_capital`. `subject_type`/`subject_id`
override per bank or item; `effective_from` dates a restatement.

## Accounting cutover: opening balances from another ledger (module `cutover`)

Opening balances go in as a batch: preview, import, reconcile, activate.
Always preview first and read the batch's `reconciliation_report` and its
`accounting_cutover_row` exceptions -- every bad row is listed, located as
`open_ap[12] bill-9: ...`, and import refuses until the errors are gone.

```bash
venturectl cutover template open_ar                       # a CSV header
venturectl cutover csv source=quickbooks cutoff=2026-01-01 currency=USD \
    open_ar=@ar.csv open_ap=@ap.csv trial_balance=@tb.csv  # previews a batch
venturectl cutover csv ... build_only=true                # print the payload JSON instead
venturectl act accounting_cutover 4 import                # then reconcile, activate
venturectl act accounting_cutover 4 rollback_preflight    # read BLOCKER lines before rollback
```

These actions cannot be staged. Traps: amounts are strict `[-]1234.56 CUR`
strings (no symbols, no `CR`, no locale formats unless the payload sets
`decimal_separator` and `thousands_separator`); every document date must be
before the cutoff; open AR needs `date`; migrated invoices and bills post
to opening clearing (3900), never income, expense or tax, and cannot be
voided normally afterwards. Reconcile ignores activity dated at or after
the cutoff, appends to the report and names failing checks with expected,
ledger and difference. An active batch cannot be rolled back. Sources:
Zoho Books, QuickBooks, Xero or any mapped ledger (`docs/cutover.org`).

## Custom values and scheduled output (modules `custom_fields`, `statements`, `backup`)

- `fields value record_type=TYPE record_id=ID name=NAME value=VALUE`
  PATCHes the owning record's `attributes`; direct writes to
  `custom_field_value` are refused. Empty values clear optional fields and
  fail required-field validation. Built-in property names cannot be
  declared as custom fields ([taxonomy.md](taxonomy.md)).
- `saved_report` stores a report with its period, options and dimension;
  `report_pack` groups saved reports on a `schedule` to `recipients`. Read
  `get report_pack ID` for `last_output` (the last successful scheduled
  result array); `report packs deliver ID` re-sends it by email.
- Accounting packs (`accounting_backup`; `POST
  /api/v1/accounting_backups/export`): version 4 packs restore document
  history into an empty organization and remap identities; external
  references resolve by UUID. Version 3 supports only manual ledger
  imports; older document packs are refused. Packs exclude installation
  credentials and attachments. Scheduled backups and drills are in
  [operations.md](operations.md).

## Budgets, owner equity, consolidation

```bash
venturectl budget vs-actual 2026-08       # GET /api/v1/budget_reports?period=2026-08&kind=actual
venturectl budget forecast 2026-08        # unpaid AR/AP plus remaining budget cash
venturectl equity contribution '1000.00 USD' memo='Owner contribution'
venturectl equity draw '250.00 USD'       # also loan_proceed, loan_payment, transfer
venturectl group income 2026-08           # balance, trial; needs the opt-in group module
```

- `budget` (`period`, `currency`, `dimension`) with `budget_line` per
  account; reports `budget_vs_actual`, `cash_forecast` (the ledger-driven
  forecast, distinct from `cash_outlook`).
- `equity_transaction` posts contributions, draws, loans and transfers
  through mapped accounts (`POST /api/v1/equity/post`).
- Consolidation (`group.enabled`): `intercompany_link` (child organization,
  ownership fraction) and `elimination`; reports `consolidated_*` default
  to the parent's book currency and leave out -- naming in a note -- a
  member book with no rate or in a separate-book/memo currency, instead of
  refusing.
