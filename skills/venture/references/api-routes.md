# API route index

Read this to find the HTTP route for a module operation or a `venturectl`
verb. Every route below is under `/api/v1` unless it starts with another
path, takes the auth in [api.md](api.md), and answers the error shape there.
`:type` routes work for every record type; everything else is a service
operation the generic routes cannot express. Extracted from the source at
the skill baseline -- `grep -rhoE 'HTMX_METHOD_[A-Z]+, *"/api/v1/[^"]*"' src`
in the repository lists today's.

## Platform and generic

| Route | Purpose | venturectl |
|---|---|---|
| `GET /health` (no auth) | build capabilities | `health` |
| `GET /schema`, `/schema/:type` | types, fields, actions | `types`, `describe` |
| `GET /modules` | modules, state, reasons | `modules` |
| `GET /settings` | resolved configuration, secrets redacted | -- |
| `GET /plugins` | loaded plugins (owner) | `plugins list` |
| `GET /venture-types`, `/venture-types/:name` | declarative types and fields | -- |
| `GET /automations` | engine state and pods | -- |
| `POST /tokens` | mint a token (session) | -- |
| `GET /palette?q=` | command-palette search | -- |
| `GET\|POST\|PATCH\|PUT\|DELETE /:type[/:id]`, `POST /:type/:id/restore`, `POST /:type/bulk`, `POST /:type/:id/actions/:action` | generic records | `list get create update delete restore bulk act` |
| `GET /confirmations`, `POST /confirmations/:id/approve\|reject` | the staging queue | -- (approve over HTTP) |
| `GET /links/:type/:id`, `POST /links` | record links | `links`, `link` |
| `GET /activity/:type/:id` | a record's timeline | `activity TYPE ID` |
| `GET /inbox`, `POST /inbox/read` | notifications | `inbox`, `inbox read` |
| `POST /watch`, `GET /watching/:type/:id` | follow a record | `watch`, `unwatch` |
| `GET /comments?subject_type=&subject_id=`, `GET /comments/:id`, `POST /comments`, `PATCH\|PUT\|DELETE /comments/:id`, `GET /comments/mentions`, `POST /comments/preview` | discussions | `comments ...` |
| `GET /reports`, `GET /reports/:name` (`?format=csv`) | reports | `report` |
| `GET /dashboards`, `/dashboards/:slug`, `/dashboards/:slug/export`, `POST /dashboards/import`, `POST /dashboards/from-template`, `GET /widget-kinds`, `GET /dashboard-templates` | dashboards | `dashboards`, `dashboard ...` |
| `GET /webhooks`, `POST /webhooks/:id/test`, `POST /webhooks/:id/secret` | webhooks out (owner) | `webhooks`, `webhook ...` |
| `GET /printers`, `GET /printers/:name/status`, `POST /printers/:name/test`, `POST /print/:type/:id` | receipt printers | `printers`, `print` |
| `POST /federation` | federation operations (owner) | `federation JSON` |
| `GET /account-identity/:id` | hosted organization identity check | -- |

## Desk, KB, factory, forge

| Route | venturectl |
|---|---|
| `GET /tickets/:id/sla`, `POST /tickets/:id/macro`, `POST /tickets/:id/worklog` | `ticket ID sla\|macro\|worklog` |
| `POST /tickets/:id/triage` (`{"apply":true}`), `GET /tickets/:id/summary`, `POST /tickets/:id/draft` | `ticket ID triage\|summary\|draft` |
| `POST /sla/sweep`, `GET /sprints`, `GET /sprints/:id` | `sprints`, `sprint ID` |
| `POST /incidents/:id/ticket`, `POST /incidents/:id/postmortem` | `incident ID ticket\|postmortem` |
| `GET /runs?state=`, `GET /budgets` | `runs`, `budgets` |
| `GET /kb/search?q=`, `POST /kb/:id/sync\|reindex\|import`, `GET /kb/:id/export`, `POST /kb/crossref/:type/:id`, `POST /kb/from/:type/:id` | `kb ...` |
| `GET /factory`, `GET /factory/actions`, `GET /factory/briefing` | `factory [actions\|briefing]` |
| `GET /releases/:id/readiness`, `POST /releases/:id/changelog\|publish\|deploy\|notes` | `release ...` |
| `GET /milestones/:id/forecast`, `POST /environments/:id/rollback`, `POST /builds/:id/ticket\|triage` | `milestone`, `environment`, `build` |
| `POST /forge/:id/settings` (JSON operation), `POST /forge/:id/verify`; retired `token`, `webhook-secret` | `forge settings\|verify` |
| `/hooks/forge/:id` (no session; HMAC) | -- |

## Money in, money out

| Route | venturectl |
|---|---|
| `POST /invoices/compose`, `POST /quotes/compose` | `compose invoice\|quote JSON` |
| `POST /invoices/:id/send`, `POST /invoices/:id/checkout` | (mail) `invoice checkout ID` |
| `POST /quotes/:id/:action` (send, accept, decline, revise, start-subscription) | `quote ACTION ID` |
| `POST /deals/:id/move`, `POST /deals/:id/quote` | `deal move\|quote` |
| `POST /leads/:id/:action` (convert, reassign, reroute, rescore) | `lead ...`, `leads ...` |
| `POST /billing/start`, `POST /billing/renew-sweep\|dunning-sweep`, `POST /customer_subscriptions/:id/:action` (renew, change, change-seats, cancel, pause, resume, mark-payment-failed, recover); metered use is `POST /usage_record` | `billing ...` |
| `POST /vendor_bill/:id/:action` (approve, pay, void), `POST /payables/pay` | `bill ...`, `bill pay-bulk` |
| `POST /purchase_order/:id/:action`, `POST /sales_order/:id/:action` | `purchase ...`, `sales-order ...` |
| `POST /supplier_portal/invite`, `POST /customer_portal/invite` | `supplier invite` |
| `POST /customers/health/sweep`, `GET /headline` (`?format=csv`) | `customers health-sweep` |
| `POST /commerce/import` | `commerce import` |
| `GET /sales-tax/export` | `sales-tax export` |
| `/webhooks/stripe/:connection_id` (no session; signature) | -- |

## The books

| Route | venturectl |
|---|---|
| `POST /journals/post` (header + lines), `POST /journals/:id/post` | `act journal 0 create_and_post`, `journal post ID` |
| `POST /post/backfill` | `post backfill` |
| `POST /fixed_assets/:id/:operation` (place-in-service, dispose, write-off), `POST /assets/run-period`, `/assets/run-tax-period` | `asset ...`, `assets run-period` |
| `POST /close/open`, `POST /close/:id/:action`, `GET /close/:id/pack` | `close ...` |
| `POST /accounting_cutovers/preview\|csv`, `POST /accounting_cutovers/:id/:action`, `GET /accounting_cutovers/template/:section` | `cutover ...`, `act accounting_cutover` |
| `POST /accounting_setups/preview`, `POST /accounting_setups/:id/:action` | -- |
| `GET /accounting/home` | `accounting` |
| `POST /bank_accounts\|bank_statements\|bank_transactions\|bank_rules/:id/:action`, `POST /reconciliation/suggest` | `bank ...`, `reconcile suggest` |
| `POST /bankfeed/:id/sync` | `bankfeed sync ID` |
| `POST /capture`, `POST /capture/:id/:action` | `capture ...` |
| `POST /expense_claim/:id/:action` | `claim ...` |
| `POST /payroll/import`, `POST /payroll_run/:id/:action` | `payroll ...` |
| `POST /tax-filings/prepare`, `POST /tax-filings/:id/:action` | `tax-filing ...` |
| `POST /contractor-tax/prepare`, `POST /contractor-tax/:id/:action`, `GET /contractor-tax/:id/export` | `contractor-tax ...` |
| `GET /budget_reports?period=&kind=` | `budget vs-actual\|forecast` |
| `POST /equity/post` | `equity KIND AMOUNT` |
| `GET /group/reports?period=&report=` | `group income\|balance\|trial` |
| `POST /accounting_backups/export`, `POST /report_pack/:id/deliver` | `report packs deliver ID` |

## CRM, mail, calendar

| Route | venturectl |
|---|---|
| `GET /activities` (`.ics`), `POST /activities/:id/:action`, `POST /activities/sweep` | `activity complete\|list` |
| `POST /sequence/:id/enroll`, `POST /sequence_enrollment/:id/:action` (pause, resume, exit, goal), `POST /sequences/run` | `sequence ...` |
| `POST /crm_imports/preview`, `POST /crm_imports/:id/:action` | `crm preview\|import\|activate\|rollback` |
| `POST /mail/:action` (send, test, deliver), `POST /mail_messages/:id/retry`, `POST /mail/sync`, `POST /mail_accounts/:id/sync`, `POST /mail_unmatched_senders/:id/create_contact\|dismiss` | `mail ...` |
| `POST /calendar/sync` | `calendar sync` |

## Market data, accounts, arbitrage (answer for `organization_id`)

| Route | venturectl |
|---|---|
| `GET /feeds`, `GET /feeds/due`, `GET /feeds/:id/runs`, `GET /feeds/:id/venues` (`?stale_after=`), `POST /feeds/:id/sync`, `POST /feeds/:id/push` (`application/x-ndjson`, `?wait=1`) | `feeds ...` |
| `GET /market/quote`, `POST /market/promote`, `GET /market/browse\|find\|deals\|venues`, `GET /market/i/:source/*`, `GET /market/watchlists[/:id]`, `GET /market/alerts`, `POST /market/alerts/:id/evaluate` | `market ...` |
| `GET /accounts`, `GET /accounts/inventory`, `GET /accounts/pnl`, `GET /accounts/:source/*` (each takes `show_ignored`) | `accounts ...` |
| `GET /arbitrage/scan\|registries\|export\|calc`, `POST /arbitrage/record` | `arbitrage ...` |

## Public routes (no session; their own guard)

`/login` (+ `/login/mfa`), `/f/:token` (lead capture), `/q/:token` and
`/q/:token/accept` (quote acceptance), `/portal/:token` (customer portal,
own subscriptions only), `/supplier/:token` (supplier portal),
`/book/:slug` (booking page), `/t/o/:token.gif`, `/t/c/:token/:n` and
`/marketing/t|u/...` (tracking and unsubscribe), `/attribution.js` and
`/attribution/:site/:operation`, `/hooks/lightsite/:site/:connection`,
`/federation/v1/identity|request` (signed), `/docs`. The web UI's own pages
(`/e/TYPE`, `/e/TYPE/ID`, `/e/TYPE/import`, `/e/TYPE/export`, `/tickets`,
`/books`, `/market/...`) need a session and are not an API.

## Lightsite provisioning

| Route | Purpose | venturectl |
|---|---|---|
| `POST /lightsite/signups` | trusted workspace service creates or links a business owner idempotently | -- |
| `POST /account/identity-link` | signed-in local member links provider identity | -- |
| `GET /account-authority` | explicit active memberships and bounded site permissions | -- |
| `POST /lightsite/billing/subscriptions` | trusted idempotent business-to-plan binding | -- |
| `POST /lightsite/billing/enroll` | fourteen-day deferred enrollment, no invoice | -- |
| `POST /lightsite/billing/prepay` | frozen 24-month prepaid terms, no invoice until publication | -- |
| `POST /lightsite/billing/setup` | hosted saved-method setup, no charge | -- |
| `POST /lightsite/billing/publish` | record first go-live; may start the first charge | -- |
| `POST /lightsite/billing/first-charge` | idempotent first collection at publication or day fourteen | -- |
| `POST /lightsite/billing/cancel` | no-charge cancel before the deadline, else period end | -- |
| `POST /lightsite/billing/guarantee` | one-time capped account credit from a retained measurement | -- |
| `GET /lightsite/billing/:organization_id/made-back` | won, invoiced and overlap since go-live | -- |
| `GET /lightsite/billing/:organization_id/notifications` | owner notification facts, no payment credentials | -- |
| `GET /lightsite/billing/:organization_id` | business plan, balance and own Stripe status | -- |
| `GET /lightsite/billing` | staff billing overview | -- |
