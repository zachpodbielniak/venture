# venturectl: setup, generic verbs, output, exit codes

Read this before running `venturectl`. Every verb is listed, grouped by
subsystem, in [cli-verbs.md](cli-verbs.md); the HTTP behind each is in
[api-routes.md](api-routes.md). Ground truth: `venturectl --help` and
`venturectl market|accounts|arbitrage help`.

`venturectl` talks to a running server over HTTP and never opens the
database: one writer, one set of validation rules, one audit trail. It is
generic over record types -- there is no `venturectl sale` subcommand and
never will be; the type is an argument, so a type added yesterday works.

## Setup

```bash
export VENTURE_SERVER=http://127.0.0.1:8747     # default; 8748 = just start, 8749 = make demo
export VENTURE_TOKEN=vk_...                     # or --token/-t
venturectl health                               # stop and fix this first if it fails
```

Mint a token at Settings -> API tokens (`/account/tokens`, admins only) or
`POST /api/v1/tokens` ([api.md](api.md)): any signed-in user may mint their
own token there with a browser session, but a bearer token may mint only if
it is admin. The token carries the minter's role and memberships. A
non-admin's token expires (`expires_in_days`, default 30, 1..90); an expired
token is a 401, not a missing record. It is shown once. If
`health` fails, every other command fails the same way and less clearly.

**Interactive sessions** (hosted workspace administration, or anything that
needs a signed-in person): `--session-file /private/session.json` reads an
owner-only (`chmod 600`), single-link regular JSON file of at most 8 KiB:

```json
{"origin":"https://workspace.example.test","cookie":"venture_session=SIGNED_SESSION_VALUE"}
```

Obtain the session through the normal sign-in and MFA ceremony; this option
neither logs in nor elevates a token. Do not put its contents in argv or
logs, and unset `VENTURE_TOKEN` (a token plus a session file is refused).
The origin must exactly match `--server`/`VENTURE_SERVER` (HTTPS, or numeric
loopback HTTP for a local fixture). Symlinks, hard links, FIFOs, non-private
files, an origin mismatch and `mcp` are refused. Session requests never
follow redirects. The ordinary `act`, `list` and `get` keep their forms.

## Global flags

`--server/-s URL`, `--token/-t`, `--session-file FILE`,
`--format/-f table|json|yaml|csv`, `--quiet/-q`, `--version`, `--license`.
One-verb flags are refused anywhere else (a flag that does nothing teaches
that it did something): `--wait` (feeds sync|push), `--dry-run` (post
backfill, billing, recurring, batch, market alerts evaluate -- `assets
run-period` and `dunning sweep` take `dry_run=true` instead), `--as-of` (sequence run, billing, recurring, collections, money
calendar), `--stake` (arbitrage calc), `-o/--output` (arbitrage export),
`--from`/`--to` (money calendar, support rollup -- **not** `report`),
`--kind` (money calendar), `--sort` (support rollup), `--matcher`/
`--threshold` (reconcile), `--html`/`--limit` (mail), `--replace` (release
changelog), `--prerelease` (release publish), `--apply-writes` (mcp only).

`--stage` proposes instead of applying, and is accepted only by `create`,
`update`, `delete`, `act` (stageable actions), `dunning sweep`, `dedupe`,
`journal post`, `sequence enroll`, `lead convert`, `billing`, `arbitrage
record|close|reopen|abandon|execute` and `accounts post|record-flips` --
the routes that read it. Elsewhere it is refused, because a write route
*ignores* an unknown parameter and would apply the change. The answer:

```text
Not applied. It is waiting for approval as 6eb8c494...
  approve: POST /api/v1/confirmations/6eb8c494.../approve
```

Nothing takes effect until somebody approves. Say so, with the id -- never
report a staged change as done.

## The generic verbs

| Verb | Does |
|---|---|
| `types [TYPE]` | every record type, or one described |
| `describe TYPE` | fields in wire spelling, kind, `required`, `-> target`, enum choices, help -- **not** actions |
| `list TYPE [k=v ...]` | records; filters `field__op=value`, reserved `limit offset page search order include_deleted period period_field` |
| `get TYPE ID` | one record (any organization you may read) |
| `create TYPE f=v ... [attributes.NAME=v ...]` | a record; `attributes.` goes into the bag (venture-type and custom fields) |
| `update TYPE ID f=v ...` | change named fields |
| `delete TYPE ID` / `restore TYPE ID` | soft delete / undo |
| `act TYPE ID ACTION [k=v ...]` | a declared action, arguments typed from the schema; ID 0 for type-level |
| `bulk TYPE 1,2,3 f=v ...` / `--delete` | one transaction; not stageable |
| `report [NAME] [PERIOD] [k=v ...]` | list reports or run one ([reports.md](reports.md)) |
| `links TYPE ID`, `link TYPE ID TYPE ID [kind=K] [note=T]` | record links |
| `modules`, `health`, `plugins [list]` | what this server is |

**Run `describe` before you write.** Guessing a field name costs a silent
no-op; reading it costs one command:

```text
$ venturectl describe forge_rule
forge_rule (table forge_rules)
  name              string     required
                    What this rule is for
  repo_id           reference  -> forge_repo
                    Leave empty for a forge-wide rule
  issue_type        enum       [task|subtask|story|epic|bug|research]
```

To see a type's actions: `curl -s -H "Authorization: Bearer
$VENTURE_TOKEN" $VENTURE_SERVER/api/v1/schema/TYPE | jq '.actions'`
(`-f json describe` still prints the table).

Traps in arguments:

- `key=value` values reach the server as JSON strings; the generic record
  routes and `act` convert them by the schema, but hand-written verbs over
  an action do it themselves. Booleans and numbers in `act` are typed for
  you; an unknown parameter is exit 2.
- Filters: `field__op=value`, operators `eq ne lt lte gt gte like ilike in
  not_in is_null not_null between` -- but `between` cannot take its two
  values from a `key=value`; use `__gte` and `__lt`. Money filters compare
  **minor units** (`amount__gte=10000` is 100.00). Any non-reserved name
  that is not a field is exit 2 with the available fields listed.
- **Which organization**: `list`, `report` and most verbs answer for the
  default organization unless given `organization_id=N`.
- GOptionContext refuses UTF-8 it cannot convert in a C locale: pass long
  text with em dashes or accents on stdin where a verb accepts `-`
  (`comments add TYPE ID -`), or run under a UTF-8 locale.

## Reading output

Output is a table on a terminal and JSON when piped, so `-f json` is only
needed for JSON *and* a terminal. A list is `{"total","count","records"}`.

```bash
venturectl -f json list sale | jq -r '.records[] | "\(.id)\t\(.gross.formatted)"'
venturectl -f json get ticket 1 | jq -r '.title'
venturectl -f json list forge_run state__eq=failed | jq -r '.records[].failure_reason'
```

`-f csv` hands data to a spreadsheet: every field appears (nothing
truncated like the table), the header uses the wire spelling, money prints
formatted, cells are escaped and a leading `=` is defused; `-f csv report
NAME` returns the server's own CSV. `-q` prints only the data (drops notes).

## Exit codes

| Code | Meaning |
|---|---|
| 0 | fine (a staged write is also 0 -- read the message) |
| 1 | anything else (a failed run with `--wait`, a refused books mode...) |
| 2 | usage, unknown field, option or parameter, malformed value (400) |
| 3 | not found -- including a record you may not read, an action the schema does not offer, a module that is off |
| 4 | conflict -- uniqueness, stale version, stale approval, a push queue full |
| 5 | auth -- token missing, wrong, or the wrong role (401/403) |
| 6 | unsupported in this build |
| 7 | network or timeout -- no server, or `--wait` gave up (the work stays queued) |
| 8 | validation -- the record was refused (422) |

Branch on these, not on message text: messages are written for people and
change.

## venturectl mcp

`venturectl mcp [--apply-writes]` serves the API to an outside agent as a
stdio MCP server. It refuses `--token` (an agent config file is readable
via `ps`/`/proc`) and reads `VENTURE_TOKEN` and `VENTURE_URL` (falling back
to `VENTURE_SERVER`); `--server` is fine. Its tools (`venture_schema`,
`venture_list`, `venture_get`, `venture_create`, `venture_update`,
`venture_delete`, `venture_reports`, `venture_report`,
`venture_confirmations`, `venture_modules`, `venture_links`,
`venture_link`, `venture_dashboards`, `venture_dashboard`,
`venture_dashboard_build`, `venture_factory`, `venture_inbox`,
`venture_runs`, `venture_desk`, plus `venture_TYPE_ACTION` per stageable
action) are generated from `/api/v1/schema` at startup.

**Writes are staged** (`?stage=1`) unless started with `--apply-writes`;
the tool answers with the confirmation id and the routes that decide it,
and the card appears in `GET /api/v1/confirmations` and the AI panel beside
the assistant's own. Against a VENTURE too old to stage (no
`staged_writes` in health) it falls back to a client-side *hold*: it
prints the request it would have sent, sends nothing, and says nothing is
queued. Read which one you got -- telling somebody to approve a change that
was never sent wastes their time and leaves it unmade. Generated action
tools always stage, even with `--apply-writes`; building a dashboard and
publishing a release refuse while staging is on.
