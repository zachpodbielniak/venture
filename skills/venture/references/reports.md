# Reports: periods, options, the catalogue, aggregate

Read this to run or explain any report. `venturectl report` (no name)
lists what this server offers; `GET /api/v1/reports` gives each report's
declared parameters. Source: `docs/reporting.org`.

## Running one

```bash
venturectl report pnl this_quarter
venturectl report pnl 2026-08 as_of=2026-08-31 organization_id=1
venturectl -f csv report receivables > aging.csv
curl -s -H "Authorization: Bearer $VENTURE_TOKEN" "$VENTURE_SERVER/api/v1/reports/pnl?period=2026&organization_id=1"
```

- **Periods**, wherever one is accepted (`report NAME PERIOD`, `period=`
  filters, widgets): named (`today`, `yesterday`, `this_week`,
  `last_week`, `this_month`, `last_month`, `this_quarter`, `last_quarter`,
  `this_year`, `last_year`), to-date (`ytd`, `qtd`, `mtd`), fiscal (`fy`,
  `fy_2026`), rolling (`last_30_days`), calendar (`2026`, `2026-03`,
  `2026-Q2`, `2026-03-14` -- one day), a range
  (`2026-01-01..2026-03-31`), and `all`. Boundaries are midnight UTC.
- The period may be left out before options: `report holdings
  organization_id=2` runs `this_month`. Options go after it as `key=value`.
- **`as_of=DATE`** gives historical visibility: the date includes that UTC
  day; a timestamp is an exact cutoff; rows deleted after it still count.
  Omit it for live figures. Not every report accepts it (`attribution`
  refuses it).
- **`organization_id=N`** -- without it a token's report answers for the
  default organization. Most also take `venture_id`, `currency`,
  `group_by`; see the catalogue. An option a report does not know is
  refused by some (`report aggregate`, the market and arbitrage reports,
  exit 2 naming it) and ignored by older ones -- check `GET /api/v1/reports`.
- **`--from`/`--to` are not report options** -- they are `money calendar`
  and `support rollup` flags; `report project_margin --from ...` silently
  runs `this_month`. Use a range period instead.
- The answer: `{"title","period","metrics":[{key,label,kind,value,formatted}],"columns","rows","note(s)"}`;
  `?format=csv` / `-f csv` is the server's CSV.

## One figure per currency

`pnl`, `ventures` and `monthly` keep one figure per currency, never a
converted total. The book currency always comes first (zero when nothing
is in it) and keeps the plain metric keys (`revenue`, `expenses`,
`profit`); any other currency adds its code (`revenue_TICKET`,
`profit_EUR`). Rows carry a `currency` column -- `pnl` repeats its lines per
currency, `ventures` is a row per venture per currency, `monthly` a row per
month per currency. Never add a TICKET row to a GOLD one. A bare date is
one day: `report pnl 2026-03-14` is the fourteenth only. When the period
finished an arbitrage trade (closed or abandoned), `pnl` adds "Arbitrage
gains", "Less arbitrage fees" and "Arbitrage result" per currency and the
result is in `profit` (metric `arbitrage`, `arbitrage_<CODE>`); `ventures`
and `monthly` add an `arbitrage` column their profit includes. No finished
trade, no lines. The P&L reads operational sales and expenses; for posted
balances use the ledger statements ([ledger.md](ledger.md)). Inventory,
holdings, listing, session and arbitrage reports are per currency the same
way.

## The catalogue (module: reports and notable options)

- finance: `pnl`, `ventures`, `monthly`, `tax` (deductions; review count),
  `tax_liability` (frozen tax by code). sales: `categories`
  (`group_by=genre|category|format|...`), `inventory` (on hand, value per
  currency, reorder flags).
- ledger: `trial_balance` (per book currency, `books` label), `holdings`
  (`location_id`, `currency`). statements: `balance_sheet`,
  `income_statement`, `cash_flow`, `general_ledger` (`account_id`),
  `account_balances`, `pnl_reconciliation` -- all take `currency`,
  `compare_to`, `dimension`, `basis=accrual|cash`, `as_of`;
  `year_end_pack` (a zip of CSVs). periods: `snapshot_vs_live`.
  autojournal: `unposted`. assets: `fixed_assets`, `deferrals`. close:
  `close_workspace`. budgets: `budget_vs_actual`, `cash_forecast`. group:
  `consolidated_trial_balance|income_statement|balance_sheet`. payroll:
  `payroll_reconciliation`. banking: `bank_reconciliation`
  (`statement_id`). sales_tax: `sales_tax_return`.
- receivables: `receivables` (aging), `customer_statement`
  (`customer_id` required, `currency`), `cash_vs_booked`. payables:
  `payables` (aging), `vendor_statement` (`vendor_id`). goods:
  `committed_spend`, `reorder_worklist`, `inventory_valuation`. quotes:
  `quotes`. projects: `project_margin`. recurring: `collections_worklist`.
  dunning: `collections`, `dunning_worklist`.
- billing: `mrr` (`currency`; MRR, ARR, `arpa`, `quick_ratio`), `churn`
  (the billing cohort: logos and MRR lost, `grr_bps`, `nrr_bps`),
  `subscriptions_due` (`days=14`). headline: `cac`, `customer_churn`
  (activity and recurring churn, `days` default 90), `ltv`, `ltv_cac`,
  `customer_cohorts` -- all accept `venture_id` and `as_of`. pnl_cuts:
  `revenue_by_customer` (`by=source`), `spend_by_vendor` (`by=category`),
  `recurring_costs`, `cash_outlook` (`weeks=N`, default 8; a different
  report from the budgets module's `cash_forecast`) -- the P&L card's links
  open these four with the card's period and scope. customer_health:
  `customer_health` (`band`, `owner`, `sort`). money_calendar:
  `money_calendar` (`from`, `to`, `kind`, `customer_id`/`vendor_id`).
- crm: `pipeline`. pipelines: `stage_duration`, `funnel`, `forecast`,
  `loss_reasons`, `overdue_deals` (`pipeline_id`, `owner=USERNAME`).
  leads: `lead_sources`, `lead_response_time`, `leads_recycled_due`,
  `lead_routing`, `lead_scoring` (`band_size=25`). activities: `worklist`,
  `calls`. sequences: `sequence_performance`, `sequence_failures`,
  `sequence_engagement`. marketing: `marketing_performance`. outreach:
  `campaigns`. attribution: `attribution` (`model=first|last`,
  `details=true`). sales_performance: `sales_attainment`. ideas: `ideas`.
- tickets: `support`, `support_rollup` (`group_by=product`). factory:
  `releases`, `lead_time`, `incidents`, `delivery` (DORA's four keys
  beside the coding runs' cost).
- market: `listing_performance`, `price_history`. production:
  `recipe_margin`. sessions: `session_performance`. goals:
  `goal_progress`, `goal_materials`. marketdata: `market_deals`,
  `venue_index`, `watchlist`, `accounts`, `account_holdings`,
  `external_pnl`. arbitrage: `arbitrage_performance`, `arbitrage_scan`,
  `craft_arbitrage`, `external_books`. Their options are in
  [market-production.md](market-production.md),
  [sessions-goals.md](sessions-goals.md), [market-data.md](market-data.md),
  [accounts.md](accounts.md) and [arbitrage-scan.md](arbitrage-scan.md).
- core: `aggregate` (below). A report option is dropped unless every door
  forwards it (web page, CLI allow-list, MCP, the assistant each keep a
  list) -- an option that works in one door and not another is a bug.

The home page's cards (`GET /api/v1/headline`, `?format=csv`) read `pnl`,
`mrr`, `cac`, `churn` (only when billing is in use, else
`customer_churn`), `ltv_cac` and `support_rollup`; follow a card's `link`
rather than guessing which report. A viewer without owner, admin or
finance gets the cards with `state` `restricted` and no figures. Read each
report's notes: MRR is contracted revenue, not cash or recognized income;
churn rates are in basis points.

## Aggregating any record type

`report aggregate` sums, averages, counts and takes min/max of any field of
any business record type, grouped and bucketed. Reach for it before totalling
`list` output yourself: it adds money per currency and rounds an average
half to even, which a sum in a script does not.

```sh
venturectl report aggregate PERIOD type=TYPE [measure=FIELD|count] \
    [aggregate=sum|avg|min|max|count|count_distinct] [group_by=F1,F2,F3] \
    [category_depth=N] [date_field=FIELD] [bucket=day|week|month|quarter|year] \
    ['filter=QUERY'] [per=hour|day] [organization_id=ID] [venture_id=ID]
venturectl report aggregate 2026 type=sale measure=gross \
    group_by=product_id.category_id date_field=occurred_at bucket=month
venturectl report aggregate all type=ticket group_by=status
venturectl report aggregate last_month type=sale measure=gross group_by=channel date_field=occurred_at per=day
```

- **Name `date_field` or the period bounds nothing**: without it every
  matching record is counted and the notes say so. It must be a declared
  date/time field (`describe TYPE`) or `created_at`/`updated_at`.
- `measure` defaults to `count`; `aggregate` defaults to `sum` for a
  money/integer/double measure, `count` otherwise. `sum`/`avg`/`min`/`max`
  of a non-numeric field is refused.
- **Money comes back one row per currency** (`currency` column), never
  converted; `value` is a money object. A record with no value is in no
  row, and the notes say how many.
- `group_by` takes up to three wire-spelled fields: plain, enum (by label),
  reference (by name), a custom field's name, or `reference.field` to follow
  one reference. Category and location groups show the path;
  `category_depth=0` rolls up to the top.
- Buckets are UTC (`2026-03`, `2026-Q1`, `2026-W09`). `per=day` adds a
  `rate` column over the days elapsed in the row's window, clipped to now
  (sum/count only; refused for `all` without a bucket).
- Quote a `filter` holding `&`: `'filter=status__not_in=done,cancelled&kind=external'`.
- Refused before any row is read: an unknown type or field (exit 3 or 2),
  a type whose module is off, personal or platform types (`user`,
  `api_token`, chat, webhooks -- exit 5), sensitive fields, more than three
  groups, a filter with `limit`/`offset`/`page`/`order`, and a question
  matching more than 20 000 records (narrow it; never silently truncated).
- The same options work on `GET /api/v1/reports/aggregate` and the
  assistant's and MCP's `venture_report`; `-f csv` exports the table.
  Dashboards' `sum` and `progress` widgets use the same arithmetic
  ([dashboards.md](dashboards.md)).
