# Data model: the books

Finance, the general journal, banking, periods and close, statements,
assets, tax, and the accounting housekeeping modules. Conventions are in
[data-model.md](data-model.md); how to *use* these is in
[ledger.md](ledger.md), [ledger-operations.md](ledger-operations.md) and
[payables.md](payables.md). Every type of a module that requires `finance`
is *financial*: organization finance/admin/owner to write.

## finance (requires sales) -- reports `pnl`, `ventures`, `monthly`, `tax`, `tax_liability`

- `expense` -- money out. **description**, `vendor`, `amount`,
  `occurred_at`, `venture_id`, `category`, `tax_category_id -> tax_category`,
  `deductibility` [none|full|partial|capital|review], `business_use_percent`,
  `cash_account_id -> account` (paid from; a holding when it has a location),
  `campaign_id`, `acquisition`, `cost_of_revenue`, `reimbursable`,
  `external_id`. Saving one with an amount posts a journal. *action*
  `batch_create (0,s)`.
- `account` -- chart of accounts. **code**, **name**, `kind`
  [asset|liability|equity|income|expense], `parent_id`, `opening_balance`,
  `cash_equivalent`, `cash_flow_class`, `location_id -> location` (makes it
  a *holding*), `allow_negative` (holdings only).
- `ledger_entry` -- read-only projection of posted journal lines.
- `tax_category` (default deductibility, `schedule_line`), `tax_code`
  (`code`, exact `rate_numerator/rate_denominator`, `jurisdiction`,
  `recoverable`) -- the rate record an invoice or bill line names.

## ledger (requires finance) -- reports `trial_balance`, `holdings`

- `journal` -- immutable posted evidence. `memo`, `occurred_at`, `state`
  [draft|posted|reversed] (service-owned), `currency`,
  `source_type/source_id/source_version`, `rule_name`, `posting_key`,
  `reverses_id -> journal`, `exchange_policy`, `tax_book`. *actions*
  `post (s)`, `reverse (s)`, `create_and_post (0,s)`.
- `journal_line` -- `journal_id`, `account_id`, `side` [debit|credit],
  `amount` (original currency), `book_amount` (valuation), `dimension`.
- `exchange_rate` -- dated exact rate: `from_currency`, `to_currency`,
  `rate_numerator/rate_denominator`, `effective_at`. Missing pairs are
  refused, never guessed.
- `holding_txn` -- a memo-currency movement of a holding: `account_id`,
  `amount` (signed), `kind` [adjust|earn|spend|transfer]; by hand only for
  memo currencies.
- `location` gains *action* `transfer (s)` (money between holdings).

## periods, statements, autojournal, setup, cutover, close

- `fiscal_year` (`start_at`, `end_at`, `period_length`
  [monthly|quarterly], `state` [open|closed|locked]) generates its
  `fiscal_period` rows (*action* `open_close` -- opens a close workspace).
  `report_snapshot` -- totals preserved at close (`snapshot_vs_live`).
- statements: `saved_report` (`report_name`, `period`, `options`,
  `dimension`), `report_pack` (`schedule`, `saved_report_ids`,
  `recipients`, `last_output`), `accounting_dimension`. Reports
  `balance_sheet`, `income_statement`, `cash_flow`, `general_ledger`,
  `account_balances`, `pnl_reconciliation`, `year_end_pack`.
- autojournal: `posting_profile` -- the twelve account mappings a sale or
  expense posts to (sales, fees, tax, shipping in/out, discounts, refunds,
  COGS, inventory, cash, payable, default expense) plus
  `expense_categories`. Report `unposted`.
- setup: `accounting_setup` (guided books setup; *actions* `preview`,
  `complete`), `accounting_control_map` (`classification` -> `account_id`,
  e.g. `currency_clearing`, `session_income`, `arbitrage_*`, `trading_*`).
- cutover: `accounting_cutover` (`source`, `cutoff`, `state`, `payload`,
  `reconciliation_report`; *actions* `import`, `reconcile`, `activate`,
  `rollback_preflight`, `rollback`), `accounting_cutover_row` (one mapped
  row and its `exception`).
- close: `close_workspace` (`fiscal_period_id`, `currency`, `status`,
  preparer/reviewer; *actions* `run_checks`, `sign`, `complete`, `reopen`),
  `close_task` (*actions* `complete`, `waive`), `close_workpaper`,
  `close_discrepancy` (*action* `explain`), `close_signoff`. Report
  `close_workspace`. None of these actions stage.

## assets (requires ledger, periods) -- reports `fixed_assets`, `deferrals`

- `fixed_asset` -- **name**, **tag**, `cost`, `salvage_value`,
  `useful_life_months`, `method` [straight_line|declining_balance|none],
  `book_convention`, tax method/convention/life, `status`
  [draft|in_service|disposed|written_off] (set by actions), the four
  account references, `acquired_at`, `in_service_at`, `disposed_at`.
- `depreciation_entry`, `tax_depreciation_entry`, `deferral_entry` --
  schedule rows (`period`, `amount`, `state`, `journal_id`); never edit.
- `deferral` -- a prepayment or accrual released monthly: `kind`
  [prepayment|accrual], `total`, `start`, `months`, source/target/funding/
  settlement accounts, `operation=settle` to settle an accrual.

## banking, bankfeed, reconciliation -- report `bank_reconciliation`

- `bank_account` (`account_id -> account`, `currency`, CSV column
  mapping, `match_window_days`), `bank_statement` (period, opening/closing
  balance, `source_file_hash`), `bank_transaction` (`date`, `amount`,
  `description`, `state`, `match_id`, suggestions), `bank_match` (what a
  line matched: `record_type/record_id`, `amount`), `reconciliation`
  (statement vs book balance, outstanding items, `state`), `bank_rule`
  (merchant/description/amount match -> category or create), `bank_transfer`.
- bankfeed (opt-in): `bank_connection` (`provider`, `provider_account_id`,
  `bank_account_id`, `status`). reconciliation has no types: the matcher
  registry behind `reconcile suggest`.

## Capture, OCR, claims, payroll, accounting home

- `capture_item` -- receipt/supplier-invoice inbox entry: `title`, `kind`,
  `status`, `vendor`, `amount`, `document_id`, `result_type/result_id`;
  *actions* `ocr_extract (s)`, `ocr_extract_all (0,s)`.
- ocr (opt-in): `ocr_job`, `ocr_batch` (bounded extraction jobs; actions
  `step`, `retry`, `cancel`).
- claims: `expense_claim` (`number`, `employee_id -> user`, `currency`,
  `status`, `total`), `expense_claim_line` (`kind`, `amount`, `miles`,
  `mileage_rate`, `document_id`, `account_id`).
- payroll (opt-in): `payroll_run` (`run_key`, period, `currency`, `status`,
  disbursement flags), `payroll_line` (`employee`, `gross`, `deductions`,
  `net`, `employer_cost`, `liabilities`). Report `payroll_reconciliation`.
- accounting: no types; `/accounting` and `venturectl accounting` (next
  actions for the books).

## Tax, budgets, equity, group, backup

- sales_tax: `tax_jurisdiction` (`code`, `rate_scaled` = percent x 10000,
  `effective_from/effective_to` half-open, no overlaps), `tax_rule`
  (`state`/`county`/`city` -> `jurisdiction_id`). Report `sales_tax_return`.
- tax_filing: `tax_filing` (jurisdiction return pack: period, `status`,
  `json_pack`/`csv_pack`, `acknowledgment_id`; *actions* `review`,
  `submit`, `acknowledge`, `amend`), `contractor_tax_form` (vendor TIN,
  sensitive), `contractor_tax_pack` (1099-NEC per vendor-year; *actions*
  `review`, `approve`, `export`).
- budgets: `budget` (`period`, `currency`, `dimension`, `status`),
  `budget_line` (`account_id`, `period`, `amount`). Reports
  `budget_vs_actual`, `cash_forecast`.
- equity: `equity_transaction` (`kind`
  [contribution|draw|loan_proceed|loan_payment|transfer], `amount`,
  debit/credit accounts, `journal_id`).
- group (opt-in): `intercompany_link` (`child_organization_id`, ownership
  fraction), `elimination`. Reports `consolidated_trial_balance`,
  `consolidated_income_statement`, `consolidated_balance_sheet`.
- backup: `accounting_backup` (an organization pack; *action* `restore`),
  `backup_schedule` (`scope`, `schedule`, `retention`, `destination`,
  `verify`; *action* `run`), `backup_run` (`status`, `scope`
  [incl. `series`], `sha256`, `parent_run_id`, `store_uuid`; *actions*
  `verify`, `restore_drill (0)`).
- recurring (requires invoicing, payables, ledger, periods, receivables,
  mail): `recurring_schedule` (`kind` [invoice|bill|expense|journal],
  `frequency`, `template` JSON, `next_run_at`, `paused`; *actions* `pause`,
  `resume`, `run (0)`, all stageable), `recurring_occurrence` (idempotency
  per cycle), `collection_policy` (*action* `run (0,s)`),
  `collection_step`, `collection_case` (owner, promise, dispute),
  `collection_notice`, `financial_batch` (*action* `apply (s)`). Report
  `collections_worklist`. Usage: [billing.md](billing.md).
