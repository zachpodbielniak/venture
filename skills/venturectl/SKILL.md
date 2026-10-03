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
| `dashboard export SLUG` | its definition as JSON; `dashboard import FILE` (or `-`) creates one from it |
| `dashboard create TEMPLATE` | `today`, `factory`, `reporting`, `progress`, `work`, `overview` or `form-results`; `dashboard templates` and `dashboard kinds` list what is accepted |
| `inbox [--all]` | what the token's user has been told: mentions, assignments, watched changes, service levels, budgets, runs; `inbox read ID\|all` marks read |
| `watch TYPE ID` / `unwatch TYPE ID` | follow a record, so changes land in the inbox |
| `activity TYPE ID` | a record's timeline: every change with who and what moved, plus a ticket's comments and worklogs |
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
`--quiet/-q`.

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
`report pnl 2026-03-14` is the fourteenth only.

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

It is refused on any command other than `create`, `update`, `delete`, `act`, `dunning sweep`, `dedupe`, `journal post`, `sequence enroll`, `lead convert` and `billing`,
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
| 7 | network — the server is not there |
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
  `expired`, `cancelled`), `sale_id`, `tags`, `notes`. A listing creates
  no sale and moves no stock.
- The save refuses a contradiction instead of guessing: `sold` with some
  but not all units counted (use `partial`), `partial` with none or all,
  `expired`/`cancelled` with units sold, a deposit or fee in another
  currency than `unit_price`, `closed_at` on a listing that was always
  open, and `closed_at` before `listed_at` (all exit 2, validation).
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

## Forms: questions, responses and the summary

Module `forms` (requires only `core`; suggests `leads`, `mail` and `marketing`). Check
`venturectl describe form`, `describe form_field`, `describe form_submission`.
URL prefill is opt-in per question (`allow_prefill=true`); invalid values are
omitted and final answers revalidate. Personal links use a published
`contact_field=name|email|phone|role|website|address` mapping. Owners can call
`act form ID personal_links contact_ids='[3,8]' origin=https://forms.example days=7`.
The nonstageable result contains private bearer URLs, valid for 1–30 days.
Do not put them in shared notes or logs. `one_per_link`/`one_per_contact` limit
retained bound responses. Consent, sensitive and repeated questions cannot
be prefilled. Marketing sends and email sequence steps accept `survey_form_id`;
links live in private delivery bodies. See `docs/forms.org` for the adapter
contract and expiration behavior.

Question labels/help and page-break headings/intros accept `{earlier_key}`;
`success_message` can refer to any non-repeated question. `{group.count}`
inserts a validated repeat-row count; same-row members can refer to earlier
members. Escape literal braces as `{{` and `}}`. Unknown, forward and
sensitive references are refused at save, including changes that would break
an existing template. Hidden/invalid answers substitute empty text. Piping
never evaluates expressions or HTML. Publish after changing question text.

Double opt-in requires `double_opt_in=true`, `optin_email_field` naming a
required non-sensitive email, `public_origin=https://forms.example`, and a
required `consent` question with `marketing_consent=true`. Optionally set
`optin_list_id` to a static marketing list, then publish. Mail, marketing and
contacts must be enabled. Initial input is a private `form_pending` working
copy; do not create/update it through CRUD. Only the email confirmation POST
creates contact, permission, membership and final response. Links expire in
24 hours; resubmission resends at most three times, 60 seconds apart. Retention
reports `expired_signups`; owner export/erasure includes them. Never log or
paste confirmation capabilities into shared records.

For repeated questions, inspect `describe form_group`, create a group with
`form_id`, stable `key`, `label`, `min_rows` and `max_rows`, then set member
questions' `group_id`. Keep members consecutive on one page and publish.
Maximum 0 means 10; the cap is 50. Public JSON accepts arrays of row objects
under the group key; HTML uses `attendee[0][name]`. Responses and owner
exports retain row arrays, with sensitive members in `sensitive_answers`.
The summary counts rows for repeated questions. Add/remove are private draft
edits, not responses. See `docs/forms.org` for same-row rules and limits.

Use `describe form_rule` before editing conditional questions. A rule has
`form_id`, `action=show|hide|require|jump|end`, `target_key`, and a `conditions`
JSON array of `{field,operator,value}`. Conditions use a closed comparison
vocabulary (see `docs/forms.org`), never expressions. Question targets must
follow their sources; jumps only go forward. Publish freezes rules as well as
questions. Hidden and skipped answers are discarded by the server.

For multiple pages, insert a `form_field kind=page_break` between questions by
`position`; its label/help introduce the next page. Publish refuses empty pages.
Forms can opt into saved drafts with `allow_resume=true`, `public_origin=https://forms.example.com`
and `resume_days=7` (maximum 30). These are ordinary `form` fields. Public Save
issues a private single-use link; Resume loads the newest published version and
asks new required questions and consent again. Do not read or distribute draft
capabilities through generic record tools. `forms` owner export/erasure includes
the optional resume inbox; retention purges expired drafts and cancels queued mail.

`form draft_minutes=60` controls unfinished-response lifetime (0 also means 60).
`form_draft` is service-owned: do not create/update it. Its sensitive answers
are absent from generic reads. Retention sweep removes expired drafts within
its limit and reports `expired_drafts`; owner export/erasure includes them.

- `form`: `name` (internal), `title`/`description`/`submit_label`/
  `success_message` (public), `state` (`draft` default, `live`, `closed`),
  `opens_at` (inclusive), `closes_at` (exclusive), `response_limit`,
  `unique_email_field` (a required email question key; blank permits repeats),
  `redirect_url` (http/https only),
  `allowed_origins` (one `https://host[:port]` per line; empty = any),
  `hourly_limit`, `min_fill_seconds`, `create_lead`, `lead_source`,
  `campaign_id`, `on_duplicate` (`merge`/`create`/`reject`),
  `confirmation_field` (an email question's key), `confirmation_subject`,
  `confirmation_message`. `public_token` and `slug` are made on the first
  save; clearing `public_token` issues a new one (old embeds stop working).
- `form_field` ("Form question"): `form_id`, `key`, `label`, `kind`
  (`short_text` default, `long_text`, `email`, `phone`, `url`, `number`,
  `date`, `single_choice`, `multiple_choice`, `checkbox`, `rating`,
  `hidden`, `consent`), `required`, `position`, `help`, `placeholder`, `choices`,
  `min_value`/`max_value`, `min_length`/`max_length`, `pattern`,
  `default_value`, `maps_to` (`name`, `email`, `phone`, `company_name`,
  `website`, `notes`).
- **`key` is fixed once saved and never reused on that form, even after
  the question is deleted** (its answers keep it). Lowercase letters,
  digits and `_`, starting with a letter.
- **`choices` are lines; each gets a stable id on save** (`Dark blue` →
  `dark_blue | Dark blue`). Answers store the id: relabel freely, but keep
  the id unless you mean a new choice.
- **Nothing is public until published.** `venturectl act form ID publish`
  freezes the questions as the next `form_version`; the public door serves
  that version. Editing questions afterwards changes only the draft until
  you publish again. Versions are read-only. Roll back by setting the
  form's `published_version_id` to an earlier version of the same form.
- `form_submission` ("Form response") **cannot be created with `create`** —
  only the form's public address makes one. Its answers are fixed; you may
  update `reviewed` and `notes`. `answers` is JSON text keyed by question
  key; `summary` is the readable version; `mapping_note` says why a lead or
  confirmation was not made.

```sh
venturectl create form name="Website contact" title="Contact us"
venturectl create form_field form_id=3 key=email label=Email kind=email required=true position=10
venturectl create form_field form_id=3 key=topic label=Topic kind=single_choice \
    choices="Sales
Support" position=20
venturectl update form 3 state=live create_lead=true
venturectl act form 3 publish
venturectl list form_submission form_id=3
venturectl report form_summary all form_id=3
```


Privacy settings and actions:

- `form.privacy_url`, `retention_days`, `retention_action` (`anonymise` or
  `purge`). `venturectl act form 0 sweep_retention organization_id=1 limit=100`
  processes a bounded batch; it is not a timer.
- `form_field.sensitive=true` keeps answers out of generic JSON, pages,
  AI tools, audit content, webhooks and notifications. Sensitive questions
  cannot map to leads or marketing permission, or have a default answer.
- `kind=consent` records the published wording, version and time. It cannot
  have `default_value`. `marketing_consent=true` explicitly maps checked
  permission to the lead captured by `create_lead` and its name/email
  mappings. The marketing module must be enabled; suppression is never
  cleared. Check `mapping_note` for a refused follow-up.
- `venturectl act form 0 erase_person organization_id=1 email=person@example.com`
  is owner-only and erases matching responses, drafts, opt-in copies and files
  across forms in that organization, including soft-deleted rows. Replace
  `email` with `contact_id=42` to use personal-link bindings even after the
  contact loses its email. Queued private mail content is erased; delivery
  identities remain cancelled. Sending/uncertain/sent mail and independent
  CRM, booking and financial records retain their own lifecycle.
- `export_person` is an owner-only access-request export containing sensitive
  answers. It accepts the same email or contact selector; unfinished files
  export metadata rather than bearer claims. It cannot be staged, so generated assistant and MCP action tools
  refuse it. A human owner performs this outside those tools; never put its
  payload into AI context or an approval card.

Public addresses (no session): `/pub/form/TOKEN` (hosted page and where
answers are posted), `/pub/form/TOKEN/fragment`, `/pub/form/TOKEN/schema`,
`/pub/forms.js`. A draft, closed, full, never-published or unknown form is
the same 404. `form_summary` rows carry a `versions` column; a question
whose kind or scale changed between versions is split.

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

For multilingual forms, create `form_translation` records with `form_id`,
`language`, `text_key` and plain `text`, then publish the form. Keys include
`field.KEY.label`, `field.KEY.help`, `choice.KEY.ID`, `form.success_message`
and `message.required`. Read `docs/forms.org` for the catalog. Partial
translations fall back to the form's `default_language`; choice IDs never
change. `venturectl report form_summary all form_id=3 language=fr` changes
report labels while counting responses from every language. Public `?lang=fr`
or the loader's `data-venture-form-lang` selects the language before header
negotiation. Signed state pins the language through pages and opt-in emails.


### Form assessments

Use `describe form_field` before setting `scoring`: choice points are JSON
keyed by stable choice IDs, for example `{"choices":{"yes":5,"no":0},"correct":["yes"]}`.
Number/rating scoring uses inclusive, non-overlapping `ranges` with `min`,
`max`, and integer `points`. Sensitive questions cannot be scored. Enable
`quiz_enabled`, create `form_result_band` records with stable `key`, `form_id`,
`minimum`, `maximum`, `message` and optional HTTPS `redirect_url`, then publish.
`show_answer_key` is opt-in and applies only after completion. Never write
response score/result properties: the server computes and freezes them.
`score_to_lead` adds the trusted `assessment_score` attribute to ordinary lead
capture before the existing scoring rules run; it preserves manual overrides.
`form_summary` shows score distributions separately for each version.

### Booking questions

A `form_field` with `kind=booking` names an existing `booking_page_id`.
Configure the form's HTTPS `public_origin` and required short-text/email
questions first; `booking_name_field` and `booking_email_field` default to
`name` and `email`. One booking question per form, without repeating groups,
prefill, defaults or sensitive storage. Publish after changing the binding.
The public schema exposes current `slots`; final submit rechecks capacity.
`booking_page.capacity=0` means one seat. `form_submission.booking_id` is a
service-owned link to the meeting. Do not write `booking_reservation` records:
they are private working copies. Signed management links go through the private
outbox body; opening one never cancels or reschedules a booking.

### Paid forms

Use `form.payment_enabled=true` and `form_price` declarations (`form_id`,
`key`, `name`, `product_id`, exact `unit_price`, optional `choice_field` /
`choice_id` / `quantity_field`), then publish. Read `describe form_price`;
all prices share one currency and quantities are bounded whole counts.
Required payer name/email keys default to `name`/`email`. Stripe Checkout
uses the server-computed invoice, never a posted amount. Paid intake and
newsletter double opt-in are separate forms.

Do not create or edit `form_payment`: it is a private working copy until
settlement creates the ordinary `form_submission` with `invoice_id`. Form
response automation and confirmation wait for settlement. Paid booking holds
last 45 minutes and use cards; ordinary paid forms retain configured ACH.
An owner can run `act form ID reconcile_payment invoice_id=N` after repairing
a failed follow-up. For `paid_needs_booking`, add `booking_start=ISO_TIMESTAMP`
for a replacement arranged with the customer. This action cannot be staged;
use invoice refund/dispute operations for the financial outcome. A payment
retry must reuse its signed intake nonce and original answers, never create a
second invoice. Pending payment answers participate in owner export/erasure;
financial and guest CRM records retain their independent policies.

The `form-results` dashboard template supplies six form statistics widgets.
Set each widget's `record_id` to a form and its `field` to the stable question
key for choice/NPS/rating kinds. `options={"version":2}` narrows before row
limits. NPS requires a 0–10 rating. `form_dropoff` counts retained unfinished
drafts by current page, not all historical visitors; it never exposes draft
answers. `dashboard SLUG` reads the same scoped aggregates as the page.

For a bounded proposed summary, use
`act form ID summarize first_version=1 last_version=3 period=this_month question=comments limit=100`.
Version endpoints are inclusive; omitted last version means the published one.
Only nonsensitive short/long text answers are sent to the organization-bound
toolless model. Theme counts use cited answer IDs and quotes are checked against
the input. Nothing is saved back to responses. Excessive/empty input is refused;
narrow the scope. This action is nonstageable and requires an editor.

Form templates and portability use generic actions:
`act form 0 templates organization_id=1`,
`act form 0 import_definition organization_id=1 template=contact`, and
`act form 12 export_definition format=yaml`. Export returns `result.text`
and `result.definition`; import accepts `definition=<JSON-or-YAML text>` or
one template name. The eight names are contact, feedback, nps, event-signup,
job-application, newsletter, appointment-request and order. Imports stay draft
with fresh tokens; newsletter needs its real `public_origin` before publish.
External record references require `bindings={"product:17":42}` (same-org
existing destination IDs). Responses and signing keys never travel with a
form definition. Appointment/order templates collect requests until an author
adds a booking question or payment prices. See `docs/forms.org`.

File questions use `kind=file`, `file_max_bytes` (default 5 MiB; max 20 MiB),
`file_max_count` (default 1; max 10), and `file_types` (PDF, PNG, JPEG and/or
plain-text MIME names, comma separated). Publish after editing limits.
`form.upload_quota_bytes` and `upload_client_hourly_bytes` default to 100 MiB
and 20 MiB/hour when zero. The documents module must be enabled.

Upload through the public multipart form, not generic record creation.
`form_upload` is a private service-owned working copy. A completed response
contains file name/size/type and document references; sensitive file metadata
is omitted from ordinary reads. Download bytes only through the authenticated
`/forms/uploads/ID` route with response read authority (owner for sensitive
files). Generic document/AI attachment readers refuse these bytes. Erasure and
the forms retention sweep remove their files; neither CLI output nor an AI
summary contains file contents.

### Retrying public form intake

Fetch a fresh public schema/fragment per intentional submission. Retain
`submission_nonce` under the schema's `submission_field` (currently `_vf_payment`)
and retry exactly the same answers after an uncertain final response. A changed
payload under an accepted identity conflicts; do not obtain a new identity merely
to retry. Ordinary forms now keep a private service-owned `form_receipt` in the
acceptance transaction. Never create or edit receipts with generic commands.
Erasure removes their fingerprints and response links but preserves a minimal
consumed-identity marker. Closed/full forms still return the uniform 404.
Copied HTML snippets first open a native confirmation form to obtain a fresh
identity; legacy clients omitting it remain outside the retry guarantee.
