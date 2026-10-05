# Money out: bills, purchasing, capture, claims, payroll, banking, tax filings

Read this to record and pay supplier bills, order and receive stock, file
receipts, reimburse employees, import payroll, import and match bank
statements, or prepare tax returns. Sources: `docs/payables.org`,
`docs/purchasing.org`, `docs/sales-orders.org`, `docs/capture.org`,
`docs/ocr.org`, `docs/claims.org`, `docs/payroll.org`,
`docs/banking.org`, `docs/bankfeed.org`, `docs/reconciliation.org`,
`docs/tax-filing.org`, `docs/contractor-tax.org`, `docs/supplier-portal.org`.

## Vendor bills (module `payables`)

Run `describe vendor_bill` and `describe vendor_bill_line` before creating
a draft and its lines. Bill line `quantity` is an exact decimal **string**
(at most three places). Supplier companies have `kind=supplier`.

```bash
venturectl create vendor_bill company_id=11 number=B-204 bill_date=2026-09-02 currency=USD status=draft
venturectl create vendor_bill_line bill_id=6 description="Paper stock" quantity="12.5" unit_price=4.00 account_id=61
venturectl bill approve 6 date=2026-09-03
venturectl bill pay 6 'amount=40 USD' date=2026-09-10      # amount omitted pays the balance
venturectl bill void 6 date=2026-09-04
venturectl bill pay-bulk 6,7,9 adapter=transfer [date=ISO]
```

- Direct bill status updates are refused; approve/pay/void go through the
  payables service (`POST /api/v1/vendor_bill/:id/approve|pay|void`, which
  take `?stage=1`). The CLI verbs apply directly; to stage from the CLI,
  create the service records: `vendor_bill_event` (`bill_id`, `vendor_id`,
  `kind=approve`, `state=approved`, `date`) or `bill_payment` (vendor, bill,
  amount, method, date). MCP's `venture_create` stages those normally.
- `report payables PERIOD` is dated aging; `report vendor_statement PERIOD
  vendor_id=ID` the supplier statement; both take `organization_id`,
  `currency`, `as_of`. Credits, refunds and immutable history:
  `docs/payables.org` (including the single-date limitation on optional
  paid-line expense conversion).
- **Supplier portal**: `supplier invite company_id=ID email=ADDR` queues the
  private access link through the mail outbox (needs an HTTPS
  `server.base_url` and mail on; then deliver the outbox). The response
  carries redacted metadata, never the bearer token. Revoke with `supplier
  revoke ID`. The supplier sees `/supplier/:token`: their bills, payment
  status and print.

## Purchasing and sales orders (module `goods`)

Create the `purchase_order` (`status=draft`, `currency`, `vendor_id` a
supplier company) and its `purchase_order_line` (`inventory_item_id`,
`quantity`, `unit_price` in the order's currency) with `create`, then:

```sh
venturectl purchase approve 7
venturectl purchase send 7
venturectl purchase receive 7 line_id=12 quantity=2 date=2026-03-02T10:00:00Z
venturectl purchase match 7 vendor_bill_id=2         # three-way match within tolerance
venturectl purchase cancel 7 ; venturectl purchase return 7 ...
venturectl sales-order allocate 1 ; venturectl sales-order ship LINE quantity=2
venturectl sales-order invoice 1 ; venturectl sales-order cancel 1
```

A receipt is a FIFO cost layer in the order's currency, journalled
Inventory against GRNI by its treatment (memo: none) -- this is how stock
priced in another currency arrives ([currencies.md](currencies.md)). A
purchase order is not a payment. Reports `committed_spend`,
`reorder_worklist`, `inventory_valuation`.

## Capture and OCR (modules `capture`, `ocr`)

```bash
venturectl capture ingest kind=receipt title="Toner" 'amount=12.50 USD' occurred_at=2026-01-15 vendor="Office Supply"
venturectl capture convert 1 as=expense category=office
venturectl capture convert 2 as=vendor_bill company_id=1
venturectl capture reject 3 reason=personal
```

Document file paths are service-owned: generic create/import/update cannot
assign or replace `document.path` or move a filed attachment to another
organization -- use upload or mail filing (valid legacy originals stay
readable; conflicting ownership and symlinks are refused). With OCR enabled (`ocr.enabled`,
needs Tesseract), `act document ID ocr_extract language=eng` queues bounded
work; `act ocr_job ID step` processes one page, `retry` and `cancel` keep
provenance; `act capture_item 0 ocr_extract_all organization_id=N limit=25
after_id=N` freezes a batch advanced by `act ocr_batch ID step`. Review
with `act document ID ocr_review job_id=N text=...`; `--stage` keeps the
document version so newer corrections conflict. Extraction is not
accounting approval.

## Claims and payroll

- `claim submit|approve|pay ID` -- `expense_claim` with lines (receipts,
  mileage as rate x miles); `/claims`.
- Payroll (opt-in `payroll.enabled`): `payroll import run_key=2026-01
  period_start=2026-01-01 period_end=2026-02-01 ...`, `payroll disburse 1
  kind=all`, `payroll reverse 1`; `report payroll_reconciliation`.

## Banking (modules `banking`, `bankfeed`, `reconciliation`)

`bank ACTION ID [JSON|@FILE]` calls the banking service:

```bash
venturectl bank map 1 @mapping.json          # CSV column mapping on a bank_account
venturectl bank import 1 @statement.json     # a statement into bank_account 1
venturectl bank match AUTO 1                 # exact automatic matching for statement 1
venturectl bank match 2 '{"parts":[{"type":"expense","id":3,"amount":"-10 USD"}]}'
venturectl bank unmatch 2 ; venturectl bank exclude 2 '{"reason":"Duplicate supplied by bank"}'
venturectl bank create 3 '{"type":"receipt","customer_id":1}'   # receipts need customer_id
venturectl bank transfer 1 '{"counterparty_bank_account_id":2,"amount":"100 USD","date":"2026-01-10"}'
venturectl bank reconcile 1 ; venturectl bank preview 1 ; venturectl bank bulk 1 '{"mode":"categorize"}'
venturectl reconcile suggest bank_transaction 14 [--matcher NAME] [--threshold 80]
```

`reconcile suggest TYPE ID` ranks matching book records; scores above the
threshold (default 80) stage bank-transaction action confirmations when
banking is installed -- it never applies. `report bank_reconciliation
statement_id=N`.

**Bank feeds** (opt-in `bankfeed.enabled`): credentials belong to the
selected `bank_connection`'s organization. Open its Settings link on
`/bankfeed` to configure or rotate the write-only provider settings, then
**Sync and test** imports the last 30 days of statement evidence -- a real
sync, not a dry run. `bankfeed sync ID [JSON]` uses the same binding and
import service. `VENTURE_BANKFEED_TELLER_KEY` is ignored; an administrator
must configure an explicit connection. A saved connection's provider,
account and organization cannot be reassigned -- create a new one.

## Tax filings and 1099-NEC (module `tax_filing`)

```bash
venturectl tax-filing prepare country=US jurisdiction=US-NY fiscal_period_id=1
venturectl tax-filing review 1 ; venturectl tax-filing submit 1
venturectl tax-filing acknowledge 1 acknowledgment_id=ACK-1 ; venturectl tax-filing amend 1
venturectl contractor-tax prepare vendor_id=1 year=2026
venturectl contractor-tax review 1 ; venturectl contractor-tax approve 1 ; venturectl contractor-tax export 1
```

A filing preserves its JSON/CSV pack bytes and versioned rule id. TINs
(`contractor_tax_form`) are sensitive and never generically exported.
1099-NEC is USD by law and refuses other currencies. Sales-tax return CSVs
per jurisdiction are `sales-tax export` ([receivables.md](receivables.md)).
