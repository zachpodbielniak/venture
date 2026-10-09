---
name: venture
description: >
  REQUIRED for any work with VENTURE, the ERP/CRM in C/GLib for running a
  portfolio of ventures: reading or changing its records (sales, expenses,
  invoices, payments, bills, journals, tickets, companies, contacts, deals,
  leads, products, inventory, subscriptions), running reports and dashboards,
  driving venturectl or the raw REST API (/api/v1), staging and approving
  writes, configuring forges, feeds, plugins, automations and webhooks, or
  explaining how the system works and why it refused something. Triggers:
  venture, VENTURE, venturectl, VENTURE_TOKEN, VENTURE_SERVER, /api/v1,
  describe, stage, confirmation, organization_id, book currency, holdings,
  ledger, trial balance, period close, dunning, billing, MRR, quote,
  pipeline, sequence, marketing send, ticket, SLA, factory, release, forge
  rule, coding run, data source, feeds push, market quote, alert rule,
  arbitrage scan, tsmctl, accounts attention, recipe craft, session post,
  goal, dashboard widget, podomation, webhook, venturectl mcp.
---

# VENTURE

<!-- Structure notes (agent-skills standard): this skill lives in the
VENTURE repository as skills/venture, named `venture` so it is invoked as
/venture, not as skills/skill-<name>. Its description is long on purpose
(a trigger list, as the immutablue skill does) because one skill covers a
whole system. SKILL.md stays under 500 lines and every reference under
~200; the maintenance rule is in AGENTS.md ("The CLI, and its skill"). -->

VENTURE is an ERP and CRM in one, written in C on GLib/GObject, for one
operator (or a small team) running a *portfolio* of ventures -- a book
imprint, an Etsy shop, a newsletter, a household, a trade inside a game's
economy -- rather than one company. It keeps double-entry books, receivables
and payables, FIFO inventory, a CRM, a support desk, a software factory,
market data and arbitrage, dashboards and reports, with an AI assistant that
reads freely and writes only through approval. This skill is for using it
(through `venturectl`, the REST API or MCP) and for explaining it.

Five things make it different, and almost every mistake comes from
forgetting one of them:

1. **Everything derives from a record type's field table.** The table,
   REST routes, forms, CLI verbs, AI tools and webhook events are generated
   -- the type is an argument (`venturectl list sale`), never a subcommand,
   and `describe` is the truth about field names. Guessing one is a silent
   no-op.
2. **One writer, many doors.** The web UI, REST, `venturectl`, MCP, the
   assistant and automations all end in the same save: same validators,
   reference checks, period guards, access policy and audit trail. Nothing
   bypasses it, and the CLI never opens the database.
3. **Organizations scope everything.** Every record belongs to one
   organization; a token answers for the *default* one unless told
   `organization_id=N`.
4. **Money is never a double.** Integer minor units plus a currency, which
   may be user-defined (`GOLD`, `POINTS`); currencies are never added or
   converted without a recorded rate, and reports keep one figure per
   currency.
5. **Writes can wait, and state belongs to services.** Any write can be
   staged for approval (`?stage=1`, `--stage`; MCP and the assistant stage
   by default); posting, settlement, billing and the like own their state
   fields and are reached through actions or verbs, not field edits.

## When to Use

- Anything that reads or changes data in a VENTURE instance, from the shell
  (`venturectl`), over HTTP (`/api/v1`) or through `venturectl mcp`.
- Questions about how VENTURE works: its modules, record types, books,
  billing, CRM, desk, factory, market data, plugins, access rules.
- Diagnosing a refusal, an empty report, a write that "did nothing", a stuck
  mail or a feed that stores nothing.
- Working on VENTURE's own code also needs `AGENTS.md` in the repository;
  this skill is about using the system, and its traps are a subset of those.

## Before doing anything else

Read what this server is. Ports differ by purpose (8747 config default and
container, 8748 `just start`, 8749 `make demo`), and talking to the wrong
one is the most expensive mistake there is.

```bash
export VENTURE_SERVER=http://127.0.0.1:8747  VENTURE_TOKEN=vk_...
venturectl health                      # version, database, ai, staged_writes, account_logins
venturectl modules                     # what is on; -f json says why a module is off
venturectl list organization           # which organizations exist; is_default marks the token's
venturectl describe TYPE               # every field you may type, enums, references
curl -s -H "Authorization: Bearer $VENTURE_TOKEN" "$VENTURE_SERVER/api/v1/schema/TYPE" | jq .actions
```

If `health` fails, stop and fix that: every other command fails the same
way and less clearly. A token is minted at `/account/tokens` or `POST
/api/v1/tokens` with a session ([api.md](references/api.md)).

## Skill baseline -- maintained

This skill was reviewed against VENTURE commit
`8955f90bfeebbc78cbd0206c106364ae0069dd8a` (2026-10-08, server version
0.6.0): every verb, route, record type, module and report it names was read
from that source or checked against a running server built from it. The rule
that keeps it current is in `AGENTS.md` ("The CLI, and its skill"): a change
to a command, flag, exit code, route, type or trap updates the skill in the
same commit and moves this baseline.

<!-- skill-baseline:start -- reviewed 2026-10-08 against 8955f90 -->
```bash
# From a checkout: what changed since this skill was written?
base=8955f90bfeebbc78cbd0206c106364ae0069dd8a
git log --oneline "$base"..HEAD -- src docs migrations data plugins AGENTS.md
git diff --stat "$base" HEAD -- src/cli docs src/web/venture-web-server.c
# Without a checkout: compare the live ground truths with what this skill says
venturectl --help ; venturectl types | wc -l ; venturectl modules ; venturectl report
```
<!-- skill-baseline:end -->

When the server and this skill disagree, the generated ground truths win
(`--help`, `describe`, `/api/v1/schema`, `/api/v1/reports`, `modules`):
answer from them, and say the skill is behind. A newer server may have
types, verbs or reports this skill has never heard of; an older one may lack
some it describes.

## Topic guides

The guides live in `references/` beside this file; read only the one the
task needs. They link to each other by bare filename. If your harness has
not told you this skill's directory, it is `skills/venture/` in the VENTURE
repository, linked as `~/.claude/skills/venture`, `~/.grok/skills/venture`
and `~/.agents/skills/venture` by `make install-skill`.

- [`references/troubleshooting.md`](references/troubleshooting.md) -- **start here when something is wrong**: symptom -> first check -> guide
- [`references/system.md`](references/system.md) -- the system as a whole: doors, threads, modules, organizations, roles, staging, audit, what lives outside the database
- [`references/records.md`](references/records.md) -- the record spine, field kinds, references, soft delete, versions, service-owned fields, sensitive fields, actions, links
- [`references/data-model.md`](references/data-model.md) -- every module, what it requires, where its types are described; core and platform types
- [`references/data-model-books.md`](references/data-model-books.md), [`data-model-revenue.md`](references/data-model-revenue.md), [`data-model-crm.md`](references/data-model-crm.md), [`data-model-work.md`](references/data-model-work.md), [`data-model-trading.md`](references/data-model-trading.md) -- every record type by module: purpose, key fields, references, actions, reports
- [`references/api.md`](references/api.md) -- the raw REST API: auth, CRUD, filters, bodies, errors, staging and approval, actions, reports
- [`references/api-routes.md`](references/api-routes.md) -- every module route, with the `venturectl` verb that calls it
- [`references/cli.md`](references/cli.md) -- venturectl setup, session files, flags, `--stage`, output, exit codes, `venturectl mcp`
- [`references/cli-verbs.md`](references/cli-verbs.md) -- every verb, grouped by subsystem, with its guide
- [`references/reports.md`](references/reports.md) -- periods, `as_of`, the report catalogue, one figure per currency, `report aggregate`
- [`references/dashboards.md`](references/dashboards.md) -- dashboards, the 26 widget kinds, templates, scope, the grid
- [`references/automation.md`](references/automation.md) -- what must be called on a schedule, podomation rules, the inbox and watches, webhooks out
- [`references/ledger.md`](references/ledger.md) -- journals, posting, reversals, statements, fiscal periods, the close, second-person consent, backfill
- [`references/ledger-operations.md`](references/ledger-operations.md) -- fixed assets and deferrals, setup and control accounts, cutover, custom values and packs, budgets, equity, consolidation
- [`references/currencies.md`](references/currencies.md) -- money, user-defined currencies, book treatment, exchange rates, holdings
- [`references/taxonomy.md`](references/taxonomy.md) -- categories, locations, tags, custom fields, declarative venture types
- [`references/receivables.md`](references/receivables.md) -- invoices, receipts, credits, refunds, quotes, client projects, retainers, sales tax, commerce, the portal
- [`references/billing.md`](references/billing.md) -- SaaS subscriptions, usage, MRR/churn, dunning, recurring documents, batch entry, Stripe
- [`references/payables.md`](references/payables.md) -- bills, purchasing and sales orders, capture and OCR, claims, payroll, banking, tax filings
- [`references/crm.md`](references/crm.md) -- companies, contacts, leads, routing and scoring, deals, activities and calls, customer health, duplicates, territories, CRM import
- [`references/forms.md`](references/forms.md) -- forms: questions, publishing, embeds, results, languages, quizzes, bookings, payments, uploads, Lightsite forms
- [`references/outreach.md`](references/outreach.md) -- sequences, marketing sends and consent, attribution, campaigns
- [`references/mail.md`](references/mail.md) -- the outbox, SMTP accounts, inbound IMAP, CalDAV, booking pages
- [`references/desk.md`](references/desk.md) -- tickets, SLAs, macros, sprints, AI triage, comments on any record, knowledge bases, the assistant
- [`references/factory.md`](references/factory.md) -- milestones to incidents, releases, forges and rules, coding runs, budgets, the harness
- [`references/market-production.md`](references/market-production.md) -- price observations, listings, recipes and crafting
- [`references/sessions-goals.md`](references/sessions-goals.md) -- sessions of effort and their yields, goals and their shopping list
- [`references/feeds.md`](references/feeds.md) -- data sources, sync and push, the series store, the position mirror, cross-cutting market traps
- [`references/market-data.md`](references/market-data.md) -- venues, instruments, the price oracle, the Trading pages, alert rules
- [`references/accounts.md`](references/accounts.md) -- the operator's accounts: where to log in, inventory, trading P&L, posting to the books
- [`references/arbitrage-scan.md`](references/arbitrage-scan.md) -- the scan, presets, fee models, calculators, exports
- [`references/arbitrage.md`](references/arbitrage.md) -- trades, legs, execution, close/reopen/abandon, performance
- [`references/plugins.md`](references/plugins.md) -- what is loaded, the five ways to extend, manifests, exec plugins, adding a type
- [`references/platform.md`](references/platform.md) -- users, tokens, membership, MFA, OIDC, AI providers, the server and its configuration, docs, printers
- [`references/operations.md`](references/operations.md) -- backups and drills, retention, federation, hosted workspaces, key maintenance, HTTP limits

## Critical rules

**Never guess a field name, enum value or option.** Read `describe TYPE`
(fields), `/api/v1/schema/TYPE` (actions) or `GET /api/v1/reports`
(options). Wire names use underscores (`invoice_id`); a dashed or misspelt
key in a body is dropped silently and the save still succeeds.

**Never bypass the API.** Do not open `venture.db`, write SQL, or edit a
series store: there is no other door that runs the validators, period
guards and audit. Service-owned fields (invoice status, journal state,
subscription status, deal stage, posting stamps) change only through their
verbs and actions.

**Say what actually happened.** A staged write is *not done*: report the
confirmation id and that it awaits approval. A second-actor "permission
denied after a proposal" is not done either. A dry run wrote nothing. A
draft (AI ticket reply, release notes, postmortem) was never posted.

**Name the organization.** Pass `organization_id=N` for anything outside
the default organization -- lists, reports, sweeps, market verbs, dashboards
-- or the answer is about the wrong books.

**Money names its currency.** Write `"250.00 USD"`, `"12g 34s 56c GOLD"`;
read `.formatted` for display and `.amount` (minor units) for arithmetic;
money filters compare minor units; never add amounts of two currencies.

**Secrets never go in argv, fields or logs.** Sensitive fields are ignored
on write and omitted on read; each credential has its own door (`forge
settings` on stdin, `webhook secret`, settings pages). Tokens act as `API
token #N`.

**Destructive and irreversible operations need a person.** Deletes are soft
but still deletes; `release publish`, `purge_history`, `bulk --delete`,
`dedupe merge`, a cutover activation, a marketing approval and a printer
send cannot be undone -- confirm with the operator first, and prefer
`--stage`, `dry_run=true` or `--dry-run` where they exist. Never retry an
`uncertain` mail or a write whose response was lost without reading first.

## Where things live

| Path | What it is |
|---|---|
| `venture` (`build/debug/venture`, installed `$(BINDIR)/venture`) | the server: REST API, HTMX UI, reports, AI, automation |
| `venturectl` | the client and `venturectl mcp`; builds alone (`make venturectl`) |
| `/etc/venture/config.yaml`, `~/.config/venture/config.yaml`, `--config FILE`, `config.c` beside it, `VENTURE_*` env, CLI flags | configuration, layered in that order; secrets only via `*_env` variables |
| state directory (`--state-dir`; default `~/.local/share/venture`) | `venture.db` (SQLite default; relative paths resolve here), `automations.pod`, `backups/`, `series/<source-uuid>/store.db`, KB sources, caches |
| `postgres://...` + `database.password_env` | PostgreSQL instead of SQLite |
| `plugins.paths`, `VENTURE_PLUGIN_PATH`; `build/debug/plugins`, `plugins-optional` | native, crispy and exec plugins; optional ones load only when named |
| `data/venture-types/*.yaml`, `plugins.venture_type_paths` | declarative venture types |
| `/docs` on every instance; `docs/*.org` in the repository | the documentation (`docs/index.org` first) |
| `AGENTS.md`, `README.org` | the repository's rules and traps; the one-page overview |
| `skills/venture/` (linked from `.claude/skills/venture`, `make install-skill`) | this skill |
| `GET /api/v1/settings` | the resolved configuration, secrets redacted |

## The command surface

`venturectl` is generic over types; domain verbs exist only where a service
operation is not a field write. Every verb is in
[`references/cli-verbs.md`](references/cli-verbs.md); its HTTP route in
[`references/api-routes.md`](references/api-routes.md).

| venturectl | Raw API | Does |
|---|---|---|
| `types`, `describe TYPE` | `GET /api/v1/schema[/:type]` | types and fields (schema adds actions) |
| `list TYPE k=v` | `GET /api/v1/:type?k=v` | `{total,count,records}`; filters `field__op=value` |
| `get TYPE ID` | `GET /api/v1/:type/:id` | one record |
| `create TYPE f=v` | `POST /api/v1/:type` | 201; `?stage=1` -> 202 |
| `update TYPE ID f=v` | `PATCH /api/v1/:type/:id` | named fields; `version` makes it conditional |
| `delete` / `restore TYPE ID` | `DELETE /api/v1/:type/:id`, `POST .../restore` | soft delete / undo |
| `act TYPE ID ACTION k=v` | `POST /api/v1/:type/:id/actions/:action` | a declared action; ID 0 = type-level |
| `bulk TYPE 1,2 f=v` | `POST /api/v1/:type/bulk` | one transaction |
| `report NAME PERIOD k=v` | `GET /api/v1/reports/:name?period=` | a report; `-f csv` / `?format=csv` |
| `--stage ...` | `?stage=1`, then `POST /api/v1/confirmations/:id/approve` | propose, then decide |
| `links`, `link` | `GET /api/v1/links/:type/:id`, `POST /api/v1/links` | record links |
| `health`, `modules` | `GET /api/v1/health`, `/modules` | what this server is |

Output is a table on a terminal and JSON when piped. Exit codes: 0 fine
(staged too), 2 usage, 3 not found, 4 conflict, 5 auth, 6 unsupported, 7
network/timeout, 8 validation, 1 anything else -- branch on these, never on
message text.

## Diagnosis first

```bash
venturectl health && venturectl modules     # right server? module on?
venturectl describe TYPE                    # right field names and enum values?
venturectl activity TYPE ID                 # what happened to this record, by whom
venturectl list audit_entry target_type=TYPE target_id=ID approved_by__not_null=1
curl -s -H "Authorization: Bearer $VENTURE_TOKEN" "$VENTURE_SERVER/api/v1/confirmations"   # waiting for approval?
venturectl accounting ; venturectl factory actions ; venturectl inbox   # what needs somebody
```

Read the error: the message names the field, the id, the service or the
switch. Then go to [`references/troubleshooting.md`](references/troubleshooting.md).

## Instructions

Deciding what kind of task this is:

1. **A question about how VENTURE works or why it behaved so?** ->
   [`system.md`](references/system.md), then the subsystem's guide; cite the
   rule, and check it against the live server when it matters.
2. **Read or report data?** -> `list`/`get` with filters for records,
   `report` for figures (never total `list` output by hand -- use `report
   aggregate`). Name the organization and the period.
   [`reports.md`](references/reports.md).
3. **Create or change ordinary records?** -> `describe`, look up reference
   ids, then `create`/`update` (`--stage` when a person should approve).
   [`records.md`](references/records.md).
4. **A business operation (post, pay, send, convert, craft, close, sweep)?**
   -> its verb or action, never a status field. Find it in
   [`cli-verbs.md`](references/cli-verbs.md) or the type's schema actions.
5. **Money moving in the books?** -> [`ledger.md`](references/ledger.md)
   (and [`currencies.md`](references/currencies.md) for anything not in the
   book currency). Use explicit dates; mind closed periods and consent.
6. **Something should happen on a schedule or on a change?** -> an explicit
   sweep from a podomation rule or cron; nothing heavy runs on its own.
   [`automation.md`](references/automation.md).
7. **Calling over HTTP instead of the CLI?** -> [`api.md`](references/api.md)
   for the conventions, [`api-routes.md`](references/api-routes.md) for the
   route.
8. **Configuring the install (modules, plugins, providers, users, backups)?**
   -> [`platform.md`](references/platform.md),
   [`operations.md`](references/operations.md),
   [`plugins.md`](references/plugins.md). Many settings are the operator's
   file, not an API write -- say so rather than inventing an endpoint.
9. **Something is refused or empty?** ->
   [`troubleshooting.md`](references/troubleshooting.md).

Then: confirm destructive steps with the operator, prefer a dry run or a
staged write, run it, and report what the server answered (ids, confirmation
ids, notes) in words.

## Common requests

| Request | Answer | Guide |
|---|---|---|
| "Provision a Lightsite owner" | trusted idempotent sign-up service | [operations.md](references/operations.md) |
| "Use an owner's provider token" | link and verify issuer/subject; check current authority | [platform.md](references/platform.md) |
| "Bill a Lightsite business" | bind its plan through the operator's billing organization | [billing.md](references/billing.md) |
| "Enroll a hosted business, defer its charge, or credit the guarantee" | trusted `/api/v1/lightsite/billing/enroll`, `prepay`, `setup`, `publish`, `first-charge`, `cancel`, `guarantee`; `made-back` and `notifications` are reads | [billing.md](references/billing.md) |
| "Is the server up / what version / which modules?" | `health`, `modules` | [system.md](references/system.md) |
| "What fields does X have?" | `describe X`; actions from `/api/v1/schema/X` | [records.md](references/records.md) |
| "List / find records" | `list TYPE field__op=value search=... order=-field limit=N` | [api.md](references/api.md) |
| "Records of the other organization" | add `organization_id=N` | [system.md](references/system.md) |
| "Record a sale / an expense" | `create sale ...` / `create expense ...` (posts a journal) | [records.md](references/records.md) |
| "Invoice a customer" | draft + lines + `update invoice ID status=sent`, or `compose invoice JSON` | [receivables.md](references/receivables.md) |
| "Record a payment / credit / refund" | `create payment ...`; `customer_credit`; `refund` | [receivables.md](references/receivables.md) |
| "What does this customer owe?" | `report receivables`, `report customer_statement ... customer_id=` | [receivables.md](references/receivables.md) |
| "Send / accept a quote" | `quote send\|accept\|decline\|revise ID` | [receivables.md](references/receivables.md) |
| "Start / change / cancel a subscription" | `billing start\|change\|change-seats\|cancel ...` | [billing.md](references/billing.md) |
| "What is MRR / churn?" | `report mrr`, `report churn` (billing) or `customer_churn` | [billing.md](references/billing.md) |
| "Chase overdue invoices" | `dunning sweep ... dry_run=true`, then for real, then `mail deliver` | [billing.md](references/billing.md) |
| "Set up a recurring invoice / bill" | a `recurring_schedule`, then `recurring run` | [billing.md](references/billing.md) |
| "Take a card payment" | `invoice checkout ID`; `act invoice ID payment_link` (Stripe on) | [billing.md](references/billing.md) |
| "Enter and pay a supplier bill" | `create vendor_bill` + lines, `bill approve`, `bill pay` | [payables.md](references/payables.md) |
| "Order and receive stock" | `purchase_order` + lines, `purchase approve\|send\|receive` | [payables.md](references/payables.md) |
| "Import a bank statement and match it" | `bank import`, `bank match AUTO`, `reconcile suggest` | [payables.md](references/payables.md) |
| "File a receipt" | `capture ingest`, `capture convert ... as=expense` | [payables.md](references/payables.md) |
| "Post / reverse a journal" | `journal post ID`; `act journal ID reverse`; `act journal 0 create_and_post` | [ledger.md](references/ledger.md) |
| "Trial balance / balance sheet / P&L" | `report trial_balance\|balance_sheet\|income_statement\|pnl PERIOD` | [ledger.md](references/ledger.md) |
| "Close the month" | `close open\|run\|sign\|complete`, close tasks | [ledger.md](references/ledger.md) |
| "Post missing journals" | `post backfill --dry-run`, then without | [ledger.md](references/ledger.md) |
| "Depreciate assets / opening balances" | `assets run-period`; `cutover csv ...` | [ledger-operations.md](references/ledger-operations.md) |
| "Define a currency / value it / who holds what" | `create currency`, `exchange_rate`, `report holdings` | [currencies.md](references/currencies.md) |
| "Move money between wallets/characters" | `act location ID transfer to_location_id= amount=` | [currencies.md](references/currencies.md) |
| "Put products in a category tree / add a custom field" | `category` + `category_id`; `fields define` | [taxonomy.md](references/taxonomy.md) |
| "Create a venture of a declared type" | `create venture venture_type=... attributes.NAME=...` | [taxonomy.md](references/taxonomy.md) |
| "Total anything by anything" | `report aggregate PERIOD type= measure= group_by= date_field=` | [reports.md](references/reports.md) |
| "Build / read / export a dashboard" | `dashboard create TEMPLATE`, `dashboard SLUG`, `dashboard export` | [dashboards.md](references/dashboards.md) |
| "Convert / route / score a lead" | `lead convert`, `leads reroute\|rescore` | [crm.md](references/crm.md) |
| "Move a deal / log a call / plan a follow-up" | `deal move ID STAGE`, `act contact ID log_call`, `activity complete` | [crm.md](references/crm.md) |
| "Which customers are at risk? / merge duplicates" | `report customer_health`, `customers health-sweep`; `dedupe scan\|merge` | [crm.md](references/crm.md) |
| "Migrate from HubSpot/Salesforce/Zoho" | `crm preview\|import\|activate` | [crm.md](references/crm.md) |
| "Build / publish / embed a form, read its answers" | `create form`, `form_field`, `act form ID publish`, `report form_summary` | [forms.md](references/forms.md) |
| "Enroll in a sequence / send a newsletter" | `sequence enroll`; `marketing_send` preview -> approve -> run | [outreach.md](references/outreach.md) |
| "Send / retry / sync mail" | `mail send\|deliver\|retry\|sync` | [mail.md](references/mail.md) |
| "Open / triage / answer a ticket" | `create ticket`, `ticket ID triage\|draft\|macro\|worklog` | [desk.md](references/desk.md) |
| "Comment on a record / mention someone" | `comments add TYPE ID BODY` (`-` for stdin) | [desk.md](references/desk.md) |
| "What have I been told?" | `inbox`, `watch TYPE ID` | [automation.md](references/automation.md) |
| "Search the knowledge base" | `kb search QUERY` (meaning), `list kb_article search=` (words) | [desk.md](references/desk.md) |
| "What should I do next (software)?" | `factory actions`; `release readiness ID` | [factory.md](references/factory.md) |
| "Ship / deploy / roll back a release" | `release changelog\|publish\|deploy`, `environment ID rollback` | [factory.md](references/factory.md) |
| "Connect a forge / let an agent work bugs" | `create forge` + `forge settings` + `forge_repo` + `forge_rule` | [factory.md](references/factory.md) |
| "What are the coding runs costing?" | `runs`, `budgets` | [factory.md](references/factory.md) |
| "Set up a price feed / push account data" | `create data_source`, `feeds sync\|push --wait` | [feeds.md](references/feeds.md) |
| "What is X worth / where is it cheapest?" | `market quote`, `market deals\|browse\|instrument` | [market-data.md](references/market-data.md) |
| "Alert me when a price drops" | `create alert_rule ...`, `market alerts evaluate --dry-run` | [market-data.md](references/market-data.md) |
| "Where do I log in next / what do my accounts hold?" | `accounts attention`, `accounts inventory`, `accounts pnl` | [accounts.md](references/accounts.md) |
| "Find and record an arbitrage" | `arbitrage scan`, `arbitrage record N ...`, `arbitrage close` | [arbitrage-scan.md](references/arbitrage-scan.md) |
| "What do my crafts make? / plan my flips" | `arbitrage crafting buy_realm=... sell_realm=...`; `arbitrage plan [reprice=1]`, `arbitrage plan export tsm` | [arbitrage-scan.md](references/arbitrage-scan.md) |
| "Import my professions' recipes" (WoW) | `act data_source ID import_recipes professions="..." skill_tier="..." max_recipes=500 create_products=true venture_id=N`; `act data_source ID set_venue_fees` | [feeds.md](references/feeds.md) |
| "Craft from a recipe / what is a recipe's margin?" | `act recipe ID craft times=N`; `report recipe_margin` | [market-production.md](references/market-production.md) |
| "Log a farming session / track a goal" | `session` + `session_yield`, `act session ID post`; `goal`, `report goal_progress` | [sessions-goals.md](references/sessions-goals.md) |
| "Run X every day / when Y changes" | a pod in `automations.pod` or cron calling the sweep | [automation.md](references/automation.md) |
| "Tell another system when records change" | `create webhook`, `webhook secret`, `webhook test` | [automation.md](references/automation.md) |
| "Approve what an agent proposed" | `GET /api/v1/confirmations`, `POST .../approve\|reject` | [api.md](references/api.md) |
| "Give an AI agent access" | a token + `venturectl mcp` (writes staged) | [cli.md](references/cli.md) |
| "Add a user / token / membership" | `create user ... password=`, `/account/tokens`, `organization_membership` | [platform.md](references/platform.md) |
| "Turn a module on/off / what is configured?" | `modules:` in config; `GET /api/v1/settings` | [platform.md](references/platform.md) |
| "Back up / verify a restore" | `backup run\|verify\|restore-drill` | [operations.md](references/operations.md) |
| "Share records with another server" | `federation '{"action":...}'` | [operations.md](references/operations.md) |
| "Add a venture type / provider / plugin" | YAML type, `*.plugin.yaml`, `plugins list` | [plugins.md](references/plugins.md) |
| "Print a receipt" | `print payment\|invoice ID [PRINTER]` | [platform.md](references/platform.md) |

## Output Format

- Answers to questions: say what VENTURE does and why, naming the module,
  record type, verb or route, and the guide or `docs/*.org` file it comes
  from. Distinguish what the live server showed from what the docs say.
- Changes: show the exact commands or requests run, then what the server
  answered -- the record id, the confirmation id for a staged write, the
  notes of a sweep or run, the exit code of a refusal -- in plain words.
- Figures: quote `.formatted` money with its currency, one figure per
  currency, and the period and organization they cover.

## Examples

### Example: "How did each venture do last quarter, and record the $40 the Bellhaven customer just paid on INV-5?"

```bash
venturectl health                                    # {"status":"ok","version":"0.6.0",... "staged_writes":true}
venturectl report ventures last_quarter organization_id=1
venturectl -f json list invoice number=INV-5 | jq '.records[0] | {id, status, company_id}'
#   {"id": 5, "status": "sent", "company_id": 8}
venturectl describe payment                          # customer_id, invoice_id, amount, date, method ...
venturectl create payment customer_id=8 invoice_id=5 'amount=40 USD' date=2026-09-25 \
    method=transfer external_id=bank-2026-09-25-01
venturectl get invoice 5 -f json | jq -r .status     # partially_paid
```

Reply: the per-venture revenue, expenses and profit for the quarter (one
row per venture per currency), then "Recorded payment #2 of 40.00 USD
against INV-5; the invoice is now partially paid." A direct `update invoice
5 status=paid` would have been refused (exit 8): paid states come from
settlement.

### Example: the same receipt proposed for approval over raw HTTP

```bash
curl -s -X POST -H "Authorization: Bearer $VENTURE_TOKEN" -H 'Content-Type: application/json' \
  -d '{"customer_id":8,"invoice_id":5,"amount":"40 USD","date":"2026-09-25","method":"transfer"}' \
  "$VENTURE_SERVER/api/v1/payment?stage=1"
# 202 {"status":"awaiting_approval","staged":true,"confirmation":{"id":"6eb8c494...",...}}
```

Reply: "Nothing is recorded yet: it is waiting for approval as 6eb8c494
(approve with POST /api/v1/confirmations/6eb8c494.../approve)."

### Example: "Why did my expense not get the vendor I set?"

`venturectl describe expense` shows the field is `vendor`; the command used
`vendor-name=...`, which the API dropped silently. Re-run `update expense ID
vendor="Hosting Co"` and check `get expense ID`.

## Constraints

- Never open the database, a series store or `automations.pod` behind the
  server's back; every business write goes through the API.
- Never guess field names, enum values, report options or action
  parameters; never present a staged, proposed, dry-run or drafted result as
  done.
- Never put tokens, passwords or secrets in argv, record fields, logs or
  replies; never ask the operator to paste one into chat.
- Never run irreversible operations (publish, purge, bulk delete, merge,
  activation, approval of a marketing send, printing) without the operator's
  explicit go-ahead; prefer `--stage` and dry runs.
- Never retry an `uncertain` mail, a lost-response write or a failed
  approval blindly; read the current state first.
- Configuration (modules, allowlists, `plugins.allow_exec`,
  `forge.runs_enabled`) is the operator's file and restart, not an API call;
  say so instead of inventing an endpoint.
- This skill describes baseline `8955f90`; when the live server disagrees,
  trust the server's generated answers and say the skill is behind.
