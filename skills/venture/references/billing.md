# Recurring money: SaaS billing, dunning, recurring documents, Stripe

Read this for subscriptions (`customer_subscription`), metered usage, MRR
and churn, overdue reminders, recurring invoices/bills/journals, batch
entry, and Stripe payment links and collection. One-off invoices and
receipts are in [receivables.md](receivables.md). Sources:
`docs/billing.org`, `docs/dunning.org`, `docs/recurring.org`,
`docs/stripe.org`.

## SaaS billing (module `billing`)

Read `describe plan_price` and the price first.

```bash
venturectl billing start company_id=4 plan_price_id=2 seats=5 [discount_id=N | discount_code=CODE] [skip_trial=true]
venturectl billing change 9 plan_price=3 [at_period_end=true]
venturectl billing change-seats 9 seats=8
venturectl billing cancel 9 [at_period_end=true]
venturectl billing pause 9; venturectl billing resume 9
venturectl billing mark-payment-failed 9; venturectl billing recover 9
venturectl billing usage 9 quantity=120 key=upload-2026-09-30 [at=DATE]
venturectl billing renew --as-of 2026-10-01 [--dry-run]
venturectl billing dunning --as-of 2026-10-01 [--dry-run]
```

- `start` creates a `customer_subscription`. Non-trial starts issue an
  invoice immediately; trials bill at activation unless `skip_trial=true`.
  A `discount_id` must be a `plan_discount` of the same plan, active and not
  past its `ends_at`; it comes off the first `periods` invoices (0 is every
  one). `discount_code` names it by the `code` the customer quoted, matched
  without regard to case among the price's plan's discounts; an unknown,
  retired or expired code is refused saying which, and giving both is
  refused. A plan with a `venture_id` is refused for another venture's
  customer.
- **Never update subscription status directly**; the billing service
  refuses it. Every change is a `subscription_event`.
- `cancel` without `at_period_end=true` credits the unused days of an
  invoiced period (a trial or an unbilled period gives nothing): a
  `customer_credit` credit note, tax included when the period's invoice was
  taxed, applied to what that invoice still owes. The result's
  `proration_amount` is that credit, negative. Other proration adjustments
  settle on the next renewal.
- A `plan_price` may carry `tax_code_id`; its invoices are taxed at that
  rate (exempt customers stay exempt). A price in use is immutable, so
  taxing an existing plan means a new price and moving customers to it (the
  plan page's "Move its N customers to..." does all or none).
- **Metered use**: `billing usage` posts a `usage_record` (typed JSON, the
  same as `create usage_record`) for a subscription whose price has a
  `usage_unit`. Reuse a `key` when retrying: a repeat is refused as a
  conflict, not counted twice. Usage before the current period, on a flat
  price or on an ended subscription is refused. At renewal the ended
  period's usage over `included_units` is a line on the renewal invoice at
  `unit_amount` -- usage counted in [start, end) at the rate in force at the
  period's end.
- `renew` sweeps due periods and queues the trial-ending reminder
  (`billing.trial_reminder_days`, default 3) once per subscription; a dry
  run queues none. `change`/`change-seats` queue a price-change notice
  unless `billing.price_change_notices` is false. Both land in the mail
  outbox (`list mail_message`), keyed `trial-reminder:<subscription uuid>`
  and `price-change:<event uuid>`; a missing address or mail off skips the
  notice, never the change. `dunning` records dunning notices/actions.
  `organization_id=N` chooses the legal entity; dry runs write nothing.
- `billing collect` records confirmed manual payments only; an authorized
  card/ACH mandate does not execute a provider charge and is refused there.
  A manual payment-method record does not authorize a Stripe charge; the
  optional Stripe adapter collects billing invoices only after verified
  hosted reusable customer authorization (below), and provider-confirmed
  cash and failures use the ordinary recovery lifecycle. Billing notices
  are delivery intents for the mail adapter -- no endpoint sends mail or
  charges a card by itself.
- `--stage` holds a billing action for approval; the assistant can stage
  a `billing_request` (`action`, `at`, `organization_id`, the subscription/
  customer/price fields). Approval refuses a subscription changed since.
- Customers with a portal link can switch price (same venture, at renewal)
  or cancel at renewal; those are ordinary events with actor `customer
  portal`.

Reports: `report mrr PERIOD currency=USD` (MRR, ARR, `arpa`,
`quick_ratio`; contracted revenue, not cash or recognized income; counts a
plan discount while it covers the period billed), `report churn PERIOD
currency=USD` (the billing cohort: logos and MRR lost, `grr_bps`,
`nrr_bps`), `report subscriptions_due PERIOD days=14`. Activity churn and
the rest of the headline metrics are in [reports.md](reports.md).

## Overdue reminders (module `dunning`)

`dunning sweep as_of=DATE` enqueues the due step of each issued, unpaid,
undisputed, unpaused invoice's `dunning_policy` (the invoice's, else its
company's, else the organization default; a deleted one falls through) and
records a `dunning_event`; rerunning sends nothing twice.

```bash
venturectl dunning sweep as_of=2026-10-01 organization_id=1 limit=200 dry_run=true
venturectl --stage dunning sweep as_of=2026-10-01 organization_id=1
venturectl mail deliver --limit 50                       # then deliver the outbox
```

- Arguments are `key=value`, not flags; `organization_id`, `limit` and
  `dry_run` are sent typed. A token or user that is not a global owner/admin
  must pass `organization_id=N` and needs owner, admin or finance there
  (reminders are financial): without it 422 `organization_id: ... runs in
  one organization`, another organization 404, an organization editor 403.
  The same holds for `act` on any type-level action (`recurring_schedule 0
  run`, `collection_policy 0 run`, `invoice 0 batch_create`).
- The answer is an unsaved policy whose `last_sweep` is a JSON string:
  `queued`, `escalated`, `suppressed`, `failed`, `failed_invoice_ids`,
  `warnings`, and with `dry_run=true` a `plan` (invoice, step, offset,
  outcome, reason, recipient, subject) with nothing written. A failing
  invoice is recorded and skipped -- read `failed_invoice_ids`. `as_of`
  more than a day ahead is refused except for a dry run.
- The escalating final step creates a `collect: <invoice>` activity owned
  by `invoice.owner`, else the customer's owner, else the policy's
  `escalation_owner`; an opted-out customer still escalates.
- `act dunning_event ID retry` re-runs a `failed`, `dead` or `uncertain`
  step under a new key (stageable; refused once a later step or attempt
  exists). `act dunning_policy ID test_send invoice_id=N [offset=N]` mails
  the rendered step to your own user email only (`[TEST]` subject, pay
  link withheld, no event recorded; not stageable; refused for a token or a
  user without an email). Pause with `update invoice|company ID
  dunning_paused_until=DATE dunning_pause_reason=...`. `dunning_event`
  cannot be created, edited or deleted directly (exit 8).
- `report collections [PERIOD]` measures effectiveness per step over events
  queued in the period (`report collections all` for everything); `report
  dunning_worklist` lists open overdue invoices with aging, last and next
  step.

## Recurring documents, collections and batch entry (module `recurring`)

`recurring run --as-of DATE [--dry-run] [organization_id=N]` generates due
`recurring_schedule` occurrences (invoices, bills, expenses, journals)
through the settlement, payables and posting services; closed periods are
skipped. `collections run --as-of DATE [organization_id=N]` enqueues
overdue reminders with durable idempotency keys (the older mechanism;
dunning policies are the templated one). `batch invoice|expense
format=csv|json payload=... [post=false] [organization_id=N] [--dry-run]`
(or `act invoice 0 batch_create` / `act expense 0 batch_create`) creates
many documents all-or-nothing. A non-admin must name the organization.

## Stripe (module `stripe`, opt-in)

- **Hosted checkout**: `venturectl invoice checkout ID` returns a Stripe
  Checkout URL for an eligible sent invoice (editor, module on).
- **One-time payment link**: with the organization's Stripe connection and
  an HTTPS `server.base_url`, `act invoice ID payment_link
  [expires_at=...]` returns a one-time `url` (default seven days; one hour
  to thirty days ahead). The capability binds that invoice revision,
  organization, account and expiry. Copy the url from that result: `get
  stripe_payment_link ID` omits the bearer URL. `act stripe_payment_link ID
  revoke` disables the resolver and expires an open session; processing ACH
  stays pending -- revoking cannot cancel a bank debit already initiated.
  An uncertain provider response keeps the attempt blocked until
  reconciled -- do not create another payment by guessing.
- **Recurring collection**: read `describe stripe_authorization` and the
  subscription's actions first. `act customer_subscription ID
  authorize_payment limit='100 USD'` returns a copy-once hosted Setup URL
  (connection and webhook pinned to Stripe API `2024-06-20`). Customer
  completion plus verified Setup/mandate evidence activates permission;
  `act stripe_authorization ID verify` recovers a missed callback;
  `revoke_authorization` stops future charges without discarding settlement
  evidence. Changing terms needs fresh permission. The running server
  advances bounded due renewals/collections only for enabled, verified
  permissions; `act stripe_authorization 0 collect_due organization_id=N
  limit=10 [now=...]` is the explicit bounded sweep (`now` controls
  scheduling, never settlement dates); `act invoice ID collect` runs the
  same service. A pending attempt blocks hosted and automatic alternatives
  across all accounts.
- `act stripe_checkout ID retry_collection` waits the recorded day and stops
  after three attempts, each confirming the old provider invoice is
  cancelled before creating a new identity; `cancel_collection` requires
  zero-receipt void/delete proof (processing payments cannot be cancelled by
  guess), and a manual cancellation stops collection until the customer
  gives fresh permission; `reconcile_collection provider_invoice_id=in_...`
  validates the original account and opaque correlation after a lost create
  response, then permits explicit cancellation or signed-event recovery.
- `act stripe_event ID retry` replays only retained verified evidence after
  a local posting failure; `accept_balance_change=true` explicitly lets the
  original provider amount allocate with any excess as customer credit when
  a manual/partial payment arrived while ACH was pending. It cannot alter
  the event's amount, currency, account or effective date. Never
  manufacture payment evidence with CRUD or treat a successful pay request
  as settled cash. Finance authorization, period guards and second-actor
  approval apply. Inbound `/webhooks/stripe/:connection_id` verifies
  `Stripe-Signature` (400 invalid, 200 processed/duplicate, 503 provider
  unavailable).

### Lightsite billing

Lightsite customers are billed from the operator's own organization, named by
`lightsite.billing_organization_id` (0 disables the billing routes).
Staging needs plans coded `team`, `growth` and `starter`, each with exactly
one active monthly `plan_price`, created with ordinary `venturectl create`
commands in that organization. See `docs/lightsite-billing.org` for the recipe.
A business appears there as a `company` with
`external_id=lightsite:organization:<id>`; change its subscription through
`billing change`, never by editing the subscription record directly.

the operator's billing organization and each customer business keep separate Stripe
connections. These routes expose connection status, never keys. A successful
page load is not payment evidence.
