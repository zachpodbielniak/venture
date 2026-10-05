# Money in: invoices, receipts, credits, quotes, projects, sales tax

Read this to invoice a customer, record a payment, issue a credit or
refund, run a quote to acceptance, bill a client project, or answer "what
does this customer owe". Subscriptions, reminders and Stripe are in
[billing.md](billing.md). Sources: `docs/receivables.org`,
`docs/documents.org`, `docs/quotes.org`, `docs/projects.org`,
`docs/project-delivery.org`, `docs/progress.org`, `docs/sales-tax.org`,
`docs/commerce.org`, `docs/portal.org`.

## Invoices: financial state comes from settlement

Create an invoice as a draft, add its lines, then send it. A direct
`status=paid` or `partially_paid` write is refused (exit 8: "status=paid is
derived from allocations"). Record the money instead:

```bash
venturectl create invoice number=INV-100 company_id=8 issued_at=2026-09-20 due_at=2026-10-20
venturectl create invoice_line invoice_id=5 description=Consulting quantity=2 unit_price=50.00 tax_code_id=3
venturectl update invoice 5 status=sent            # issues it: freezes amounts, posts the journal
venturectl describe payment
venturectl create payment customer_id=8 invoice_id=5 'amount=40 USD' date=2026-09-25 method=transfer external_id=bank-42
```

- Or in one call: `venturectl compose invoice
  '{"company_id":8,"issued_at":"2026-09-21","lines":[{"description":"Setup","quantity":1,"unit_price":"120.00 USD","tax_code_id":3}],"send":false}'`
  (also `compose quote`). `issued_at` defaults to today at midnight UTC;
  `due_at` may name an explicit due date, otherwise `due_days` (default 30)
  counts from the invoice date (zero or negative: no due date). Sending
  keeps the invoice date. Numbers are allocated per organization when
  omitted. To remember an exemption on the customer atomically, add
  `remember_tax_exemption=true`, `tax_exemption_kind`, `tax_exemption_number`
  beside `tax_exempt=true`; those fields are part of issuance approval, and a
  refused or proposed operation does not change the customer.
- **Dates must fall inside configured fiscal periods**, and closed periods
  refuse ("Organization 1 requires a date within its configured fiscal
  periods"). Check `list fiscal_period` when a create is refused for a date.
- The receipt and its allocation settle atomically. Omit `invoice_id` to
  keep a deposit, then create `payment_allocation` rows with `payment_id` or
  `credit_id`. Overpayments remain customer credit. Standalone credits use
  `customer_credit kind=credit_note`; a `refund` names an `allocation_id` or
  an unused `credit_id`. `remaining`, invoice `paid_at` and
  `workflow_state` are derived. Issued amounts and settlement history
  cannot be edited or deleted. `external_id` on payments is unique per
  organization -- reuse it to make an import idempotent.
- Applied cash creates its sale in the same transaction; void reverses an
  unpaid invoice; a refund reopens the balance. `POST
  /api/v1/invoices/:id/send` issues a draft and queues the rendered
  document by mail in one transaction. `/invoices/:id/print` renders it.
- A line is taxed by a rate record (`tax_code_id`; the rate is made at
  `/tax-rates/new` from a percent and stored as an exact fraction); a quote
  line too, so the accepted quote copies it onto the invoice. The exemption
  lives on the customer; the invoice freezes it at issue.

## Balances and statements

```bash
venturectl report receivables 2026-09 organization_id=1          # aging as of the period end
venturectl report customer_statement 2026-01 customer_id=1 currency=USD organization_id=1
venturectl report cash_vs_booked 2026 organization_id=1
```

`customer_statement` requires `customer_id`. Report options also accept
`venture_id`, `group_by` and `as_of`; REST, the report page and CSV exports
preserve them. Receivables requires invoice event history: existing invoices
without issue events need an explicit migration, not a guessed balance.
See `docs/receivables.org` for dates, account configuration and the current
posting-currency restriction.

## Quotes (module `quotes`)

```bash
venturectl quote send 7
venturectl quote accept 7 'by=Full Name'
venturectl quote decline 7 'reason=Explanation'
venturectl quote revise 7
venturectl quote start-subscription 7          # the accepted quote's plan line, once
venturectl deal quote 12                       # a draft quote from the deal's lines (or revise it)
```

Acceptance creates and issues the invoice in the same transaction unless
`billing_mode=progress`. A `quote_line` with `plan_price_id` (quantity =
seats, no discount or tax) is left off that invoice -- the subscription
bills it -- and `quote start-subscription` starts the subscription once and
returns `result_subscription_id`; a second start is refused. To stage an
action use `--stage create quote_action quote_id=ID action=accept
expected_version=N 'accepted_by=Full Name'` with the quote's current
`version` (`revision` is the separate commercial revision number). The
public proposal is `/q/:token` (accept with a typed name). Progress billing
(`act quote ID progress_invoice percent=N`), retainers and retention:
`docs/progress.org`.

## Client projects and handoff (module `projects`)

Read `describe project_time` and `describe client_project` before entering
work.

- Sales handoff: `act quote ID handoff` or `act deal ID handoff` with
  `name`, `owner`, `scope`. `client_project` actions `plan_work` (`key`,
  `title`, `scope_id`, `due`, `amount`), `change_scope` and `manage` retain
  agreement and delivery decisions; `project_deliverable` actions `accept`
  (evidence) and `bill` require finished work and retained acceptance.
  Request keys deduplicate planned work; replaying an accepted billing
  returns its invoice. Generic writes cannot replace or remove delivery
  evidence.
- `act project_time ID approve` freezes the billable amount and actual
  labour cost from the project's rate; `act client_project ID bill
  date=YYYY-MM-DD` invoices approved unbilled time and billable costs. All
  stageable; finance or organization administration is required. Generic
  edits cannot approve time, rewrite frozen evidence or remove billing
  allocations. Fixed-price projects invoice accepted slices through
  progress billing; their approved labour is cost evidence, not a second
  time-and-materials charge. Full-billed quotes already have an invoice.
- `report project_margin 2026-01-01..2026-12-31` distinguishes budget,
  billed allocations, approved unbilled work and recorded actual cost;
  unknown historical cost suppresses total cost/profit rather than assuming
  zero. It is management profitability, not cash or statutory revenue.
  (`--from`/`--to` are ignored by `report` -- use a range period.)

## Customer retainers

`act company ID collect_retainer 'amount=250 USD' liability_account_id=N`
records already-received cash against an active same-organization
liability account; `act customer_retainer ID release 'amount=100 USD'`
recognizes earned income and reduces the liability. Finance-authorized, not
stageable. They charge no provider and settle no invoice: never record the
same cash again as an invoice receipt. A linked retainer stays separate from
invoice-billed project margin.

## Sales tax by jurisdiction (module `sales_tax`)

`tax_jurisdiction` rate windows (`rate_scaled` = percent x 10000,
half-open, no overlaps; a rate change is a new row) are picked by the
customer's `address_state/county/city` through `tax_rule` and frozen on the
invoice line. `venturectl sales-tax export period=2026-Q3
[jurisdiction=CODE] [organization_id=N]` writes the return CSV per
jurisdiction (gross, exempt, taxable, collected, credited, net due);
`report sales_tax_return` is the same. What is USD by law (the US
adapter, 1099-NEC) refuses other currencies by name.

## Commerce connectors (module `commerce`, opt-in)

`venturectl commerce import '{"organization_id":1,"connector":"shopify"}'`
uses that organization's explicitly configured account; without
`organization_id` the API uses the browser's organization or the default.
An organization owner/admin connects, tests, rotates or disconnects Shopify
at `/organizations/ID/settings/commerce` (write-only vault values; the shop
is a canonical `your-shop.myshopify.com`; `VENTURE_COMMERCE_SHOPIFY_*` env
vars are ignored). Test connection is the explicit provider request;
disconnect keeps history but blocks use; rotation during a fetch refuses
that response. `commerce_import_link` is immutable identity evidence (the
account namespace keeps equal order ids apart across shops). A legacy
invoice needs explicit adoption, never automatic ownership:

```sh
venturectl describe integration_connection
venturectl act integration_connection 7 adopt_commerce_invoice invoice_id=42 'reason=Reviewed original shop order evidence'
venturectl list commerce_import_link
```

Adoption needs organization integration administration, pins the selected
active binding and leaves historical invoice/settlement amounts unchanged;
use the account record linked from settings. Generic edits cannot rewrite
or delete import identities. Never pass credentials as
CLI arguments.

## Customer portal

A `customer_portal_access` token gives a customer `/portal/:token`: their
invoices, pay links, and their own subscriptions (switch price within the
venture at renewal, or cancel at renewal -- recorded as
`subscription_event` rows with actor `customer portal`). Anything not the
token's customer's is not found. Revoke with `act customer_portal_access ID
revoke`.
