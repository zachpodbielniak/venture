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
| `create TYPE field=value ...` | a new record |
| `update TYPE ID field=value ...` | change a record |
| `delete TYPE ID` | soft delete — the row stays, stamped |
| `restore TYPE ID` | clear that stamp |
| `forge set-token ID` | set a forge's access token, read from stdin |
| `forge set-secret ID` | set or generate its webhook secret |
| `forge verify ID` | record which account the token belongs to |
| `report [NAME] [PERIOD] [as_of=DATE] [organization_id=ID] [customer_id=ID] [currency=CODE]` | list reports, or run one with an optional historical cutoff and legal entity |
| `links TYPE ID` | every link touching a record, read from it |
| `link TYPE ID TYPE ID [kind=K] [note=T]` | link two records; kinds: related, blocks, blocked_by, depends_on, required_by, parent_of, child_of, duplicates, causes, caused_by, produces, produced_by, references, referenced_by, supersedes, superseded_by; unlink with `delete record_link ID` |
| `modules` | which modules the server runs; `-f json` for types, reports and reasons |
| `factory` | the software factory at a glance: milestones with progress, releases, builds, environments and what they run, open incidents |
| `release changelog ID [--replace]` | draft a release's changelog from the tickets marked fixed in it |
| `journal post ID` | post a draft through the shared service; editors propose, `--stage` always proposes |
| `release publish ID [--prerelease]` | cut the release on the forge; creates the tag, cannot be undone here |
| `dashboards` | the dashboards the token may see |
| `dashboard SLUG` | one dashboard, every widget evaluated; `-f json` for the whole answer |
| `dashboard export SLUG` | its definition as JSON; `dashboard import FILE` (or `-`) creates one from it |
| `dashboard create TEMPLATE` | `factory`, `reporting`, `work` or `overview`; `dashboard templates` and `dashboard kinds` list what is accepted |
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
| `health` | is the server up |
| `mcp [--apply-writes]` | serve the API to an AI agent as a stdio MCP server |

Flags: `--server/-s`, `--token/-t`, `--format/-f table|json|yaml|csv`,
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

For a forge, use the `forge` subcommand — the one part of `venturectl` that
is not generic over types, and the exception earns itself:

```bash
printf '%s' "$FORGE_TOKEN" | venturectl forge set-token 1
venturectl forge set-token 1 < token.txt

printf '' | venturectl forge set-secret 1    # generates one, returns it once
printf '%s' "$SECRET" | venturectl forge set-secret 1

venturectl forge verify 1                    # records the bot account
```

**The value comes from standard input, and there is no flag to put it in
argv.** A command line is visible to every process on the host through
`/proc` and lands in shell history; a secret that has been in either has to
be rotated. An empty token is refused rather than treated as "leave it
alone" — a script that sent an empty string meant to send something and its
variable was unset.

`forge verify` is not optional if you want webhooks: it records which
account the token belongs to, and that is the loop guard. Without it VENTURE
cannot tell an issue it filed itself from one somebody else opened.

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
end. It reports book currencies separately. The existing P&L still reads
operational sales and expenses; it is not the authoritative journal balance.

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

It is refused on commands other than `create`, `update`, `delete` and `journal post`,
because those are the only routes that read it -- and an unknown query
parameter on a write route is ignored, so a quietly accepted `--stage` would
apply the change it was asked to hold back.

**Some types need more than an editor.** `forge` is owner-only, `forge_rule`
and `plugin_config` are admin-only, `user` and `api_token` are owner-only. A
403 here means the token's role, not a bug.

## Worked example: wire up a forge

```bash
# 1. The server, and where git lives — often a different host
venturectl create forge name="Home" kind=forgejo \
    base_url=https://git.example.com \
    clone_base_url=git@git-ssh.example.com \
    active=true

# 2. The credentials, from a script
FORGE=$(venturectl -f json list forge name__eq=Home | jq -r '.records[0].id')
printf '%s' "$FORGE_TOKEN" | venturectl forge set-token "$FORGE"
venturectl forge verify "$FORGE"

# Generate a webhook secret and keep it — it is shown once, and you paste
# it into the forge's webhook settings.
SECRET=$(printf '' | venturectl -f json forge set-secret "$FORGE" \
         | jq -r '.secret')

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

Collection pulls return at most ten results, `next_offset` and `more`; continue pages while `more` is true and inspect per-record errors. Pull imports/merges without pushing; sync pushes conflict-free changes with an expected remote version. Edits require the local replica version. A conflict blocks that record until resolved; choosing remote can accept a removed/revoked field. Never update `federation_replica` through generic CRUD: its merge state belongs to the service. Copies remain usable during outages but are not authoritative local accounting rows. New source objects and binary attachments are not created/copied offline. See `docs/federation.org` for key exchange, grants, scheduling and revocation.

## Organization membership

Read `docs/orgaccess.org` for the role matrix. Membership and team records use
generic CRUD. Tokens intersect mint-time memberships with current authority;
new grants never widen an old token. Missing membership gives empty results or
404; a refused in-organization write gives 403. Owner/admin data authority and
output formats remain unchanged. `journal post ID` returns a confirmation for
an organization editor. Treat that response as pending until finance approves.
