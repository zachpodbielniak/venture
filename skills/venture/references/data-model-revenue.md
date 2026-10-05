# Data model: selling, billing, buying and getting paid

Sales and inventory, invoicing and receivables, quotes, SaaS billing,
client projects, payables, purchasing and sales orders, dunning, the
headline metrics, Stripe and commerce connectors. Conventions are in
[data-model.md](data-model.md); usage is in [receivables.md](receivables.md),
[billing.md](billing.md) and [payables.md](payables.md).

## sales (requires core) -- reports `categories`, `inventory`

- `product` -- what you sell. **venture_id**, **name**, `sku`,
  `category_id -> category` (plus legacy `category`/`subcategory` text),
  `genre`, `format`, `list_price`, `cost`, `isbn`/`asin`, `tags`,
  `active`, `tax_exempt`, revenue `recognition_policy`/`recognition_months`.
- `inventory_item` -- stock of a product at a place: **product_id**,
  `location_id -> location`, `unit_cost`, `reorder_point`,
  `reorder_quantity`, `lead_time_days`, `allow_negative`, `tags`.
  Quantity on hand is the sum of its transactions, never stored.
- `inventory_txn` -- a signed movement: `kind`
  [purchase|production|sale|return|adjustment|write_off|transfer],
  `quantity`, `unit_cost`, `occurred_at`, `reference` (e.g. `recipe:3`,
  `session:8`), `sale_id`.
- `sale` -- money in from a sale: **venture_id**, `product_id`,
  `contact_id`, `campaign_id`, `channel`, `occurred_at`, `quantity`,
  `gross`, `discount`, `shipping_collected`, `shipping_cost`, `fees`,
  `tax_collected`, `tax_remitted`, `refunded`/`refunded_at`,
  `cash_account_id -> account` (a holding when located), `external_id`
  (unique per organization, deleted rows included -- re-import safe). Saving
  one with gross posts a journal; invoices create sales when paid.
- `location` -- a place tree (see [taxonomy.md](taxonomy.md)).

## invoicing (requires finance, crm, ledger) and receivables

- `invoice` -- **number**, `company_id -> company`, `contact_id`,
  `venture_id`, `status` [draft|sent|paid|void|partially_paid] (only
  `draft -> sent` is yours; paid states come from settlement), `issued_at`,
  `due_at`, `paid_at`, `workflow_state`, `tax_exempt`, `dunning_policy_id`,
  `dunning_paused_until`, `owner`, `external_id`. *actions*
  `batch_create (0,s)`, `payment_link`, `collect` (stripe).
- `invoice_line` -- `invoice_id`, `description`, `quantity` (double),
  `unit_price`, `discount_percent`, `tax_code_id -> tax_code`,
  `tax_jurisdiction_id` (frozen at issue), `product_id`. Amount is computed
  exactly, never stored loose.
- `invoice_event` -- frozen issue/void/credit history per invoice.
- `payment` (a receipt: **customer_id -> company**, `invoice_id`, `amount`,
  `date`, `method`, `external_id` unique per organization),
  `payment_allocation` (payment or credit -> invoice), `customer_credit`
  (`kind` e.g. `credit_note`, `remaining`), `refund` (names an
  `allocation_id` or unused `credit_id`), `customer_portal_access`
  (tokenised portal link; *action* `revoke`). Reports `receivables`,
  `customer_statement`, `cash_vs_booked`.

## quotes (requires crm, invoicing) -- report `quotes`

- `price_list`/`price_list_item`; `quote` (**number**, `revision`,
  `status` [draft|sent|accepted|declined|expired|superseded] via actions,
  `company_id`, `deal_id`, `currency`, `billing_mode` (e.g. `progress`),
  totals, `invoice_id`, `subscription_id`; *actions* `progress_invoice (s)`,
  `handoff (s)`); `quote_line` (`plan_price_id` for a subscription line);
  `quote_event`, `quote_delivery`, `quote_action` (a staged
  send/accept/decline/revise: `action`, `expected_version`, `accepted_by`);
  `progress_billing`, `customer_retainer` (*action* `release`),
  `contract_retention`.

## billing (requires invoicing, receivables) -- reports `mrr`, `churn`, `subscriptions_due`

- `plan` (`venture_id`, `code`), `plan_price` (`plan_id`, `amount`,
  `currency`, `interval` [month|year|quarter|half_year], `per_seat`,
  `trial_days`, `tax_code_id`, metered `usage_unit`/`unit_amount`/
  `included_units`; immutable once in use), `plan_discount`
  (`percent_off` or `amount_off`, `periods`, `code`, `ends_at`).
- `customer_subscription` -- `company_id`, `plan_price_id`, `seats`,
  `status` [trialing|active|past_due|paused|cancelled|expired] (billing
  service only), period dates, `discount_id`, `discount_periods_used`,
  `quote_id`; *action* `authorize_payment` (stripe).
- `subscription_event` (every lifecycle change with MRR before/after,
  `invoice_id`, `final_invoice_id`), `billing_request` (a staged billing
  instruction), `billing_notice`, `dunning_step`, `customer_payment_method`,
  `usage_record` (`quantity`, `idempotency_key`).

## projects (requires invoicing, receivables) -- report `project_margin`

`client_project` (`customer_id`, `quote_id`, `deal_id`, `billing_kind`,
`budget`, `retainer`, `delivery_status`; *actions* `plan_work`, `manage`,
`change_scope`, `bill`), `project_rate` (`billing_rate`, `cost_rate`),
`project_time` (`minutes`, *action* `approve` freezes amount and cost),
`project_cost` (`billable`), `project_billing`, `project_scope`,
`project_deliverable` (*actions* `accept`, `bill`, `link_invoice`). All
project actions stage.

## payables (requires finance, ledger, crm) -- reports `payables`, `vendor_statement`

`vendor_bill` (**company_id** = a `kind=supplier` company, **number**,
`bill_date`, `due_date`, `currency`, `status` via actions,
`purchase_order_id`), `vendor_bill_line` (`quantity` is an exact decimal
*string*, at most 3 places; `unit_price`, `account_id`, `tax_code_id`,
`purchase_order_line_id`), `bill_payment`, `bill_payment_allocation`,
`vendor_credit`, `vendor_bill_event` (approve/void, service-frozen),
`bill_refund`. supplier_portal: `supplier_portal_access` (*action* `revoke`).

## goods (requires payables, ledger, sales) -- reports `committed_spend`, `reorder_worklist`, `inventory_valuation`

`purchase_order` (`vendor_id`, `currency`, `status`, match tolerance),
`purchase_order_line` (`inventory_item_id`, `quantity`, `unit_price` in the
order's currency), `goods_receipt`/`goods_receipt_line`,
`inventory_cost_layer` (FIFO: `original_qty`, `remaining_qty`, `unit_cost`,
`lot_txn_id` -- sibling layers of several currencies are one lot),
`sales_order`/`sales_order_line` (`allocated_qty`, `fulfilled_qty`,
`invoiced_qty`), `fulfillment`.

## dunning, headline, customer_health, pnl_cuts, money_calendar

- dunning (requires receivables, mail): `dunning_policy` (`steps` JSON:
  offsets and templates, `is_default`, `final_escalation`,
  `escalation_owner`, `last_sweep`; *actions* `sweep (0,s)`, `test_send`),
  `dunning_event` (one step per invoice; *action* `retry (s)`; never
  written by hand). Reports `collections`, `dunning_worklist`.
- headline: `headline_setting` (health thresholds, hourly rates, the
  classic home switch). Reports `cac`, `customer_churn`, `ltv`, `ltv_cac`,
  `customer_cohorts`; the cards at `/` and `GET /api/v1/headline`.
- customer_health, pnl_cuts, money_calendar: no types; reports
  `customer_health`; `revenue_by_customer`, `spend_by_vendor`,
  `recurring_costs`, `cash_outlook`; `money_calendar`.

## stripe and commerce (opt-in)

- stripe (requires receivables, integrations): `stripe_payment_link`
  (one-time hosted link; *action* `revoke`), `stripe_checkout` (*actions*
  `cancel_collection`, `retry_collection`, `reconcile_collection`),
  `stripe_event` (verified evidence; *action* `retry`),
  `stripe_authorization` (customer's recurring permission; *actions*
  `verify`, `revoke_authorization`, `collect_due (0)`),
  `stripe_price_link`, `stripe_customer_link`, `processor_payout(_item)`,
  `processor_dispute`, `processor_exception`. Never write evidence by CRUD.
- commerce (requires invoicing, receivables, integrations):
  `commerce_import_link` -- immutable identity of an imported order or
  customer per connected shop account.
