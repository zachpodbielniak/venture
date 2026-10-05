---
name: venturectl
description: Drive a VENTURE server from the command line — read, create, update, delete and restore any record type, run reports, and configure the git-forge integration. Use whenever the task involves a VENTURE instance, its data (sales, expenses, invoices, tickets, contacts, ledger), or its forge/AI-agent setup. Also trigger on "venturectl", "VENTURE server", or a request to query or change records in VENTURE.
---

# venturectl

`venturectl` is VENTURE's command-line client. It talks to a running server
over HTTP and never opens the database: one writer, one set of validation
rules, one audit trail.

Everything it does is generic over record types. There is no `venturectl
sale` subcommand and there never will be — the type is an argument, so a
record type added yesterday already works.

## Before anything else

```bash
export VENTURE_SERVER=http://127.0.0.1:8747     # default; omit if it matches
export VENTURE_TOKEN=vk_...                     # or pass --token
venturectl health
```

Mint a token from the UI (Settings → API tokens) or
`POST /api/v1/tokens` as an admin. It is shown once.

For an operation requiring an interactive session (including hosted workspace
administration), `--session-file /private/session.json` accepts an owner-only,
single-link regular JSON file, at most 8 KiB, containing:

```json
{"origin":"https://workspace.example.test","cookie":"venture_session=SIGNED_SESSION_VALUE"}
```

Obtain the session through the normal sign-in and MFA ceremony; this option
neither logs in nor elevates a token. Keep the file private (`chmod 600`), do not
put its contents in argv or logs, and unset `VENTURE_TOKEN`. The origin must
exactly match `--server`/`VENTURE_SERVER` (HTTPS, or numeric loopback HTTP for a
local fixture). Symlinks, hard links, FIFOs, non-private files, origin mismatch,
token combinations and MCP use are refused. Session-authenticated requests never
follow redirects, including redirects to another path on the same origin.
The ordinary generic `act`, `list` and `get` commands keep their existing forms.

Hosted administration uses declared actions: `tenant_workspace.set_state`,
`tenant_membership.set_membership`, `tenant_invitation.invite`,
`tenant_membership.invite_recovery` and `tenant_support_grant.revoke`. Inspect
parameters with `describe`; do not create or update these control rows directly.
`invite_recovery` is a privately delivered one-time recovery for an explicitly
reviewed, quarantined ordinary member. It preserves the user ID and member role;
it does not activate a suspended workspace or grant platform authority.

Operator maintenance is the local server binary's interface, not a `venturectl`
subcommand: `venture --tenant-admin USER --tenant-password-file FILE|-` requires
`--tenant-reason`, and existing identities additionally require `--tenant-recover`.
Use stopped-workspace `--tenant-revoke-credentials --tenant-reason REASON` before
restored authority is activated; it suspends and quarantines all restored login
capabilities. `--tenant-status` and explicit `--tenant-state` expose the operator
lifecycle contract. See `docs/hosted-workspaces.org` for the pinned configuration,
recovery, scoped support and audit requirements. Never pass passwords in argv.

If `health` fails, stop and fix that. Every other command will fail the same
way and less clearly.

## The one habit that prevents most mistakes

**Run `describe` before you write.** It prints every field in the spelling
you must type, what each reference points at, what each enum accepts, and
what the field is for:

```bash
venturectl describe forge_rule
```

```
forge_rule (table forge_rules)

  name                       string     required
                             What this rule is for
  repo_id                    reference  -> forge_repo
                             Leave empty for a forge-wide rule
  issue_type                 enum       [task|subtask|story|epic|bug|research]
  all_issue_types            boolean
                             Ignore the issue type and apply to every ticket in scope
```

Guessing a field name costs a silent no-op. Reading it costs one command.

## Commands

| Command | What it does |
|---|---|
| `types` | every record type |
| `describe TYPE` | fields, references, enum choices, help text |
| `list TYPE [filters]` | records, filtered and paged |
| `get TYPE ID` | one record |
| `create TYPE field=value ... [attributes.NAME=value ...]` | a new record; `attributes.NAME=value` goes into the record's attribute bag (a venture type's fields) |
| `update TYPE ID field=value ...` | change a record |
| `delete TYPE ID` | soft delete — the row stays, stamped |
| `restore TYPE ID` | clear that stamp |
| `forge settings ID` | encrypted configure/test/disconnect/import operation, JSON from stdin |
| `forge set-token ID` / `forge set-secret ID` | retired; refuse with encrypted-settings guidance |
| `forge verify ID` | record which account the token belongs to |
| `report [NAME] [PERIOD] [as_of=DATE] [organization_id=ID] [customer_id=ID] [currency=CODE] [compare_to=PERIOD] [account_id=ID] [basis=cash\|accrual] [dimension=VALUE] [band_size=N] [location_id=ID]` | list reports, or run one with an optional historical cutoff, legal entity, accounting basis, dimension and score-band width; the period may be left out before options (`report holdings organization_id=2` is `this_month`); `report aggregate PERIOD type=… measure=… group_by=…` totals any type (see *Aggregating any record type*) |
| `links TYPE ID` | every link touching a record, read from it |
| `link TYPE ID TYPE ID [kind=K] [note=T]` | link two records; kinds: related, blocks, blocked_by, depends_on, required_by, parent_of, child_of, duplicates, causes, caused_by, produces, produced_by, references, referenced_by, supersedes, superseded_by; unlink with `delete record_link ID` |
| `reconcile suggest TYPE ID [--matcher NAME] [--threshold N]` | rank matching book records; scores above the threshold (default 80) stage bank transaction action confirmations when banking is installed; never applies |
| `modules` | which modules the server runs; `-f json` for types, reports and reasons |
| `factory` | the software factory at a glance: milestones with progress, releases, builds, environments and what they run, open incidents |
| `factory actions` | what in the factory needs somebody, most pressing first: incidents with no fix, a red default branch, milestones slipping, releases ready or never deployed, budgets running out. Each names the record and the action that deals with it. Start here when asked "what should I do next" |
| `factory briefing` | the same and where things stand, as a few paragraphs from the assistant (needs the ai module) |
| `release readiness ID` | can it go out: checks that pass, warn or fail, with `ready` and a `score`; advice, never a gate |
| `release deploy ID ENVIRONMENT_ID [NOTES]` | record the release going live in an environment; records, does not deploy |
| `release notes ID [AUDIENCE]` | release notes for the people who use it, drafted by the assistant; nothing is written |
| `environment ID rollback [REASON]` | mark the current deployment rolled back and record the release before it as live again; refused when there is nothing to go back to |
| `milestone ID forecast` | when it lands at the pace of the last 28 days: `on_track`, `at_risk`, `overdue`, `stalled`, ... |
| `build ID ticket` | open the bug for a failed build; one per build |
| `build ID triage` | what a failed build's log excerpt is complaining about: category, cause, suggestion, whether a retry alone may pass (AI) |
| `incident ID postmortem` | draft a blameless postmortem from the records (AI); nothing is written |
| `release changelog ID [--replace]` | draft a release's changelog from the tickets marked fixed in it |
| `invoice checkout ID` | return a hosted Stripe Checkout URL for an eligible sent invoice; editor role, Stripe module required |
| `compose invoice\|quote JSON` | create lines, tax and optionally send in one request |
| `journal post ID` | post a draft through the shared service; editors propose, `--stage` always proposes |
| `release publish ID [--prerelease]` | cut the release on the forge; creates the tag, cannot be undone here |
| `dashboards` | the dashboards the token may see |
| `dashboard SLUG` | one dashboard, every widget evaluated; `-f json` for the whole answer |
| `dashboard export SLUG` | its definition as JSON; `dashboard import FILE` (or `-`) creates one from it; a trailing `organization_id=N` files it under that organization (default: the default one) |
| `dashboard create TEMPLATE` | `today`, `factory`, `reporting`, `progress`, `work`, `operations` or `overview`; `organization_id=N` as for import; `dashboard templates` and `dashboard kinds` list what is accepted |
| `inbox [--all]` | what the token's user has been told: mentions, assignments, watched changes, service levels, budgets, runs; `inbox read ID\|all` marks read |
| `watch TYPE ID` / `unwatch TYPE ID` | follow a record, so changes land in the inbox |
| `activity TYPE ID` | a record's timeline: every change with who and what moved, plus a ticket's comments and worklogs, and any other record's discussion comments (each with a `url` to the comment) |
| `comments list TYPE ID` | a record's discussion, threaded: top-level comments oldest first, each with its replies; `-f json` gives `{subject, count, comments}` |
| `comments add TYPE ID BODY` | say something on any record that takes comments; BODY is markdown, `-` reads it from stdin. `@username` tells somebody **who may read the record** (anybody else stays text, silently); `#type/id` links a record |
| `comments reply ID BODY` | answer comment ID; it lands in that comment's thread (one level deep) |
| `comments edit ID BODY` / `comments delete ID` / `comments get ID` | change your own (only the author may), delete yours (or any, as an organization owner/admin), read one |
| `feeds sync ID [--wait] [organization_id=N]` | queue a market data source's sync (`data_source` ID); answers `{"status": "queued", "data_source_id": ID}` at once. `--wait` polls the source's runs once a second and prints the run the sync recorded -- exit 1 when that run `failed`; after five minutes it gives up with exit 7 (the sync stays queued) |
| `feeds push ID FILE\|- [--wait] [organization_id=N]` | send JSON lines (a file, or `-` for standard input) to a `push` data source; answers `{"status": "queued", "push_id": ...}` at once. `--wait` makes the server hold the request until the run is written (at most two minutes) and prints the run; a `failed` run (a malformed line, named) exits 1, and an answer without the run (too slow, or two other requests already waiting) prints the queued answer and exits 7. Exit 4 when the source is not a push source, is off, or already has four pushes not yet stored -- push again when they finish |
| `feeds runs ID [organization_id=N]` | a data source's runs, newest first: status, units, rows, error |
| `feeds due [organization_id=N]` | every unit of every source in the organization and when it is checked next, soonest first; `null` for a unit that never runs on its own |
| `market quote ID [basis=B] [venue=KEY\|GROUP] [venue_id=N] [at=DATE] [currency=C] [prefer_currency=C] [fallback=true] [source=S] [organization_id=N]` | the price oracle for an `instrument` ID (or `product=ID` instead of ID): `{basis, found, price, value, evidence}`. Bases: `market` (default), `min`, `market_14d`, `historical_60d`, `region_median`, `region_p33`, `region_market_avg`, `sale_avg`, and the numbers `sale_rate`, `sold_per_day`, `quantity` (in `value`) |
| `market promote SOURCE_ID instrument\|venue KEY [organization_id=N]` | make (or find, or restore) the record for a venue or instrument a data source's store has seen; prints the record |
| `market browse [source=N] [search=T] [category=PATH] [venue=KEY] [group=G] [stock=true] [sort=COL] [dir=asc\|desc] [page=N] [per_page=N] [organization_id=N]` | every store row now, as `/market/browse` shows it (`rows`) |
| `market deals [source=N] [venue=KEY] [group=G] [category=PATH] [min_value="10.00 GOLD"] [max_pct=80] [top=N] [organization_id=N]` | in stock at or under the group's deal price (`rows`) |
| `market venues [source=N] [group=G] [organization_id=N]` | the venue index: cheaper/equal/dearer than the region, update interval, data age (`venues`) |
| `market instrument SOURCE_ID KEY [venue=KEY] [units=N] [organization_id=N]` | one instrument: its figures, every venue's row, history; `units=` prices a bulk buy (`bulk`) |
| `market watchlist [ID] [organization_id=N]` | the watchlists, or one priced now against its targets (`entries`); `watchlists` is the same verb |
| `accounts [list\|attention] [source=N] [group=REALM] [basis=B] [expiring_hours=N] [mail_days=N] [stale_days=N] [sort=S] [dir=asc\|desc] [organization_id=N]` | the operator's characters and banks, as `/accounts` shows them: `summary` (money on hand, inventory value, listed, mail, 30-day net), `attention` (where to log in next, most urgent first, each with its reasons), `accounts` (one row each). `attention` prints just the places and reasons |
| `accounts show SOURCE_ID KEY [basis=B] [ledger=N] [organization_id=N]` | one account: listings against the market (`undercut`, `vs_market_pct`, `urgency`), mail, holdings by place valued, the newest ledger rows. KEY is the account's key in the store (`"Drgold-Thorium Brotherhood"`); a `/` in it is fine |
| `accounts inventory [source=N] [account=KEY] [place=P] [category=PATH] [search=T] [min_value="100.00 GOLD"] [dead=true] [dead_days=N] [basis=B] [sort=S] [dir=asc\|desc] [page=N] [per_page=N] [organization_id=N]` | everything held, per item across accounts, valued (`rows`, `totals`, `portfolio_value`) |
| `accounts pnl [source=N] [period=P] [group_by=G] [account=KEY] [venue=KEY] [instrument=KEY] [label=TEXT] [top=N] [organization_id=N]` | the source's own trading ledger summed (`totals`, `buckets`, `top_items`) and the `flips` (buys matched to later sales, first in first out) |
| `accounts post SOURCE_ID [from=DATE] [until=DATE] [account=KEY] [dry_run=true] [organization_id=N]` | **writes to the books**: the source's external ledger as one journal per account per day (needs the source's `books: daily`); `--stage` proposes it. Answer: the data source with the pass in `result` (JSON text: `days_by_status`, `days[]`, `capital`, `notes`) |
| `accounts record-flips SOURCE_ID [from=DATE] [until=DATE] [instrument=KEY] [min_profit="5.0000 GOLD"] [limit=N] [dry_run=true] [organization_id=N]` | **writes to the books**: each sale matched FIFO to earlier buys becomes a closed `arbitrage_trade` (strategy `flip`), once (needs `books: trades`); `--stage` proposes it |
| `market alerts [count=N] [organization_id=N]` | the alert rules and the recent hits |
| `market alerts evaluate RULE_ID [--dry-run] [organization_id=N]` | what the rule fires now; **writes the hits** (under the cooldown) unless `--dry-run` |
| `arbitrage scan [STRATEGY] [option=value ...]` | opportunities now (`rows`; the table numbers them); options are the `arbitrage_scan` report's, plus `organization_id=N` |
| `arbitrage record ROW\|KEY [STRATEGY] [option=value ...]` | record scan row ROW (same options, `organization_id` included) or KEY as a planned trade; `--stage` proposes it |
| `arbitrage record name=N legs=JSON\|@FILE [strategy=S] [venture_id=N] [expected=JSON\|@FILE] [notes=T] [organization_id=N]` | a trade from explicit legs (the `record` action); `--stage` |
| `arbitrage calc surebet ODDS... --stake AMOUNT`, `calc back-lay BACK LAY --stake AMOUNT [commission=PCT]`, `calc flip BUY SELL [units=N] [cut=PCT] ...` | the calculators, on the server; `payout`/`profit` (surebet) and `worst` (back-lay) are the sure, rounded figures, `*_ideal`/`ideal` the unrounded ones |
| `arbitrage export FORMAT [STRATEGY] [option=value ...] [-o FILE]` | the scan as a registered export (`csv`, `shopping_list`, a plugin's such as the Blizzard plugin's `tsm`); raw bytes to stdout or FILE |
| `arbitrage close\|reopen\|abandon TRADE_ID [...]`, `arbitrage execute LEG_ID [occurred_at=T]` | the trade and leg actions, typed from their schemas; `--stage` |
| `arbitrage registries` | registered strategies, fee models, export formats and scan options |
| `plugins [list]` | loaded plugins: kind, runtime, what each provides (owner only) |
| `ticket ID sla` | a ticket's service-level clocks: state and seconds remaining for first reply and resolution |
| `ticket ID macro NAME` | apply a macro (canned reply plus field changes) — not stageable |
| `ticket ID worklog HOURS [NOTE]` | log time; the ticket's `logged_hours` follows |
| `sprints` / `sprint ID` | the sprints with their burn; one with its tickets |
| `bulk TYPE 1,2,3 field=value ...` | change many records in one transaction; `--delete` removes them; not stageable — use `update` per record to propose |
| `incident ID ticket` | open the bug for an incident, prioritised from its severity |
| `runs [--state S]` | mission control: every coding run with state, model, tokens, cost; totals |
| `budgets` | the agent budgets and their spend this window |
| `ticket ID triage [--apply]` | have the assistant propose a priority, issue type and tags; `--apply` keeps them. Changes nothing without it |
| `ticket ID summary` | what the ticket's whole thread amounts to |
| `ticket ID draft [AIM]` | draft the next reply. **Never posted** — show it to the operator, then `create ticket_comment` if they want it |
| `webhooks` | outbound webhooks: active, signed, consecutive failures, events, URL |
| `webhook test ID` | send a `webhook.test` delivery and wait for the answer |
| `webhook secret ID` | generate a signing secret — shown once, owner only |
| `kb search QUERY` | search knowledge bases by meaning; `--kb SLUG`, `--limit N` |
| `kb sync KB_ID` | re-read the base's source directory on the server |
| `kb reindex [KB_ID] [--force]` | re-embed articles that need it |
| `kb export KB_ID` | write an archive to stdout; `--format zip\|tar.gz` |
| `kb crossref TYPE ID` | link the knowledge bearing on one record |
| `kb article TYPE ID --kb N` | write a KB article from a record |
| `act TYPE ID ACTION [key=value ...]` | discover and perform a business action; `--stage` proposes it |
| `recurring run [--as-of DATE] [--dry-run] [organization_id=N]` | generate due invoices, bills, expenses and journals; a non-admin must name the organization |
| `collections run [--as-of DATE] [organization_id=N]` | queue overdue invoice reminders through the outbox; a non-admin must name the organization |
| `dunning sweep [as_of=DATE] [organization_id=N] [limit=N] [dry_run=true]` | templated reminder policies: one step per invoice per day, escalation to the owner; `dry_run=true` returns the plan and writes nothing |
| `batch invoice\|expense format=csv\|json payload=... [post=false] [organization_id=N] [--dry-run]` | all-or-nothing CSV/JSON document create |
| `sales-tax export period=PERIOD [jurisdiction=CODE]` | sales tax return CSV per jurisdiction: gross, exempt, taxable, collected, credited, net due |
| `customers health-sweep [as_of=DATE] [organization_id=N] [limit=N]` | one `check in: <company>` activity per red customer, never a second while one is open |
| `dedupe scan [kind=company\|contact] [organization_id=N]`, `dedupe merge ID survivor=N`, `dedupe dismiss ID` | propose duplicate companies or contacts; fold one into the other; close a proposal |
| `health` | is the server up |
| `mcp [--apply-writes]` | serve the API to an AI agent as a stdio MCP server |

Flags: `--server/-s`, `--token/-t`, `--session-file FILE`, `--format/-f table|json|yaml|csv`,
`--quiet/-q`. One-verb flags are refused anywhere else: `--wait` (feeds sync and push),
`--dry-run` (post backfill, billing, recurring, batch, market alerts
evaluate), `--stake AMOUNT` (arbitrage calc), `-o/--output FILE` (arbitrage
export), `--stage` (the writes that honour it; `act` checks the action is
stageable). `venturectl market help` and `venturectl arbitrage help` print
every verb of the group with options and examples.

`mcp` is the one command that refuses `--token`: it is spawned from an agent's
config file, and a credential written there is visible in `ps` to every
account on the host. It reads `VENTURE_TOKEN` and `VENTURE_URL` from the
environment. `--server` is fine — a hostname is not a secret.

Knowledge bases are ordinary record types, so `list kb_article`,
`get knowledge_base 1` and `create kb_article ...` all work and are the way
to read or write articles. The `kb` verbs are only the part that is not
CRUD. Two things to know before using them:

- **`kb search` finds meaning, not words.** It is the right tool when the
  operator asks about something written down rather than recorded — a
  policy, a specification, a handbook. `list kb_article search=...` matches
  characters and will miss a passage that answers the question in different
  words.
- **`kb reindex` without `--force` is cheap and safe**; with `--force` it
  re-embeds everything, which is what a change of embedding model requires
  and is otherwise a waste. Articles indexed by a different model are always
  re-embedded, force or not, because vectors from two models cannot be
  compared.

Periods, anywhere one is accepted (`report NAME PERIOD`, `period=` filters):

For historical financial visibility, pass `as_of` after the period, for
example `venturectl report pnl 2026-08 as_of=2026-08-31 organization_id=1`.
The date includes that UTC day; a timestamp is an exact cutoff. Rows deleted
after it still count. Omit it for live visibility. Fiscal calendars are
ordinary `fiscal_year` and `fiscal_period` records; creating a year generates
its monthly or quarterly periods. Closed or locked dates refuse financial
writes. Reopening needs `periods.reopen`, held by active administrators and
owners, and locked periods cannot reopen. `report snapshot_vs_live PERIOD`
compares preserved close totals with live reports.
named (`today`, `yesterday`, `this_week`, `last_week`, `this_month`,
`last_month`, `this_quarter`, `last_quarter`, `this_year`, `last_year`),
to-date (`ytd`, `qtd`, `mtd`), fiscal (`fy`, `fy_2026`), rolling
(`last_30_days`), calendar (`2026`, `2026-03`, `2026-Q2`, `2026-03-14`),
an explicit range (`2026-01-01..2026-03-31`), and `all`.

## Five traps, in the order they bite

**1. Field names use underscores on the wire.** Properties are `forge-id`
inside VENTURE and `forge_id` when you type them. `describe` prints the
typed spelling, so copy from there. A dashed key is ignored field by field:
the record saves and the value simply is not there.

**2. Filters are `field__operator=value`.**

```bash
venturectl list ticket issue_type__eq=bug status__ne=done
venturectl list expense amount__gte=100 occurred_at__gte=2026-01-01
venturectl list contact name__ilike=smith
```

Operators: `eq ne lt lte gt gte like ilike in not_in is_null not_null
between`. Bare `field=value` also works as equality.

`limit`, `offset`, `page`, `search`, `order` and `include_deleted` are
reserved and are *not* field filters. Anything else must be a real field or
the request is refused — with the available fields listed, which is usually
the fastest way to find the name you wanted.

**3. References are ids, so look the id up first.**

```bash
REPO=$(venturectl -f json list forge_repo name__eq=zach/venture \
        | jq -r '.records[0].id')
venturectl create ticket title="It crashes" issue_type=bug repo_id="$REPO"
```

A guessed or stale id is refused, not saved: writing a reference to a row
that does not exist (or is soft-deleted) fails validation (exit 8) with a
message naming the field and the id. Restore the target first if it was
deleted.

**4. Enums are their nick, never a number.** `issue_type=bug`, not
`issue_type=4`. `describe` lists them.

**5. Money is written plainly and read back structured.** Send
`amount=12.34`; you get `{"amount": 1234, "currency": "USD", "exponent": 2,
"formatted": "12.34 USD"}`. It is integer minor units, never a float. Use
`.formatted` for display and `.amount` for arithmetic.
A currency code is 2–15 characters (`[A-Z][A-Z0-9_]{1,14}`), not only ISO:
an install may define `GOLD` or `POINTS` as a `currency` record, and then
`gross="150 POINTS"` or, for a currency with denominations,
`gross="12g 34s 56c"` (or `"12g 34s 56c GOLD"`) is accepted. `.formatted`
stays the canonical decimal (`"12.3456 GOLD"`). See "User-defined
currencies" below.

**Comments are not `create comment`.** The generic routes refuse the
`comment` type (403) and list it only to the owner; use `comments add`,
which takes the author from your token -- a comment made with a token reads
"API token #N" with its owner's name beside it. A ticket keeps its own
conversation (`create ticket_comment`, with `internal`), and has no generic
discussion. A record you cannot read is "not found" to every `comments`
verb, never "forbidden". Nothing about comments stages: `--stage` does not
apply, because a comment changes no business record. A token minted with
the viewer role may read a discussion but not add to it.

## What it cannot do, by design

**Invoice financial state comes from settlement.** Create an invoice as a
draft, add its lines, then `update invoice ID status=sent`. A direct
`status=paid` or `status=partially_paid` write is refused. Record the money
instead, after reading the field declarations:

```bash
venturectl describe payment
venturectl create payment customer_id=1 invoice_id=1 \
    'amount=40 USD' date=2026-07-10 method=transfer external_id=bank-42
```

The receipt and its allocation settle atomically. Omit `invoice_id` to keep
a deposit, then create `payment_allocation` rows with `payment_id` or
`credit_id`. Overpayments remain customer credit. Standalone credits use
`customer_credit kind=credit_note`; a `refund` names an `allocation_id` or
an unused `credit_id`. `remaining`, invoice `paid_at`, and `workflow_state`
are derived. Issued invoice amounts and settlement history cannot be
edited or deleted. External payment IDs are unique per organization.
See [customer receivables](../../docs/receivables.org) for dates, reports,
account configuration and the current posting currency restriction.

**Sensitive fields are never accepted from a payload.** A forge access
token, a webhook secret, a password hash — naming one in `create` or
`update` is ignored, not an error, and the rest of the payload still
applies.

Each credential has its own way of being set correctly, which is why there
is no generic "write this sensitive field" command: a password must be
hashed, a forge token must not be, and one command for both would be a way
to get one of them wrong.

For a forge, use the encrypted settings adapter. The request is JSON on stdin,
never secrets in argv:

```bash
venturectl forge settings 1 < protected-settings.json
```

Create input: `{"operation":"configure","connection_id":0,"version":0,"settings":{"token":"SUPPLY_PRIVATELY","webhook_secret":"SUPPLY_32_OR_MORE_RANDOM_BYTES_PRIVATELY"}}`.
The response contains only connection metadata. Rotation repeats `configure` with
both credentials and the exact current `connection_id` and `version`. Operations
`test` and `disconnect` take that same identity without settings. `import` with
both identity numbers zero explicitly verifies, encrypts and transactionally clears
legacy plaintext; old backups may still contain it. Failed import changes nothing.

The forge must name an explicit organization. Account/origin changes require
disconnect first. Settings remain platform-owner/admin capability; organization
membership does not authorize arbitrary forge origins or host execution.
`forge set-token` and `forge set-secret` are retired and refuse. `forge verify`
uses the encrypted binding; configure already verifies its account. See
[forge documentation](../../docs/forge.org) for worker revocation and clone limits.

A user's password still has no CLI path and is set on the account page.
Setting a hash directly is what hashing exists to prevent.

**Audit entries and run records refuse writes entirely**, for everybody.
They are the record of what happened.

**Ledger entries are read-only journal projections.** Generic writes to
**ledger_entry**, including staged writes, are refused. **journal** and
**journal_line** can be edited while draft; posting and reversal use the ledger
service, and generic updates cannot advance the journal state. Saving a sale
with gross or an expense with an amount posts through that service and needs
an organization. Correcting financial values creates reversal and replacement
journals. Deleting a source record does not erase its journal.

Use **report trial_balance PERIOD** for posted account balances at the period's
end. It reports book currencies separately, the organization's book currency
first, each row labelled in `books` (`Book currency: GOLD`, `Separate book:
TICKET`, `Own book: EUR (no rate to GOLD)`). The existing P&L still reads
operational sales and expenses; it is not the authoritative journal balance.

**`pnl`, `ventures` and `monthly` keep one figure per currency, never a
converted total.** The book currency always comes first (zero when nothing
is in it) and keeps the plain metric keys (`revenue`, `expenses`,
`profit`); any other currency adds its code: `revenue_TICKET`,
`profit_EUR`. Rows carry a `currency` column — `pnl` repeats its eight
lines per currency, `ventures` is a row per venture per currency, `monthly`
a row per month per currency. Read `revenue_<CODE>` for another currency's
figure; do not add a TICKET row to a GOLD one. A bare date is one day:
`report pnl 2026-03-14` is the fourteenth only. When the period finished an
arbitrage trade (closed or abandoned), `pnl` adds "Arbitrage gains", "Less
arbitrage fees" and "Arbitrage result" per currency and the result is in
`profit` (metric `arbitrage`, `arbitrage_<CODE>`); `ventures` and `monthly`
add an `arbitrage` column their profit includes. No finished trade, no
lines.

**`venturectl mcp` stages writes rather than applying them**, unless started
with `--apply-writes`. A staged write sends the request with `?stage=1`, and
the server mints a confirmation instead of applying the change. The tool
answers with the confirmation's id and the routes that decide it. It really
is queued: `GET /api/v1/confirmations` lists it, `venture_confirmations`
lists it, and the AI panel shows it beside anything VENTURE's own assistant
has staged.

Nothing takes effect until somebody approves. Say that, with the id — do not
report a staged change as done.

Against a VENTURE too old to stage (no `staged_writes` in `GET
/api/v1/health`) the tool falls back to a client-side *hold*: it prints the
request it would have sent and sends nothing, and it says outright that
nothing is queued. Read which of the two you got; they are worded
differently on purpose, and telling somebody to go and approve a change that
was never sent wastes their time and leaves the change unmade.

**Any write over the REST API can be staged the same way**, not only through
`venturectl mcp`: add `?stage=1` to a `POST`, `PUT`, `PATCH` or `DELETE` and
the change waits for approval instead of applying. The response is a `202`
carrying the confirmation. Two traps:

- A `stage` value the server does not recognise is a `400`, not a silent
  "no". Use `stage=1`.
- Approving is refused with a `409` if the record changed since it was
  staged, and the confirmation is then dropped. Re-read the record and stage
  the change again rather than retrying the approval.

A card waits `ai.confirmation_ttl` seconds (an hour by default) and is then
dropped; at most `ai.confirmation_limit` may wait at once. Both settings keep
the `ai.` prefix but govern every staged change.

`venturectl --stage` does the same from the shell:

```bash
venturectl --stage create expense description="Cover art" amount=250.00
# Not applied. It is waiting for approval as a3f9c118.
#   approve: POST /api/v1/confirmations/a3f9c118/approve
```

It is refused on any command other than `create`, `update`, `delete`, `act`, `dunning sweep`, `dedupe`, `journal post`, `sequence enroll`, `lead convert`, `billing`, `arbitrage record|close|reopen|abandon|execute` and `accounts post|record-flips`,
because those are the only routes that read it -- and an unknown query
parameter on a write route is ignored, so a quietly accepted `--stage` would
apply the change it was asked to hold back.

**Some types need more than an editor.** `forge` is owner-only, `forge_rule`
and `plugin_config` are admin-only, `user`, `api_token` and `mail_account`
are owner-only. A 403 here means the token's role, not a bug.

## Worked example: wire up a forge

```bash
# 1. The server, and where git lives — often a different host
venturectl create forge name="Home" kind=forgejo organization_id=1 \
    base_url=https://git.example.com \
    clone_base_url=git@git-ssh.example.com \
    active=true

# 2. Both credentials in an encrypted organization binding (JSON stdin)
FORGE=$(venturectl -f json list forge name__eq=Home | jq -r '.records[0].id')
venturectl forge settings "$FORGE" < protected-forge-settings.json
# Keep the separately generated webhook secret privately and install it on the forge.

# 3. A repository
venturectl create forge_repo name=owner/project forge_id="$FORGE" \
    default_branch=main branch_prefix=venture/ \
    push_issues=true accept_issues=true active=true

# 4. A rule: bugs anywhere on this forge get an agent and a draft PR
venturectl create forge_rule name="Bugs" forge_id="$FORGE" \
    issue_type=bug enabled=true runner=agent outcome=draft_pr \
    trigger=on_create max_runs_per_day=10

# 5. Check what a ticket would resolve to
venturectl list forge_rule forge_id__eq="$FORGE"
```

Rules resolve most-specific-first: repository+type, repository catch-all,
forge+type, forge catch-all, then nothing. **Nothing is a legitimate answer**
— a repository nobody enrolled does not get an AI because a rule elsewhere
was written generously. A disabled rule falls through to a broader one; to
suppress work for one repository, enable a rule with `ai_enabled=false`.

Coding runs are off until `forge.runs_enabled` is true in the configuration.
Everything else works without it.

## Reading output in scripts

Output is a table on a terminal and JSON when piped, so `-f json` is only
needed when you want JSON *and* a terminal.

```bash
venturectl -f json list sale | jq -r '.records[] | "\(.id)\t\(.gross.formatted)"'
venturectl -f json get ticket 1 | jq -r '.title'
venturectl -f json list forge_run state__eq=failed | jq -r '.records[].failure_reason'
```

`-f csv` is for handing data to a spreadsheet or another tool: every field
appears (nothing truncated like the table), the header uses the wire
spelling `describe` documents, money prints as its formatted form, and
`-f csv report NAME` returns the server's own CSV rendering. Cells are
escaped and a leading `=` is defused.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | fine |
| 2 | usage |
| 3 | not found |
| 4 | conflict — usually a uniqueness constraint |
| 5 | auth — token missing, wrong, or the wrong role |
| 6 | unsupported |
| 7 | network or timeout — the server is not there, or `feeds sync --wait` / `feeds push --wait` gave up waiting (the sync or push stays queued) |
| 8 | validation — the record was refused |
| 1 | anything else |

Branch on these rather than on message text; the messages are written for
people and will change.

## Keeping this accurate

This file is maintained with the code. When a command, a flag, an exit code
or a trap changes, change it here in the same commit — a skill that is
confidently wrong is worse than no skill, because it is followed.

Check it against reality with `venturectl --help` and
`venturectl describe <type>`; those two are generated from the source and
cannot drift.

Customer statements require `customer_id`: `venturectl report customer_statement 2026-01 customer_id=1 currency=USD organization_id=1`. Report options also accept `venture_id` and `group_by`; REST, the report page and CSV exports preserve these filters. Receivables requires event history; existing invoices without issue events require an explicit migration, not a guessed balance.

## Federation and offline working copies

Federation is opt-in. `federation_peer` and `federation_grant` are owner-only generic records; use `describe` first. Peer keys are public Ed25519 base64 pins verified out of band, never private keys. Federation uses its own `federation.origin`, which may differ from the ordinary web URL. The assistant cannot administer federation trust.

`venturectl federation JSON` sends one operation to the local authenticated server. Examples:

```sh
venturectl federation '{"action":"identity"}'
venturectl federation '{"action":"remote","peer_id":1,"operation":{"action":"list"}}'
venturectl federation '{"action":"pull_collection","peer_id":1,"collection":"joint_business","offset":0}'
venturectl federation '{"action":"pull","peer_id":1,"type":"venture","uuid":"UUID"}'
venturectl federation '{"action":"edit","id":1,"version":1,"fields":{"description":"Offline work"}}'
venturectl federation '{"action":"sync","id":1}'
venturectl federation '{"action":"resolve","id":1,"version":5,"field":"description","keep_local":false}'
```

Federation is platform-only in hosted mode: tenant CLI/API calls are refused,
and retained replicas never reconnect automatically. Explicit local operator
maintenance may invoke the service; it does not lend authority to timers.

Collection pulls return at most ten results, `next_offset` and `more`; continue pages while `more` is true and inspect per-record errors. Pull imports/merges without pushing; sync pushes conflict-free changes with an expected remote version. Edits require the local replica version. A conflict blocks that record until resolved; choosing remote can accept a removed/revoked field. Never update `federation_replica` through generic CRUD: its merge state belongs to the service. Copies remain usable during outages but are not authoritative local accounting rows. New source objects and binary attachments are not created/copied offline. See `docs/federation.org` for key exchange, grants, scheduling and revocation.

## Fixed assets and recurring journals

Use `asset place ID`, `asset dispose ID proceeds=AMOUNT`, or `asset write-off ID`.
Set dates with `in_service_at=` or `disposed_at=`; ordinary profile fields
are configured on the draft first.

Run `assets run-period YYYY-MM organization_id=ID --dry-run` to preview,
then omit `--dry-run` to post. The whole month commits or rolls back; retries
post once. A closed period is refused. Use generic `create deferral` to
build a prepayment/accrual schedule, and stage `operation=settle` with a
settlement account after an accrual's releases complete. Never set derived
status or edit schedule rows directly. `report fixed_assets` and
`report deferrals` accept the ordinary period and `as_of` options.
## Organization membership

Read `docs/orgaccess.org` for the role matrix. Membership and team records use
generic CRUD. Tokens intersect mint-time memberships with current authority;
new grants never widen an old token. Missing membership gives empty results or
404; a refused in-organization write gives 403. Owner/admin data authority and
output formats remain unchanged. `journal post ID` returns a confirmation for
an organization editor. Treat that response as pending until finance approves.
## SaaS billing actions

Use `billing start company_id=N plan_price_id=N seats=N [discount_id=N | discount_code=CODE]
[skip_trial=true]` to start a `customer_subscription`. Read
`describe plan_price` and the price first: non-trial starts issue an invoice
immediately, while trials bill at activation unless `skip_trial=true`. A
`discount_id` must be a `plan_discount` of the same plan, active and not past
its `ends_at`; it comes off the first `periods` invoices (0 is every one).
`discount_code` names the discount by the `code` the customer quoted instead,
matched without regard to case among the chosen price's plan's discounts;
an unknown, retired or expired code is refused saying which, and giving
both `discount_id` and `discount_code` is refused.
A plan with a `venture_id` is refused for another venture's customer.
Use `billing change ID plan_price=N [at_period_end=true]`,
`billing change-seats ID seats=N`, `billing cancel ID [at_period_end=true]`,
`billing pause ID`, `billing resume ID`, `billing mark-payment-failed ID`
and `billing recover ID` for lifecycle actions. Never update subscription
status directly; the service refuses it.
`billing cancel ID` without `at_period_end=true` credits the unused days of
an invoiced period (a trial or an unbilled period gives nothing): a
`customer_credit` credit note, tax included when the period's invoice was
taxed, applied to what that invoice still owes. The result's
`proration_amount` is that credit, negative. A `plan_price` may carry
`tax_code_id`; its invoices are taxed at that rate (exempt customers stay
exempt), and a price in use is immutable, so taxing an existing plan means a
new price and moving customers to it (the plan page's "Move its N
customers to..." does all or none). MRR counts a plan discount while it
covers the period being billed.

`billing usage SUB quantity=N [key=K] [at=DATE]` reports metered use for a
subscription whose price has a `usage_unit` (typed JSON; the same as
`create usage_record`). Reuse a `key` when retrying: a repeat is refused as a
conflict, not counted twice. Usage before the current period, on a flat price
or on an ended subscription is refused. At renewal the ended period's usage
over `included_units` is a line on the renewal invoice, at `unit_amount`.

Customers holding a portal link can switch price (same venture, at renewal
by default) or cancel at renewal themselves; those changes appear as
ordinary `subscription_event` rows with the actor `customer portal`.

`billing renew --as-of DATE [--dry-run]` sweeps due periods, and also
queues the trial-ending reminder (`billing.trial_reminder_days`, default 3)
to each customer whose trial ends within that many days -- once per
subscription; a dry run queues none. `change` and `change-seats` queue the
customer a price-change notice per change unless
`billing.price_change_notices` is false. Both land in the mail outbox
(`list mail_message`), keyed `trial-reminder:<subscription uuid>` and
`price-change:<event uuid>`.
`billing dunning --as-of DATE [--dry-run]` records dunning notices/actions.
Pass `organization_id=N` to choose the legal entity. Dry runs write nothing.
`billing collect` records confirmed manual payments only; an authorized card/ACH mandate does not execute a provider charge and is refused here.

`--stage` holds a billing action for approval. The assistant's generated
create tool can instead stage `billing_request` with `action`, `at`,
`organization_id` and the relevant subscription/customer/price fields.
Approval refuses a subscription changed since staging.

`report mrr PERIOD currency=USD`, `report churn PERIOD currency=USD`, and
`report subscriptions_due PERIOD days=14` read the registered reports.
`churn` ("Subscription churn (billing)") is the billing cohort: logos and MRR
lost, gross and net revenue retention (`grr_bps`, `nrr_bps`); `mrr` also
gives `arpa` and `quick_ratio`. Headline activity and recurring churn is
`customer_churn`; `cac`, `ltv`, `ltv_cac` and `customer_cohorts` are the
others, and all accept `venture_id=` and `as_of=`. The home page's churn card
reads `churn` only when billing is in use, else `customer_churn` -- follow
the card's `link` rather than guessing. `/api/v1/headline?format=csv` exports
the cards; a viewer without the owner, admin or finance role gets them with
`state` `restricted` and no figures.
The P&L cuts are `revenue_by_customer` (`by=source` to group by lead
source), `spend_by_vendor` (`by=category`), `recurring_costs` and
`cash_outlook` (`weeks=N`, default 8). `cash_forecast` is the budgets
module's ledger-driven forecast, a different report. The P&L card's
`links` open the four with the card's period and scope.
Read their notes: MRR is contracted revenue, not cash or recognized income;
churn rates are in basis points. Proration adjustments are settled on the
next renewal. The optional Stripe adapter collects billing invoices only after
verified hosted reusable customer authorization; provider-confirmed cash and
failures use the existing billing recovery lifecycle. A manual payment-method
record does not authorize a Stripe charge. Billing notices remain delivery
intents for the mail adapter.
## Transactional mail

`mail send to=... subject=... body=...` queues mail; `--html FILE` supplies
HTML. `mail test to=...` immediately tests real SMTP. `mail deliver --limit N`
submits due rows. `mail list state=uncertain` lists uncertain acceptance;
`mail retry ID` is a deliberate resend with the same Message-ID. `mail sync
[organization_id=N] [limit=N]` runs the bounded inbound IMAP sweep over every
active `mail_account` in the organization that is not backing off; the
report's `skipped` counts accounts in backoff or mid-sync elsewhere and
`deferred: true` means a message or time budget ran out, so run it again to
continue. `mail sync account_id=N` syncs one readable account now, ignoring its backoff. Pass `organization_id=N` to scope another organization. Never
automatically retry uncertain rows. Actions reject `--stage`; propose an
enqueue with the generic `--stage create mail_message` command when approval
is required.

`mail_account` is assigned by organization administrators and uses an explicit
encrypted binding in connector settings; `secret_env` is unused historical
metadata. A positive `private_owner_id` imports private messages and attachments
without CRM capture. Zero is explicitly shared business mail. Its
`consecutive_failures`, `next_attempt_at`, `last_error`, `cursors` and
`sync_lease_until` are maintained by the sync; do not write them. Five failed
syncs ending in a refused login set `active=false`: fix the credentials, then
update `active=true`. Generic writes to `mail_inbound` are refused;
`mail_unmatched_sender` is ordinary CRM data. Use `list mail_inbound` and
`list mail_unmatched_sender dismissed=false` to read what the sweep filed; a
`mail_inbound` row with `skip_reason` is a message filed as a stub after
repeated failures or read truncated for size. `mail contact ID` turns an
unmatched sender into a contact and backfills its earlier mail; `mail dismiss
ID` keeps the address as an ignore-list entry. Both take the unmatched
sender's id, not a contact id.

## Calendar sync

`calendar sync [organization_id=N] [limit=N]` runs the bounded two-way CalDAV
sweep over every active `calendar_account`: dated calls and meetings go up as
VEVENTs, events made on the calendar come back as planned meetings, removals
cancel rather than delete, and a change on both sides is settled by
last-modified with the loser noted on the activity timeline. It refuses
`--stage`. `calendar_account` uses an explicit encrypted connector binding;
`secret_env` is unused. A positive `private_owner_id` selects private, read-only
imports without shared activity mirroring or export. Ownership is immutable.
Generic writes to `calendar_event` are refused.
`booking_page` (slug, owner, duration, buffer, IANA timezone, availability
JSON of weekday to `HH:MM-HH:MM` windows) is ordinary editor data and serves
the public `/book/<slug>` page, which books a contact and a meeting.

### Commercial quote actions

`quote send ID`, `quote accept ID 'by=Full Name'`,
`quote decline ID 'reason=Explanation'`, `quote revise ID` and
`quote start-subscription ID` call the quote
service. Acceptance creates and issues the invoice in the same transaction unless `billing_mode=progress`.
A `quote_line` with `plan_price_id` (quantity = seats, no discount or tax)
is left off that invoice; `quote start-subscription ID` on the accepted quote
starts the subscription once and returns `result_subscription_id`. A second
start is refused.
`compose quote JSON` and `compose invoice JSON` create a draft (or send) in one call.
For invoices, `issued_at` defaults to today at midnight UTC; `due_at` may name an
explicit due date. Otherwise `due_days` defaults to 30, counted from the invoice
date (zero or negative leaves no due date). Sending preserves the invoice date;
configured fiscal-period checks still apply to drafts and issuance.
To remember an invoice exemption on its customer atomically, include
`remember_tax_exemption=true`, `tax_exemption_kind` and `tax_exemption_number`
alongside `tax_exempt=true`. These fields are included in issuance approval;
a refused or proposed operation does not change the customer.
For a staged action use `--stage create quote_action quote_id=ID action=accept
expected_version=N 'accepted_by=Full Name'`; obtain the quote's current
`version` first. `revision` is the separate commercial revision number.
## Leads

Use `describe lead` before capture or qualification. `lead convert ID
[deal=yes|no] [company_id=ID] [contact_id=ID]` requires a qualified lead and
creates or links CRM records atomically. `--stage` proposes conversion for
approval. Never set `status=converted` or conversion ids with generic updates.
A new deal retains the lead's assigned owner, source and campaign. Existing
linked company/contact records retain their values; ownership grants no role.
`lead reassign ID [owner=NAME]` assigns explicitly or reruns the matching
rules; staged reassignment is refused. Recycle with `update lead ID
status=recycled unqualified_reason=... recycle_until=YYYY-MM-DD`.

`leads reroute ID` clears the owner and evaluates the `lead_routing_rule`
records again in `position` order, writing "Lead routed" or "No rule matched"
to the timeline. `leads rescore ID` clears the `score_manual` mark and applies
the `lead_scoring_rule` formula. Both take no other arguments, refuse a
converted lead, and refuse `--stage`; each is also spelled `lead reroute` /
`lead rescore`. Rules are ordinary records -- `create lead_routing_rule
position=N 'conditions=source=web' action=assign_user|round_robin|assign_venture
...` and `create lead_scoring_rule 'conditions=...' points=N` -- so use
`describe lead_routing_rule` for the exact enum values. A malformed condition
or an action missing its target is refused at the create, not when a lead
arrives. `round_robin` needs the `orgaccess` module for teams. Never set
`routing_rule_id` with a generic update, and never create a
`lead_score_history` row: both are refused.

Reports are `lead_sources`, `lead_response_time`, `leads_recycled_due`,
`lead_routing` and `lead_scoring`; `lead_scoring` takes `band_size=N` (default
25). See `docs/leads.org` for definitions and public capture forms.
## Planned activities

`activity complete ID outcome=...` completes a planned activity, writes interaction history and advances recurrence atomically. `activity list mine|overdue|today` reads your daily worklist. Generic `create activity` and `update activity` edit the plan; generic `status=done` is refused. The existing `activity TYPE ID` command still reads a record timeline. Use `report worklist organization_id=ID` for the current UTC week per owner.
## Portal invitations

`supplier invite company_id=ID email=ADDR` queues the private supplier access link through the mail outbox. Configure an HTTPS `server.base_url` and enable mail first, then deliver the outbox. The response contains redacted access metadata, never the bearer token. Revoke with `supplier revoke ID`.

## Vendor payables

Run `describe vendor_bill` and `describe vendor_bill_line` before creating
a draft and its lines. Bill quantity is an exact decimal string, with at
most three decimal places. Supplier companies have `kind=supplier`.

Use `bill approve ID date=DATE`, `bill pay ID 'amount=40 USD' date=DATE`,
and `bill void ID date=DATE` for financial actions.
`bill pay-bulk 1,2,3 adapter=transfer` pays selected approved bills through
the payables service adapters, never generic writes.
`close open|run|sign|complete|reopen|pack` is the accountant close
workspace. `capture ingest|convert|reject` files receipts and supplier
invoices. `accounting` lists the daily books next actions. Omitted payment amount
pays the outstanding balance. Direct bill status updates are refused.
These CLI actions apply directly; to stage, use generated record creation:
`vendor_bill_event` with `bill_id`, `vendor_id`, `kind=approve`,
`state=approved`, and `date`, or `bill_payment` with vendor, bill, amount,
method and date. MCP `venture_create` stages those records normally.

`report payables PERIOD` is dated aging. `report vendor_statement PERIOD
vendor_id=ID` is the supplier statement. Both accept `organization_id`,
`currency` and `as_of`. See `docs/payables.org` for credits, immutable
history and the single-date limitation on optional paid-line expense conversion.
Banking business actions use `bank ACTION ID [JSON|@FILE]`. Import identifies
an account, auto/reconcile a statement, and match/unmatch/exclude/create a
transaction. `bank match AUTO STATEMENT_ID` runs exact automatic matching.
Match parameters are `{"parts":[{"type":"expense","id":1,"amount":"-10 USD"}]}`;
exclude needs a reason, and receipt creation needs customer_id. See docs/banking.org.
## Sales pipeline actions

`venturectl deal move ID STAGE [NOTE]` calls the deal transition service.
Use `describe pipeline_stage` and `list pipeline_stage` to find the destination.
Fill required deal fields and a loss reason before moving to a lost stage.
`update deal` cannot change either stage field or the closing timestamp.
Reports: `stage_duration`, `funnel`, `forecast`, `loss_reasons`, `overdue_deals`;
filter with `pipeline_id=N` and `owner=USERNAME`.
## Follow-up sequences

Use `describe sequence`, `describe sequence_step` and
`describe sequence_enrollment` before configuring a journey.
`sequence enroll ID contact_id=ID enrollment_reason=...` calls the service;
`--stage sequence enroll` queues approval. Generic staged
`create sequence_enrollment sequence_id=ID contact_id=ID` is equivalent.
Approval rechecks suppression and duplicate enrollment at application time.

`sequence run [--as-of TIMESTAMP] [organization_id=ID]` processes due steps
for one organization. Use an ISO timestamp including timezone. Email steps
create pending `sequence_delivery` rows; this command does not send mail.
`sequence status ENROLLMENT_ID` shows progress and delivery history.
Pause, resume and exit use the REST service actions documented in
`docs/sequences.org`; generic enrollment edits are refused. Completed
step identities are retained across restarts and sequence edits.
## Record actions

Use `venturectl -f json describe TYPE` to discover `actions`, their parameters
and whether they can be staged. `venturectl act TYPE ID ACTION key=value`
uses those declarations; `venturectl --stage act journal 42 post` proposes a
posting. A staged result is awaiting approval, never completed. Generated
action tools in the assistant and MCP always stage, including when other
writes are configured to apply automatically.

Journal reversal: `venturectl act journal 42 reverse occurred_at=2026-09-13 memo="Correction"`.
Type-level creation: `venturectl act journal 0 create_and_post 'journal={...}'`,
with header fields and a `lines` array. Both support `--stage`. Use real
source and account IDs from the same organization. Invalid lines leave no
draft behind; closed periods and repeat reversals are refused.

The `--stage` help lists `create/update/delete/act/dunning sweep/sequence enroll/lead convert/billing`; the same flag also
applies to a type-level journal creation at ID zero.

### Accounting second-person consent

When an organization enables a post/pay second-actor rule, an operation that
posts or pays first returns permission denied after saving a pending proposal.
That response is not a successful posting and is separate from `--stage`/202.
A different authorized account must repeat the same business command with the
same inputs; two tokens from one account do not qualify. One consent covers
its generated invoices, allocations and journal entries, and is consumed only
when the whole operation succeeds. Changed inputs or business/configuration
records require a fresh proposal; consent expires after 24 hours and must be
re-proposed after a server restart or posting-rule replacement. Draft editing
and read-only previews remain available. Use an explicit date for reproducible
posting commands, and reread records after any failed operation.

### Automatic journals

`post backfill [organization_id=ID] [--dry-run]` is an editor action which posts
missing sale/expense versions in date order. Use `report unposted all` to review
candidates, then `post backfill --dry-run` to validate without retaining writes.
The response includes `candidates`, `posted`, `skipped` and `dry_run`. Period
refusals abort the entire batch. `posting_profile` uses the normal generic
CRUD commands; consult `describe posting_profile` for its account mappings.
## Recurring documents, collections and batch entry

`recurring run --as-of DATE [--dry-run]` generates due schedules through the
existing settlement, payables and posting services. Closed periods are skipped.
`collections run --as-of DATE` enqueues overdue reminders with durable
idempotency keys. `act invoice 0 batch_create` / `act expense 0 batch_create`
create many documents in one transaction. See `docs/recurring.org`.

## Overdue reminders (dunning)

`dunning sweep as_of=DATE` enqueues the due step of each issued, unpaid,
undisputed, unpaused invoice's `dunning_policy` (invoice's, else its
company's, else the organization default; a deleted one falls through to the
next) and records a `dunning_event`; rerunning it sends nothing twice.
Arguments are `key=value`, not flags; `organization_id`, `limit` and
`dry_run` are sent typed. A token or user that is not a global owner/admin
must pass `organization_id=N` and needs the owner, admin or finance role in
that organization (reminders are financial); without it the answer is 422
`organization_id: ... runs in one organization`, from another organization
404, as an organization editor 403. The same holds for `act` on any
type-level action (`recurring_schedule 0 run`, `collection_policy 0 run`,
`invoice 0 batch_create`). The answer is an unsaved policy whose `last_sweep`
is a JSON string: `queued`, `escalated`, `suppressed`, `failed`,
`failed_invoice_ids`, `warnings`, and with `dry_run=true` a `plan` (invoice,
step, offset, outcome, reason, recipient, subject) with nothing written. A
failing invoice is recorded and skipped, not fatal: read `failed_invoice_ids`.
`as_of` more than a day ahead is refused except for a dry run.
`--stage dunning sweep` proposes the sweep for approval. The escalating final
step creates a `collect: <invoice>` activity owned by `invoice.owner`, else the
customer's owner, else the policy's `escalation_owner`; an opted-out customer
still escalates. Follow with `mail deliver`.

`act dunning_event ID retry` re-runs a `failed`, `dead` or `uncertain` step
under a new key (stageable; refused once a later step or attempt exists).
`act dunning_policy ID test_send invoice_id=N [offset=N]` mails the rendered
step to your own user email only, `[TEST]` subject, pay link withheld, no
event recorded; not stageable, and refused for a token or a user without an
email. Pause with `update invoice|company ID dunning_paused_until=DATE
dunning_pause_reason=...`. `report collections [PERIOD]` measures effectiveness per
step over events queued in the period (default this month; `report
collections all` for everything); `report dunning_worklist` lists open overdue invoices with aging, last
step and next step. `dunning_event` cannot be created, edited or deleted
directly (exit 8). See `docs/dunning.org`.

## Accounting cutover

Opening balances from another ledger go in as a batch: preview, import,
reconcile, activate. Always preview first and read the batch's
`reconciliation_report` and its `accounting_cutover_row` exceptions: every
bad row is listed, located as `open_ap[12] bill-9: ...`, and import refuses
until the errors are gone. `venturectl cutover template SECTION` prints a CSV
header; `venturectl cutover csv source=quickbooks cutoff=2026-01-01
currency=USD open_ar=@ar.csv open_ap=@ap.csv trial_balance=@tb.csv`
previews a batch from CSVs (`build_only=true` prints the payload JSON
instead). Then `act accounting_cutover ID import`, `reconcile`, `activate`.
These actions cannot be staged.

Traps: amounts are strict `[-]1234.56 CUR` strings (no symbols, no `CR`, no
locale formats unless the payload sets `decimal_separator` and
`thousands_separator`); every document date must be before the cutoff; open
AR needs `date`; migrated invoices and bills post to opening clearing (3900),
never income, expense or tax, and cannot be voided normally afterwards.
Reconcile ignores activity dated at or after the cutoff, appends to the
report and names failing checks with expected, ledger and difference.
Before `rollback`, run `act accounting_cutover ID rollback_preflight` and
read the `BLOCKER` lines; an active batch cannot be rolled back. See
`docs/cutover.org`.

## Customer health

`report customer_health PERIOD [band=red|amber|green] [owner=USERNAME] [sort=[-]column]`
lists every customer company with last touch, open deals, overdue invoices
and days, open tickets and SLA breaches, dunning step and trailing-12-month
revenue, banded against the organisation's `headline_setting` thresholds
(`health_touch_days` 30, `health_overdue_days` 15, `health_open_tickets` 3;
zero means the default). An unknown band or sort column is refused (exit 2).
`customers health-sweep [as_of=DATE] [organization_id=N] [limit=N]` creates
one planned `check in: <company>` activity for each red company's account
owner and answers `{"created": N}`; it never duplicates an open one. Needs
the `customer_health` and `activities` modules. See `docs/reporting.org`.

## Ledger statements

`report balance_sheet`, `income_statement`, `cash_flow`, `general_ledger`,
`account_balances` and `pnl_reconciliation` read posted evidence per exact
organization and currency, book currency first, with a `books` label per row
(a separate book or a currency with no rate is its own section; memo never
appears). Pass `compare_to=2026-07` after the selected period
for prior/delta columns; general ledger also accepts `account_id=ID`.
For example: `venturectl -f csv report balance_sheet 2026-08 organization_id=1 currency=USD compare_to=2026-07`.
Synthetic totals have no single account ID; actual account/journal IDs link
to their record pages. Cash-flow controls use the conventional chart codes
documented in `docs/statements.org`.

## Accounting custom values and scheduled output

`fields value record_type=TYPE record_id=ID name=NAME value=VALUE` PATCHes the
owning record's `attributes` object; direct writes to `custom_field_value` are
refused. Empty values clear optional fields and fail required-field validation.
Built-in property names cannot be declared as custom fields.

Use `get report_pack ID` to read `last_output`, the last successful scheduled
result array. Version 4 accounting packs restore document history into an empty
organization and remap record identities. External references resolve by UUID.
Version 3 supports only manual ledger imports; old document packs remain
refused. Accounting packs exclude installation credentials and attachments.
See `docs/backup.org`.

## Duplicates (dedupe)

`dedupe scan kind=company|contact [organization_id=N]` proposes
`duplicate_candidate` rows (exact normalised email/phone/website, same email
domain with a similar name, or a similar name) and merges nothing; rerunning
it updates the same rows and drops pairs that stopped matching. `dedupe merge
ID survivor=N` folds the other record into `survivor` in one transaction:
every reference field naming the loser is re-pointed, empty survivor fields
are filled, the loser is soft-deleted with `merged_into_id`, and the old id
answers 301 to the survivor. Refused across organizations, onto itself, or
when the loser has issued invoices/bills in a currency the survivor's issued
documents do not use. `dedupe dismiss ID` closes a proposal. Arguments are
`key=value`; `--stage dedupe merge` proposes the merge for approval.

## Project approval and profitability

Read `describe project_time` and `describe client_project` before entering work.
Use `act project_time ID approve` to freeze the billable amount and actual
labour cost from that project's rate, then `act client_project ID bill
date=YYYY-MM-DD` to invoice approved unbilled time and billable costs. These
are generic actions, available for staging under the ordinary policy. Finance
or organization administration is required. Generic edits cannot approve
time, rewrite frozen evidence or remove billing allocations.

`report project_margin --from YYYY-MM-DD --to YYYY-MM-DD` distinguishes
budget, billed allocations, approved unbilled work and recorded actual cost;
unknown historical cost suppresses total cost/profit rather than assuming zero.
This is management profitability, not cash received or net statutory revenue
after credits/refunds. Check the report's source IDs and period basis.

## Structured calls

Use the generated `log_call` action on `company`, `contact`, `lead`, or a
planned call `activity`; inspect `describe TYPE` for the typed parameters.
Actual occurrence, direction, duration and structured outcome belong to the
historical call. Free-text outcome remains narrative. An optional followup
is created in the same transaction. A verified CRM relation is required.
External source/ID pairs provide replay identity for adapters; changed
payloads conflict rather than adding duplicate history. `report calls` counts
historical calls once; `activity_churn` remains the existing financial metric.
Sales handoff uses `act quote ID handoff` or `act deal ID handoff` with name,
owner and scope. `client_project` actions `plan_work`, `change_scope` and
`manage` retain agreement and delivery decisions. `project_deliverable`
actions `accept` and `bill` require finished work and retained acceptance.
Inspect the generated schemas first. Request keys deduplicate planned work;
replaying accepted billing returns its invoice. Fixed-price projects invoice
accepted slices through progress billing; their approved labour is cost
evidence, not a second time-and-materials charge. Full-billed quotes already
have an invoice. Generic writes cannot replace or remove delivery evidence.

## Attachments and local OCR

Document file paths are service-owned. Generic create/import/update cannot
assign or replace `document.path` or move a filed attachment to another
organization. Use the existing upload or mail-filing workflow; valid legacy
originals remain readable, but conflicting ownership and symlinks are refused.

With OCR explicitly enabled, `act document ID ocr_extract language=eng`
queues bounded work. `act ocr_job ID step` processes one page; `retry` and
`cancel` retain provenance. `act capture_item 0 ocr_extract_all
organization_id=N limit=25 after_id=N` freezes a bounded batch, advanced by
`act ocr_batch ID step`. Review with `act document ID ocr_review job_id=N
text=...`; `--stage` keeps the document version so newer corrections conflict.
Extraction is not accounting approval. See `docs/ocr.org` for dependencies,
limits and failure diagnostics.
### Share a Stripe invoice link and reconcile a payment

With the organization's Stripe connection configured and `server.base_url` set
to the trusted HTTPS origin, `act invoice ID payment_link` returns a one-time
`url`. Copy it from that result: ordinary `get stripe_payment_link ID` omits the
bearer URL. The default expiry is seven days; `expires_at=...` accepts a datetime
from one hour through thirty days ahead. The capability binds that invoice
revision, organization, account and expiry.

`act stripe_payment_link ID revoke` disables the resolver and expires an open
provider session. Processing ACH remains pending; revoking a link cannot cancel
an already initiated bank debit. An uncertain provider response keeps the
attempt blocked until reconciled, so do not create another payment by guessing.

`act stripe_event ID retry` replays only retained verified evidence after a local
posting failure. For a manual/partial payment received while ACH was pending,
`act stripe_event ID retry accept_balance_change=true` explicitly permits the
original provider amount to allocate with excess as customer credit. Organization
finance authorization, period guards and second-actor accounting approval apply.
The action cannot alter the event's amount, currency, account or effective date.
### Organization SMTP accounts

Outbound `mail test`, `mail send` and `mail deliver` use the explicit business
organization's SMTP binding. Missing configuration never falls back to
installation credentials. An organization owner/admin configures it at
`/organizations/ID/settings/mail`; the operator must first permit the relay
in `mail.allowed_endpoints`. Password inputs are write-only.

A delivery retains `connection_id` and `connection_version` before SMTP.
Retry can use rotated credentials for the same connection, but replacing an
account does not move old attempts to it. Inspect `last_error` on a `dead`
row and make a deliberate retry/new-message decision. Uncertain acceptance
still must never be retried automatically. The settings page's test sends
only its selected test message and shows retained delivery evidence.

### Organization bank feeds

Bank-feed credentials belong to the selected `bank_connection` organization.
Open its Settings link on `/bankfeed` to configure or rotate the write-only
provider settings, then use **Sync and test** to import the last 30 days of
statement evidence. This is a real sync, not a dry run. The existing
`bankfeed sync ID` command uses the same current binding and import service.
`VENTURE_BANKFEED_TELLER_KEY` is ignored; an administrator must configure an
explicit connection. A saved connection's provider, account and organization
cannot be reassigned. Create a new connection for a different identity.

### Market data feeds

A `data_source` is a provider (`http_json`, `csv`, `file_jsonl`, `push`, or
one a plugin registered) plus YAML `settings`, a `schedule` (`auto` -- also what
empty means: learn when each venue updates -- `hourly`, `manual` or five
cron fields), `enabled` and `track` (`all` or `known`, below). The module
is off unless the operator sets `feeds.enabled: true`. With it off every
`/api/v1/feeds` route is NOT_FOUND, so the `feeds` verbs exit 3; the
`market` and `arbitrage` verbs and the market reports still answer, with
`available: false` or no rows and a note saying feeds are off -- not an
error -- and evaluating an alert rule is refused. Creating or changing a
source needs an administrator: it decides which outside host the server
calls.

- Credentials never go in `settings`: the save refuses a setting the
  provider marks sensitive (`token`, `api_key`). Set them on
  `/feeds/ID/credentials`; templates use `{secret:token}`.
- An address must be on `feeds.allowed_origins` and a file under
  `feeds.file_roots`, both operator settings; a run says so when not.
- A sync never waits. `feeds sync ID --wait` is the CLI waiting, not the
  server, and it gives up after five minutes with exit 7 (the sync stays
  queued; `feeds runs ID` shows it later). `act data_source ID test
  [unit=U]` fetches one unit and reports what came back, writing nothing;
  `act data_source ID purge_history` (administrator) deletes the source's
  stored history and cannot be undone.
- `data_source_run` is written by the server only; creating or updating
  one is refused (403).
- A `push` source is filled from outside: `venturectl feeds push ID FILE`
  (or `-` for standard input) POSTs JSON lines to
  `/api/v1/feeds/ID/push` (`Content-Type: application/x-ndjson`). It is
  never scheduled and a `feeds sync` of it fails its run. Pushing needs an
  editor (like a sync); a source whose provider is not `push`, that is
  switched off, or that already has four pushes not yet stored is refused
  (exit 4 -- wait for those runs, then push again); feeds off is exit 3; a
  body past `feeds.max_push_mb` (32 MiB) is refused. Every push is a run of
  its own with trigger `push`. With `--wait`, a `failed` run exits 1 and an
  unwaited answer (too slow, or two other pushes already being waited on)
  exits 7 with the push still queued.
- A refused `balance`, `holding`, `position` or `inbound` line (too many
  decimal places, the wrong currency) takes that kind out of its account's
  `account_snapshot`: what the store had of it is kept, not swept, and
  the run's notes say so. Fix the line and push again.
- The account-operations lines (`account`, `account_snapshot`, `balance`,
  `holding`, `position`, `inbound`, `txn`; docs/plugins.org) refuse an
  unknown member, and their money is in the **data source's** currency:
  set the source's `currency` (a `balance` in another is refused and
  noted). An `account_snapshot` replaces the covered kinds of that one
  account and must come before its rows. Ledger `txn` rows upsert on `id`:
  re-sending is safe. This data lands in the source's store, not in
  records -- no `list` verb reads it, except what the mirror below copies.
- After every run the marketdata module **mirrors** a source's accounts
  into `location` records (kind `character`/`shared`/`guild`/`other`,
  inside a `group` location per realm) and its open positions into
  `listing` records (`list listing data_source_id=ID outcome=open`; their
  `external_id` is `<source uuid>:<position id>`). A gone position is closed
  from the ledger: sold, partial, expired, or cancelled after
  `mirror_grace_hours` (default 48) with no ledger row. The run's `notes`
  say what it did, including positions "not mirrored: their items have no
  product" -- link the instrument (`update instrument ID product_id=N`) or
  set `create_products: true` and `products_venture_id: N` in the source's
  settings -- and "their items' products are deleted" (restore the product
  or link another; the mirror never makes a replacement). Never `update listing ID data_source_id=` or `mirror_state=`:
  refused. Editing a mirrored listing's price or outcome is fine and
  sticks; an outcome you set makes the listing yours. Source settings:
  `mirror_positions`, `auto_promote_accounts` (both default true),
  `account_namespace` (share places between two sources of the same
  characters), `mirror_max_writes` (500; every record a pass writes,
  the instruments and venues it promotes included), `mirror_grace_hours`.
- The data lands in a series store per source, not in records, so `list`
  cannot read it: the `market` verbs and the market reports do.
- `backup run SCHEDULE_ID` on an installation schedule also copies every
  store (unless `series.include_in_backup` is false), but answers before
  the copies finish: each is its own `backup_run` with `scope` `series`,
  `parent_run_id` (the installation run) and `store_uuid` (the source's
  UUID), `running` until the worker answers. Check them with
  `venturectl list backup_run scope=series`; a `failed` one says why and
  did not fail the database copy. Restoring one is a manual, server-stopped
  file copy (`docs/backup.org`, "Restoring a store").
- Plugins add providers: `blizzard_auctions` (World of Warcraft auction
  houses; optional plugin, needs a registered `GOLD` with exponent 4 and
  `client_secret` on the credentials page), `odds_api` (bookmakers' odds;
  `units` are sport keys, `api_key` on the credentials page) and
  `supplier_csv` (an exec plugin, see the traps). Read a source's provider
  with `get data_source ID` before guessing its settings.
- A `blizzard_auctions` source has one more action:
  `act data_source ID import_recipes max_recipes=50 create_products=true venture_id=N`
  reads Battle.net's professions into `recipe` records; the `result` ends
  with `cursor=P/T/O` when more remain -- pass it back as `cursor=...`.
  Without `create_products` recipes whose items have no product are
  skipped, and say which.

Traps across feeds, market and arbitrage, in the order they bite:

- **Which organization.** The `feeds` and `market` verbs and `arbitrage
  scan|record|export` answer for the **active organization** -- with a
  token, always the default one -- unless told `organization_id=N`. It is
  an option like any other on `market browse|deals|venues|instrument|
  alerts|quote` and the arbitrage scan verbs, and a trailing word after
  the positional ones elsewhere: `feeds sync ID --wait organization_id=N`,
  `feeds runs ID organization_id=N`, `feeds due organization_id=N`,
  `market promote SOURCE venue KEY organization_id=N`, `market watchlist
  [ID] organization_id=N`, `market alerts evaluate RULE_ID [--dry-run]
  organization_id=N` (anything else there is exit 2). A member of that
  organization is answered; anybody else is NOT_FOUND (exit 3), as for its
  records. A source, list, rule or scan row of another organization than
  the one asked about is NOT_FOUND too -- so a second organization's
  source asked about without `organization_id` reads exactly like a
  missing one ("No such data source in organization 1"). `organization_id=two`
  is exit 2. `arbitrage calc` and `registries` read no organization, and
  `arbitrage close|reopen|abandon|execute` act on a record that already
  has one: `organization_id=` there is "Unknown action parameter" (exit 2).
  The demo seeds its second organization exactly this way, then `update`s
  what the store does not say (fees, a location, the product).
- **Wire names use underscores**, and `describe` is the truth:
  `data_source_id`, `group_key` (not `group`), `venue_namespace`,
  `instrument_namespace`, `fee_model`, `fee_params`, `transfer_cost`,
  `threshold_number`, `cooldown_minutes`, `buy_venues`, `trade_id`. A
  dashed key in a body is ignored field by field. (The `market` verbs take
  the *pages'* query names `source`/`group`/`category`, and the report
  spellings too.)
- **Money names its currency.** A bound on a scan or a deal --
  `min_value="10.00 GOLD"`, `min_profit=`, `max_capital=`, `total_stake=`
  -- is refused bare, never read as dollars. The calculators are the
  other way round: `--stake 100`, `fixed=` and `transfer=` and a flip's
  buy and sell prices are read in the install's default currency, so name
  the currency there too. A leg's bare `amount=100` is read in its venue's
  currency (the book currency when the venue names none). Ratios are plain
  percent strings (`min_roi=15`, `max_pct=80`); `min_confidence` is a 0-1
  fraction.
- **`series:` is a price source, not a record source.**
  `price_source=series:min@realm-a` prices from the stores; an
  observation's `source` may not start with `series:`; a number basis
  (`quantity`, `sale_rate`, `sold_per_day`) is refused as a price.
- **`track: known`** (`create data_source ... track=known`) stores only
  the instruments in the settings' `instruments` list and the `instrument`
  records filed under the source -- promote or create the instrument
  first, or a feed run stores nothing for it.
- **Exec plugins need `plugins.allow_exec: true`** (operator config,
  default off): with it off an exec plugin does not load at all -- it is
  missing from `plugins list` and its provider name is unknown -- and a
  source frozen while it was off fails its sync saying so. Nothing from
  the CLI turns it on.

### Market data records and the price oracle

The `marketdata` module (on by default; needs `market`) has `venue`,
`instrument`, `watchlist` and `watchlist_entry`, all generic records. An
instrument names its store row (`data_source_id` + `key`) and the
`product_id` it is; that link is what lets reports price a product from feeds.

- `market promote SOURCE_ID instrument|venue|account KEY` is how a store's
  row becomes a record (it prints the record; its id is `.id`); an account
  becomes a `location`, and the venue it trades on gets that location as
  its `location_id` when it had none. Run it twice and you get the same
  record; a deleted one is restored (automatic promotion after a run never
  restores). Do not `create
  instrument` with a key a record already has: `external_ref`
  (namespace:key) is unique per organization *including deleted rows*, so
  the save is refused -- restore or promote instead.
- `market quote` never converts currencies. `currency=EUR` with a figure in
  USD is `found: false` and the evidence's `note` says why. Nothing observed
  is `found: false`, not an error; read `evidence.note`.
- Without `venue=`, a group-wide basis (`region_*`, `market`, the 14/60-day
  figures) uses the store's only group; a store with several groups is
  refused -- name one (`venue=eu`). `venue=` is a venue key when the store
  knows one by that name, otherwise a group.
- `fallback=true` lets the newest `price_observation` answer when no store
  does (prices only); the evidence's `origin` is then `observation`.
- `recipe_margin` and `goal_materials` take `price_source=series:<basis>[@<venue or group>]`,
  e.g. `price_source=series:min@realm-a`. It needs `marketdata` and `feeds`
  on and never falls back to observations.
- A `listing` may name its `venue_id`; refused while marketdata is off.

Alert rules (`alert_rule`) and their hits (`alert_hit`) are generic records too.
Run `describe alert_rule` for the kind nicks; each kind takes only its own
thresholds, and the others must be left empty or the save is refused:

- `below`/`above`/`spread` take `threshold` (money, e.g. `"1.20 USD"`);
  `pct_vs_reference` (a percent: 80 fires 20% under), `shortage` (units) and
  `spike` (a percent change, negative for a drop) take `threshold_number`;
  `spike` also needs `window_hours` (1-336); `entry_match` takes `pattern`
  (plain text, not a regex); `out_of_stock`, `back_in_stock`,
  `undercut` and `collect_ready` take none.
- The operator's-account kinds read a store's accounts, positions and
  mail: `position_expiring` and `inbound_expiring` take
  `threshold_number` = **hours** ahead (0 < n <= 720, fractions fine:
  2 fires on what lapses within two hours); `account_stale` takes
  `threshold_number` = **days** unseen (0 < n <= 365; shared and guild
  accounts are never judged); `collect_ready` (mail with money or goods,
  or positions expired awaiting login) takes nothing. Their hits carry
  `account_key`; `group_key` narrows by the *account's* group (a realm).
- Every market kind but `undercut` and `entry_match` needs a scope:
  `watchlist_id`, `instrument_id` or `category_id`. The two expiring
  kinds may take one (it narrows by instrument); `account_stale` and
  `collect_ready` refuse one.
  `venue_id` or `group_key` narrows, never both. `enabled` starts true
  and `cooldown_minutes` 60.
- **The two ways to evaluate a rule default opposite ways.** `act
  alert_rule ID evaluate` only looks; `record=true` writes. `market alerts
  evaluate RULE_ID` **writes** the hits (the cooldown applies, a webhook
  and the inbox fire); `--dry-run` only looks. Neither is stageable
  (`--stage` is refused) and both are refused with feeds off. Hits are
  written only by the server -- creating or editing an `alert_hit` is
  refused (403); read them with `list alert_hit rule_id=N`.

The Trading pages have JSON twins, and three reports read the same answers:

- `report market_deals [PERIOD] [data_source_id=N] [venue=KEY] [group_key=G]
  [category_path=PATH] [min_value="10.00 GOLD"] [max_pct=80] [top=N]` --
  instruments in stock at or under their group's deal price, cheapest against
  the region first. `min_value` is compared in its own currency only;
  `max_pct` is a percent of the region median; `top` is 1-500 (50). There is
  no `limit` option.
- `report venue_index [data_source_id=N] [group_key=G]` -- per venue: shares
  cheaper/equal/dearer than the region, price to region, listings, last
  snapshot, the learned update interval and the data's age.
- `report watchlist watchlist_id=N` -- the list priced now against its
  targets; a target in another currency is not compared.
- The period does not narrow any of them: they read the stores as they are.
- The pages themselves have verbs: `market browse|deals|venues|instrument|
  watchlist|alerts` (table above). They take the **page's** query names --
  `source`, `group`, `category` -- and also the report spellings
  `data_source_id`, `group_key`, `category_path`. Each option is typed
  before anything is sent: `page=two`, `stock=maybe`, `dir=sideways`, an
  unknown name (`limit=5`) are exit 2. `browse` sorts only by
  `min_price, quantity, market_value, region_median, pct_vs_region,
  sale_rate, sold_per_day, deal_price, listings, name, updated, venue`
  (anything else is a 400, exit 2); `per_page` is at most 200 (50).
- `market instrument SOURCE_ID KEY`: KEY is the store key (`herb`,
  `2589:b1234`), not an instrument record id; a key with `/` is fine.
- Tables show money with its currency and leave a missing figure blank,
  then the answer's notes (`-q` drops them); `-f json` is the whole answer
  (notes, echoes), `-f csv` the rows.

### The operator's accounts: where to log in, inventory, trading P&L

A data source that reports the operator's own accounts (a `push` source fed
by tsmctl, say) has four pages under Trading > Your accounts, each with a
JSON twin and an `accounts` verb (table above):

- **Where to log in next** is `accounts attention`: one row per realm (an
  account's group; a shared bank is its own place), most urgent first --
  listings already expired, listings and mail about to expire, then money
  or items waiting in the mail, then accounts not seen in `stale_days`
  (14). Thresholds: `expiring_hours` (12, 1-720), `mail_days` (3, 1-60),
  `stale_days` (14). Each row has `title` ("Log in to Thorium
  Brotherhood"), `severity` (`overdue`, `soon`, `waiting`, `stale`) and
  `reasons[]` with `account_name`, `kind` and `text`.
- **Valuation** (`basis=`): `conservative` (default: the lower of the
  region's sale average and the realm's market value), `market`, `min`,
  `historical`, `region_market`, `region_sale_avg`. Anything else is exit 2.
  Unpriced items count for nothing and are noted. `min_value` must name the
  **source's** currency (`"100.00 GOLD"`); another currency or none is
  exit 2. Game currencies (`place=currency`) are left out of the inventory
  unless asked for by place.
- **Dead stock** is `accounts inventory dead=true dead_days=N`: items with
  no sale in the ledger for N days (30).
- **Trading P&L** is the source's ledger, not the books: `accounts pnl
  period=last_90_days group_by=week|month|account|instrument|venue|source`.
  Sales are after the venue's cut; `net` = sales + income - purchases -
  expenses, per currency, integers. Flips match buys to later sales FIFO;
  `open_units` is bought and not yet sold, `held` what the accounts hold now.
- Reports: `report accounts`, `report account_holdings` (**not**
  `holdings`, which is the ledger's report of what each location holds in
  the books) and `report external_pnl` (the period is its window), with
  options `data_source_id`, `group_key`, `basis`, `expiring_hours`,
  `mail_days`, `stale_days`, `sort` / `account_key`, `place`,
  `category_path`, `min_value`, `dead_days`, `top` / `group_by`,
  `account_key`, `venue`, `instrument`, `source`.
  `report listing_performance group_by=location` gives a sale rate per
  character (each mirrored account is a location).
- Dashboard kinds: `accounts_attention`, `accounts_summary`,
  `holdings_value`, `external_pnl`; the `operations` template puts them
  together (`dashboard create operations organization_id=N` -- file it
  under the organization whose accounts it shows, or it opens in the
  default one and shows nothing).
- **Getting accounts in.** A `push` source takes what tsmctl sends
  (`feeds push ID FILE --wait organization_id=N`); the optional `tsmctl`
  exec provider (plugins/exec/tsmctl, needs `plugins.allow_exec`) pulls
  the same lines by running `tsmctl export --format venture`, with
  settings `accounts`, `sources`, `market`, `since`, `currency`,
  `include_internal`, `offline`. docs/examples/wow-operations.org is the
  whole setup.
- **Into the books** is opt-in, per source, by its `books` setting:
  `none` (default), `daily` (`accounts post`, or `post_to_books: true` to
  post after every push) or `trades` (`accounts record-flips`). A source
  books one way; the other verb is refused (exit 1) and a dry run works
  in any mode. Always `dry_run=true` first and read `days_by_status` /
  `flips`. `books_from: 2026-09-01` starts the books there.
- Daily journals: sales Cr `trading_sales`, purchases Dr
  `trading_purchases`, other income / expenses, the net through the
  character's purse; an opening from the first balance seen, and
  `trading_capital` (equity) for gold the ledger never showed arriving
  (mail from an alt). A day that changed is reversed and posted again by
  the next `post`; never delete an `external_posting` record (refused).
- Flips: one closed trade per sale, legs at the buyer's and seller's
  places, `external_ref` `<uuid>:<sale>:<n>`; running it again records
  only what is new. Never `update` a recorded flip's `expected`,
  `external_ref` or `data_source_id`: the save is refused (validation),
  because the next run subtracts what `expected` says it took. `report external_books data_source_id=N` shows each
  day: `posted`, `unposted`, `changed`, `left_out` (flips recorded) ...
- Both are financial: an organization's finance member may, an editor
  member may not. They are type-level actions:
  `act external_posting 0 post_ledger data_source_id=N` and
  `act arbitrage_trade 0 record_flips data_source_id=N`.

## Organization sign-in

OIDC configuration and explicit identity linking use the web settings described
in `docs/oidc.org`; existing local passwords, roles and MFA remain authoritative.
Do not create identity/provider records through generic CRUD or infer a local
user from the provider's email. Credential inputs are write-only, and a rotated
or disabled provider invalidates its old sessions. API tokens keep their existing
local authorization behavior; provider sign-in does not mint global authority.

## Organization AI provider settings

Use the organization AI settings page to choose disabled, organization-owned or
explicitly granted platform service separately for chat, coding and embeddings.
No missing or failing private connection falls back to platform AI. Read
`docs/ai-organizations.org` before configuring provider actions; generic record
writes cannot manufacture grants or overwrite service-owned usage evidence.
Platform credentials remain operator-only even in their billing organization.

### Authorize and operate recurring Stripe collection

Use `describe stripe_authorization` and the subscription's actions to inspect the
current contract. `act customer_subscription ID authorize_payment limit='100 USD'`
returns a copy-once hosted Setup URL. The connection and webhook must explicitly
use Stripe API `2024-06-20`. Customer completion plus verified Setup/mandate evidence
activates permission; `act stripe_authorization ID verify` recovers a missed
callback. `act stripe_authorization ID revoke_authorization` stops future charges
without discarding settlement evidence. Changing terms requires fresh permission.

The running server advances bounded due renewals/collections only for enabled,
verified permissions. `act stripe_authorization 0 collect_due organization_id=N
limit=10` is the explicit bounded sweep; optional `now=...` controls scheduling,
never settlement dates. `act invoice ID collect` runs the same collection service.
A pending attempt blocks hosted and automatic alternatives across all accounts.

`act stripe_checkout ID retry_collection` waits the recorded day and stops after
three attempts. Each retry confirms the old provider invoice is cancelled before
creating a new identity. `act stripe_checkout ID cancel_collection` requires
zero-receipt void/delete proof; processing payments cannot be cancelled by guess.
Manual cancellation stops collection until the customer gives fresh permission.
For a lost create response, `act stripe_checkout ID reconcile_collection
provider_invoice_id=in_...` validates original account and opaque correlation,
then permits explicit cancellation or signed-event recovery. Never manufacture
payment evidence with CRUD or treat a successful pay request as settled cash.
## Offline integration master-key maintenance

These are server-binary operator commands, not venturectl actions. Stop the workspace,
then run `venture --config FILE --check-integration-key` with its current private
environment key. Rotate with `venture --config FILE --rotate-integration-key PRIVATE_FILE`;
the new file must be owned, mode 600/400, single-link canonical base64 for 32 bytes.
Update the environment secret, check again, then restart. Retain old keys for old
backups. A lost commit response requires checking both candidates separately while
stopped; never blindly retry rotation. See `docs/integration-key-maintenance.org`.
## Platform workspace lifecycle

`tools/venture-tenantctl` is a local trusted-operator tool, not a venturectl or AI
action. It provisions isolated stopped workspaces and supports status, stop/start,
offline state changes, encrypted export/restore, bounded maintenance upgrades and
retained offboarding. It never provisions Lightsite. Read `docs/tenant-operations.org`
for private password/key files, immutable workspace identity, maintenance locks,
restore quarantine, explicit administrator recovery and the distinction between
offboarding and erasure. Success exits 0; refusals and failures exit 2. Status
and provisioning return JSON; empty restore targets have no persisted-state result.

For hosted offline integration-key checks or rotation, supply `--tenant-reason`
to the server command. The workspace must be stopped; maintenance acquires its
process lease before any hosted operation, does not migrate or start providers,
and cannot be combined with other tenant operation flags.

Sales territories and quotas use the generic record commands. Read
`describe sales_territory`, `describe sales_quota` and `describe lead_routing_rule`
before configuring their organization/team references. `report sales_attainment`
shows captured booked sales, targets and current pipeline by recipient, currency
and quota period. It is not posted accounting revenue. Assignment/credit rows are
service evidence; correct the source deal instead of editing that history.
## Marketing sends

Use `describe marketing_list`, `describe marketing_member`, `describe
marketing_send` and `describe marketing_recipient`. These are ordinary record
commands; consent and delivery evidence are service-owned. An organization
editor records explicit permission with `act contact ID consent_marketing
source="Signed preference form" evidence="Requested marketing email"
evidence_key=FORM_ID occurred_at=2026-09-20T10:00:00Z` (company and lead have
the same action). Use the real evidence timestamp, never a fabricated one.

Create a static list and member rows with exactly one `contact_id`,
`company_id` or `lead_id`; a company means its own primary mailbox. Segments
use `mode=segment target=contact filters='name=Alice'`, with ordinary typed
filters and no organization/history/pagination override. Create a
`marketing_send` referencing that list, then `act marketing_send ID preview`.
Review `list marketing_recipient send_id=ID` and the frozen counts/content
before `act marketing_send ID approve`. Preview is immutable; new copy,
filters or recipients require a new draft. Existing CRM records never imply
permission. Configure HTTPS `server.base_url` before preview.

`act marketing_send ID run limit=100` examines a bounded audience, queues at
most one due recipient and attempts that exact organization-bound outbox
message. Repeat for progress; it does not start an unbounded job. The default
interval is 60 seconds. `pause`, `resume` and `cancel` preserve identities;
uncertain SMTP acceptance blocks progress until deliberately resolved through
the existing outbox retry workflow. Retry may duplicate a delivery and is
never automatic for uncertainty. `report marketing_performance` uses approval
cohorts and current retained outcomes; acceptance is not inbox delivery, and
observed opens/clicks are not proof of reading.

`act marketing_consent ID withdraw` suppresses the address and stops applicable
queued campaigns and sequences. Transactional messages remain independent.
Recipient unsubscribe links are private capabilities sent only in mail; GET
shows confirmation, POST performs an idempotent organization-scoped withdrawal.
Do not request or expose private body/token fields. Record reviewed relay
feedback with `act marketing_recipient ID feedback kind=hard_bounce
source="Reviewed DSN 5.1.1" event_key=DSN_ID occurred_at=TIMESTAMP`;
`temporary_bounce` does not suppress, `complaint` does. A new consent row cannot
clear retained suppression. Tracking requires both organization
`marketing_tracking=true` and send `tracking=true` before preview.

## First-party source attribution

`describe attribution_site` exposes the organization-owned site configuration;
only organization owners/admins may change it.
Set its exact HTTPS `origin`, verified `external_tenant_id`/`external_site_id`,
`lead_form_id`, `consent_policy`, and `active=true`; `campaign_map` maps bounded
UTM labels to same-organization campaign IDs. Lookback/retention default to
30 days. A changed site configuration requires fresh analytics permission.
Use the organization Lightsite form settings page to pair/rotate/disconnect
write-only signing credentials, then name that `connection_id` on the site.
Do not put secrets in generic records or CLI arguments. Site identities cannot
be deleted: deactivate them so existing withdrawal capabilities remain usable.

`report attribution 2026-09 organization_id=1 model=first` groups source
records by source/campaign, measure and currency. `model=last` selects last-touch;
`details=true` lists exact contributing record identities. Read `period_basis`
and `evidence`: new leads, conversions, first applied-cash customers, won deal
value, issued net and applied cash have different dates/denominators. Imported
`cac_*` and `campaigns_*` metrics are the existing reports under the same scope,
not extra attributed revenue. Currency buckets are never added together.
Legacy/current CRM source is explicitly labelled; no touch or consent is inferred.
`venture_id` and `as_of` are refused because partial reconstruction would mislead.

Owners/admins run `act attribution_visitor 0 retention_sweep organization_id=1
limit=100` to redact expired private observations and forget expired capability
hashes. Analytics withdrawal is independent of marketing permission: it stops
tracking and removes visitor linkage, while coarse business acquisition and
separate email-choice evidence remain. Generic CRUD cannot manufacture or remove
that evidence. See `docs/attribution.org` for the versioned signed Lightsite
contract and browser consent methods. No Lightsite provisioning is performed.
### Commerce account ownership

`commerce import '{"organization_id":1,"connector":"shopify"}'` uses that
organization's explicitly configured account. Without `organization_id`, the
API uses the browser organization cookie or the installation default.
An organization owner/admin connects, tests, rotates or disconnects Shopify at
`/organizations/ID/settings/commerce`. Credentials are write-only vault values;
`VENTURE_COMMERCE_SHOPIFY_TOKEN` and `VENTURE_COMMERCE_SHOPIFY_SHOP` are ignored.
The shop must be a canonical `your-shop.myshopify.com` hostname. Configuration
is local; Test connection is the explicit provider request. Disconnect keeps
historical imports but blocks further use. Rotation during a fetch refuses
that response before import.

`commerce_import_link` is immutable identity evidence. Its account namespace
prevents equal provider order/customer IDs from colliding across shops or
organizations; reconnecting the same account recognizes previous imports.
A legacy invoice needs explicit account adoption, never automatic ownership:

```sh
venturectl describe integration_connection
venturectl act integration_connection 7 adopt_commerce_invoice invoice_id=42 'reason=Reviewed original shop order evidence'
venturectl list commerce_import_link
```

The action requires organization integration administration, pins the selected
active binding and leaves historical invoice/settlement amounts unchanged.
Use the account record linked from settings. Generic edits cannot rewrite or
delete import identities. Do not use credentials in CLI arguments.


### Operator backup retention

`tools/venture-tenantctl` (not `venturectl` or an HTTP action) provides
`backup-list`, explicit authenticated `backup-enroll --archive FILE --key-file FILE`,
`retention-plan --days 30`, `retention-execute --plan UUID`,
`retention-recover`, and `backup-retire --copy COPY-UUID`. Each takes the tenant
slug; writes require `--reason`.
Use `--root` before the command. Review the plan's exact registered copy IDs
before execution. Holds block expiry; offboarding starts an additional retention
period. A pending journal requires recovery, which records missing files and
preserves survivors without another unlink. A retained file that has left its
registered path (moved offsite) makes plan and execute refuse with exit 2 naming
its copy id and path; `backup-retire` tombstones that entry as `retired` and
refuses while the file is still present, so nothing live is retired by mistake. Never remove the catalog to bypass
a refusal. Read `docs/backup-retention.org`, including the original-ledger
transfer gap for restoration to a new host. No offsite or erasure claim follows
from local archive deletion.

### Close checklist actions

`act fiscal_period ID open_close [currency=USD]` opens the checked workspace.
Use `list close_task workspace_id=ID`, then `act close_task ID complete
'notes=Finding'` or `waive 'notes=Reason'` for each task. Explain retained
differences with `act close_discrepancy ID explain 'explanation=Evidence'`.
`act close_workspace ID run_checks|sign|complete|reopen` calls the same
service as the close convenience commands; `sign` requires
`role=preparer|reviewer`. Review must come from a different authenticated
account. These transactional actions cannot be staged. Organization finance,
owner or administrator membership is required; signed/closed task evidence
must be reopened before completion or waiver can change it.

A close ties out one currency (the workspace's; the book currency when
omitted). An unmatched bank line in a separate-book, rate-less or memo
currency is left out and named in the `bank_recon` task's notes rather than
refusing; one in a currency with a rate still refuses. Consolidated reports
(`consolidated_*`, group module) default to the parent's book currency and
leave out — naming in a note — a member book with no rate or in a
separate-book/memo currency, instead of refusing.

### Customer retainer actions

`act company ID collect_retainer 'amount=250 USD' liability_account_id=N`
records already-received cash against an active same-organization liability
account. `act customer_retainer ID release 'amount=100 USD'` recognizes earned
income and reduces the remaining liability. These finance-authorized actions
cannot be staged. They do not charge a provider or settle an invoice. Never
record the same cash again as an invoice receipt; a linked retainer remains
separate from invoice-billed project margin.


### HTTP transport refusal

Every server route, including generic record writes, receives the same early body
and connection limits. HTTP 413 means the body exceeded `server.max_request_size_mb`;
503 may mean the aggregate receive budget is full. A parsed incomplete request
can receive 408; an incomplete TLS/header or saturated connection can close
without an HTTP response. Rejected partial bodies never enter record handlers.
Do not blindly retry a write whose response was lost after dispatch: read its
retained identity first. Configure `server.max_buffered_request_mb`,
`server.max_connections` and `server.request_timeout` with the platform budget,
then restart. The timeout bounds reception/idle connections, not synchronous
business execution. See `docs/configuration.org` for gateway responsibilities.

## Receipt printers

- `venturectl printers [list]` lists configured names and the default. State
  is `unknown` until explicitly queried; listing does not probe sockets.
- `venturectl printers status NAME` reads printer and paper status;
  `venturectl printers test NAME` prints a test page. Both require admin/owner.
- `venturectl print payment ID [PRINTER]` or `print invoice ID [PRINTER]`
  prints immediately using the configured default when PRINTER is omitted.
  IDs must be positive integers. Read access to the record is required.
- Printers are configured on the server. Never pass a host/port as a printer
  name. Unknown names and an unconfigured default are refused.
- Printing is not a record write and cannot use `--stage`. A send failure
  may have delivered part of the receipt: inspect paper before retrying.

## User-defined currencies

- `currency` is a record type: `code`, `name`, `kind`
  (`virtual|points|commodity|other`), `exponent` (0–6), `symbol`,
  `symbol_position` (`prefix|suffix`), `denominations` (a JSON **string**),
  `book_treatment` (`valued|separate_book|memo`), `description`. Check with `venturectl describe currency`.
- Creating, editing or deleting one needs the `admin` or `owner` role;
  reading is open. An editor's write is refused with 403.
- `code` and `exponent` cannot change once saved, and a built-in ISO code
  (USD, EUR, JPY, …) cannot be defined. The code is unique install-wide.

```sh
venturectl create currency code=GOLD name=Gold exponent=4 \
  denominations='[{"suffix":"g","units":10000},{"suffix":"s","units":100},{"suffix":"c","units":1}]'
venturectl create sale venture_id=1 gross="12g 34s 56c"
```

- Denominations: largest first, each dividing the one before, the last
  worth exactly 1 minor unit, suffixes without digits/spaces/points.
- A coin amount without a code is read in the field's default currency if
  its coins fit, else in the one currency that has those coins; two
  candidates is refused as ambiguous — add the code.
- A deleted currency still displays its amounts the same way; restore it to
  edit it. Its code stays taken.
- Stripe checkout, tax filing and Shopify import take ISO money only and
  refuse a user-defined currency (even a registered three-letter code).
- Value one in another with an ordinary `exchange_rate`
  (`from_currency=GOLD to_currency=USD rate_numerator=15 rate_denominator=1000`).
- `book_treatment` says what the ledger does with it: `valued` (default;
  converted into the organization's book currency when an `exchange_rate`
  to it exists on the date, else posted as its own balanced journal),
  `separate_book` (always its own journal, never converted) or `memo`
  (never posted). It may change; only later postings follow it. An
  organization's `default_currency` can never be `memo` (refused both
  ways: on the currency record and on the organization).
- Posting follows the rule for every currency, ISO included: once a
  `EUR`→`USD` rate is recorded, a EUR expense in a USD organization posts a
  USD journal whose lines keep the EUR amount. `report trial_balance`
  still shows each separate book as its own balanced section.
- Stock bought in several currencies keeps them apart: an issue posts one
  cost-of-goods-sold pair per currency its FIFO layers cost (each by its
  treatment; memo posts nothing), and `report inventory_valuation` and
  `report inventory` give one row per currency (`currency`, and on the
  valuation `books`: book currency / valued into the book currency /
  separate book / memo, not posted). Their metrics are `valuation` /
  `value` for the book currency and `valuation_<CODE>` / `value_<CODE>`
  for the others. `report inventory` shows the location's path and the
  average unit cost of the units carrying each currency.
- A manual journal may mix currencies through `act journal 0
  create_and_post 'journal={...}'`: kept-apart currencies each get a
  journal balanced through the "Currency clearing" equity account.
  `act journal ID post` on a saved draft cannot split, and refuses a line
  the rule keeps apart, saying why. A memo-currency line is refused.
- A purchase order line and a vendor bill line must be in their order's or
  bill's `currency`; the save is refused otherwise.
- **Stock priced in another currency comes in through a purchase order**,
  which is not a record action but its own command (`POST
  /api/v1/purchase_order/:id/:action`). Create the `purchase_order`
  (`status=draft`, `currency=TICKET`, `vendor_id` a supplier company) and
  its `purchase_order_line` (`inventory_item_id`, `quantity`,
  `unit_price="4 TICKET"`) with `create`, then:

  ```sh
  venturectl purchase approve 7
  venturectl purchase send 7
  venturectl purchase receive 7 line_id=12 quantity=2 date=2026-03-02T10:00:00Z
  ```

  The receipt is a FIFO cost layer in the order's currency, journalled
  Inventory against GRNI by its treatment (memo: none). Selling the item
  for gold posts GOLD revenue and a TICKET cost of goods. A purchase order
  is not a payment: a memo currency paid out of a purse is a `holding_txn`
  `kind=spend` (negative), a posted one is the vendor bill.

## Holdings: what each wallet, till or character holds

Module `ledger`. A **holding** is an `account` with `location_id` set (a
purse, a till, a petty-cash tin). It holds every currency. What moves it:

- a `sale` or `expense` whose `cash_account_id` is the holding account
  (sales honour it now, like expenses always did);
- `act session ID post`: each money yield goes into the holding at the
  session's `location_id` (credit: `session_income` control-map account,
  else `<org>:4900` "Session income"); no location, or ledger off → the
  money yield stays unposted (skipped, not refused);
- `act location FROM transfer to_location_id=TO amount="5 TICKET"
  [occurred_at=…] [notes=…]` — one currency per transfer, a bare number is
  in the book currency, both locations in one organization;
- a `holding_txn` by hand — **memo currencies only** (`amount` signed;
  `kind` `adjust` default, `earn` positive, `spend` negative; `transfer`
  refused by hand).

Posted currencies (book, `valued`, `separate_book`) are held as the
account's journal lines; memo currencies as `holding_txn` rows the ledger
writes. **Never create a `holding_txn` in a posted currency** (refused) and
never edit or delete one whose `source_type` is set (refused: change the
sale/expense/session instead). **A holding cannot go below zero at any
moment** — the document that would do it is refused whole ("… holds 12
TICKET on 2026-03-02 and this takes 50 TICKET …") — unless the account has
`allow_negative=true`. It is the balance *on the document's date*, not
today's: a spend back-dated to before the takings it needs is refused, so
record (or date) what came in first.
Accounts with no location are never judged. A location with two holding
accounts makes `transfer` and `session post` refuse; name the account.
One is made on first use (`<org>:holding:<location>`) when there is none.

```sh
venturectl create account code=EVM-1101 name="Aria's purse" kind=asset location_id=12 organization_id=2
venturectl create expense venture_id=3 amount="20 TICKET" cash_account_id=40 description=Prize organization_id=2
venturectl act location 12 transfer to_location_id=13 amount="5 TICKET"
venturectl report holdings all organization_id=2 [location_id=12] [currency=TICKET] [as_of=DATE]
```

`report holdings`: one row per location path **and currency**, book
currency first: `location`, `currency`, `books` (how it reaches the books,
e.g. `Memo: TICKET (counted here, never posted)`), `earned`, `spent`,
`transfers` (net), `net`, `balance`. Metrics `held` (book currency) and
`held_<CODE>`. `location_id` includes every location beneath it.

## Categories, locations and tags

- `category` (always on): `name`, `parent_id`, `applies_to` (a record type
  name such as `product`, or blank for any), `position`, `color`,
  `description`. `location` (sales): `name`, `parent_id`, `kind` (free
  text: warehouse, bin, character, bank), `description`, `active` — set
  `active=true` yourself; a new one is inactive otherwise.
- Build a tree top-down: create the parent, read its id, then the child
  with `parent_id=`. A child's `applies_to` must equal its parent's.
- Refused (exit 8): a loop ("would close a loop"), a parent in another
  organization, more than 32 levels, an `applies_to` that is not a record
  type, a parent grouping a different type, and changing `applies_to` on a
  category that has children.
- `product.category_id` and `inventory_item.location_id` are the references;
  the old `category`/`subcategory`/`location` text fields still exist and
  are not kept in step. A product cannot take a category whose tree groups
  another type ("groups expense records, not product").
- Paths ("Materials / Herbs") are computed, not a field: there is nothing
  to `get` or `update`. Rename the parent and every path follows.
- `tags` on `product`, `inventory_item` and `sale` is one comma-separated
  string, found by `search=`.

```sh
venturectl create category name=Materials applies_to=product
venturectl create category name=Herbs parent_id=1 applies_to=product
venturectl update product 12 category_id=2 tags=herb,farmable
venturectl create location name="Alt 1" kind=character active=true
venturectl list product search=farmable
```

- Custom fields gain `kind=double` and `kind=reference`; a reference names
  its target in options: `venturectl fields define record_type=sale
  name=shelf kind=reference options='{"target":"category"}'`. Its values are
  record ids, checked like a built-in reference (missing or deleted target
  refused when written, a kept value left alone).

## Aggregating any record type

`report aggregate` sums, averages, counts and takes the minimum or maximum
of any field of any business record type, grouped and bucketed. Reach for
it before totalling `list` output yourself: it adds money per currency and
rounds an average half to even, which a sum in a script does not.

```sh
venturectl report aggregate PERIOD type=TYPE [measure=FIELD|count] \
    [aggregate=sum|avg|min|max|count|count_distinct] [group_by=F1,F2,F3] \
    [category_depth=N] [date_field=FIELD] [bucket=day|week|month|quarter|year] \
    ['filter=QUERY'] [per=hour|day] [organization_id=ID] [venture_id=ID]
```

- Options go after the period, as `key=value`; the period comes first
  (`2026`, `2026-03`, `last_30_days`, `all`). Quote a `filter` that holds
  `&`: `'filter=status__not_in=done,cancelled&kind=external'`.
- **Name `date_field` or the period bounds nothing.** Without it every
  matching record is counted and the result's `notes` say so. It must be a
  declared date/time field (`describe TYPE`), or `created_at`/`updated_at`.
- `measure` defaults to `count` (the records). `aggregate` defaults to
  `sum` for a money/integer/double measure, `count` otherwise.
  `sum`/`avg`/`min`/`max` of a non-numeric field is refused.
- **Money comes back one row per currency** (`currency` column), never
  mixed and never converted. Read `value` as a money object
  (`amount` in minor units, `currency`, `exponent`, `formatted`).
- `group_by` takes up to three fields, wire spelling: plain, enum (shown by
  label), reference (shown by name), custom field by its name, and
  `reference.field` to follow one reference —
  `group_by=product_id.category_id`. Category and location groups show the
  path ("Materials / Herbs"); `category_depth=0` rolls them up to the top.
- Buckets are UTC calendar buckets (`2026-03`, `2026-Q1`, `2026-W09`).
  `per=day` adds a `rate` column: the value over the days elapsed in the
  row's window (bucket or period, clipped to now); only for `sum`/`count`,
  and refused for `all` without a bucket.
- Refused before any row is read: an unknown type or field (exit 3 or 2), a
  type whose module is off, personal or platform types (`user`,
  `api_token`, chat, webhooks — exit 5), sensitive fields anywhere, more
  than three groups, a filter with `limit`/`offset`/`page`/`order`, and a
  question matching more than 20 000 records (narrow it; it is never
  silently truncated).
- `-f csv report aggregate …` exports the table. The same options work as
  query parameters on `GET /api/v1/reports/aggregate` and as arguments to
  the assistant's and MCP's `venture_report` tool.

```sh
# Sales gross by product category per month this year
venturectl report aggregate 2026 type=sale measure=gross \
    group_by=product_id.category_id date_field=occurred_at bucket=month
# Tickets by status, all time
venturectl report aggregate all type=ticket group_by=status
# Average sale per channel last month, with a daily rate of the total
venturectl report aggregate last_month type=sale measure=gross \
    group_by=channel date_field=occurred_at per=day
```

Dashboards have the same arithmetic as widgets: `sum` (a money/number
`field` over a `filter`, with `period` bounded by
`options={"date_field":"occurred_at"}`) and `progress` (`field` against
`options={"target_field":"budget"}`, of one `record_id` or summed over a
filter). Their fields are checked when the widget is saved.

`report`, `chart` and `metric` widgets run their report in the page's
organization (the one picked, or the one the dashboard is filed under) --
never put `organization_id` or `venture_id` in their `options`, the save
refuses it. Their `options` pass the report's **declared** parameters
only (what `GET /api/v1/reports` lists for that report), type-checked at
the save; `tiles`/`table` are the report kind's own switches:

```sh
venturectl create dashboard_widget dashboard_id=4 kind=report \
    report_name=holdings period=all options='{"currency": "TICKET", "tiles": false}'
```

## Market: price observations and listings

Module `market` (requires `sales`). Check `venturectl describe listing`
and `describe price_observation` for the wire names and the enum.

- `price_observation`: `product_id`, `source` (free text, matched
  **exactly** everywhere: `market value` ≠ `Market value`), `price`
  (money, not negative), `volume` (optional integer), `observed_at`,
  `location_id`, `notes`. Nothing here moves money.
- `listing`: `product_id`, `inventory_item_id`, `channel`, `quantity`
  (≥ 1), `quantity_sold`, `unit_price`, `deposit`, `fees`, `listed_at`
  (required), `closed_at`, `outcome` (`open` default, `sold`, `partial`,
  `expired`, `cancelled`), `sale_id`, `tags`, `notes`, `venue_id`,
  `expires_at`, `location_id` (the account or place that posted it),
  `bid` (per unit; not above `unit_price` unless that is 0), `external_id`
  (unique per organization, deleted listings included). `data_source_id`
  and `mirror_state` are the position mirror's; writing them is refused. A
  listing creates no sale and moves no stock.
- The save refuses a contradiction instead of guessing: `sold` with some
  but not all units counted (use `partial`), `partial` with none or all,
  `expired`/`cancelled` with units sold, a deposit or fee in another
  currency than `unit_price`, `closed_at` on a listing that was always
  open, `closed_at` or `expires_at` before `listed_at`, a `bid` in another
  currency or above the price, and an `external_id` another listing holds
  (all exit 2, validation).
- It fills in: `outcome=sold` with `quantity_sold` 0 becomes all units;
  an ended listing with no `closed_at` gets now; moving back to `open`
  clears `closed_at`. A given `closed_at` is never overwritten.

```sh
venturectl create listing product_id=12 channel="auction house" quantity=20 \
    unit_price="0.1500 GOLD" deposit="0.0120 GOLD" listed_at=2026-03-02T10:00:00Z
venturectl update listing 7 outcome=partial quantity_sold=12
venturectl update listing 8 outcome=sold          # quantity_sold filled for you
```

Reports:

- `listing_performance` — `group_by=product|category|channel`,
  `category_depth` (category only), `venture_id`, `organization_id`; the
  period bounds `listed_at`. One row per group **and currency**.
  `sale_rate` = units sold on *closed* listings ÷ units on closed listings:
  open listings are left out, partial counts its sold units, cancelled
  counts as unsold, and a group with nothing closed has an empty rate (not
  0%). `units_closed` is the denominator.
- `price_history` — `product_id` (required; another organization's
  product is exit 3, not found), `source`, `bucket=day|week|month` (UTC,
  ISO weeks). Columns `min`, `avg` (half to even, not volume-weighted),
  `max`, `volume`, `observations`, per bucket, source and currency.

```sh
venturectl report listing_performance this_month group_by=channel
venturectl report listing_performance 2026 group_by=category category_depth=0
venturectl report price_history last_30_days product_id=12 source="market value" bucket=week
```

## Production: recipes and crafting

Module `production` (requires `sales`; suggests `market` and `goods`).
Check `venturectl describe recipe`, `describe recipe_component` and
`-f json describe recipe` (for the `craft` action's parameters).

- `recipe`: `name`, `venture_id`, `output_product_id` (required),
  `output_quantity` (batch size, ≥ 1), `category_id`, `active`, `notes`.
  **Pass `active=true`**: a boolean has no default through the API, and an
  inactive recipe has no Craft, refuses the action and is left out of
  `recipe_margin`.
- `recipe_component`: `recipe_id`, `product_id`, `quantity` (≥ 1),
  `reusable`, `notes`. `reusable=true` marks a tool or catalyst: it must be
  on hand but is not used up. Leaving it out means **consumed**.
- The save refuses (exit 2): no output product, a batch below 1, a
  component that is the recipe's own output, a second line for the same
  product (change the existing line's quantity instead), and any
  reference into another organization.

Crafting is the `craft` action on a recipe; `act` types each argument from
the action's schema (`times` and `location_id` are integers):

```sh
venturectl create recipe name="Healing Potion" output_product_id=14 output_quantity=3 active=true
venturectl create recipe_component recipe_id=3 product_id=11 quantity=2
venturectl create recipe_component recipe_id=3 product_id=13 quantity=1 reusable=true
venturectl act recipe 3 craft times=5
venturectl act recipe 3 craft times=5 location_id=2 occurred_at=2026-03-14T18:00:00Z
venturectl --stage act recipe 3 craft times=20    # approval crafts, recounting stock then
venturectl list inventory_txn reference=recipe:3  # everything the recipe made or used
```

- One transaction: every consumed component leaves as a `production`
  inventory transaction (quantity × times), the output arrives as one, and
  the made units carry the consumed FIFO cost exactly. Any refusal writes
  nothing. No journal is posted (inventory to inventory).
- Inputs costed in two currencies (GOLD dust, TICKET tokens) are **not**
  refused: the output gets one set of cost layers per currency sharing a
  lot (`lot_txn_id`), and a later sale of a made unit gives up its share of
  each. Such a transaction has no `unit_cost`; its `notes` say what each
  currency came to.
- Refusals (exit 2) say what to do: `Short of <product>` (unless its item
  allows negative stock), a reusable component not on hand (allow-negative
  does **not** apply to tools), a product kept in several places ("name
  the location_id to use"), no inventory item for the output
  ("create an inventory item for it there (product_id=… location_id=…)"),
  an inactive recipe, no components.
- `location_id` means exactly that location for every component and the
  output, not its children. The craft never creates the output's
  inventory item.
- It returns the output's `inventory_txn`.

Report `recipe_margin` — `price_source` (exact; needs the market module,
refused without it), `currency` (only prices observed in it count; left
out, the book currency's price wins wherever the product was seen in it,
else the newest in any; not a code is refused), `as_of`, `venture_id`,
`category_id` (and everything beneath it), `organization_id`. One row per active recipe: `cost`,
`value`, `profit`, `margin`, `cost_per_unit`, `craftable_now`,
`priced_by`, `note`. Market on: latest observed prices only; a product
never priced is named in `note` and its figures are blank, **not zero**.
Market off: item unit cost (else product cost) for inputs, list price for
the output. Two currencies in one recipe: a note and no money figures.

```sh
venturectl report recipe_margin all price_source="market value"
venturectl report recipe_margin all category_id=4 as_of=2026-03-01
venturectl report recipe_margin all currency=TICKET
```

## Sessions: runs of effort and what they yielded

Module `sessions` (requires only `core`; suggests `sales` and `market`).
Check `venturectl describe session` and `describe session_yield`.

- `session`: `name`, `venture_id`, `activity` (free text, one spelling per
  kind of run — the report groups by the exact string), `category_id`,
  `location_id`, `started_at` (required), `ended_at` (empty = still open),
  `minutes`, `cost` (money), `tags`, `notes`, `posted_at` (technical).
- **`minutes` is derived**: both times set → the difference; only
  `started_at` and `minutes` → `ended_at` is filled in; only `started_at` →
  open, minutes 0. A `minutes` that disagrees with the two times is
  refused (exit 2) — change `ended_at` instead. An end before the start,
  negative minutes and a run over a year are refused. Writing
  `posted_at` by hand is ignored.
- `session_yield`: **goods** (`product_id` + `quantity` ≥ 1, optional
  `unit_value`, optional `inventory_item_id`) **or money** (`amount` > 0),
  never both, never neither. `unit_value` is a valuation, not a cost.
  `inventory_txn_id`, `journal_id` and `holding_txn_id` are set by posting
  only (writing them is refused). Goods need the sales module; money does
  not.
- A **posted** yield (any of those three set) cannot change product,
  quantity, amount, stock or session, and cannot be deleted; nor can a
  session with posted yields. `unit_value` and `notes` stay editable.
  Correct stock with an adjustment, money with an expense or a
  `holding_txn` adjustment.

Posting is the `post` action on a session; it takes no arguments:

```sh
venturectl create session name="Elwynn loop" activity=herbing location_id=2 \
    started_at=2026-03-01T10:00:00Z ended_at=2026-03-01T10:40:00Z cost="0.5000 GOLD"
venturectl create session name="Mine run" activity=mining started_at=2026-03-02T19:00:00Z minutes=55
venturectl create session_yield session_id=8 product_id=11 quantity=38 unit_value="0.1200 GOLD"
venturectl create session_yield session_id=8 amount="12.3400 GOLD"
venturectl act session 8 post
venturectl list inventory_txn reference=session:8
```

- One transaction: each unposted goods yield arrives as a positive
  `production` inventory transaction (reference `session:<id>`, dated the
  session's end) with a **zero-cost** layer, and is stamped with it and
  the stock it landed in; each unposted money yield lands in the holding
  at the session's location (see *Holdings*) as a journal (posted
  currency, `journal_id`) or a holding movement (memo, `holding_txn_id`);
  the session gets `posted_at`. Any refusal writes nothing. **Posting
  counts money yields now**: a session with a location posts them too.
- **Idempotent**: posting again posts only yields added since; nothing
  new is a success that changes nothing. Retrying is safe.
- Stock: the yield's `inventory_item_id`, else the one item for the
  product at exactly the session's `location_id` (anywhere when none).
  Several places → refused, naming them ("name the location_id to use";
  set the session's location or the yield's Stock); none → "No stock of
  <product>" with what to create. Sales off → refused. Stageable.

Report `session_performance` — `group_by` (`activity` default, `category`,
`location`, `venture`), `category_depth` (category/location only),
`price_source` (exact; needs the market module, refused without it),
`currency` (as recipe_margin: which observations count),
`as_of` (value every yield at that date instead of its session's end),
`venture_id`, `organization_id`; the period bounds `started_at`. One row
per group **and currency**: `sessions`, `open`, `hours` (finished
sessions), `units`, `value` (goods), `amount` (money yields), `cost`,
`net`, `value_per_hour`, `net_per_hour` (finished sessions over their
hours), `priced_by`, `note`. **`sessions`/`open`/`hours`/`units` repeat on
each currency row of a group — never sum them down the column.** Goods are
valued at `unit_value`, else the latest observation from `price_source`
(market on) or the list price (market off); goods with no value are named
in `note` and value, net and the rates are blank, **not zero**.

```sh
venturectl report session_performance this_month group_by=activity price_source="market value"
venturectl report session_performance 2026 group_by=category category_depth=0
```

## Arbitrage: trades, legs and the books

Module `arbitrage` (requires `marketdata` and `ledger`; suggests
`production` and `goods`). Check `venturectl describe arbitrage_trade` and
`describe arbitrage_leg`. Financial: an organization **finance** member
records and closes trades; an editor member is refused (exit 5).

- `arbitrage_trade`: `name`, `strategy` (spread, transform, deal, cover,
  back_lay or a plugin's — exact string, the report groups by it),
  `venture_id`, `status` (planned, open, closed, abandoned), `opened_at`,
  `closed_at` (both derived), `expected` (technical JSON, e.g.
  `{"profit":["40 GOLD"]}`), `close_journal_id` (technical), `tags`,
  `notes`. **`closed`/`abandoned` are set only by the `close`/`abandon`
  actions and left only by `reopen`** — writing them is refused (exit 8).
- `arbitrage_leg`: `venue_id`, `trade_id` (required), `kind` (buy, sell,
  fee, transfer, stake, payout, refund; `write_off` is the abandon
  action's), `status` (planned, executed, failed, cancelled — **only
  executed posts**), `instrument_id`, `inventory_item_id` (buy/sell only),
  `quantity`, `unit_price`, `amount`, `fees`, `occurred_at`, `cost` and
  `inventory_txn_id` (technical, set by execute). **A bare amount ("100")
  is read in the venue's currency** (the book currency when the venue
  names none); an amount in another currency than
  the venue's is refused. Negative only on a transfer (arriving). Fees in
  the amount's currency. A unit price × quantity fills an empty amount.
- Posting: saving a leg `status=executed` posts it (Dr arbitrage positions
  / Cr the venue's cash for money out, the reverse for money in, fees to
  arbitrage fees). Saving it unchanged posts nothing; changing an executed
  leg reposts; cancelling it reverses. The venue's cash is its location's
  holding (so the holding floor applies, from the leg's date), else its
  account, else the organization's cash.
- **A leg with `inventory_item_id` is executed only by
  `act arbitrage_leg ID execute`** (setting `status=executed` is refused):
  a buy is received paid from the venue's cash at an exact split of the
  amount, a sell is issued at FIFO cost into the position. **An executed
  stock leg cannot be changed or cancelled** — record a counter-leg.
- An executed leg of a closed or abandoned trade is frozen until `reopen`.

```sh
venturectl act arbitrage_leg 12 execute occurred_at=2026-03-05T10:00:00Z
venturectl act arbitrage_trade 4 close closed_at=2026-03-07T10:00:00Z
venturectl act arbitrage_trade 4 reopen
venturectl act arbitrage_trade 4 abandon goods=write_off      # or goods=keep (default)
venturectl --stage act arbitrage_trade 0 record organization_id=1 name="Peacebloom flip" \
    strategy=spread expected='{"profit":["40 GOLD"]}' \
    legs='[{"kind":"buy","venue_id":3,"amount":"100","status":"executed"},
           {"kind":"sell","venue_id":4,"amount":"150","status":"executed"}]'
```

- `close` moves what is left on the positions account to arbitrage gains,
  **one journal per currency section, never added together**, and is
  refused before the last executed leg's date. `reopen` reverses it.
  `abandon goods=write_off` writes unsold stock off at cost (a `write_off`
  leg) before closing; `goods=keep` leaves it in stock.
- `record` (type-level, stageable) creates the trade and its legs in one
  transaction and executes the legs marked `"status":"executed"` in
  order. A leg naming anything but kind, status, venue_id, location_id,
  instrument_id, inventory_item_id, quantity, unit_price, amount, fees,
  occurred_at and notes is refused. A member must pass `organization_id`.
  `location_id` moves the leg's money through that place's holding (the
  character who bought or sold) instead of the venue's.
- Accounts: control-map classifications `arbitrage_positions`,
  `arbitrage_gains`, `arbitrage_fees`; unmapped they are made as
  `<org>:1460`, `<org>:4960`, `<org>:6960`.

Report `arbitrage_performance` — finished (closed or abandoned) trades in
the period they finished; `group_by` (`strategy` default, `venue_pair`,
`instrument`, `month`), `strategy` (exact), `venture_id`,
`organization_id`. One row per group **and currency**, book currency
first: `trades`, `wins`, `hit_rate`, `realised`, `capital`, `roi`, `fees`,
`avg_hold_days` (group-wide, repeats per row), `expected`, `slippage`
(only trades whose snapshot named that currency).

```sh
venturectl report arbitrage_performance this_month group_by=venue_pair strategy=spread
```

Finding opportunities (read-only until recorded):

- Report `arbitrage_scan` — opportunities **now** (the period is ignored)
  by one `strategy`: `spread` (default; cross-venue flips, `buy_sources=N`
  lists the N cheapest suppliers per item, 1–10), `deal` (under the group's
  deal price), `transform` (a recipe's inputs at their cheapest venues;
  `recipe_id`, `units` = batches), `cover` (surebets; `total_stake`),
  `back_lay`, or a plugin's. Filters: `preset_id`, `data_source_id`,
  `buy_venues`, `sell_venues` (comma-separated **venue keys**),
  `group_key`, `category_path`, `kind`, `instrument`, `units`,
  `sell_basis` (`min`, `market`, `sale_avg`, `region_median`, `bid`),
  `min_profit`, `max_capital`, `total_stake` (**money names its currency**:
  `"10.00 GOLD"`; a bare amount is refused), `min_roi`, `min_sale_rate`,
  `max_buy_pct`, `share` (**percent strings**, `"15"` = 15%),
  `min_confidence` (0–1), `max_age_hours`, `sort` (`profit` default,
  `roi`, `roi_per_day`, `annualized`, `ev`, `confidence`), `top` (1–500).
  **An unknown option is refused by name.** A row with something
  unquoted has blank figures and names it in `missing`/"Unquoted" — never
  zero. Mixed currencies convert only through an `exchange_rate`, else
  the row is skipped and a note says so.
- Report `craft_arbitrage` — per recipe, each input at its cheapest venue
  (a shopping list; a reusable input is bought once), the output's best
  venue and the profit: `recipe_id`, `units`, `data_source_id`,
  `buy_venues`, `sell_venues`, `group_key`, `sell_basis`, `max_age_hours`.
- Presets are `arbitrage_strategy` records (`name`, `strategy`,
  `data_source_id`, `buy_venues`, `sell_venues`, `options` as YAML of the
  other filters); the save refuses a misspelt filter.
- `arbitrage scan STRATEGY [option=value ...]` asks the same question
  through `/api/v1/arbitrage/scan`; the table numbers its rows.
  `arbitrage record N STRATEGY [same options]` records row N: the CLI
  re-asks the question, takes the row's `key` and `narrow`, and the server
  re-runs the scan and performs `record` with the plan — a moved
  opportunity is exit 3 "no longer there", a row number past the end is
  exit 3 too. **Pass exactly the options the scan had**, or row N is a
  different row. `arbitrage record KEY [options]` takes a key from
  `-f json` output instead. `--stage` proposes it (202 + confirmation);
  planning still promotes the legs' venues and instruments.
- `arbitrage scan|record|export` take `organization_id=N` like the
  `market` verbs (see *Which organization* in the traps under *Market data
  feeds*); a row recorded with it is filed there. They **refuse**
  `venture_id`, `as_of`, `key`, `format` and `calc`, which the routes
  would ignore or drop (exit 2). Every other option travels as text; the
  server refuses by name one that neither the scan nor the chosen
  strategy declares, and a plugin strategy's own options get through.
- `cover` and `back_lay` leave out an event that has started (its
  `commence_time` attribute at or before now) and say so in a note; an
  event with no `commence_time` is kept and noted -- bound it with
  `max_age_hours`.
- `arbitrage record name=... legs=JSON|@FILE` is the `record` action with
  typed arguments (`legs=@legs.json` reads a file; `expected=@FILE` too).
  `arbitrage close|reopen|abandon TRADE_ID` and `arbitrage execute LEG_ID`
  are the actions (`closed_at`, `goods=keep|write_off`, `abandoned_at`,
  `occurred_at`); an unknown parameter is exit 2. All five take `--stage`.
- `arbitrage calc surebet 2.10 2.05 --stake "100.00 USD"` (odds as bare
  words; a negative American price would read as a flag, so write
  `odds="+150 -120" format=american`), `calc back-lay BACK LAY --stake X
  [commission=5] [back_commission=0]`, `calc flip BUY SELL [units=N]
  [cut=5] [fixed=AMOUNT] [deposit=15] [refundable=true] [sale_rate=40]
  [sold_per_day=12] [share=100] [transfer=AMOUNT] [transit_hours=0]`.
  Give the stake once (`--stake` or `stake=`); a flip takes none, its
  capital is the buy price. **A bare `--stake 100` is read in the
  install's default currency**, unlike a scan bound -- name it.
  Stakes are rounded to the currency's minor unit; `residual` is what the
  rounding left over. A surebet's `payout` is the **least** rounded payout
  (what any result is sure to pay) and `profit` that less the stakes;
  `payout_ideal`/`profit_ideal` are the unrounded T/S and T(1/S - 1). A
  back-lay's `worst` rounds against you (winnings down, liability up);
  `ideal` is unrounded. Quote the sure figures as what a person gets.
- `arbitrage export csv|shopping_list|<plugin's> [STRATEGY] [options] [-o FILE]`;
  `arbitrage registries` lists strategies, fee models, export formats and
  the scan's option names.
- A venue's `fee_model` (`percent`, `commission`, `none`, or a plugin's)
  and `fee_params` YAML are checked when written: `percent` takes
  `cut_percent`, `fixed_per_unit`, `fixed_per_order`, `min_fee`,
  `deposit_percent`, `deposit_basis`, `deposit_refundable`, `buy:`;
  `commission` takes `rate_percent`.

```sh
venturectl report arbitrage_scan strategy=spread group_key=eu min_profit="10.00 GOLD" min_roi=15 top=20
venturectl report craft_arbitrage recipe_id=4 units=10
venturectl arbitrage scan spread group_key=eu min_profit="10.00 GOLD" min_roi=15 top=20
venturectl arbitrage record 1 spread group_key=eu min_profit="10.00 GOLD" min_roi=15 top=20
venturectl --stage arbitrage record name="Peacebloom flip" strategy=spread legs=@legs.json organization_id=1
venturectl arbitrage execute 12 occurred_at=2026-03-05T10:00:00Z
venturectl arbitrage close 4
venturectl arbitrage calc surebet 2.10 2.05 --stake "100.00 USD"
venturectl arbitrage export shopping_list transform recipe_id=4 units=10 -o list.txt
```

## Plugins

`venturectl plugins list` (owner only; the answer names server paths)
shows each loaded plugin's `name`, `kind`, `runtime` (`native`, `crispy`,
`exec`, `declarative` for a venture-type YAML) and `provides` (`data_source_provider`,
`automation_handler`). A provider an exec plugin registers (only under
`plugins.allow_exec`, see the traps under *Market data feeds*) is an
ordinary `data_source` `provider`. "No plugins are loaded." is an answer,
not an error; `plugins` takes no other word than `list` (exit 2).
Strategies, fee models and export formats plugins add appear in
`arbitrage registries`.

## Goals: targets, steps and the shopping list

Module `goals` (requires only `core`; suggests `production` and `market`).
Check `venturectl describe goal` and `describe goal_step`.

- `goal`: `name`, `venture_id`, `parent_id` (a larger goal; no loops, same
  organization), `category_id`, `metric`, `unit`, `start_value`,
  `current_value`, `target_value` (doubles), `due_on` (date), `status`
  (`active` default, `paused`, `achieved`, `abandoned`), `achieved_at`,
  `tags`, `notes`.
- **`target_value` must differ from `start_value`** (0 to 0 is refused as
  no target). It may be **below** the start (a weight to lose): progress
  is `(current - start) / (target - start)`, never `current / target`.
- `achieved_at` follows `status`: stamped when it becomes `achieved` and
  the date is empty, kept when given, cleared when the status leaves
  `achieved`, refused when typed on a goal that is not achieved.
- **Nothing updates a goal for you.** Reaching the target does not mark it
  achieved; ticking a step does not move `current_value`. Update both
  yourself.
- `goal_step`: `goal_id` (required, same organization), `position`
  (lowest first), `name`, `from_value`/`to_value` (optional; must not run
  opposite to the goal), `recipe_id` (production module only; same
  organization), `repetitions` (**crafts/batches, not units**; ≥ 0, 0 =
  unsaid), `done`, `done_at` (follows `done` like `achieved_at`), `notes`.

```sh
venturectl create goal name="Alchemy 300" metric=level start_value=1 current_value=1 \
    target_value=300 due_on=2026-12-31
venturectl create goal_step goal_id=3 position=1 name="Minor Healing Potion" \
    from_value=1 to_value=25 recipe_id=7 repetitions=20
venturectl update goal_step 11 done=true
venturectl update goal 3 current_value=25
```

Report `goal_progress` — `status` (a nick or several comma separated;
every status by default; unknown refused), `category_id` (and beneath),
`venture_id`, `as_of` (the moment the pace is measured to — it does not
read an old `current_value`), `organization_id`. One row per goal, by path
("Alchemy 300 / Alchemy 150"): `start`, `current`, `target`, `percent`
(a fraction, unclamped), `remaining` (in the goal's direction; negative =
past it), `steps_done`/`steps_total`, `due_on`, `days_left` (negative =
overdue), `forecast` (straight line from `start_value` at creation to
`current_value` at `as_of`; active goals with progress only), `status`,
`note`.

Report `goal_materials` — `goal_id` (with its sub-goals; every active or
paused goal by default), `venture_id`, `price_source` (market module
only), `currency` (as recipe_margin), `include_on_hand` (`true` default / `false`), `as_of`,
`organization_id`. Reads steps **not done** with a recipe and
`repetitions` > 0: consumed components × repetitions summed per product;
**reusable components once, at the largest single step's need** (never
summed); less stock on hand in **every** location; `to_acquire` priced at
the latest observation (market on) or recorded cost (off). One `Total` row
per currency; a product with no price is named and its cost blank —
**not zero** — and the totals become "Total of priced lines". Gross: what
an earlier step makes is not netted. **Refused while production is off.**

```sh
venturectl report goal_progress all status=active,paused
venturectl report goal_materials all goal_id=3 price_source="market value"
venturectl report goal_materials all include_on_hand=false
```

A dashboard `progress` widget on `goal` should set
`options.start_field=start_value` beside `target_field=target_value`, or a
downward goal reads as done before it starts.

## Venture types: typed attributes, enforced at the save

A venture's `venture_type` names a declarative type (a YAML file in
`data/venture-types/`, or a plugin's): `books`, `etsy`, `newsletter`,
`virtual_economy`, `general` ship. `GET /api/v1/venture-types` (or
`/api/v1/venture-types/NAME`) lists each type's fields -- `describe venture`
does **not**, because they live in the venture's attribute bag, not in
columns.

- Write them with `attributes.NAME=value`, in the same command as the rest:
  `venturectl create venture name="Silverfen AH" venture_type=virtual_economy
  attributes.world="Evermoor Online" attributes.marketplace=auction_house`.
  Over REST they are the nested `attributes` object; a PATCH merges, `null`
  removes one. (`fields value record_type=venture record_id=ID name=N
  value=V` PATCHes one attribute too.)
- **The type's rules are checked at the save** (exit 8, validation):
  `required` fields present, `enum` values within `choices`, `integer`/
  `double` text that parses as a number within `min`/`max`. Creating a
  `virtual_economy` venture without `attributes.world` is refused -- so do
  not create it bare and patch the attribute on afterwards.
- **Only what the save writes is checked.** Creating a venture, or changing
  its `venture_type`, checks every declared field; otherwise only the
  attributes whose value changes. A venture stored before a rule existed
  stays editable.
- **An unregistered `venture_type` is refused when written**, naming the
  registered ones -- unless no types are loaded at all (plugins off).
  Keeping an old, since-removed type is allowed.

`virtual_economy` fields: `world` (required), `region`, `currency_code`,
`faction`, `handle`, `marketplace` (`auction_house`, `player_trade`,
`vendor`, `platform_store`, `mixed`). `general` fields: `kind`
(`household`, `hobby`, `club`, `project`, `personal`, `other`), `purpose`,
`members` (integer ≥ 0). `docs/examples/game-economy.org` builds a whole
game economy -- currency, organization, venture, trees, stock, prices,
sessions, recipes, listings, a goal and a dashboard -- with commands that
run top to bottom.

`make demo` seeds that example as a **second organization** ("Evermoor
Trading", book currency `GOLD`); pass `organization_id=ID` to its reports
(`listing_performance`, `session_performance`, `recipe_margin`,
`goal_progress`, `goal_materials`, `aggregate`), or they answer for the
default organization and read nothing.
