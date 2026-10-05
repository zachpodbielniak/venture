# VENTURE

An ERP/CRM hybrid in C on GLib/GObject, for running a portfolio of small
business ventures. Read `README.org` first, then `docs/index.org`.

## Build and test

A `Justfile` drives a local instance out of the build tree — `just start`,
`just ctl …`, `just sql …`, `just reset`; `just` alone lists them. It only
ever calls make, so there is one build system. It listens on 8748 because
8747 is where a containerised instance lives, and quietly talking to that
one instead is the failure worth designing out.

```sh
make DEBUG=1                 # debug build into build/debug
make DEBUG=1 test            # the whole GTest suite
make DEBUG=1 test-one T=test-money
make                         # release build into build/release
make compose-up              # rebuild the image and recreate the local stack
```

**Always build and test the DEBUG version.** `make test` builds the plugins
first, because the suite loads the example plugin for real; without them that
test skips itself rather than failing.

**Before landing, also run a plain `make` (release).** The release build
optimises, and GCC's `-Wmaybe-uninitialized` only sees through an `-O2`
call graph: an ignored return value that leaves out-parameters unset built
clean in every debug run of the arbitrage epic and broke `make demo`, which
builds release.

`make -j clean all` is safe: a clean goal beside any other goal finishes in
a sub-make before the build starts. The generators write into the directory
`clean` removes, so the two goals must not run at once.

Zero warnings, always. `-Werror` is on with a wide warning set, and a warning
is a latent bug. Never silence one by lowering the warning level.

The three-file Makefile pattern (`config.mk` / `rules.mk` / `Makefile`) is
deliberate — do not collapse it. Two binaries, `venture` and `venturectl`,
and each must build on its own: `make venturectl` has to work on a machine
that cannot build the server at all.

## The one idea

**Everything derives from GObject property metadata.** A record type is a
field table plus a registration. From that one declaration VENTURE builds the
database table, REST CRUD, JSON and YAML serialisation, the web list, the web
form, the AI tool schema, the audit diff and the CLI subcommands.

Adding a field is a line. Adding a record type is a registration. If you find
yourself hand-writing a form, a route, a serialiser or a SQL statement for a
record type, stop — that is the thing this design exists to avoid.

The corollary has bitten repeatedly: **anything the field table declares must
survive into `VentureFieldSpec`.** Recomputing a property's kind or order
from its `GType` loses what the declaration said. A long text field and a
short string field are both `G_TYPE_STRING`; GObject returns properties in
its own order, not the declared one. Both were lost at some point, and the
symptoms were a Notes field rendering as a one-line box and a list whose
first seven columns were empty.

## The series store is the exception, and it stays one

`src/series/` is the one place VENTURE hand-writes SQL for data: one SQLite
file per market data source, `<state_dir>/series/<source-uuid>/store.db`,
outside `VentureDatabase`. Its rows are derived, append-heavy and
high-volume -- ten thousand instruments a venue an hour -- closer to the
audit log than to a record, and nobody edits one; the field table's
audit, versions and validators per row would cost more than the data.
`docs/market-data.org` has the schema and every formula. The traps of
everything built on it -- plugins, feeds, the oracle, alerts, the Trading
pages and arbitrage -- are in the sections at the end of this file, in the
order the data flows.

- **It never touches `VentureDatabase`, the entity registry or the
  configuration.** Its writer runs on the feeds worker thread. Limits
  (the size cap, retention days) are read on the main thread and handed
  in as numbers. It logs with `g_debug`/`g_message`, never `g_warning`,
  which the harness makes fatal from any thread.
- **One writer handle, any number of readers, one connection each, one
  thread each.** Every cached statement goes back reset
  (`SeriesCachedStmt`): a SELECT left mid-step pins a reader's WAL
  snapshot and the page shows the same prices forever.
- **The schema steps are append-only**, like `migrations/`: never edit a
  shipped one; add a step and bump nothing else -- `user_version` is the
  count. A newer file is refused, and a foreign SQLite file is refused
  before even its journal mode is touched.
- **Do not add a record type for anything in it.** What belongs in the
  main database (a venue someone trades at, an instrument someone
  watches) is promoted into ordinary records by the marketdata module;
  the store never writes back. And do not grow the store into a second
  record system: if a person edits it, it is a record.
- **Money in it is still never a double**, and SQLite turns an integer sum
  that overflows into a REAL: sums in its SQL saturate (`SERIES_SAT_ADD`),
  and sums in C are checked or 128-bit (`venture-series-math.c`).
- **Everything priced is keyed by its currency, the listing set too.** A
  sale estimate values what vanished at the *previous* set's price, so the
  set keeps `listing_set_currency` (step 5) and a snapshot in another
  currency restarts the set instead of diffing. Anything new that carries
  a price from one snapshot to the next must do the same; NULL there means
  "not recorded" and restarts rather than guesses.

## Conventions

- gnu89, tabs, 4-wide. `/* */` comments only, never `//`.
- GObject-Introspection-compatible doc comments on every public function.
- `g_autoptr`/`g_autofree`/`g_steal_pointer` throughout.
- Money is never a double. `VentureMoney` is integer minor units plus an ISO
  4217 code and exponent; rounding is half to even; cross-currency arithmetic
  is refused rather than guessed.
- Comments say *why*, not *what*. Several in this tree exist because a
  plausible implementation was wrong in a way that only shows up in a tax
  return — leave those alone.

## Things that are easy to get wrong

- **Sensitive fields** (`VENTURE_COLUMN_FLAG_SENSITIVE`) must never reach a
  response, a log line, a form or the AI. A URI carrying a password gets
  `venture_string_redact_uri()` before it is shown anywhere.
- **An API token acts as "API token #<id>", never under its name.** The
  name is whatever the owner typed and the actor string reaches the audit
  log, every record's activity and the inbox, which viewers read. The
  upgrade rewrote only what it could find exactly: the audit log's `actor`
  and `approved_by` (000640), the inbox's `actor` and the title it led
  (000645), and `api_token` audit labels and name diffs (000685). Older
  fields that copied a token's name as free text -- a comment's author, an
  assignee, a worklog author, `deployed_by`, the bank-match, enrolment and
  approval stamps -- keep it, and so do their audit diffs: rewriting those
  would mean guessing which strings were ever a token.
- **`VentureActor` is filled field by field at every call site.** It is a
  plain struct declared as a bare local, `-Wmissing-field-initializers` is on
  so nobody writes `= {0}`, and `venture_database_record_audit()` reads every
  member. Adding a field means visiting all seven hand-filling sites *and the
  tests* — `venture_auth_to_actor()` covers the other thirty-odd callers.
  Miss one and the audit writer dereferences an uninitialised stack pointer,
  which is a segfault a long way from the edit. Adding `approved_by` did
  exactly that to `tests/test-database.c`.
- **`/api/v1/health` needs no credentials, alongside the
  webhook and opt-in federation identity discovery**, because a container healthcheck runs before anybody has
  credentials. It may say what this *build* can do — the version, the
  backend, whether staging exists — and must never say what this *install* is
  doing. A count of pending confirmations was drafted for it and removed for
  this reason: how much work is queued is business activity, and it lives
  behind the viewer role at `/api/v1/confirmations`.
- **A staged write and a direct one are the same code until the last line.**
  `venture_web_api_write()` takes a `stage` flag and branches only at the
  save, and approval calls `venture_database_save()` on the staged object.
  Two paths would be two behaviours, and the behaviour in question is what
  gets written to the books. It is also what makes a stale approval a
  conflict for free: the staged record carries the version it was read at, so
  the optimistic-concurrency check in `venture_database_update()` catches it
  with no second mechanism.
- **A staged change that fails to apply is dropped, not retried.**
  `venture_database_save()` bumps the record's version *before* the UPDATE
  that then matched nothing, so the staged object describes a version that
  never existed and a second approval could not work either. The refusal says
  which fields moved — from the `original` snapshot the confirmation keeps —
  and says to stage it again.
- **Declarative venture types are enforced at the save, on what is
  written.** `venture_venture_type_check_install()`
  (`src/core/venture-venture-type-check.c`) holds a venture to its YAML
  type's `required`, `choices`, `min`/`max`: every declared field when the
  venture is created or changes type, otherwise only the attributes whose
  value changed -- the reference rule, so a venture stored before a rule
  existed stays editable. An attribute is text; numeric fields are parsed
  before the bounds are compared (handing the spec a string made
  `venture_field_spec_value_as_double()` skip min/max entirely, which is
  how every shipped bound went unenforced). An unregistered type is
  refused only when written and only when the registry holds something:
  plugins off loads nothing, and nothing is not evidence. The registry
  hangs off the database (the last context over it wins), because the
  tests build several contexts per database. `venturectl` writes the bag
  with `attributes.NAME=value` in the same request, which is what lets a
  type with a required field be created at all.
- **A save short-circuits on an empty diff.** Anything excluded from the
  audit diff silently refuses to change: the call returns success and the row
  keeps its old value. Only genuine machinery belongs in the identity spine.
- **Every route carries its own authentication check.** Use
  `venture_web_ui_require_session()` or `venture_web_api_require()`; a
  missing guard looks exactly like nothing. `tests/test-auth.c` starts a real
  server and asserts every page redirects and every endpoint 401s when
  anonymous, so add new routes there.
- **User and API-token records are owner-only everywhere**, via
  `venture_web_require_for_type()`. Creating a user is access management, not
  data entry.
- **Style inputs by exclusion, not enumeration.** Listing which `type=`
  values get themed is how the login form's username box ended up unstyled.
- **Query-string names are either reserved or filters.** `search`, `order`,
  `limit`, `offset`, `page` are reserved in `venture_query_apply_query_string`;
  anything else is parsed as a field filter and refused if the field does not
  exist. A new UI parameter that is not reserved there breaks every list page
  with "no field named X".
- **`ticket.kind` and `ticket.issue-type` answer different questions.** Kind
  is internal/external — whose problem it is, which decides who may read the
  replies. Issue type is epic/story/task/subtask/research/bug — what shape
  the work is, which is what forge rules key on. An external bug is both.
  Do not merge them. `VENTURE_ISSUE_TYPE_TASK` must stay the enum's zero
  value: the column was added to a populated table, so every historical row
  reads back as whatever is first.
- **References are checked at the save, but only when written.** A save
  that writes a reference field refuses a target that does not exist or is
  soft-deleted; a field keeping the value it already had is left alone, so
  records pointing at since-deleted rows stay editable. This is app-layer,
  not SQL FK — `venture_schema.c` emits no FOREIGN KEY clauses, so the
  `PRAGMA foreign_keys=ON` is inert by design. Tests must create real
  referents (`create_organization()` in `tests/test-database.c`); a
  fabricated id that used to save quietly now fails the save.
- **A polymorphic subject buys nothing from the generic machinery.**
  `ticket_relation` names its subject by type and id, so it gets no picker,
  no reverse section from `venture_web_append_related()` and no rendered
  link — each is written by hand. Create one only through
  `venture_ticket_relation_create()`: nothing in the database stops a row
  naming a type that was never registered. And a type with a panel of its
  own must be skipped in the generic related-records walk, or the page shows
  the same relationships twice, once uselessly.
- **Chat threads are per-user, not per-entity.** The chat routes must filter
  on the caller's `user-id` and report NOT_FOUND (never FORBIDDEN) on a
  mismatch, and `chat_thread`/`chat_message` stay owner-only in
  `venture_web_require_for_type()`. `tests/test-auth.c` pins this.
- **The AI executor must be built empty.** `ai_tool_executor_new()`
  pre-registers `bash`, `read`, `write`, `edit`, `glob`, `grep`, `ls` and
  `web_fetch`. Use `ai_tool_executor_new_empty()`; a `bash` tool bypasses the
  staged confirmations, the audit trail and the sensitive-field rules in one
  call. (`ai_tool_executor_unregister()` *can* now remove a built-in —
  `ai_tool_executor_execute()` checks `executor_offers_tool()` first, so the
  allowlist is structural. Building empty is still the right default, but the
  old claim that a built-in was unremovable is no longer true of the vendored
  ai-glib.)
- **ai-glib's file tools are not a sandbox.** `executor_resolve_path()`
  returns an absolute path unchanged and never normalises `..`, and
  `tool_bash` only sets a subprocess cwd. An in-process agent given them
  could read `venture.db`, the config and `/proc/self/environ`. The coding
  runner uses `venture_work_tools_register()` instead, which canonicalises
  every path against the checkout — and compares against `root + "/"`, not
  `root`, or a sibling named `<root>-evil` passes.
- **The webhook bypasses local sessions, and the HMAC is its
  guard.** `/hooks/forge/:id` has no session check by design. A forge with no
  webhook secret set must be *refused*, not trusted — podomation's forgejo
  module returns TRUE in that case, which is why VENTURE reimplements the
  check rather than reusing it. Compare in constant time, hash
  `htmx_request_get_body_bytes()` and never `htmx_request_get_body()` (a
  `g_strndup`, so a NUL truncates the payload), and enforce the body size cap
  here because `server.max_request_size_mb` has no consumer anywhere else.
- **The forge client deliberately bypasses the AI's SSRF guard.**
  `venture_ai_url_is_fetchable()` refuses loopback and site-local addresses,
  which is exactly where a self-hosted Forgejo lives. What replaces it is
  that no caller may choose a host: the origin is parsed once at
  construction, redirects are disabled (a 302 would carry the Authorization
  header to a host the *response* chose), and the final URI is compared back
  against the pinned one.
- **git runs through an argv array, never a command line.** Branch and
  repository names reach this path from webhook payloads. A command line is
  word-split, so a branch named `--upload-pack=...` becomes an option git
  obeys. podomation's git module uses `g_spawn_command_line_sync`; do not
  copy it. The push credential goes in the environment, never in argv, which
  is world-readable through `/proc`.
- **Every background thread is in this list, and none touches the
  database.** Writing a record emits `entity-saved`, whose automation handler
  enters podomation, which runs a nested main loop on the *default* context —
  driving that from a second thread is a context-ownership failure, not
  merely a data race. So results cross back as plain data and are applied on
  the main thread. The threads:
  - the **coding run** thread (`venture-work`, `venture-work-service.c`);
  - `g_task_run_in_thread` in the **bank-feed transport** and the **mailer**
    (`send_task`): network only, the result handed back by the task;
  - the **feeds worker** (`venture-feeds`, `venture-series-worker.c`): its
    own `GMainContext`, `SoupSession` and every series store's writer; it is
    handed frozen sources and hands back runs, and the service writes them
    on the main thread. It also takes installation backups' store copies:
    handed a directory and a destination, it hands back a digest;
  - `g_task_run_in_thread` under the feeds worker: provider parsing, exec
    providers and `venture_func_data_source_provider_new()` fetches. They
    see only their `VentureFeedRequest`.

  None of them may touch `VentureDatabase`, the entity registry, the
  configuration, a plugin's settings or a non-atomic counter of the main
  loop's. `VentureDatabase` remembers the thread that made it, and `save`,
  `begin`, `begin_serializable` and `execute` from any other raise a
  `g_critical` (fatal in tests). `count` and `find` are not checked:
  `tests/test-database.c` probes the lock from a second thread on purpose.
  A new thread goes into this list in the same commit.
- **Customer subscription mail is queued inside the billing instruction.**
  The trial reminder (renewal sweep) and the price-change notice
  (`change`/`change-seats`) enqueue through the outbox in the service's own
  transaction, keyed `trial-reminder:<subscription uuid>` and
  `price-change:<event uuid>`. A missing address or mail switched off
  skips the notice; it must never refuse the change. Do not move them to
  a timer or a signal handler: the key is what makes them once-only.
- **The portal manages a subscription only by token and only its own.**
  `/portal/:token/subscriptions/:id` answers NOT_FOUND for anything not the
  token's customer's, and goes through `venture_billing_service_execute()`
  like every other writer; `tests/test-portal.c` pins the cross-customer
  cases.
- **The JSON wire format uses underscores.** Properties are `invoice-id` in
  C and `invoice_id` on the wire; a POST body with dashed keys is silently
  ignored field by field — the record saves and the values just aren't
  there.
- **An automation reload is a rebuild.** Never parse new rules into a
  running engine: pods hold timer and breaker state, and stacking a second
  generation beside the first doubles every side effect.
- **Pod handlers must stay synchronous.** `pod_engine_new()` defaults
  `handler_timeout_seconds` to 30, and any non-zero value selects a dispatch
  path that runs the handler on a `GTask` worker thread while the caller
  blocks in a nested `g_main_loop_run()` on the default context. VENTURE's
  handler writes to the database and reads a non-atomic cascade guard.
  `venture_automation_build_engine()` sets the timeout to 0 for this reason;
  `tests/test-automation.c` pins it.
- **Nested transactions must balance their lock.** `venture_database_begin()`
  takes the recursive lock on every call and tracks depth plus owning thread;
  commit and rollback each release one level. Returning early from a nested
  begin without releasing — as it once did — leaks a level, which is
  invisible on one thread and a permanent silent deadlock the moment anything
  else touches the database.
- **Header dependencies come from `-MMD`, not from a separate rule.** The
  `%.d` rules that used to sit in `rules.mk` were unreachable, so no depfile
  was ever written and the `-include` matched nothing: editing a header
  rebuilt *nothing*. If a header change ever stops triggering a rebuild,
  check `DEPFLAGS` is still on all three compile rules and that
  `TEST_OBJS` is still in the `-include` list.
- **A dep bump needs `make clean-all`, not `make clean`.** Dependencies
  change their output layout and soname between versions (orm-glib went
  flat `build/liborm-glib-0.1.a` to `build/$(BUILD_TYPE)/liborm-glib-0.2.a`).
  A `git checkout` gives objects the same mtime as their sources, so make
  thinks stale ones are current and re-archives them: the link fails on a
  symbol in neither version, or worse, succeeds against half-stale objects.
  `clean-all` now removes every dep build tree for this reason.
- **A report's narrowing belongs in the query its row bound counts.**
  Reports that total in C fetch at most `venture_aggregate_get_max_rows()`
  rows and refuse past it. A filter applied to the rows *after* that
  fetch -- venture, category, status, active -- leaves an organization
  past the bound refused however narrowly it asks, by a message telling
  it to narrow. Push it into the `VentureQuery` (an `IN` over
  `venture_category_descendants()` for "this category and beneath"), and
  test with `venture_aggregate_set_max_rows()` rather than 20 000 rows.
- **SQL that aggregates must say what type it wants.** SQLite returns
  `SUM(integer)` as an integer; PostgreSQL widens it to numeric, and the
  typed accessor asserts. Cast aggregates explicitly — `CAST(SUM(x) AS
  BIGINT)` — or the SQLite suite passes while PostgreSQL dies.
- **Vectors from two embedding models cannot be compared.** The cosine
  between them is not a weak match but noise that reads exactly like one: a
  number in [-1, 1] that looks plausible and means nothing. The model is
  therefore recorded on the base, the article *and* the chunk, and checked
  at each: a base is claimed by the first model to index it and refuses a
  second, a reindex skips what is current, and a search skips any passage
  the configured model did not write. Changing `kb.embedding_model` requires
  `kb reindex --force`; there is no cheaper correct answer.
- **`kb.embedding_*` is deliberately separate from `ai.*`.** The assistant is
  whichever model writes well; the embedder is whichever model the corpus was
  indexed with, and only one of those can be changed freely. Sharing them
  would mean switching chat models silently invalidated the index.
- **Knowledge-base indexing is synchronous, and there is nowhere to move it.**
  No background thread may touch the database (see the thread list above),
  so an article is embedded on the request that saved it and bulk work is an
  explicit command. `kb sweep`-style operations take a limit for this reason.
- **A chunk and a link are purged, not soft deleted.** Both are derived and
  carry no history of their own, and a soft-deleted one would still be read
  back by the query that walks the table -- so every reindex would make the
  corpus slower and the old text would stay findable.
- **Sync hashes the file's bytes, never its extracted text.** A PDF re-saved
  with identical text is therefore seen as changed, which is the cheap
  direction to be wrong in: hashing the extraction pays the extraction cost
  for every file on every sync just to decide it had nothing to do.
- **An article whose source file has gone is archived, not deleted.** A file
  removed from a checkout is usually a move, and destroying the article takes
  its cross-references with it. Archived articles are not searched, so the
  behaviour is the same and the mistake is recoverable.
- **`kb_link` is polymorphic like `ticket_relation`,** so it gets no picker,
  no reverse section from `venture_web_append_related()` and a hand-written
  panel. `kb_chunk` is skipped in that walk too, for the opposite reason: it
  *does* reference `kb_article`, and an article's page listing its own
  passages is a wall of useless links.
- **Crossref eligibility is derived from the field table, not listed.** A
  type with a long text field participates. Do not add a hardcoded list --
  that is the thing this design exists to avoid, and a plugin's record type
  should be covered the day it registers.
- **A convenience CLI verb over an action must type its arguments.**
  `venture_cli_values_from_args()` sends every `key=value` as a JSON string,
  and `venture_action_validate_parameters()` refuses a string for an
  integer or boolean parameter. `dunning sweep organization_id=1` failed
  that way for as long as it was documented; `act` types from the schema,
  a hand-written verb has to do it itself.
- **A `before-send` handler judges by the delivery run's clock, not the
  wall clock.** `deliver_due()` takes an explicit `now`, and the claimed
  row's `lease-until` minus `VENTURE_MAIL_LEASE_SECONDS` recovers it; wall
  time makes a delivery run "as of" a day decide against another day.
- **A calendar date is midnight UTC, and so is every period boundary.**
  `venture_time_from_string()` stores "2026-03-01" and "today" as midnight
  UTC on that day; `venture_date_range_parse()` reads *which* day it is in
  the configured zone but builds every boundary at midnight UTC. Building
  them in `locale.timezone` put the first of every month in the month before
  (and 1 January in the previous year) for any zone west of UTC, on every
  report. `venture_time_to_date_string()` shows an exact UTC midnight as the
  date it is in any zone. Do not "fix" a boundary back into local time.
- **A type-level action is judged in the organization it names.** With no
  `subject-parameter`, the ID-0 placeholder used to belong to organization
  0, which the access policy refuses for every non-administrator -- so an
  organization's own finance member could not run its dunning sweep.
  `venture_action_prepare_target()` now places it from `organization_id`,
  and `venture_action_require_organization()` refuses a member who names
  none. A new type-level action must declare an `organization_id`
  parameter (or a subject parameter), call both on any new checking path,
  and run in `venture_entity_get_organization_id(entity)`, not a re-read
  parameter or the default.
- **Whether a subscription's period was discounted is counted, not read.**
  `discount-periods-used` stops at the discount's N, so the Nth invoice and
  the one after both leave it at N. MRR and the cancellation credit count
  the start and renewal events that issued an invoice instead
  (`period_discount()` in `venture-billing-service.c`). A cancellation's
  event must never carry `invoice-id`: that would count its usage as another
  base period. It carries `final-invoice-id` for any terminal usage charge,
  which the pages and collection resolve separately. Collection can retain
  existing consent for that final charge after cancellation, never for new
  terms or ordinary invoices.
- **`make DEBUG=1 test` does not relink the server binary.** After editing
  `data/static/*` verify `build/debug/venture` is newer than
  `build/debug/venture-assets.h`, or the browser serves last hour's JS
  while the tests pass against this hour's.

## The CLI, and its skill

`venturectl` is generic over record types: `list`, `get`, `create`,
`update`, `delete`, `restore`, `describe`, `report`, `health`, with the type
as an argument. A record type added today already works, and no subcommand
should ever be added per type.

**The agent-facing guide is `skills/venturectl/SKILL.md`, and it is
maintained with the code.** It lives there rather than under `.claude/`
because three different agents read it and none of them owns it;
`.claude/skills/venturectl` is a symlink to it so Claude Code still finds it
with the project open. `make install-skill` links it into
`~/.claude/skills`, `~/.grok/skills` and `~/.agents/skills` — a symlink, not
a copy, so editing the working tree is immediately what every agent reads.
It only ever replaces a symlink; a real directory of the same name is
reported and left alone. When a command, a flag, an exit code or one of
its documented traps changes, change it there in the same commit. A skill
that is confidently wrong is worse than none, because it gets followed. Its
two ground truths — `venturectl --help` and `venturectl describe <type>` —
are generated from the source and cannot drift, so check against those.

`describe` is the command that earns its keep: it prints field names in the
wire spelling, what each reference points at, and what each enum accepts.
Most CLI mistakes are a field name guessed rather than read.

## Dependencies

Nine git submodules under `deps/`, all linked statically, all treated as the
canonical copies: `yaml-glib`, `htmx-glib`, `ai-glib`, `orm-glib`,
`podomation`, `crispy`, `mail-glib`, `stripe-glib`, `oidc-glib`. Fixing a bug in
one and pushing it upstream is expected and has happened several times.

Fedora build packages are printed by `make list-deps` (one per line, under a
heading — skip that first line if you pipe it into `dnf`), plus `libetpan-devel`
and `libgudev-devel` for podomation. That pair matters more than it looks:
podomation resolves its whole dependency set in one `pkg-config` call, so one
missing package empties the flags and the build fails on a missing `glib.h`.
`oidc-glib` fails the same way without `libjose-devel` and `jansson-devel`.

## Tests

One file per subsystem, `tests/test-<area>.c`, picked up by a wildcard. Write
the test around the behaviour that matters rather than the function that
implements it, and say in a comment what breaks if it regresses.

When you fix a bug, add the test that would have caught it, and check that it
fails without the fix — several tests here were written after a bug that
looked like a data problem and was an ordering one.

A fixture that needs a state directory removes it with
`venture_test_remove_tree()` from `tests/venture-test-util.h`. **Never
`g_rmdir()` and never `g_file_delete()`** — both do nothing at all to a
directory that is not empty, and both report it in a way every caller here
discarded, so a fixture that wrote one file inside the directory it made left
that directory behind once per test per run. Five did; a green suite left
seventeen in `/tmp` and said nothing.

`make test` runs under a private `TMPDIR` and `tools/venture-test-litter.sh`
fails a **green** run that left one, comparing the run against itself — a
*failing* run's directories are evidence and must stay, which is also why the
teardown never runs on that path: `g_assert` aborts the process.

**Never derive a test port from the pid, and never probe one and close
it.** A test server is configured with `server-port` 0 and reads the
kernel's choice back with `venture_web_server_get_port()` (or the base
URL) after `venture_web_server_start()`. `20000 + (getpid() + K) % 20000`
collided with suites in other worktrees and PID namespaces, with every
other listener, with TIME_WAIT and with a second server in the same
process; a probed-and-closed port can be taken before the bind. Both
failed a fixture now and then and passed on a rerun. Only a subprocess
that must be told `--port` picks one in advance, through
`venture_test_free_port()`, and retries on "Cannot listen on" with a
fresh port, bounded by `VENTURE_TEST_PORT_ATTEMPTS`. A hosted origin is
not a listening port: fix it and send its authority as `Host`.
`test-ports` greps the tests for the pid arithmetic. See
`docs/testing.org` "Server ports".

It found a second thing worth knowing: **a test that starts a coding run must
drain it before returning.** `venture_work_service`'s worker was still creating
the run's workspace while the teardown removed the directory above it.
`settle_runs()` in `tests/test-work-service.c` waits on
`venture_work_service_count_live()`, bounded — a test that can hang is worse
than one that fails.

## Modules, links and the factory

- **Every record type belongs to a module, and the module registry is the
  only thing that hides types.** `venture_module_registry_apply()` masks the
  process-wide entity registry; everything else asks the registry. Do not
  add a second "is this type enabled" check anywhere -- if a surface still
  offers a hidden type, the bug is in the registry mask, not in that surface.
  A page or service that belongs to a module calls
  `venture_web_require_module_ui()` / `_api()` or
  `venture_context_module_enabled()` first; the generic record routes do
  not need to.
- **The built-in module table is written bottom-up.** A module may only
  require what precedes it; `venture_module_registry_add()` refuses anything
  else, which is what rules a cycle out. Adding a module means placing it
  after everything it requires, and adding a type means listing it under
  exactly one module -- a type claimed twice is refused at startup.
- **A field may not take a name the entity spine owns.** `id`, `uuid`,
  `created-at`, `updated-at`, `version`. A release with a field called
  `version` installed a string over the optimistic-concurrency counter and
  every second save failed as a conflict with itself; the release's version
  is now `number`, and `venture_entity_class_install_fields()` aborts on a
  collision so it cannot recur.
- **Cross-row invariants are save validators, not constructor checks.**
  `venture_database_add_save_validator()` runs inside the lock for every
  writer -- the form, the API, an approved staged change, the AI. A check
  that lives only in a constructor is one door; `record_link` does both, and
  `ticket_relation` predates the mechanism.
- **The module registry is live.** It re-resolves on the configuration's
  `notify` and `module-switch-changed` signals and the context re-applies
  the masks, so a test may flip a switch on a running fixture. Reports are
  hidden, never removed, for the same reason: turning a module back on must
  restore them with nothing re-registered.
- **Tests that hand-make records must file them under the default
  organisation.** Every report and page scopes to it; a record with
  organisation 0 is invisible to them and the report reads zero rows.
- **A refused webhook signature is a `g_warning`, which the test harness
  makes fatal.** Wrap a deliberately bad delivery in
  `g_test_expect_message()`.

## The factory's operations live in core

- **`src/core/venture-factory.c` is the one implementation** of drafting a
  changelog, publishing a release, a milestone's progress, an environment's
  running release and the status summary. The pages, `/api/v1/factory`,
  `/api/v1/releases/:id/{changelog,publish}`, `venturectl factory|release`,
  the assistant's `venture_factory` and the MCP tool all call it. Do not
  re-implement any of these in the web layer.
- **Publishing cannot be staged.** It creates a tag on another system;
  the assistant offers it only under the autonomous policy and
  `venturectl mcp` only with `--apply-writes`. A changelog draft is an
  ordinary update and stages like one. So is recording a deployment (one
  record). Rolling back (two writes in one transaction) and a build's
  ticket (a ticket and a link) cannot be held by the queue, so they follow
  publish's rule.
- **Publish asks permission before it touches the forge**
  (`venture_access_policy_check_write()` at the top), and a conflict on the
  final save re-reads the row and writes the forge's id, tag and page onto
  it. The forge's own webhook can land while the blocking call is waiting
  and save the row first; giving up there leaves a release that is out on
  the forge and unpublished here.
- **The factory's dates are derived, on every writer.**
  `venture_factory_install()` registers save validators: an incident's
  started/resolved, a deployment's deployed, a milestone's completed, a
  release's released, a build's started/finished. The reports select on
  exactly these fields, so a status moved without its date is a record no
  report sees. A date given is kept; only an empty one is filled. Never
  stamp these in a handler.
- **`postmortem` is not open.** An incident being written up is fixed.
  "Open" is `open` or `mitigated`; filter both `resolved` and `postmortem`
  out, or use `venture_factory_incident_is_over()` inside the file.
- **Cancelled tickets did not ship.** They are in no changelog, no lead
  time, no "tickets shipped" and no readiness count.
  `venture_factory_release_tickets()` returns them all (the release's page
  lists them), so every consumer skips `CANCELLED` itself.
- **What needs you, readiness and the forecast are arithmetic, not AI.**
  `venture_factory_next_actions()`, `_release_readiness()` and
  `_milestone_forecast()` are derived from the records so they are the same
  every time and free to draw. The model narrates them
  (`venture_ai_factory_briefing()`); it never decides what is on the list.
- **`src/ai/venture-ai-factory.c` follows the desk's rules**: toolless path
  only, the records between `BEGIN RECORDS`/`END RECORDS` with the sentence
  that says they are data, bounded input, nothing to read refused before a
  model is called, a structured answer held to its closed vocabulary
  (`venture_ai_factory_restrict()`), and nothing written -- a draft reaches
  a record only as a person's own save (`POST /factory/draft/:type/:id`).
- **A finished build stays finished.** The workflow webhook ignores a
  non-terminal delivery for a finished row unless its `started_at` is later
  than the one on record (a re-run). Zero timestamps (year < 2000) are
  absent, not stored.
- **A forge release is matched by id, then tag, then version**, and by tag
  or version never onto a row that already carries a different forge id.
  Going out happens once: only a planned or in-progress row becomes
  released, and `released-at` is never overwritten. A deletion's empty
  strings blank nothing.
- **Time comparisons are normalised in the query emitter.** Stored times
  are text and GLib omits a zero fraction, so `...:00Z` sorts after
  `...:00.5Z`. `venture_query_append_filter()` writes every `<`/`<=`/`>`/
  `>=`/BETWEEN operand on a date-time field with six fraction digits, and
  pads the column for `>`, `<=` and BETWEEN. Build cutoffs with
  `venture_time_to_string()` as before; do not hand-roll a padding.
- **Forgejo sends `action_run_success`/`action_run_failure`, not
  `workflow_run`** (that is Gitea's). Both are routed to the one build
  handler; `venture_forgejo_parse_action_run()` reads Forgejo's shape.
- **Only a web address is a link.** Render any stored or derived URL
  through `venture_web_url_is_web()` first.
- **Orders put NULL last, said outright** (`NULLS LAST` in
  `venture-query.c`). SQLite and PostgreSQL disagree by default, and under
  a limit that is different rows, not just a different order.
- **A quote's plan line is billed by its subscription, never by the
  acceptance invoice.** `handoff()` skips lines with a `plan-price-id`;
  invoicing them there as well charges the first period twice. The one
  subscription a quote may start is guarded in the billing service
  (`quote_available()`), not only on the quote, so a start made around the
  quote is refused too.
- **Usage is counted in [start, end) and priced at the rate in force at
  the period's end.** At renewal the subscription has already moved to a
  scheduled price, so `venture-billing-usage.c` reads the rate from the
  last subscription event. Usage before the current period is refused
  rather than billed late: that period's invoice is already issued.
- **A currency code is not three letters.** `VENTURE_MONEY_CURRENCY_LEN` is
  16 and the grammar is `[A-Z][A-Z0-9_]{1,14}`; test a code with
  `venture_currency_is_valid()` (or `_is_normalised()` where the stored
  uppercase spelling matters), never `strlen() == 3`, and never copy one into
  a small buffer -- a four-byte copy turned `GOLD` into `GOL`. Nothing
  truncates a code: `venture_money_new()` refuses an over-long one.
- **Doors to the outside ask `venture_currency_is_iso()`.** Stripe, tax
  filing and the Shopify import settle real money; a registered currency is
  refused there even when it is three letters. "Three letters" alone is not
  the test, and neither is the built-in table (THB is not in it).
- **The currency registry loads deleted rows.** `venture_currency_load_registry()`
  reads every `currency` row including soft-deleted ones, because deleting
  the record must not change how stored amounts display. It reloads on the
  `audit` signal and again on `transaction-finished` (so a rollback takes a
  currency back out), and after `venture_database_migrate()` -- the context
  is built before the schema exists. A scratch database with no context
  leaves the registry alone. Tests that register a currency clear it.
- **Which book a currency posts to is decided once, in the posting
  service.** `venture_posting_service_route_currency()` reads the
  currency's `book-treatment` (`valued`=0, `separate_book`, `memo`) and
  the organization's rate table: memo never posts, the book currency and a
  valued currency *with a rate on the date* go to one book-currency
  journal (the line keeps `amount`, `book-amount` is the valuation), a
  separate-book currency or a valued one with no rate gets a journal of
  its own. `venture_posting_service_post_by_currency()` applies it and
  balances a document spanning journals through currency clearing
  (`currency_clearing` control map, else `<org>:3900`, made on first
  use). Every automatic journal -- sale, expense, refund, inventory pair,
  `create_and_post` -- goes through it; do not post a document with
  `venture_posting_service_post()` and a NULL policy, and do not repeat
  the rule in a report or module: ask the route. The core `post` refuses
  a memo line and any conversion into or out of a separate book whatever
  policy it is handed; reversals are exempt.
- **Recording a rate converts fiat too.** A EUR expense in a USD
  organization with a EUR-to-USD `exchange_rate` now posts one USD journal
  valued at that rate (functional-currency accounting), not a EUR
  journal. Only new postings: a re-save with the same original amounts is
  not reposted because a rate or a treatment changed since -- keep it
  that way, or recording one rate rewrites every past period the next
  time a note is edited. `venture_ledger_save_source()` decides "changed"
  by building the *previous* version's lines under today's rule and
  comparing them (`same_posting()`: accounts, sides, original amounts,
  date) with the new version's, never by comparing the new plan with the
  posted journals: a plan can split into a different number of journals
  after a rate or treatment change, and a plan with no journals at all
  (every amount memo) is still a change to reverse.
- **Which saves post is a registry, never a type check.**
  `venture_ledger_register_source_type()` (type, rule name, postable
  function, date property, flags) is the only way a record type posts on
  save; sale and expense are registered first through it
  (`postable_when_set` on `gross`/`amount`, `occurred-at`,
  `REFUSE_UNPOST`). Never add a `VENTURE_IS_*` check to
  `venture_ledger_wrap_source()` or `_save_source()` -- register the type
  at install time, once per database (twice is refused). **The rule is
  built twice on every save** (stored and new version) to decide
  "changed", so it must be pure: no clock, no counters, nothing written
  that is not idempotent, and the postable function answers from the
  record alone. A version that stops posting (postable FALSE, or its rule
  builds no lines) has its journals reversed and its memo movements
  cleared unless the type carries `REFUSE_UNPOST` -- sale and expense do,
  so clearing their amount is still refused and a zero amount still posts
  a zero journal. Such a save takes the consent boundary when the stored
  version left anything in the books; one that never posted does not.
- **A document line is in its document's currency.** Purchase order lines
  and vendor bill lines are held to their order's or bill's `currency` by
  save validators (`venture_purchasing_install_validators()`), and the
  document cannot move to a currency its lines are not in. An automatic
  sale's cost-of-goods pair is the exception by design: it balances by
  itself in the product cost's currency.
- **Operational reports anchor on the book currency and keep one figure
  per currency.** `pnl`, `ventures` and `monthly` total with
  `venture_money_totals_*` and show a block/row per currency, the book
  currency (`venture_posting_service_book_currency()`) always and first;
  its metrics keep the plain key (`revenue`) and every other currency's
  adds `_<CODE>` (`revenue_TICKET`) -- the headline and dashboard widgets
  read the plain key, so never put a non-book figure under it. Do not
  bring back "the currency most records use": it dropped a gold sale
  among ticket ones, and an empty total made in `NULL`/USD blanked a gold
  organization's profit. A report that still shows one figure per column
  anchors it on the book currency and says in a note what it left out.
- **Statements label a section, they never re-decide it.** The `books`
  column and the section order (book first) come from
  `venture_posting_service_book_label()` and the book currency; a
  converted amount is already in the book section. The close leaves an
  unmatched bank line in a kept-apart currency out with a note, and a
  consolidation leaves out a memo, separate-book or rate-less member book
  with a note -- both used to refuse. Ask the route; do not compare codes
  against the organization's currency and refuse.
- **A market price answers only in its own currency.**
  `venture_market_latest_price()` takes a `currency` (NULL: whatever was
  seen last). The valuing reports read their `currency` option through
  `venture_market_valuing_currency()` and price with
  `venture_market_price_preferring()` (book currency wherever seen, else
  any) or strictly in the named one. The query cannot filter on a money
  field's currency, so the lookup pages newest-first; keep it paged.
- **A holding has one truth per currency.** A holding is an `account`
  with `location_id`. A posted currency's holding is the account's journal
  lines (original `amount`, never `book-amount`); a memo currency's is
  `holding_txn` rows, which `venture_holdings_record_memo()` writes from the
  memo lines `post_by_currency_full()` would have dropped. Never write a
  movement beside a journal line for the same money: the validator refuses
  a hand movement in a posted currency, and `venture_ledger_save_source()`
  leaves an *unchanged* document an earlier treatment journaled alone
  (no movement beside its journal); a changed one is reversed and written
  under today's treatment, journals and movements together. Re-saves
  *replace* a document's movements (unchanged ones kept), session yields and
  transfers *append* -- pick the right one or an edited expense spends twice.
- **Deleting a document keeps its holding movements, as it keeps its
  journal.** Deletion is not a financial correction (`docs/ledger.org`);
  the holdings report notes the deleted documents it still counts. Do not
  make a delete remove or reverse a memo movement unless deleting a posted
  sale reverses its journal too -- one rule for both halves of a holding.
- **The holding floor lives in two places, both before any write.** A memo
  movement is judged by the `holding_txn` save validator, a journal by the
  posting service's `posting` signal (`holdings_posting_guard()`), per
  holding account and original currency. Only accounts with a location are
  judged -- an overdraft on 1000 Cash is a real balance -- and
  `allow-negative` (FALSE, the zero value) lifts it. Reversals are exempt.
  Do not add a third check in a handler or a page. Both judge the
  *running* balance from the change's date on (`holdings_check_floor()`),
  never the all-time total: a back-dated spend that passes today's balance
  is exactly the case to refuse. The rule is "no moment below zero that
  was not, and none deeper than it was", so a dip a reversal or an
  allowed overdraft left behind does not block unrelated changes.
- **A sale honours `cash-account-id` like an expense.** The autojournal's
  `use_cash_account()` swaps the profile's cash account for the document's
  on the sale, refund and expense legs. It sets it on the profile object
  the rule fetched for this call, never on a saved profile.
- **A session money yield posts only into a location's holding.** No
  location, or the ledger off, skips it (not a refusal -- the goods post
  must still work) and it stays unposted for a later post. Either stamp
  (`journal-id`, `holding-txn-id`) makes it posted and frozen; adding a
  third form of posting means adding its stamp to `sessions_yield_posted()`
  and the removal query. Existing sessions' money yields post on their next
  post -- a behaviour change, documented in docs/sessions.org.
- **A word beside a number is a code only if it is three letters or
  registered.** Widening the parser to every short word would read
  `100.00 CR` as a hundred of "CR"; `venture_money_word_is_code()` is the
  one place that decides.
- **Currency writes need an administrator, in the access policy.** The type
  carries the `venture-access-admin-write` qdata, checked in
  `venture_access_policy_can()`; the create route passes `VIEWER` to
  `venture_web_require_for_type()`, so a check there alone would not hold.
- **A category's path is computed, never stored.** `venture_category_path()`
  walks the parents on every call, so renaming one renames every path under
  it with no cascade. Do not add a path column or cache one on a record.
  Display names cannot show it -- `get_display_name` has no database -- so
  pickers show the node's own name; that is documented, not a bug to fix
  with a per-type table.
- **Tree walks are bounded and remember what they saw.** `category` and
  `location` nest through `parent-id`, and with no FOREIGN KEYs only the
  save validator in `venture-category.c` stops a loop. Every walk (path,
  ancestor, descendants, the validator) stops at
  `VENTURE_CATEGORY_MAX_DEPTH` and at a node seen before, so a loop written
  by hand-written SQL ends a request instead of hanging it. A new walk must
  do the same.
- **The category-reference check is derived from the field table.** Any
  `VENTURE_FIELD_REF` to `category`, on any type, is held to the tree's
  `applies-to` by a validator registered on `VENTURE_TYPE_ENTITY`. Do not
  list the types that have a category. A custom `reference` field calls
  `venture_category_check_applies_to()` itself.
- **A custom reference follows the built-in reference rule.** Checked when
  written, never when kept (compare against the previous attribute), and a
  hidden target module is refused with the switch named. It has no GObject
  property, so the form's query-string prefill skips it.
- **A backfill whose target table is a hidden type's creates it.**
  Reconciliation brings a disabled module's *existing* tables up to date but
  never creates one, so sales switched off after use has `inventory_items`
  and no `locations`. `000695` creates `locations` with its field-table
  columns rather than guarding on it, or the backfill would be recorded as
  done and never run once sales came back.
- **The aggregate report fetches and buckets; it never GROUPs BY.**
  Rows come through `VentureQuery` (organisation, access scope, module
  mask) and a money measure is keyed by currency *before* anything is
  added, so no code below the key can add across currencies. Which types
  it reads is derived from `venture_data_class_for_type()` (tenant and
  reference only), not listed. Past `VENTURE_AGGREGATE_MAX_ROWS` it refuses
  with the count; it must never total a truncated set. Buckets are UTC,
  like every period boundary.
- **A report option is dropped unless every door forwards it.** The web
  API and the report page (`strings[]`/`integers[]` and the `names[]`
  that preserve a question across period links), the CLI's allow-list
  after the period, the MCP `venture_report` catalog and argument list,
  and the assistant's `venture_report` parameters are five separate
  lists. An option missing from one is silently absent there -- the
  report runs and answers a different question.
- **Sale rate's denominator is closed listings.** `listing_performance`
  divides units sold on listings whose outcome is not `open` by the units on
  those listings. An open listing is in neither half; a partial one counts
  its sold units as sold and the rest as unsold; a cancelled one counts as
  unsold; nothing closed is an empty cell, never 0%. Rows are group *and*
  currency, keyed by the unit price's currency before anything is added.
- **A listing's outcome and count must agree, and only `sold` fills.**
  `venture_market_validate_listing()` fills `quantity-sold` to `quantity`
  on `sold` when it is 0 and refuses every other disagreement (sold with
  some, partial with none or all, expired/cancelled with any). Do not make
  it guess further. `closed-at` follows the factory's derived-date rule:
  stamped only when empty, kept when given, cleared only on the transition
  back to `open`, refused on a listing that was always open.
- **Price sources match exactly.** `venture_market_latest_price()` and
  `price_history` filter `source` with equality; never widen it to a
  search, or two sources blend into one valuation. The lookup is per
  organization and skips soft-deleted observations; a tie on
  `observed-at` goes to the higher id so repeated calls agree. Nothing
  observed returns TRUE with a NULL price -- the caller says so, it does
  not read zero.
- **A craft never issues through `venture_inventory_service_issue()`.**
  Issue posts cost of goods sold for stock that is still on the shelf, as
  the thing it was made into. `venture_inventory_service_produce()` writes
  PRODUCTION transactions, posts no journal, and gives the output cost
  layers that sum *exactly* to the consumed FIFO cost (split into a
  one-minor-unit-higher layer and a floor layer); one layer at the rounded
  average leaves a cent behind every craft. It consumes what layers there
  are and treats the rest as uncosted, because stock typed in by hand has
  no layer and must still be usable; and it always gives the output a
  layer, even a zero one, or `consume_fifo()` would refuse to sell it.
- **Stock cost is a total per currency, never one `VentureMoney`.** A cost
  layer holds one currency; an issue, a craft, a transfer, the valuation
  and the `inventory` report sum layers with `venture_money_totals_add()`
  and hand every currency on (issue: one pair each, in *one*
  `post_by_currency()` call; memo layers post nothing). Summing into one
  amount is what refused a GOLD+TICKET issue and failed the valuation for
  the whole organization. A zero total is a zero, not a clash.
- **Sibling layers are one lot; FIFO walks lots.** A unit made from GOLD
  and TICKET inputs gets one exact split per currency, all with the same
  `lot_txn_id` (the arrival movement). `consume_layers()` takes a lot's
  units out of every currency together; treat siblings as separate layers
  and the first sale takes only the gold. `lot_txn_id` 0 (every older
  layer) is a lot of its own, so no migration was needed. A transfer
  re-splits each currency exactly at the destination.
- **`reusable` is the flag, and a tool must really be there.** A boolean
  has no default, so the zero value is the safe one: unticked means
  consumed. A reusable component is needed once per craft, not per batch,
  and `allow-negative` does not apply to it. Do not add a `consumed`
  spelling.
- **A craft is a record action judged in its recipe's organization.** It
  needs no `organization_id` parameter; every inventory item is looked up
  in the recipe's organization, at exactly `location_id` (not its
  children), and none or several are refused with what to create or which
  location to name. It never creates the output's item. It is stageable
  because approval re-performs it from parameters, recounting stock then.
- **`recipe_margin` never falls back.** Market on means observed prices
  only, from `price_source` exactly; a product never priced is named and
  its figures blank. Market off means recorded costs, and a
  `price_source` is refused. A recipe priced in two currencies gets a note
  and no money figures.
- **A session yield's `unit-value` is never a cost.** `post` receives
  goods through `venture_inventory_service_produce()` with no draws, which
  writes a *zero* cost layer: nothing was paid for them, and a layer at
  market value books the profit when they were gathered and none when
  they sold. The session's own `cost` is not spread over the units either
  -- `session_performance` nets it off, and it would count twice.
- **`inventory-txn-id` and `posted-at` are the post's to write.** The
  validators let them change only for the object `sessions_save_permitted()`
  names on the database; a hand-set `inventory-txn-id` is refused (it would
  make a yield look posted, or clearing it would post it twice) and a
  hand-set `posted-at` is silently restored (forms post every field back).
  The stamp is the whole of posting's idempotency, so a second post is a
  success that writes nothing -- do not make it an error, and do not hide
  the action when there is nothing new, or a retry is refused.
- **A posted yield and its session cannot be deleted.** The refusal is
  `venture_sessions_check_write()` in the database's subsystem guard list
  (removal), not a save validator -- validators never see a delete.
- **`minutes` follows the times.** Both set: derived. Only minutes
  written: the end is filled. A written minutes that disagrees is refused
  -- but only when it *changed*, because a form posts the stored value
  back after the end was edited.
- **`sessions` requires only core, so its code must survive sales off.**
  Goods yields and `post` refuse with the switch named; the report still
  runs (money yields, list prices); `group_by=location` is refused. Test
  with `venture_config_set_module_enabled(config, "sales", FALSE)`.
- **`session_performance` repeats group counts on every currency row.**
  A group is sessions x currencies; `sessions`, `open`, `hours` and
  `units` belong to the group. Rates divide finished sessions' money by
  finished hours only; an unvalued goods yield blanks the group's value,
  net and rates in every currency rather than reading zero.
- **`session` was on the accounting approval's snapshot exclusion list**
  as a guess at a login table that never existed. It is business data now
  and was removed from that list; do not add it back.
- **A goal's progress is `(current - start) / (target - start)`.** Never
  `current / target`: targets below the start (a weight, a backlog) are
  ordinary. `venture_goals_fraction()` is the one formula; the dashboard's
  `progress` kind gets the same answer only with `options.start_field`.
  A target equal to the start is refused -- 0 to 0 included, because a
  double cannot say "left empty".
- **Nothing writes a goal but its owner.** A step ticked done does not
  move `current-value`, and reaching the target does not set `achieved`.
  Either would be a derived write (a version bump under the caller, a
  number moving by itself) and neither is knowable from the records. Do
  not add one as a validator or a handler; `goal_progress` notes "mark it
  achieved" instead.
- **`achieved-at` and `done-at` follow their state** (`goals_derive_stamp()`):
  stamped when empty, kept when given, cleared on leaving the state,
  refused when typed on a record never in it.
- **Self-nesting trees share one loop check.**
  `venture_category_check_tree_node()` (public in `venture-category.h`)
  serves category, location and goal; a new type whose `parent-id` names
  itself calls it from its validator rather than copying the walk.
- **`goal_step.repetitions` counts crafts, not units,** and
  `goal_materials` counts a reusable component once at the largest single
  step's quantity -- never multiplied, never summed across steps. On hand
  is every location. The list is gross (intermediate outputs are not
  netted), a missing price blanks that line's cost and relabels the totals
  "Total of priced lines", and the report refuses with production off.
- **The generic reference check fires before a module's own.** Writing a
  reference to a type whose module is off is refused by the reference
  check ("belongs to the production module, which is disabled") before any
  save validator runs, so a validator's own "module is off" refusal for a
  newly written reference is a backstop. Tests should match on the module
  name, not on either sentence.
- **A currency nobody named is the book currency, never "USD".**
  `venture_database_get_book_currency()` is the organization's book
  currency, or the install's default with no organization; it never
  returns NULL. A literal "USD" fallback passes every USD test and writes
  dollars into a euro or gold organization -- budgets, payroll, quotes,
  capture, documents, setup and an action's bare money parameter all did.
  What is USD by law (the US sales-tax adapter, 1099-NEC, Stripe ACH) says
  so in a comment and refuses other currencies by name. The generic
  decoders read a bare amount in the install default and *mark* it; the
  save (and staging) re-reads marked fields in the record's book currency
  via `venture_database_resolve_bare_money()` -- in the record's own
  `currency`, else a referenced document's, else the book currency. A
  service that decodes a body and then does arithmetic before saving must
  resolve first; the posting service does so on entry, which is what
  keeps a bare-amount `create_and_post` from planning a USD journal with
  EUR lines.
- **Sum run costs with `venture_money_sum_dominant()`.** `venture_money_sum()`
  refuses mixed currencies and returns NULL, which silently blanked the
  totals the day one run was priced in another currency.

## Dashboards

- **A widget kind writes its JSON and its HTML from one loop.** Two
  renderings from two code paths are two answers; the assistant reading
  `data` while the operator looks at the card is the disagreement a
  dashboard exists to prevent. Add a kind to the table in
  `src/core/venture-dashboard.c`, list the fields it reads in `uses` (the
  editor shows only those), and never render a kind in the web layer.
- **`venture_dashboard_render_widget()` never returns NULL.** A widget that
  cannot be answered is a result with `error` set, rendered in place. A
  page is a set of independent questions.
- **The widget validator accepts a type or report whose module is off.**
  Use `venture_entity_registry_lookup_any()` there, not `_lookup()`; a
  factory dashboard must survive the factory being switched off. The
  kind's own `module` is what makes it say "off" at render time.
- **A widget never scopes itself.** The scope (`VentureWidgetScope`) comes
  from the caller -- the sidebar picker on a page, `?organization_id=` on
  the API, nothing for the assistant -- so one widget answers about the
  same rows through every door. `{me}` is the scope's username.
- **Before a sidebar pick, a dashboard page opens in the organization the
  dashboard is filed under.** `venture_web_dashboard_unpicked_organization()`
  is the page choosing the scope per request, not the widget scoping
  itself; a pick (the `venture_entity` cookie, `all` included) is always
  authoritative, and the API still scopes by `?organization_id=` alone.
  File a business's dashboard under that business's organization rather
  than telling people to switch the sidebar first.
- **Import and from-template file under the organization named.**
  `POST /api/v1/dashboards/import?organization_id=N` and
  `/from-template` (`"organization_id"` in its body, or the query) go
  through `venture_web_request_organization()` like the Trading routes --
  a stranger is told the organization does not exist, a viewer member is
  refused by the record policy -- then `venture_dashboard_import_in()`.
  With none named it is the default organization, as it always was: a
  definition is never read for one, and a session's sidebar pick is not
  consulted, so the CLI and an old script keep their meaning.
  `venturectl dashboard create|import ... organization_id=N`.
- **A report widget gets the scope as report options.**
  `venture_widget_run_report()` hands the report `organization_id` (the
  scope's first entity) and `venture_id`; a new kind that runs a report
  must go through it, or its card answers for the default organization
  beside cards that answer for the page's. Widget `options` reach the
  report only when the report's parameter schema declares them, and the
  save refuses the rest -- `organization_id`/`venture_id` always. A report
  that needs a widget option must declare it in `describe_parameters`.
- **Dashboards and widgets are checked at the save, like everything.**
  Slug uniqueness and the single home page live in the dashboard's save
  validator, which writes the other dashboards from inside the lock (it is
  recursive); the kind, type, report and options checks live in the
  widget's. The web handlers do none of this themselves.
- **`/` is the home dashboard when one is visible, `/overview` is always
  the built-in page.** A test that asserts on the home page must know
  which it is looking at; `tests/test-auth.c` and `tests/test-plugin.c`
  list both.
- **The grid is resolved at render, never trusted from the rows.**
  `venture_dashboard_layout()` re-flows anything that overlaps or falls
  off the edge; `venture_dashboard_place_widget()` is the only checked
  writer and refuses a cell another *placed* widget holds -- a flowed
  neighbour is not an obstacle, it flows around. Do not add a second
  overlap check in the web layer, and do not make the save validator
  refuse a placement that does not fit today's layout: the layout can
  change after the widget was placed.
- **`sum` and `progress` share the aggregate report's accumulator.**
  `venture_aggregate_sum()` is the one place a widget adds up a field, so a
  card and `report aggregate` cannot disagree; do not total money in the
  kind itself. Their `period` bounds `options.date_field` and a period with
  none is an error in place, never an all-time figure. The save validator
  checks their fields only while the type is visible.
- **Templates are written in the export format.** A template is what an
  export of the dashboard it makes would be, and `test-dashboard` imports
  every one, so a template naming a field that does not exist fails the
  suite rather than a user.

## The workdesk

- **The inbox is fed by the audit signal, and only there.**
  `src/core/venture-notify.c` listens to `VentureDatabase::audit`; a
  mention, an assignment, a watched change and a finished run all become
  notifications from that one handler, so every writer tells the same
  people. Never send a "watched" notification from a page handler. The
  handler skips the system actor's updates -- a first-reply stamp, a
  logged-hours total, a breach mark -- or every event would be told twice.
  Notifications about `notification`, `watch`, the audit log and chat are
  refused by type, which is what stops the loop.
- **Derived writes bump the version.** The first-reply stamp
  (`venture-sla.c`), the logged-hours roll-up (`venture-desk.c`) and a
  budget's warned-at (`venture-factory.c`) are saves of the record, so an
  object read before them is a version behind and its save conflicts.
  `venture_desk_apply_macro()` saves the ticket *before* its reply for this
  reason; a test that comments on a ticket must re-read it before saving
  it again. Four tests in `tests/test-desk.c` were written after hitting
  exactly this.
- **A service level's clocks are set once, on the first save, and never
  moved.** The validator returns early when either due time is set or when
  `previous` is non-NULL. Do not "fix" a stale deadline by recomputing it
  on priority change -- a deadline that moves is not a deadline.
- **The sweep is lazy and bounded.** `venture_sla_sweep()` runs from the
  board, the inbox and `POST /api/v1/sla/sweep`, never from a timer: no
  background thread may touch the database.
- **A bulk edit is one transaction and re-uses the single-record path.**
  `venture_desk_bulk_update()` calls `venture_entity_set_field_from_string()`
  and `venture_database_save()` per record inside one `begin`/`commit`, so
  every validator and every audit entry is exactly what a form would
  produce. Do not add a SQL `UPDATE ... WHERE id IN` fast path.
- **A budget sums the runs' own `cost`; nothing stores a running total.**
  `venture_factory_budget_allows_run()` is asked from
  `venture_work_service_start_for_ticket()` before the rule is resolved.
  The warned-at and exhausted-at stamps are compared against the window's
  start, which is how a warning is sent once per window rather than once.
- **Inbox and watch rows are owner-only through the generic routes**, like
  chat; `/inbox`, `/api/v1/inbox` and the watch button filter to the caller.
  `tests/test-auth.c` lists every new route.

## Webhooks out, routing and the assistant at the desk

- **Outbound delivery is asynchronous on the main loop, never a thread.**
  `venture_webhook_send()` fires from inside a database write; a blocking
  POST there would make every save in the program wait on somebody else's
  server. The completion callback writes the delivery record on the main
  thread, which is also why it holds a reference on the context.
- **`venture_webhook_test()` waits in a nested main loop rather than
  blocking.** It has to return an answer -- somebody pressed Test -- but
  `soup_session_send_and_read()` stops every source in the process,
  including the one serving the far end when the far end is this install.
  That is the first thing an operator tries, and it deadlocked until the
  timeout. A nested loop on the main thread is safe here for the reason
  the automation engine's are not: no handler of ours is on a worker
  underneath it.
- **The quiet list is what stops the loop.** A webhook about a
  `webhook_delivery` would fire a webhook about a webhook. `webhook`,
  `webhook_delivery`, `notification`, `watch`, the audit log, chat, the
  KB's derived rows, `api_token` and `user` are never published. Adding a
  type that the sender writes means adding it there.
- **A signature is an HMAC-SHA256 over the exact body bytes**, hex,
  prefixed `sha256=`, in `X-Venture-Signature` -- deliberately the same
  shape VENTURE verifies inbound from a forge. Do not change one without
  the other.
- **Ten consecutive failures switch a webhook off** and tell the admins.
  There is no retry: a retry that is not idempotent at the far end is
  worse than a gap, and the delivery id is what lets a receiver decide.
- **`webhook` is owner-only and `webhook_delivery` refuses writes**, via
  `venture_web_require_for_type()` and `venture_web_type_accepts_writes()`.
  The row names the host this install's data is posted to and holds the
  secret; the delivery is evidence. The assistant is given no webhook tool
  at all, exactly as it is given no way to read a forge token.
- **Routing is a save validator, not a signal handler.** Assigning after
  the write would be a second save, a second audit entry and a
  notification about a change nobody made. It only ever fires when
  `previous` is NULL and the assignee is empty.
- **The three ticket judgements run on the toolless executor.**
  `VentureAiService` holds a second `AiToolExecutor` that never gets a
  tool, and `venture_ai_service_complete()` runs against it. A ticket's
  text is a stranger's writing; every prompt in `venture-ai-assist.c` also
  says so and says to ignore instructions inside it. Do not route these
  through `self->executor`.
- **A triage is a proposal and a draft is a draft.** `..._apply_triage()`
  goes through `venture_entity_set_field_from_string()`, so an invented
  enum value changes nothing, and it merges tags rather than replacing
  them. Nothing in this file posts a comment or saves a ticket -- the
  caller does, under the ordinary write policy.

## The dashboard grid is a canvas

- **The editor measures the cells; it does not do arithmetic on one cell's
  size.** `.widget-grid` is `grid-auto-rows: minmax(150px, auto)`, so rows
  are as tall as their contents and there is no such thing as "the" row
  height. Multiplying a pointer offset by the first cell's height put a
  card in row 15 of a nine-row page. `geometry()` reads every
  `[data-cell]` rect and `slotAt()` hit-tests against them.
- **A cell with no measurable box means the grid has collapsed** to one
  column under 900px, where a position means nothing. `geometry()`
  returns NULL and no drag starts. Without that guard the same
  division-by-nothing produced nonsense placements on a phone.
- **A move follows the pointer with a transform; a resize changes the
  card's own span live.** The transform neither reflows the grid nor
  fights the placement already in the card's style. Live resizing is safe
  only because every card on an editable grid carries an explicit
  `grid-column`/`grid-row`, so nothing flows around the one that is
  growing -- do not rely on that anywhere cards are auto-placed.
- **Swap exists because placement refuses a taken cell.** On a tidy page
  there is no hole to move a card through, so dragging one card onto
  another has to mean something. `venture_dashboard_swap_widgets()` writes
  both in one transaction -- saving the first alone would land it on cells
  the second still holds -- and refuses two of different sizes rather than
  guessing where the odd one goes.
- **The pointer gestures are a nicer way to reach the same endpoints.**
  Every drag ends in the POST a nudge button sends, and the nudge forms
  stay for the keyboard and for no scripting. Do not add placement rules
  to the client: it may predict a refusal to colour the ghost, but the
  server decides, and a refusal puts the card back where it was.

## The demo

- **`make demo` is `tools/venture-demo.sh`, and it seeds through the API.**
  Every record goes in with `venturectl`, so a demo that seeds is also a
  demonstration that the REST API can build one. Nothing in it reaches
  past the server into the database.
- **It listens on 8749.** 8747 is the config default and the container;
  8748 is `just start`. It refuses to start when something else already
  answers on its port, because seeding example data into a real instance
  is the one mistake it must never make -- a demo of ours it will stop and
  replace, anything else it will not touch.
- **`build/demo` is deleted and rebuilt on every run.** The demo is the
  same demo every time and nothing anybody does to it matters.
- **Progress goes to stderr.** Several seeding functions hand an id back on
  stdout; a progress line printed there becomes part of the answer, and
  the first run tried to create a company called "==>".
- **Every `cd` sets `CDPATH=''`.** A `cd` that resolves through CDPATH
  prints where it landed, which inside a command substitution becomes
  part of the result -- and CDPATH is set in any shell configured to jump
  around by name. The repository root came back as two lines.
- **The server is a child of the script.** `start_server` backgrounds a
  subshell and `exec`s venture into it. `wait` only reaps children; a
  server started *inside* the subshell is reparented when that subshell
  returns, `wait` returns 127 at once, and the EXIT trap then kills the
  instance. `make demo` would print a port and leave nothing listening.
  `tests/demo-foreground.sh` is the check.
- **Relative dates count from one anchor, never from the clock each time.**
  `at_hour` and `at_minute` used to read the wall clock and round their
  own answers, so a spend at "26 hours ago, on the hour" fell before the
  session "1610 minutes ago" that funded it in the last ten minutes of
  every hour, and the purse refused to go below zero. They now count from
  `set_economy_clock`'s anchor (the current UTC hour, or
  `VENTURE_DEMO_ECONOMY_NOW`), so the order of two dates is the order of
  their offsets. `tests/demo-clock.sh` replays the economy at awkward
  clock times and checks every purse is funded before it is spent. The
  same class of bug hid in the bank feed: a statement ending five days ago
  refused a row dated the first of the month on the 1st to 5th.
- **The enum values are checked against `venturectl describe`, not
  guessed.** Four of them were wrong on the first run: a campaign is
  `running` not `active`, an idea is `researching` not `exploring`, a
  build trigger is `webhook`/`rule` not `push`/`schedule`, and
  `venture_type` is a registered type (`books`, `etsy`, `newsletter`,
  `virtual_economy`, `general`), not free text -- and since the save
  validator, a type written that is not registered is refused outright.
- **The demo has a second organization, and every lookup must say
  which.** `seed_virtual_economy` files an auction-house trade under
  "Evermoor Trading" (book currency `GOLD`, a user-defined currency with
  g/s/c coins). Its sales post to a chart of its own, so an unscoped
  `list account code=1000` can answer with *its* account: `account_id`,
  the opening journal and the fiscal-period lookup pass
  `organization_id=${home_org}` (the `is_default` organization). A new
  lookup must too, and every record the second organization owns passes
  `organization_id` or it lands in the default one.
- **Stock that the demo sells needs a cost layer.** A sale of a product
  with an inventory item issues FIFO layers and refuses units with none,
  and an adjustment has none. The game economy's goods arrive through
  posted sessions (zero-cost layers) and crafts, and the listings and
  sales are seeded *after* both. Only craft inputs get opening
  adjustments.
- **The demo's market data is generated, deterministically, from the
  economy's anchor.** `tools/venture-demo-market.sh` writes the
  JSON-lines files every demo data source reads (`build/demo/market`, the
  only `feeds.file_roots`; no allowed origin, so nothing reaches the
  network). Prices come from a seeded integer hash -- never `$RANDOM`,
  never the clock -- and listing generations count from the anchor, so
  `--days` changes how far back the history goes and never the newest
  snapshot. The weekly rhythm makes prices depend on the weekday: only an
  anchor a whole week later gives the same bytes, which is what
  `tests/demo-market.sh` compares. Its planted spreads must hold in every
  snapshot (the alert, the scans and the trades are written against
  them), so planted cells never stand empty between listings.
- **Evermoor is worked with `organization_id=N`, like a person would.**
  A token's active organization is always the default one, so every
  Trading verb for Evermoor names it: `feeds sync ID --wait
  organization_id=N` (`sync_source`), `market promote SOURCE venue|instrument
  KEY organization_id=N` then `update` for what the store does not say
  (fees, location, product), `arbitrage record ... organization_id=N`.
- **Every purse a trade spends is funded first, in date order.** A
  bookmaker wallet is a holding: the demo funds it with a posted journal a
  hundred hours before the stakes. Brisk's purse holds only what three
  farming sessions earned (7g 23s), and the flip and the craft leave him
  1g 91s -- raise a trade's quantity and the holding floor refuses it.
  `tests/demo-market.sh` replays every purse and wallet from the recorded
  calls; its recorder writes a value's newlines as spaces, because a
  multi-line `fee_params` split one call into two and the venue lost its
  location.
- **Trades get their own inventory items, and a sell of goods still to
  be made is recorded `planned`.** First in, first out would issue the
  guild bank's zero-cost potions against the craft trade; the sell leg is
  executed (`arbitrage execute`) after the `craft` action, which takes
  `location_id` and finds every input and the output at exactly that
  location.
- **The exec plugins are copied into the state directory.** The supplier's
  snapshot is dated by its CSV's mtime; loaded from the checkout it is as
  old as the clone and the Venues page calls it overdue. `start_server`
  copies `plugins/exec` to `build/demo/plugins` and points
  `plugins.paths` there.
- **The surebet preset's `max_age_hours: 6` is about stale odds, not
  finished matches.** The scan leaves a started match out by its
  `commence_time`; six hours keeps a price nobody would still bet at off
  the Trading desk.
- **`(( x = 0 ))` is a failing command under `set -e`.** The generator
  assigns with `x=$(( ... ))`; a hash or an age of zero ended the run
  silently after the first realm's header.
- **Evermoor's accounts go in through the push door, as tsmctl's would.**
  `tools/venture-demo-accounts.sh` writes one push body from the anchor
  (it sources the market generator for the realms, items and hash) and
  `seed_evermoor_accounts` sends it with `feeds push ... --wait
  organization_id=N` to a `push` source whose namespaces are the market
  source's (`realm`, `item`): otherwise its positions name venues and
  instruments nobody promoted and no listing finds a product. Brisk,
  Tallow and the guild bank are *adopted* (`update location ...
  external_ref="evermoor:<account key>" data_source_id=...`) before the
  push, so the mirror reuses them and the books post into the purses the
  economy already has. Books mode is `daily` (`accounts post`, before
  `seed_periods` closes anything); trades mode is documented, not seeded.
  The mirror runs a moment after the push's run is written:
  `wait_for_mirror` polls the run's notes for "mirror:".
- **Every balance in the accounts body is the ledger replayed.** The
  generator moves each account's gold by its own rows and skips a buy the
  purse cannot pay; an opening is dated before the first row (or at the
  account's sighting, for an alt seen before the ledger starts -- a balance
  after the snapshot that restates it was the bug `--days 7` found).
  `tests/demo-accounts.sh` re-reads the body independently at awkward
  anchors: contract members, places, decimals, zoned times, snapshot
  before rows, ledger oldest first, every balance equal to the replay,
  never below zero, and the story the docs tell (auctions waiting for a
  login, some running out within twelve hours, a mail about to be lost, a
  stale alt, moonsteel flips).

## The assistant panel

- **A question carries its page.** The composer posts `context` (the
  path); `venture_web_chat_describe_context()` turns a record page into
  the record's readable fields and any other page into its name, prepended
  to the model's turn only. It repeats the page's own permission check
  because the path comes from the browser. The transcript keeps
  `[While viewing /e/<type>/<id>]` for record pages and nothing otherwise;
  the client's optimistic echo mirrors that rule.
- **Starters are server-rendered per path** in
  `venture_web_chat_append_starters()`, only when an AI service exists;
  the script hides them once the log has a message or the thread list.
- **The reply renderer is escape-then-format.** Fences are cut out before
  the prose pipeline so nothing inside them is a list or bold; links match
  only `https?://` or `/` targets. `tests/test-auth.c`
  chat-reply-rendering pins this, including that `<script>` in a fence
  stays text.
- **History is capped at the tail** (`VENTURE_WEB_CHAT_HISTORY_CHARS`,
  `_MESSAGES`) as a view over the full list; the full list must outlive
  the handler, which is why `all_history` and `history` are two arrays.
- **Rename and export** are `/ui/chat/thread/:id/rename` (JSON) and
  `/export` (org, verbatim bodies, `*`-led lines escaped with a comma),
  scoped like reading: another person's thread is NOT_FOUND.
- **A streamed answer is two requests.** `POST /ui/chat` with `stream=1`
  parks a `VentureWebChatTurn` (history refs, images, staged-before, the
  model text) behind a one-shot token and returns an empty bubble carrying
  `data-chat-stream`; `GET /ui/chat/stream/:token` opens an
  `HtmxSseConnection`, returns `htmx_response_new_streaming()` so the
  router leaves the message alone, and answers. Events are `delta`,
  `status` and `done`; `done` carries the fully rendered bubble and
  replaces what the deltas painted. Without `stream=1` the blocking path
  answers, which is what the tests, the CLI and a no-script form take.
- **The stream owns a copy of the principal.** The request returns long
  before the model does, and `VentureAuthPrincipal` is a plain struct with
  no refcount, so borrowing it is a use-after-free the moment a tool stages
  a write.
- **One AI turn at a time, per service.** `venture_ai_service_answer_stream_async()`
  refuses with `VENTURE_ERROR_CONFLICT` while `streaming` is set: the
  executor's stream flag, its tool list and `current_prompt` are single
  slots. The blocking path enforced this by holding the main loop; going
  async made it something that has to be said out loud.
- **`ai_tool_executor_set_stream()` is set per turn, not once.** The same
  executor answers the CLI and MCP, which have nowhere to put a delta.
- **Every `<select>` is enhanced into a themed picker** by `wirePickers()`
  in venture.js; the native element stays in the DOM, keeps its name and
  is still what the form posts, so every existing `change` listener and
  the no-script path are untouched. Opt out with `data-no-picker`.
  Floating panels (`.picker-panel`, `.record-results`) are
  `position: fixed` and placed by `placeFloating()`, because `.card` has
  `overflow: hidden` and clipped them.
- **An error notice is announced.** `<div class="notice negative" role="alert">`
  and the error page's `role="alert"` are what a test asserts on now;
  toasts go into the server-rendered `<div class="toasts" role="status"
  aria-live="polite">`. A button showing only a glyph needs an
  `aria-label` -- `tests/test-record-view.c` walks every button on the
  main pages and fails on one without a name.
- **The phone menu is folded by script, drawn open.** `.sidebar` gets
  `menu-closed` only on a narrow screen, from the inline script after the
  sidebar, so with scripting off every link is still reachable.
- **Never put a class name a test greps for inside venture.js.** The
  script is inlined into every page, so `"notice negative"` in a JS string
  made a dashboard test find an error notice that was not on the page.
  Build dynamic notices as DOM nodes, and assert on markup
  (`<div class="notice negative">`), not on two words.
## Forms and refusals

- **A PDF header is two columns and text keeps to its column.** Use
  `venture_pdf_writer_fit_size()` and `venture_pdf_writer_wrap_aligned()`
  for anything a person typed; a fixed-size `_text()` call is only for
  labels and figures whose width is known.
- **A person never sees error JSON.** `venture_web_error_response()` tags
  the response with its `GError`, and the catch-all middleware turns it
  into a page for a browser that asked for HTML without `HX-Request` or
  `X-Venture-Inline`. The page's own script posts ordinary forms with
  `X-Venture-Inline` and shows the refusal above the form. Do not add a
  per-route HTML error path; `tests/test-record-view.c` pins all three
  answers.
- **A refused save always says why.** `venture_database_get()` answers a
  missing row with NULL and *no* error, so a check that does
  `x = get(...); if (!x) return FALSE;` refuses without a reason, and the
  person saw a 500 "Unknown error" (an invoice line with no invoice did
  this). Name the field with `venture_set_error_validation()`.
  `venture_database_save()` now turns a FALSE with no error into
  `VENTURE_ERROR_FAILED` naming the record, plus a `g_warning` -- fatal in
  tests, so the check that did it is found; `/record-view/save-refusal-says-why`.
- **"Attention of" is a flag, not a check in a handler.**
  `VENTURE_COLUMN_FLAG_SAME_PARENT` on a reference makes the save refuse a
  target under another parent and the form narrow its options; the parent
  is derived by `venture_entity_class_get_shared_parent()`. Never let it
  pick `organization-id`. Deals, tickets and sales orders carry it too; an
  empty parent on either end is not a mismatch (imports match people with
  no company yet).
- **An invoice line is taxed by a rate record** (`tax-code-id`), and so is
  a quote line -- the accepted quote copies it onto the invoice, so the
  two totals agree. The rate is made at
  `/tax-rates/new` from a percent and stored as an exact fraction. The
  exemption lives on the customer; the invoice freezes it at issue.

## The agent harness

- **Two different things are called a harness.** `/harness` is the agent
  harness: a coding agent in a workspace, driven a turn at a time, in
  `venture-work-service.c` alongside runs. `/assistant` lists the chat
  harness's commands. Do not merge them; one changes files, the other
  answers questions.
- **A session keeps nothing in memory between turns.** Every turn rebuilds
  the provider, the executor and the message list from the stored
  `agent_turn` records, which is what makes a session survive a restart.
  The only live state is a `GCancellable` in `session_live`.
- **Where a session may work is decided on the main thread** in
  `venture_work_service_session_open()`, because it reads config and a
  repo record. `forge.workspace_roots` is empty by default; paths are
  compared after `realpath()` and the separator check matters
  (`/srv/venture-secrets` is not under `/srv/venture`).
- **The model catalogue is generated, never written.**
  `tools/venture-models.sh` reads `AI_*_MODEL_*` defines out of
  `deps/ai-glib/src/providers/*.h` into `$(OUTDIR)/venture-models.h`; the
  provider list is walked out of `AiProviderType` via
  `ai_provider_type_to_string()`. Do not hand-edit either list -- add the
  model to ai-glib. Prefix matching is longest-first, or
  `AI_CLAUDE_CODE_MODEL_*` files under `claude`.
- **Effort is per provider, not per model**, because that is how ai-glib
  passes it (a CLI flag). `cursor` is the trap: a CLI provider with *no*
  effort flag, since the level is in the model id.
- **`[hidden]` needs `!important` here.** `.field` sets `display: flex`,
  which beats the attribute; a field the script had hidden stayed on the
  page looking operable until that rule was added.
- **A CLI provider gets no tools from us** (its subprocess has its own);
  an API provider gets `venture_work_tools_register()` rooted at the
  workspace. Neither gets `ai_tool_executor_new()`'s built-ins.
- **`session-output` and `session-finished` are emitted on the main
  thread** and the SSE route forwards them, filtered by session id. The
  `done` event makes the page reload rather than rendering the turn in
  JavaScript, so a transcript has one renderer.

- **The harness is `src/ai/venture-ai-harness.c`**, ai-glib's
  (`AiResourceRegistry` -> `AiCommandSet` -> `AiCompletionContext`, the
  shape `ai-tui` uses) wired to a browser. It owns completion for `/`,
  `@` and `#`, command expansion, `@type/id` mention expansion, and
  `venture_ai_harness_describe_record()` -- the one place a record is
  written out for a model, which the page context also calls.
  `venture_context_get_ai_harness()` builds it on first use.
- **The harness never decides permissions.** It takes a
  `VentureHarnessAllowFunc`; the web layer passes one that calls
  `venture_web_require_for_type()`, because which role a type needs is
  that file's policy and a second copy would go stale.
- **Shell escapes in command bodies are off** (`AI_COMMAND_SHELL_NEVER`).
  Those directories are shared with every agent tool on the machine and
  this process answers a port.
- **`/ui/chat/complete` takes `buffer` and `cursor`**, not a fragment, and
  answers with a kind, a byte range and items. The client recomputes the
  range in UTF-16 units because a textarea slices in those; the server's
  range is in bytes and only the kind and items are used.
- **Skills live in `src/ai/venture-ai-skills.c`.** Built-ins are a static
  table there; `ai_skill` records (chat module) add to it and a record's
  trigger shadows a built-in's. `venture_ai_skills_expand()` runs on the
  typed message in `/ui/chat` and replaces only what the model sees; the
  transcript keeps the slash line. `/ui/chat/complete` feeds the composer's
  `/` and `#` menus (skills, and knowledge bases by slug for a viewer who
  may read them); the client-side commands are listed in venture.js, not
  the endpoint, so the menu cannot offer what typing could not do.
- **The client guards one send at a time** with a capture-phase submit
  listener that runs before the hx runtime; slash commands are handled
  there too and never reach the server.

## Comments: a discussion on every record

`comment` (core module) names its record by type and id, like
`ticket_relation`, so its panel, routes and API are hand-written
(`src/web/venture-web-comments.inc`) and its rules live in
`src/core/venture-comment.c` and `comment_allows()` in the access policy.
`docs/comments.org` has the whole of it.

- **Every type takes comments unless its class says otherwise.**
  `venture_entity_class_set_commentable(klass, FALSE)` in the type's own
  `class_init` is the opt-out; never a list in the panel or the service.
  A ticket opts out because `ticket_comment` is its conversation (internal
  notes, the portal, the first-reply clock); do not draw the generic card
  beside it.
- **The audit diff is taken before the save validators run.** A field a
  validator derives (`mentions`, `edited-at`) is written but never appears
  in the diff, so nothing may key on it there: the inbox works out who an
  edit newly names by resolving the diff's old `body`, not by diffing
  `mentions`. The same holds for any derived field anywhere.
- **Reference checks run before the save validators, under the writer's
  scope.** A field the validator stamps from the principal must not be a
  `REFERENCE`: a client that sent somebody else's id would be refused
  ("points at user #1, which does not exist") before the stamp could
  overwrite it. That is why `author-user-id` is an integer.
- **The author is never an input.** The validator overwrites `author` and
  `author-user-id` from `venture_access_policy_get_actor()` on insert and
  refuses a later change; only trusted internal work with no principal
  keeps what it set.
- **`comment_allows()` applies to administrators too.** It sits before the
  `!administrator()` block: an owner may delete anybody's comment but may
  never edit one. Reading and writing are the record's read; a refusal is
  NOT_FOUND. Viewers comment without a proposal -- `role_proposes()`
  excludes comments, and the generic routes refuse the type (writes via
  `venture_web_type_accepts_writes()`, reads owner-only via
  `venture_web_require_for_type()`), because an approved comment would be
  posted under the approver's name. A read-only token stays read-only.
- **A mention is only somebody who may read the record.**
  `venture_comment_resolve_mentions()` reads names with the markdown
  renderer itself (so code is never a mention) and keeps only active
  users the policy lets read the subject; everybody else is plain text
  and told nothing. Refusing the comment instead would tell the writer who
  exists and who may read what. The inbox checks each recipient again at
  the moment of telling, and the @ menu offers only readers.
- **A private record takes no comments.** Its inbox lines and webhooks
  would publish an excerpt of it; `venture_comment_subject_accepts()` is
  the one place that says so.
- **Markdown is escaped character by character, never by a regex over the
  source.** `src/util/venture-markdown.c` writes only its own tags; a link
  is written only to `http(s)`, a `/path` or an `#anchor`. The chat
  panel's renderer is separate and older, with pinned output; it was not
  folded in. Its closers are cached per call so a wall of delimiters stays
  linear.
- **The page posts in place, and the script must get there first.** The
  page's inline-form handler follows a redirect with `fetch()`, whose
  `response.url` drops the fragment, so a comment would land at the top of
  the page. `discussion_submit()` listens in the capture phase and asks for
  the card again with `X-Venture-Discussion`; the server answers with the
  card and `X-Venture-Comment: N`, and the card is swapped whole -- one
  renderer. Its hooks are `data-discussion-*` attributes, never class
  names.
- **A comment has no page.** `/comments/N` is the permalink (302 to
  `/e/TYPE/ID#comment-N`, or not found), and `/e/comment/N` redirects to it
  before the type gate, so the audit log, a search and an old webhook land
  on the comment. The inbox, `venture_notify_to_json()` and a webhook's
  `record.url` give `/comments/N` for a comment target.
- **The demo's colleagues post as themselves through `--session-file`.**
  An editor may not mint a token, and a comment's author is the
  credential; Bob, Carol and Dave are finance members (the trades they
  discuss are books) with a random password used once to sign in. A
  body goes on standard input (`comments add TYPE ID -`), never in argv:
  `venturectl` parses argv with GOptionContext, which refuses UTF-8 it
  cannot convert in a C locale -- the docs quickstart runs the demo in
  one, and an em dash in a comment failed it.

## The interface

- **Two looks, one markup.** `data/static/venture-industrial.css` is the
  instrument panel (the default): tactical telemetry on the dark ground,
  Swiss print on the light one, Catppuccin Mocha as a skin.
  `data/static/venture-classic.css` is the editorial design that came
  first. `ui.look` picks the default and the `venture_look` cookie (set by
  POST `/look`, the switch in the sidebar footer) overrides it per browser;
  `venture_web_append_stylesheet()` inlines exactly one. Both share every
  class name and every theme, so a page or widget is written once and
  must be checked in both -- the tests grep for "instrument panel" and
  "warm monochrome", one comment unique to each file, to tell them apart.
  Every rule is written once against tokens; a theme is a block of values.
  Never write a literal colour into markup or a plugin stylesheet -- use
  the tokens in `docs/interface.org`.
- **Two registers of type, and prose.** Headlines are the heavy uppercase
  grotesque (`h1`, `h2`, `.stat-value`, `.figure`). Every label is the
  micro register: small uppercase mono, tracked out (`th`, `.field-label`,
  `.card-head h2`, `.nav-item`, `.badge`, `.btn`). Anything a person reads
  -- a body, a comment, a subtitle -- stays sentence-case sans. Do not
  uppercase prose and do not set a label in the sans.
- **No radius, no gradient, no shadow.** The `--radius-*` and `--shadow-*`
  tokens exist and resolve to zero and none. A floating surface uses
  `box-shadow: var(--ring)`, a hard double ring.
- **Compartments share their rules.** `.grid`, `.bento`, `.stat-row` and
  `.desk-sla` use `gap: 1px` with a one-pixel `outline` on every cell, so
  neighbours have exactly one line between them. A painted parent
  background would fill a short last row with a block; the outline trick
  does not.
- **Framing is CSS, not markup.** `/// SECTION`, `>>> EYEBROW`, `[ CARD
  HEAD ]`, `#tag` are pseudo-elements. Tests that inspect the HTML see
  plain text, and a test that asserted the brackets would be asserting the
  stylesheet.
- **Red means look here, and nothing else is red.** Links, focus, the
  active rail, unread, the cursor row, the ghost. Negative readings are
  the accent too, so there is one red. Green and amber appear only on a
  status. The primary button is ink.
- **The scanline and grain layers are `body::before` / `body::after`** at
  z-index 9000, above every overlay, pointer-events none, hidden in
  print. A new fixed overlay does not need to go above them.

## Inbound mail

- **No IMAP reply may grow with the mailbox.** A `UID SEARCH` answer is one
  line naming every UID; it passed the 8 KiB line cap at about 1,200
  messages and that inbox never synced. List with paged `UID FETCH a:b (UID
  RFC822.SIZE)`, and read lines with the bounded reader in
  `venture-imap-client.c`, never `g_data_input_stream_read_line()`, which
  buffers the whole line before anything can refuse it.
- **A blocking socket on the main loop needs a deadline, not a timeout.** A
  per-read timeout lets a server that trickles a byte just inside it hold
  every request forever. The IMAP client re-arms each read with what is left
  of the call's time budget; a new blocking client must do the same.
- **`VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION` indexes include soft-deleted
  rows.** An upsert that looks up only live rows inserts a duplicate of a
  deleted one and fails the index on every retry -- a deleted unmatched
  sender stalled its mailbox this way. Look up with
  `venture_query_set_include_deleted()` and restore.
- **After rolling back a transaction that saved a record, re-read it.** The
  save bumped the object's version before the UPDATE the rollback undid, so
  the next save of that object conflicts with itself. The sync's
  `refresh_account()` exists for this.
- **One message must not stall an account.** The sync classifies: the
  session or a locked database stops and retries; a failure that is the
  message's own spends an attempt, and the third files a stub with
  `skip_reason`. A new per-message step must fail with an error code
  `error_is_transient()` classifies the right way.
- **The outbox commits `sent` before the timeline.** The relay already has
  the message; rolling its state back because a CRM write failed made the
  row `uncertain`, and the operator's retry mailed the customer twice. Keep
  `venture_mail_sync_record_outbound()` after the commit and non-fatal.
- **IMAP tests talk to the scripted server in `tests/test-mail-sync.c`**, on
  its own thread with a cancellable accept, through the socket client's
  test-only `allow-plaintext`. The fake hides exactly the protocol bugs
  worth testing.

## Versioned database migrations

Every database feature ships paired, append-only SQL in `migrations/sqlite/` and `migrations/postgresql/`, meaningful upgrade/restart/failure tests, and docs in the same change. Read `docs/migrations.org` before editing persistent fields or storage behavior. Keep the GObject field table authoritative; the SQL expresses backfills and backend-specific invariants, and runs after additive schema reconciliation but before seeds. Never edit an applied script or manage transactions/history inside it. Test representative old data and affected disabled-module configurations; do not infer historical accounting events from current status. `OrmMigrator` validates checksums and unknown versions before schema reconciliation and applies each SQL batch atomically. Reconciliation also updates the existing tables of a disabled module (it never creates them): a module switched off after use still holds rows, and a script that read a column its hidden table never gained would refuse startup, or, skipped, never backfill once the module came back. Seeds after the scripts run only for a registered type (`venture_entity_registry_is_type_enabled()`): an install that never had finance on has no `accounts` table, and a seed that counted it refused startup. A skipped seed is caught up by its own empty-table test on the first start with the module on. Build embeds the complete script history into the server. Run DEBUG build/tests and ShellCheck for generator changes.

## Federation

- Federation defaults off and uses `federation.origin`, independently of `server.base_url`. Named peers require exact HTTPS origins and verified Ed25519 pins; global access is only explicit global read grants. Neither mode implicitly shares records.
- `/federation/v1/*` uses application signatures rather than local sessions. Responses must be signed and bound to the request, including discovery's cryptographically random caller nonce: TLS termination alone is not the peer's identity. Local administration still requires ordinary roles.
- Record types opt in through class metadata; grants enumerate exact UUIDs and fields. Never infer sharing from an organization, a parent record or a reference. Sensitive fields and credential-bearing URIs remain excluded.
- Replicas are durable isolated working copies, not local accounting rows. Preserve unresolved three-way conflicts and local versions across every network call; never mutate a merge base via `json_node_copy()` because JSON-GLib shares nested objects.
- Reconnect runs on the main context, never a database worker. Peer or grant revocation is checked again on each request. Test the actual HTTPS path with `test-federation`, including outages, restart, replay, response proofs and local authentication boundaries.

## The record view

- **A page is derived from what each field is for, not from the field
  list.** `venture_field_spec_get_role()` classifies every field -- status,
  content, fact, technical, structured -- and the record page, the list's
  columns and the form's groups all read it
  (`src/web/venture-web-record-view.inc`). Never render a type's fields by
  hand, and never add a per-type presentation table: mark machinery with
  `VENTURE_COLUMN_FLAG_TECHNICAL` (or name it like machinery) and name the
  type with `venture_entity_class_set_labels()`.
- **Technical is not sensitive.** A technical field is folded under "All
  fields" and "Advanced"; a sensitive one is never rendered at all, folded
  or not. The classifier returns technical for both, so every renderer
  still checks `VENTURE_COLUMN_FLAG_SENSITIVE` itself.
- **Nothing on a page by default shows an internal name, a raw UUID or an
  empty row.** Type pickers use `venture_web_append_type_option()`; the
  display-name fallback is the type's label ("Contact #4"), not its table.
  `tests/test-record-view.c` pins the submission page for this.
- **The type's own block comes before the generic panels.** On the detail
  page an invoice's lines, a ticket's desk and a release's changelog are
  appended before actions, related records, links and activity. A new
  type-specific block goes in that first group.
- **A new sidebar page is filed under a question.** Appending a row to the
  link table draws it under the heading it carries; appending its path to
  one of the `venture_web_nav_*[]` lists files it under Money in, Money
  out, Customers, Support, Growth, Bank or Books. `test-plugin` and
  `test-auth` hold the map.
- **Captions are sentence case.** Field names beside a value or above an
  input use the caption register in both looks; the uppercase micro
  register is for chrome only. Both looks must be checked.

## Receipt printers

- **Names cross the HTTP boundary, addresses do not.** Resolve every printer
  through `venture_config_find_printer()`; never accept host/port overrides.
- **One deadline covers the whole exchange.** The sender uses async GIO and a
  nested main loop; copy needed configuration before entering it, and never
  hold a database transaction across it. A successful TCP write is not proof
  that paper emerged, and failed writes must not be retried automatically.
- **ESC/POS lengths count bytes.** QR keeps UTF-8; body layout runs after
  conversion. CP1252 bytes 0x80–0x9f are printable, not removable controls.
  Font A/B are measured 48/64 columns on 576 dots; do not correct them back.
- **The YAML library turns quoted empty scalars into null.** The disabled
  printer default accepts that representation; test the compiled default YAML,
  not just populated printer lists, or every normal startup fails.
- **Payment print fixtures need `date` and `method`, not only an amount.**
  Share the save fixture with a socket-free rendering test so a sandbox's
  TCP refusal cannot hide missing required fields. Assert the `GError` before
  asserting a save's boolean result, or GTest hides the validator's reason.

## Plugins: runtimes, manifests and exec

`src/plugin/` loads every plugin through a runtime, and everything a plugin
adds goes into a registry on the context. `docs/plugins.org` is the
reference (and the JSON-lines protocol's one definition); `docs/extending.org`
is the tour.

- **Plugins load through runtimes, and a manifest never leaves its
  directory.** `native`/`crispy`/`declarative`/`exec` are
  `VenturePluginRuntime`s; a plugin may add one, and a file nothing claims
  is held and retried -- after the registering plugin's load returns, never
  inside it. `*.plugin.yaml` is matched before any extension (it is also a
  `.yaml`). Every manifest `entry` and every exec program goes through
  `venture_exec_resolve_within()`: realpath, then compared against
  `root + "/"`, and for exec again at every run. The manager must be on
  the context *before* loading (`main.c` set it afterwards, and every
  plugin's `venture_context_get_plugin_manager()` got NULL).
- **Exec plugins: argv, a socket, a cleared environment, and stdin only.**
  `venture_exec_run()` spawns with an argv array (never a shell), a fresh
  process group (every early end kills the group, or a script's `sleep`
  keeps stdout open), the environment cleared to PATH/LANG/HOME plus the
  manifest's `env` names, and stdin on a socket pair -- a pipe to a program
  that exits unread raises SIGPIPE and kills the server. Settings and
  secrets go in the stdin request only, never argv (`/proc`) or the
  environment (inherited), and secret values are redacted out of every
  string handed back. It iterates a *private* main context, so it may run
  on a worker; `plugins.allow_exec` (off by default) is checked at load
  and at every run. Money in the JSON-lines protocol is a decimal
  *string*; a JSON number is refused. `docs/plugins.org` is the protocol's
  one definition -- `file_jsonl` reads the same vocabulary.
- **What a plugin provides is a registered kind, not a case in the
  loader.** `venture_context_get_plugin_provides()` holds the kinds; a new
  one is registered by the subsystem that understands it, and an unknown
  kind fails the plugin's load. A kind whose module can be switched off
  stays registered and skips while off.
- **A failed load leaves nothing registered.** A manifest's `provides` is
  judged whole (each kind's validate function) before any entry is
  registered (`venture_plugin_provides_registry_add_full()`), and the
  manager wraps every load in a frame: on failure every name added since
  to the providers, fee models, export formats, strategies, automation
  handlers, reports, posting rules, provides kinds and runtimes is
  removed, and web extensions by position. A nested success is kept.
  Record types, modules, actions, validators, ledger sources and service
  registries are *not* rolled back -- register them last. Adding a named
  registry a plugin may write means adding it to `manager_registries[]`
  in `venture-plugin-manager.c` with a `_remove()`;
  `/plugin-runtime/failed-register-takes-back` pins the list.
- **What the `venture` pod module answers to is a registry, not a list.**
  `venture_context_get_automation_handlers()` holds every handler and
  event; the built-ins and the three record events are registered first
  (`venture_automation_register_builtins()`), a held name is never
  replaced, and an unknown one is still refused. It lives on the context
  because plugins load before the engine exists and a reload rebuilds the
  engine. Do not bring back a handler array or an if-chain in the module.
  A handler that returns FALSE without a `GError` is taken to have logged
  its own reason (every built-in does): the registry reports
  `VENTURE_ERROR_AUTOMATION` and a pod does not log it twice -- so a new
  handler that fails silently must `g_warning()`, and one that sets an
  error must not use that code for it. Handlers stay synchronous; an exec
  plugin's `automation_handler` blocks the main loop up to its manifest's
  `timeout`, and `plugins.allow_exec` is checked at every call.
- **`venture_automation_emit()` goes through the same cascade guard as
  record events.** An event raised from inside a handler goes nowhere. It
  refuses an unregistered event even with automation off (a typo must be
  found the first time) and refuses `on_created`/`on_updated`/`on_deleted`,
  which only the database raises.
- **A plugin's pages are web extensions, run after every core route.**
  Plugins load before the server exists, so they call
  `venture_context_add_web_extension()` and `venture_web_server_new()` runs
  each one last -- the router takes the first match, so a built-in always
  wins a path both serve. Extension routes go through
  `venture_web_server_add_classified_route()` and guard themselves with
  `venture_web_server_require_page()`/`_require_api()`/`_require_module()`;
  `/auth/web-extension-routes` in `tests/test-auth.c` pins the guards and
  the ordering.
- **A plugin's sidebar row is not in the link table.**
  `venture_web_server_add_nav_link()` is refused outside an extension
  (built-in pages belong in the static table, which `test-plugin` holds to
  exact membership), needs a plain path a classified GET route already
  serves, escapes its label, and takes an icon only as a name from the
  built-in set -- never markup, because it is drawn into every page. Rows
  render under a "Plugins" heading only when one exists, so with none the
  sidebar and its exact-match tests are unchanged; an accountant-only
  sidebar shows a row only when its module is one that sidebar offers.

## Market data feeds

`src/feeds/` (module `feeds`, opt-in) fetches outside data into the series
stores. `docs/market-data.org` has the whole of it; these are the traps.

- **Providers never touch the database.** A `VentureDataSourceProvider`'s
  `fetch_async` runs on the feeds worker (or a test fetch's private
  context) and sees only its `VentureFeedRequest`; anything from the main
  thread -- an exec plugin's program, its stored settings -- is taken in
  `freeze`, on the main thread, and must be immutable. JSON crosses threads
  as text and each thread parses its own: JSON-GLib's reference counts are
  not atomic, and `json_node_copy()` shares nested objects. The worker is
  in the thread list under "Things that are easy to get wrong".
- **Every outside fetch goes through the request's helper.** Origin on
  `feeds.allowed_origins` (deny by default), no redirect followed and the
  final address compared, a byte cap judged on what arrives, one deadline,
  If-Modified-Since, Retry-After, the hourly budget. A provider that opens
  its own socket skips all of it. Errors name an origin, never an address:
  a query string may carry a key.
- **Files only under `feeds.file_roots`,** realpath()'d and compared
  against `root + "/"`. No record holds a path: a store is always
  `<state_dir>/series/<uuid>/` (`venture_feeds_store_dir()`).
- **Credentials never go in `settings`.** The save refuses a setting the
  provider's schema marks `x-sensitive`; they are sealed under
  `feed-<uuid>` and redacted out of every run, note, store string and log
  line. A template names one as `{secret:NAME}`.
- **A credential goes only where it was sent when it was set.** A setting
  that replaces a provider's fixed address is `x-endpoint-override` in
  its schema (Blizzard `api_base`/`oauth_base`, odds `api_base`) and is
  honoured only with `feeds.allow_endpoint_overrides` (test/dev, default
  off): refused when written, refused again at every freeze. An address
  that is the source's own choice is `x-endpoint` (http_json/csv `url`):
  `venture_feeds_set_credentials()` seals the credentials with the
  origins those settings name (`x-bound-origins`, never handed to the
  provider) and the freeze withholds them when the origins moved. Seal a
  feed credential only through that function -- tests included; a
  binding made with `venture_integration_service_configure()` directly is
  unbound and never sent. The allowlist alone did not stop an editor
  pointing the secret at another allowlisted origin.
- **A run is written once, on the main thread, as the system,** and never
  inside somebody else's transaction: a run that arrives while one is open
  waits (`feeds_service_drain()`). `data_source_run` refuses writes from the
  generic routes. Scheduled passes share a run per
  `feeds.run_window_minutes`; do not make every pass a row.
- **A sync never waits.** The action, the API, `feeds_sync` and the page
  queue and return; `venturectl feeds sync --wait` is the client polling.
  The Test action does wait, on the main thread, so its wait is bounded
  past the cancel (`FEEDS_TEST_GRACE_SECONDS`) and a provider that ignores
  cancellation is abandoned with a timeout, never waited for.
- **Runs come back on the default main context.** The worker delivers
  there, not to the caller's thread-default, because the service's
  retries (`g_timeout_add`/`g_idle_add`) are there; a worker first started
  inside a pushed context delivered to one nobody iterated.
- **A run window is written, not dropped.** A source coming off the worker
  hands its open window back; `venture_feeds_stop()` (the server's signal
  handler, loop still running) writes the windows and undelivered runs
  `venture_series_worker_take_flushed()` returns. Dispose and module-off
  cannot write a run (context going, types masked) and say how many they
  drop.
- **Later modules hook in, they do not edit the worker.**
  `venture_feeds_add_hook()` gives a main-thread freeze, a worker-side
  after-commit with the store's writer, and a main-thread after-run.
- **Tests drain the worker.** `venture_feeds_service_count_pending()` to
  zero, bounded, iterating the default context; the worker is joined when
  the context is disposed, so a test that drops its context leaves no
  thread behind.
- **Pending never reads zero mid-sync.** Each stage counts itself before
  the one before it stops counting: the command, the sync waiting for the
  worker's timer (`worker_source_set_manual()`), the pass, the delivery. A
  gap between the command being freed and the pass starting let a settle
  loop read zero and look for a run that did not exist yet -- one
  test-alerts run in five, under load. `/feeds/pending-covers-a-queued-sync`
  polls without sleeping and fails within a few syncs if a gap returns.
- **A series store is copied on a thread of its own, never the main
  thread nor the worker's loop, and the copy never makes the installation
  backup wait.** A store can be gigabytes; on the loop it held every sync
  behind it (`/backup-series/sync-during-copy`). The copy's GTask touches
  only its own SQLite connections; the worker counts it (`copies`) and
  its join waits for it, bounded. The backup service knows nothing about stores: the feeds
  module registers a companion
  (`venture_backup_schedule_service_add_companion()`) that records each
  store's `backup_run` as running, queues the copy and returns; the answer
  is recorded later on the main thread, after any open transaction, with
  `_finish_companion()`. The copy holds one read transaction across every
  `sqlite3_backup_step()` -- drop it and SQLite restarts the copy at each
  commit, so a busy store is never backed up at all
  (`/backup-series/consistent-under-writer` fails at its deadline). A
  copy nobody will answer (a restart) is marked interrupted by the next
  backup; one still in flight is skipped, not started twice. Retention and
  `latest_run()` tell a companion from the schedule's own copies by scope
  -- a pruning or drill query that forgets the scope prunes a store by the
  database's count or drills a store as if it were books.
- **The operator's accounts are the series store's second half, and
  stay there.** `src/series/venture-series-accounts.c` shares the store's
  connection, statement cache and transaction counter through
  `venture-series-store-private.h` (never installed, never included
  outside `src/series/`). Do not open a second connection for it and do
  not move the rows into VentureDatabase: a first sync is tens of
  thousands of ledger rows on the worker thread, which may not touch the
  database. Anything that mirrors these rows into records (positions into
  `listing`) is a main-thread after-run hook reading a reader handle.
- **Account-operations lines are strict; market lines are not.** The
  seven kinds (`account`, `account_snapshot`, `balance`, `holding`,
  `position`, `inbound`, `txn`) refuse an unknown member; the market kinds
  ignore one. It is deliberate both ways -- a misspelt `expires_at` would
  store listings that never expire, and an older reader must survive a
  new optional market member. The contract is shared with tsmctl's Python
  exporter: a member added here is added there, in the same change, or
  every push fails at its first line.
- **A snapshot replaces only what it covers, for one account, and never
  goes backwards.** Mark and sweep (rows stamped -1, restated rows written
  over the stamp, the rest deleted), not delete-then-insert, so a
  position keeps `first_seen`. A snapshot older than the one that last
  replaced a kind is stale and skipped (`accounts.<kind>_at`); one after
  its own rows, or a second for the same account, is refused by the batch
  and the rows upsert alone. Times before 1970 are refused because -1 is
  the mark. A covered-kind row the batch refuses (a decimal place too
  many, the wrong currency, a row past a cap) takes that kind out of its
  account's snapshot (`feed_batch_refuse_account_row()`), before or after
  the snapshot line: sweeping a kind the batch could not read whole
  deletes the row it refused -- a listing vanishes, a purse reads zero.
  A balance with no `at` inside a snapshot is as of the snapshot, like
  every other covered kind; the push time would outrun the next sweep.
- **A read of "all of them" is all of them or a refusal.**
  `list_accounts`, and `list_holdings`/`_positions`/`_inbound` with no
  count (or one past the bound), and `value_lines` read one row past
  `VENTURE_SERIES_MAX_ACCOUNTS`/`_ACCOUNT_ROWS` and refuse with "narrow
  the read" when they get it. Never a silent LIMIT: the mirror closes the
  listing of every position it does not see. A count under the bound is a
  page. Tests lower the bounds with `venture_series_accounts_set_max_rows()`
  / `_set_max_accounts()` and restore them.
- **The operator's money is in the data source's currency.** Only
  `balance` names a currency and it must be the source's; every ledger
  row of a source with a currency carries it, money or not, so an
  expired auction is in the same P&L bucket as the sales beside it. A
  source with no currency refuses money rows with a note, it does not
  guess.
- **`VentureSeriesStats` has a `source_figures` flag.** Its three new
  members (historical, sale_rate, sold_per_day) are read only when it is
  TRUE; `venture_series_stats_init()` sets it. A caller that clears the
  struct with memset() -- every older one did -- leaves it FALSE, and
  its zeros are not read as "a sale rate of nothing". The source's figures
  live in `current.source_*`, beside the store's estimates, and fill
  `venture_series_store_reference()` only where the store has none (flag
  `*_from_source`).
- **A push source is never scheduled.** Its freeze forces `manual` and
  `feeds_source_scheduled()` skips its provider, whatever the record
  says; a sync of one fails its run. Each push is a run of its own
  (trigger `push`), queued on the worker ahead of a waiting sync, its
  body handed over as plain bytes and parsed by the `push` provider
  through `feed_parse_jsonl()` -- the file_jsonl function. Do not parse in
  the route: one path, so a push and a file of the same lines agree.
- **`?wait=1` is a bounded nested loop, like the Test action.**
  `venture_feeds_service_wait_push()` iterates the default context until
  the push's run is written, holding a reference on the service (a
  request served inside the loop can switch feeds off). The push id is
  remembered in memory (last 1024), not on the run record; there is no
  migration for it. Nested loops end last in, first out, so at most
  `VENTURE_FEEDS_PUSH_WAITERS` (2) wait at once; the next is answered 202
  with a note, unwaited. Do not raise it without parking requests instead.
- **A source has at most `VENTURE_FEEDS_PUSHES_IN_FLIGHT` (4) pushes with
  the worker.** Each holds its body until its run is handed back;
  `pushes_in_flight` (push id -> source id, main thread) is cleared on the
  run's hand-back, on a source taken off the worker (the worker drops its
  queued pushes without a run) and on halt. Past it the push is 409. A
  path that drops a push without a run must forget it too, or the source
  refuses every push until restart.
- **The JSON-lines reader feeds the batch a slice at a time.**
  `venture_feed_batch_add_jsonl()` keeps its cross-message state (venue
  currencies, log count, account snapshots) on the batch so
  `feed_parse_jsonl()` can free each 256 KiB slice's messages before the
  next. Holding a fifty-thousand-line stream as parsed JSON objects is
  the allocation storm this avoids; the account rows themselves live in
  one `GStringChunk`.

## Market data records and the price oracle

`src/marketdata/` (module `marketdata`, requires `market`, suggests
`feeds`) holds venue, instrument and watchlist records and the price oracle.
`docs/market-data.org` ("Venues, instruments and the price oracle").

- **The oracle never converts a currency.** A figure in another currency
  than the one asked for is no answer, and the evidence names both. Nothing
  observed is TRUE with a NULL price, as `venture_market_latest_price()`
  answers -- feeds off, an empty store, the module off are notes, not
  errors. Only an unreadable store is an error.
- **`series:` is a grammar, parsed before any exact match.**
  `venture_marketdata_parse_price_source()` runs first on every
  `price_source`; text without the prefix is an observation source matched
  exactly, as before. Never widen either half into a search, and keep the
  market validator refusing an observation source that starts with
  `series:` -- it could never be asked for. A report reached through
  `series:` never falls back to observations (`recipe_margin`'s rule), and
  `recipe_margin` and `goal_materials` share
  `venture_marketdata_oracle_source_price()` so they cannot disagree.
- **Promotion looks up deleted rows and restores them.** `external-ref`
  (namespace:key) is `UNIQUE_ORGANIZATION`, which counts soft-deleted rows;
  a lookup that skipped them would insert a duplicate and fail the index on
  every retry. An existing record is returned as it is -- never overwritten
  from the store -- so edits survive a second promotion.
- **A group-wide figure needs one group.** With nothing named the oracle
  uses the store's only group and refuses a store with several, naming
  them. Do not make it pick one: an EU recipe priced from US realms reads
  like a good answer.
- **Oracle objects are per report.** `VentureMarketdataOracle` caches store
  read handles for its lifetime; a long-lived one would read a purged store
  through a stale handle. Main thread only: stores are opened through the
  feeds service.
- **`track: known` reads instrument records at freeze time.**
  `venture_marketdata_known_keys()` runs in `feeds_freeze()` on the main
  thread; the worker sees only the frozen set. An instrument save queues a
  coalesced refreeze -- keep it coalesced, promotion saves in bulk.

## Market data alerts

`src/marketdata/venture-marketdata-alerts.c`: `alert_rule` and `alert_hit`
in the marketdata module. `docs/market-data.org` ("Alerts").

- **Candidates are found on the worker against frozen rules; hits are
  written on the main thread.** The feeds hook "alerts" freezes every
  enabled rule of a source's organization into plain structs (store keys,
  venue key, threshold in minor units, open listings for undercut) in
  `feeds_freeze()`; the after-commit runs the evaluation against the
  writer and leaves JSON on the run; the after-run writes `alert_hit` as
  the system ("alerts"). The worker never touches the database. A rule,
  watchlist(-entry), venue, listing or category save queues
  `venture_feeds_queue_refresh()` -- keep the list in
  `alerts_entity_changed()` in step with what the freeze reads.
- **One evaluation, two callers.** The worker and the `evaluate` action
  (`venture_marketdata_alerts_evaluate()`, a read handle on the main
  thread) run the same `alerts_evaluate()`. Never write a second matcher
  for "test this rule".
- **The cooldown is judged on the main thread, per rule and `subject`.**
  A hit about the same subject observed at the same snapshot or later
  always suppresses (so a hand evaluation after a run says nothing twice);
  within `cooldown-minutes` it suppresses. Deleted hits count. The cap is
  `VENTURE_ALERTS_MAX_HITS_PER_RUN`, and the overflow is a note on the run
  record -- never silently dropped.
- **No separate alert event.** The hit's creation is the event:
  `alert_hit.created` to webhooks, `on_created` to automation, a
  `VENTURE_NOTIFICATION_KIND_ALERT` (appended last; the kind is stored by
  number) to the recipient's inbox. Do not add a custom "alert_fired"
  event: two events for one fact is two things to keep in step. A rule has
  no personal owner for the same reason -- a personal record is kept from
  webhooks and automation.
- **Never evaluated inside an automation handler.** A hit written there
  raises no `on_created` (the cascade guard). The run hook defers while
  `venture_automation_is_dispatching()`, and `evaluate` refuses with
  `VENTURE_ERROR_CONFLICT`.
- **Stock transitions come from the store, not from memory.**
  `current.stock_changed_at` (series schema step 3) is stamped by the
  statement that crosses zero; out_of_stock/back_in_stock fire when it
  equals the row's `taken_at`. A first sighting leaves it NULL -- otherwise
  a source's first sync is "back in stock" for everything. Never keep the
  previous snapshot in hook state: frozen data is immutable and the worker
  is restarted at will.
- **Each kind carries exactly its fields.** The validator refuses a
  threshold, number, pattern or window a kind does not read. `below` is the
  zero kind *because* it requires a threshold: a rule saved without its kind
  is refused, not quietly watching something. Patterns are bounded plain
  text, case-folded substring -- never a user regular expression on the
  worker.
- **A rule's category is an instrument category.** `alert_rule` sets the
  `VENTURE_CATEGORY_APPLIES_AS_QDATA` type qdata to "instrument" so the
  generic category check judges it as an instrument's reference; do not
  list the type in the validator.
- **The account kinds read the store as it stands, at a moment, and
  their moment decides the cooldown.** `position_expiring`,
  `inbound_expiring`, `account_stale` and `collect_ready` are judged
  after every commit whatever the unit brought (time passing is what
  makes a position expire), at the run's start on the worker and at
  `now` by hand (`venture_marketdata_alerts_evaluate_at()` pins it for
  tests). The expiring kinds and collect_ready are observed at that
  moment, so they repeat once per cooldown; account_stale is observed at
  the account's last sighting, so one absence is told once -- observing
  it "now" would nag hourly for a month. An account's group is a realm,
  not a venue's region: a watchlist's group is never inherited by them.
- **Undercut freezes listings newest first and never one query each.**
  Pages of `ALERTS_PAGE` up to `VENTURE_ALERTS_MAX_LISTINGS` (20000),
  a mirrored listing's instrument from its `mirror-state`, anyone
  else's per distinct product. The bound reached is a run note. The old
  oldest-first read of a thousand silently dropped the listing just
  posted -- the one a rival undercuts.

## Accounts and positions as records

`src/marketdata/venture-marketdata-mirror.c`: a source's accounts become
`location` records and its positions `listing` records, on the main thread
after every run (feeds hook "mirror"). `docs/market-data.org` ("Accounts
and positions in the books") has every rule, the matching table included.

- **Compare with `mirror-state`, never with the store, to decide what to
  write.** The listing's `mirror-state` holds what the mirror last wrote;
  a field that differs from it is a person's edit and is left alone for
  good, and an outcome a person set makes the whole listing theirs. A
  "fix" that writes whatever the store says undoes every correction on the
  next push.
- **Only the mirror writes the mirror's fields.** `data-source-id` and
  `mirror-state`, and a mirrored listing's `external-id`, change only in
  the save `venture_market_save_mirrored_listing()` names (a permit on the
  database, like the sessions post). The mirror owns exactly the listings
  filed under its source whose `external-id` starts with the source's uuid;
  a typed-in listing is never read for writing.
- **There is no cursor, on purpose.** A pass walks the whole store, writes
  at most `mirror_max_writes` records and stops; what it did not reach is
  still out of step, so the next pass does it, soonest expiry first.
  Unchanged positions cost a lookup, not a write. Do not add a "since last
  run" filter: a pass that stopped at its bound would then never come back
  for the rest.
- **Every record a pass writes counts, promotions included.** Instruments
  (parents too) and venues go through a `VentureMarketdataBudget`
  (`venture-marketdata-private.h`) and land in the pass's batch; leaving
  them out let a first push save ten thousand instruments in one
  transaction. A unit cut at the bound must be resumable -- a listing is
  not made before its venue, a child instrument not before its parent --
  and the two pairs nothing would finish later (a new location and its
  venue link, a product and its instrument link) need room for both.
- **A database error rolls the batch back by hand.** A save inside the
  mirror's transaction did not open it and rolls nothing back: SQLite then
  commits the rest of the batch around the failure, and PostgreSQL turns
  the aborted transaction's commit into a silent rollback.
  `mirror_database_failed()` calls `venture_database_rollback()`, restores
  the report's counts and stops the pass.
- **A read that may be short judges nothing gone.** A positions read or a
  group's ledger read that comes back at the bound
  (`venture_marketdata_mirror_get_max_rows()`; tests lower it with
  `_set_max_rows()`) has left rows out, and an unread position is not a
  gone one. Treat a failed ledger read the same way: skip the group, say
  so, never judge on what came back. The mirror's bound is clamped to the
  store's *current* one (`venture_series_accounts_get_max_rows()`, which
  a test lowers too): above it the store refuses a positions read and
  cuts a ledger page in silence. A store read refused past its bound
  (`INVALID_ARGUMENT`, "narrow the read") stops the pass with a note --
  never a failed run, never a closed listing (`/account-mirror/store-bound`).
- **A position is judged gone only after a complete positions snapshot.**
  Positions leave the store only through an `account_snapshot` covering
  them; an account with no `positions_at` keeps its listings open. The time
  it was first found gone is kept in `mirror-state.vanished_at` -- the
  account's `positions_at` moves with every snapshot, and a grace measured
  from it would never run out.
- **Sales fill units; expiries and cancellations end whole remainders.**
  First in, first out within one account and instrument; an expired or
  cancelled row goes to the listing whose remainder is exactly its
  quantity before any splitting. Filling them unit by unit gave a
  one-unit cancellation to the oldest five-unit listing and called the
  cancelled one cancelled by inference, at the wrong time. A row's units
  used by a closed listing are in its `mirror-state.txns` and are taken
  off before matching -- deleted closed listings included -- so no row
  closes two listings.
- **A listing's own sales come before first in, first out.** Each listing
  taking part, oldest first, first claims sale rows for its
  `quantity-sold` (units it lost while listed), and the group's listings
  still on the market take part for exactly that and are never judged.
  Without it an older stack that expired took the sale of a newer one
  still listed, closed as partial, and the newer one never found its sale.
- **Automatic promotion never restores.** A location, instrument or venue a
  person deleted stays deleted through every push; only an explicit
  promotion brings one back. Restoring on every pass would be fighting a
  person every few minutes. The books promote an account too: they call
  `venture_marketdata_promote_account_full(..., restore FALSE, ...)` and
  first ask `venture_marketdata_source_promotes_accounts()`, so
  `auto_promote_accounts: false` holds for every door.
- **A deleted product is no product.** The mirror does not file a listing
  under it (the reference check would refuse it every pass) and does not
  make another with `create_products`; the run names the items.
- **A venue's location is set only when empty.** The first character on a
  realm becomes the place its money moves through; a later one, or a link
  a person made, is never replaced.
- **Products are made only when asked** (`create_products` with
  `products_venture_id`); otherwise an unpriced item's position is left
  out and the run note names the items. Never make a product per item by
  default: a game's bags hold thousands nobody sells.

## The Trading pages

`/market/...` and `/arbitrage...` with their `/api/v1/` twins, under the
sidebar's Trading section. `docs/market-data.org` ("Browsing: the Trading
pages").

- **One answer per page, and the twin is the same handler.** Each
  `/market/...` page is one function in
  `src/marketdata/venture-marketdata-browse.c` that answers JSON; the page
  and its `/api/v1/market/...` twin are one handler branching on the path
  prefix, and the reports and widgets read the same functions. Never
  compute a figure in `venture-marketdata-pages.inc` or in a widget kind.
- **A store is only sorted and filtered on its own indexed columns.** The
  server is single-threaded. A new browse sort or filter means a column on
  `current` (a schema step), its index, and a writer -- the region
  recompute, on the worker -- never a per-request aggregate. That is why
  `sale_rate` and `sold_per_day` are columns (step 4). Sorts are an
  allowlist (`venture_marketdata_browse_sorts()`) and an unknown one is a
  400, never ignored.
- **An instrument key is the rest of the path.** libsoup hands handlers the
  path already unescaped, so a key holding `%2F` arrives as two segments:
  the routes are `/market/i/:source/*` and `mdw_path_key()` takes
  everything after the source. Do not unescape again (a key holding `%`
  would change), and do not add an action as a path suffix -- the actions
  are a POST to the page itself with `action=promote|watch|alert`.
- **A bound must name its currency.** `venture_money_from_string()` falls
  back on the install's default currency; `venture_marketdata_parse_amount()`
  refuses an amount that reads differently under two defaults. Use it for
  any amount a market filter compares.
- **Charts carry no colour.** `src/web/venture-web-chart.c` draws in
  `currentColor` with classes both stylesheets colour from tokens; a new
  chart class goes into both, in the market block at their end, and
  `/market-pages/doors-and-looks` checks the page in both looks.

## The Accounts pages

`/accounts`, `/accounts/SOURCE/KEY`, `/accounts/inventory` and
`/accounts/pnl` with their `/api/v1/accounts` twins, first under Trading
(label "Your accounts"). `docs/market-data.org` ("The Accounts pages").

- **One answer per page, computed in core.** Each is one function in
  `src/marketdata/venture-marketdata-accounts.c`; the page, its twin, the
  reports (`accounts`, `account_holdings`, `external_pnl`), the widgets and
  `venturectl accounts` read it. Never compute a figure in
  `venture-marketdata-accounts-pages.inc` or a widget kind.
- **Holdings are valued, sorted and paged in the store's SQL.**
  `venture_series_store_value_instruments()` joins each line's unit price
  from `current`, `region` and `daily`, multiplies and sums in integers and
  sorts there; a new inventory sort or filter is an expression in
  `accounts_value_cte()` / `accounts_value_grouped()`, never a sort in C
  over store rows. The account table *is* sorted in C: it is bounded by the
  store's 10 000 accounts.
- **The region's venue is a convention, not a guess.** The sale average
  comes from the venue keyed `region-` + the account venue's group in lower
  case (`region-us`), where tsmctl sends region-wide statistics. An account
  on no venue (a shared bank) gets the region of the store's *only* group;
  with several it gets none. Do not make it pick one -- the rule the
  oracle keeps for group-wide figures.
- **Everything is in the source's currency.** A bound (`min_value`) must
  name it; a source with no currency lists holdings unpriced and says so.
  Nothing converts.
- **A store's zero units-sold-a-day is "no evidence" on a stats-only
  venue**, so days of supply prefer the source's figure over a store zero
  (`COALESCE(NULLIF(sold_per_day, 0), source_sold_per_day, ...)`), the same
  substitution the reference figures make.
- **Flips split cost and proceeds by share-and-subtract.** Each take is the
  remaining amount times units over remaining units, half to even, taken
  from what remains, so the shares add back to the lot exactly. A buy and
  a sale in the same second: the buy first. Past 100 000 rows the read is
  refused, never matched in part.
- **Pages round money for reading, answers never do.** `acw_money_display()`
  drops sub-units past a thousand whole units and groups the figure; the
  JSON keeps minor units. Tests compare the JSON, and a widget's card and
  data share their own formatting.
- **Periods are labelled in UTC.** A week starts Monday 00:00 UTC, which a
  zone west of UTC would label Sunday: period labels use
  `acw_period_label()`, never `mdw_time_label()`.
- **`holdings` is the ledger's report.** The inventory report is
  `account_holdings`; report names are one namespace.
- **The fixed routes come before `/accounts/:source/*`**, whose key is the
  rest of the path (escaped `%2F` arrives as two segments); the router takes
  the first match.
- **Names in a table row are `td.row-head`, not `th`.** A `th` takes the
  labels' uppercase micro register, which turned every character's name
  into capitals.

## Finding opportunities: the arbitrage scan

`src/arbitrage/` (module `arbitrage`, requires `marketdata` and `ledger`):
the strategies, fee models, export formats and calculators that read the
series stores. `docs/arbitrage.org` ("Finding opportunities").

- **A scan never adds or compares across currencies.** A sell side in
  another currency is converted into the buy side's through
  `venture_arbitrage_scan_convert()` (the organization's `exchange_rate`
  table on the scan's date, never inverted), or the candidate is skipped
  and a note counts it. A filter amount in another currency is converted
  the same way or the row is left out; a profit sort groups by currency
  first. `venture_money_compare()` across currencies warns, which is fatal
  in tests.
- **Unquoted is blank and named, never zero.** An input with no price, a
  venue whose fee model is not loaded, an outcome nobody allowed quotes:
  the row keeps `net`/`capital`/`roi` null and says what in `missing`. A
  blank row is kept only while no numeric bound is asked, and has no plan.
  A venue with *no record* is different: it charges nothing, and the row
  warns rather than blanks.
- **`plan()` and the `record` action are the only path from an
  opportunity to the books.** `venture_arbitrage_record_opportunity()`
  re-runs the question (top at its most), finds the key in what the data
  says now, plans it and calls `venture_action_registry_perform()`, so the
  organization's roles, second-actor approval and consent apply. Never
  record from a JSON opportunity a client posted; a moved one is
  NOT_FOUND "no longer there".
  Staging (`?stage=1`, `venture_arbitrage_stage_opportunity()`) shares
  the parameter building, so approval performs exactly the plan a direct
  record would have; it still promotes the legs' venues and instruments.
- **The scan routes drop `key` and `format` and ignore `venture_id`,
  `as_of`.** Anything sending a question must not rely on them:
  `venturectl arbitrage` refuses them by name, and the calculator route
  passes `format` (the odds format) on explicitly -- dropping it read
  American odds as decimal. `organization_id` is read, by the route and
  not the scan (see "A Trading API answers for its organization_id").
- **A Trading API answers for its `organization_id`, judged by the record
  policy.** Every `/api/v1/feeds`, `/api/v1/market` and
  `/api/v1/arbitrage` route resolves its organization through
  `venture_web_request_organization()`: the query string's
  `organization_id`, else the JSON body's (a number or its digits), else
  the active organization -- which for a token is always the default one,
  so without the parameter a second organization's market data was
  unreachable from venturectl. The organization's own row is read under
  the request's access scope before anything else: a member may read it,
  a stranger is told NOT_FOUND, exactly what reading any of its records
  would say. That check matters because these routes read series stores,
  which are files the row-by-row policy never sees. Pages ignore the
  parameter and answer for the sidebar's pick, as every page does. A new
  Trading route calls the helper, never `venture_web_active_organization()`
  directly; `/auth/trading-organization` in `tests/test-auth.c` holds the
  families.
- **The option table in `venture-arbitrage-scan.c` is the one list of scan
  options, and every one is a report option.** A new option goes there,
  into the `arbitrage_scan` schema, and through all five doors in the
  same commit. An unknown name is refused by the reader -- presets and the
  `opportunities` widget are judged by it at their save. No option is
  `limit`.
- **Fee models and strategies are judged when written, never when kept,**
  and only while the arbitrage module is on (`venture_arbitrage_fees_validate_venue()`,
  the preset validator). The registries live on the context; the
  database holds the last context's (the tests build several contexts per
  database), and the built-ins are always registered, so a plugin's name
  written while the plugin is not loaded is refused.
- **Fee models get the instrument's attributes.** `wow_auction`'s deposit
  is a share of `vendor_sell`, which only the store knows:
  `VentureFeeModelComputeFunc` takes `attrs`, and
  `venture_arbitrage_scan_fees()` takes the side's instrument key and reads
  them (cached per scan). A new strategy pricing one instrument passes its
  key; a calculator passes none.
- **Every strategy read is bounded and indexed.** Candidates come from an
  indexed store query with a limit (`pct_vs_region`, `deals_only`,
  `_list_quoted_parents()`, `_list_quoted_instruments()`), then one
  indexed read each; past a bound the answer notes it. Never walk a
  region's rows on a request -- the server is single-threaded.
- **With feeds off, a scan must not query `data_source`.** Its table may
  not exist (the registry is process-wide and masks types per context);
  answer "no data sources" without looking. The same holds for any page
  that reads a module's records while that module is off.
- **An event that has started is not an opportunity.** `cover` and
  `back_lay` leave out an event whose `commence_time` attribute (on the
  event, or on an outcome's parent) is at or before the scan's time: books
  stop moving a finished match's odds, so its last quotes look exactly
  like a surebet. An event naming no start is kept, counted and noted.
  A test fixture with a fixed kick-off is a time bomb -- serve it moved
  into the future, as `serve_odds()` in `tests/test-plugin-examples.c`
  does.
- **What a calculator presents as the result is rounded against the
  person.** A surebet's `payout` and `profit` are the least rounded payout
  and it less the stakes; the unrounded T/S and T(1/S - 1) are
  `payout_ideal` and `profit_ideal` (a scan row's `ideal_profit`).
  Back/lay rounds winnings down and the liability up, so `worst` is what
  the pair is sure to make; `ideal` is beside it. Never show an `_ideal`
  figure as what a person gets.

## Arbitrage trades and the ledger

`arbitrage_trade` and `arbitrage_leg`, posted through the ledger's source
registry ("Which saves post is a registry" above). `docs/arbitrage.org`
has the posting table, close and abandon.

- **Everything a trade does passes through arbitrage positions.** Out legs
  debit it, in legs credit it, a stock sell's FIFO cost and a write-off
  debit it, so what is left on it is the trade's profit. Fees go to
  arbitrage fees, never through the position. `docs/arbitrage.org` has
  the table; keep the rule and that table in step.
- **A leg posts through the registry; its stock moves once, by
  `execute`.** `arbitrage_leg` is a registered source (rule
  `arbitrage_leg`, postable when executed with an amount). Its rule is
  built twice per save and must never call the inventory service: a stock
  buy is received with `venture_inventory_service_receive_from()` (Cr the
  venue's cash, never GRNI) and a stock sell issued with
  `venture_inventory_service_issue_to()` (Dr positions, never COGS) by the
  execute action, which stamps `inventory-txn-id` and `cost`; the rule
  then posts only the proceeds and fees. One truth per unit: posting a
  stock leg's principal from the rule would spend its cash twice.
- **Close is an action, per book section.** A save-time rule over sibling
  legs always looks unchanged to `same_posting()`, so `close`, `reopen`
  and `abandon` are record actions. Close sums the positions lines of
  every journal the trade touched (legs -- deleted ones too --, their
  stock movements, its own closes and reversals) per *journal* currency in
  book amounts, and posts one journal per section in that section's own
  currency, keyed `arbitrage_close:<trade>:<n>:<CODE>`. Never close by
  original currency (a converted euro section would never reach zero) and
  never add two sections together.
- **Only writers make an arbitrage account.** `venture_arbitrage_account()`
  finds or makes `<org>:1460`/`4960`/`6960`; it is for the posting rule
  (inside the save that posts, so idempotent find-or-create is fine, as
  the session and holding rules do), execute, close and write-off. Every
  read -- the trade page, the summary, a position, a report -- uses
  `venture_arbitrage_find_account()`, where "none" means nothing was ever
  posted there. A GET that made an account wrote an audited row under a
  viewer's name; `/arbitrage-ledger/reads-make-no-accounts` pins it.
- **A stock leg's cost is per currency.** Units from a GOLD+TICKET craft
  are one lot and leave with both shares; `arb_cost_parts()` stamps
  `cost` (the leg's currency, else the first) and `cost-detail` (JSON
  {CODE: amount}, technical, the actions' alone like `cost`), and
  `venture_arbitrage_compute_figures()` adds the detail per currency, so
  the gold result is what the close posts. Never refuse a multi-currency
  issue again, and never read `cost` alone where the detail exists.
- **Leg validators never write the trade.** A derived write bumps the
  trade's version under whoever holds it. The actions (execute opening a
  planned trade, close stamping it) are the writers; their stamps pass the
  validators by an id-based permit, because a leg is a ledger source and
  the ledger saves a *copy* -- a pointer permit never matches.
- **`arbitrage` is in the accounting boundary's financial module list**
  (`venture_accounting_operation_is_financial()`), so the action framework
  wraps execute, close, reopen, abandon and record in one whole-operation
  proposal. Without it, a second-actor rule refused every one of them with
  "must begin before the enclosing business transaction";
  `/arbitrage-ledger/second-actor` pins it.
- **An executed stock leg is for good.** Changing or cancelling it is
  refused with the counter-leg message, and so is any financial change to
  an executed leg of a closed or abandoned trade until it is reopened.
- **The operational reports count finished trades through one function.**
  `pnl`, `ventures` and `monthly` add `venture_arbitrage_realised_totals()`
  -- trades closed or abandoned in the period by `closed-at`, per
  currency, the trade page's arithmetic -- as "Arbitrage gains", "Less
  arbitrage fees" and "Arbitrage result" (a column in the other two), and
  the result goes into Profit. Legs are neither sales nor expenses, so
  nothing is counted twice; never add a trade's legs to a sales or
  expense total. The lines appear only when the period finished a trade,
  so an organization that never trades reads as it always did, and the
  module off is no lines and no error. `/arbitrage-ledger/operational-reports`
  pins all three.

## An external ledger in the books

`src/arbitrage/venture-arbitrage-books.c`: a data source's own ledger
(sales after the venue's cut, purchases, income, expenses) into the
general journal, by `post_ledger` (`books: daily`) or `record_flips`
(`books: trades`). `docs/market-data.org` ("The external ledger in the
books") and `docs/arbitrage.org` ("Flips from an external ledger").

- **A source books one way, and the data enforces it.** The `books`
  setting picks the door and the other refuses, but the setting can
  change: `record_flips` leaves out a flip touching a day posted daily,
  and `post_ledger` a day whose flips are recorded (from the trades'
  `expected.source`). Keep both data checks; a setting alone books a
  month twice the day somebody switches it.
- **"Changed" is the day's own figures, compared with its
  `external_posting` record's fingerprint** -- never the journals (a
  treatment changed since must not repost) and never the store's row
  counts (an expiry row is not money). Purged days (before
  `series.daily_days`) and days in a closed period are kept as posted:
  rows retention removed are not a correction. So is a posted opening
  with no stored balance at or before its unchanged day: retention
  purges points by their own time, so an opening on or after the
  horizon's day can lose its point (judging by `day < horizon` alone
  reversed every account once). A follower in a closed period is kept
  too -- its figures are unchanged and its reversal would be refused.
- **Capital is computed from the source's own chain, not the books'
  state.** The least amount that keeps a day's running balance (the
  opening and every day before) at zero, credited to `trading_capital`.
  It is part of the fingerprint, so a changed day reposts every posted
  day after it. Computing it from the holding's actual balance made it
  depend on other documents and turned "unchanged" into "changed"
  whenever anything else touched the purse.
- **Reverse every changed day first, newest first, then post oldest
  first.** Reversals are never refused by the floor; posting in that
  order means each posting is judged against a timeline where the purse
  never goes short. Reposting a day while later days still held their old
  capital could be refused for a shortfall the next posting fixes.
- **`external_posting` is the books' memory: only the pass writes it.**
  A save validator refuses every other writer, the generic routes refuse
  the type, and `venture_arbitrage_books_check_write()` (in the
  database's guard list) refuses a delete -- a deleted record makes the
  next pass post the day again beside its journal.
- **The actions are type-level on the books' types, not record actions
  on `data_source`.** `post_ledger` on `external_posting`, `record_flips`
  on `arbitrage_trade`, each with `data_source_id` and `organization_id`:
  that is what makes the role matrix judge them as financial (finance
  member yes, editor member no). A `data_source` action is an
  administrator's alone (`venture-access-admin-write`), which would refuse
  the finance member too. They are `service-transaction`: the service
  begins the posting boundary before its one transaction, with a digest
  of the plan in its arguments, so an approval is bound to what it writes.
- **A recorded flip's `expected.source` is what the next run subtracts.**
  `venture_series_store_flip_pairs()` takes it as `used` (key -> units and
  money already given) and walks from what is left, so a sale merged
  larger later is recorded for its new units (`:<n>` in `external_ref`)
  and no lot is costed twice. Deleted trades count: their journals stay.
  So `expected` is refused as a hand edit on any trade with a
  `data_source_id` (compared as JSON), like `external_ref`: edited, the
  rows it stopped naming are matched and the sale recorded again.
  The walk starts at `books_from`; past 100 000 rows an item is skipped
  with a note, never matched in part.
- **One trade per sale, cash legs only, the money at the character.**
  Legs name `location_id` (the buyer's and seller's places) because a
  realm venue's location is only its first character. Nothing moves
  stock -- the units were in the game's bags. Before each buy leg
  executes, `venture_holdings_floor_shortfall()` says what the purse
  lacks and it is posted first as `trading_capital`; never execute a buy
  leg into a purse the ledger did not fund without that, or the first
  flip of every alt is refused.
- **The automatic pass never files proposals.** With a second-actor rule
  on posting it does not post and notes why on the run; it posts only
  days that are over (until today's midnight UTC), whole accounts at a
  time, bounded by `books_max_writes`.

## The reference plugins: Blizzard, the-odds-api, supplier-csv, tsmctl

Four plugins are both connectors and the worked examples of a provider
as a GObject (`plugins/blizzard-auctions/`, native), as one function
(`plugins/scripts/odds-api.c`, crispy) and as a program
(`plugins/exec/supplier-csv/` and `plugins/exec/tsmctl/`, exec).
`docs/plugins.org` "The reference plugins", `docs/examples/*-feed.org`
and `docs/examples/wow-operations.org` have the rest.

- **The Blizzard plugin lives in `plugins-optional` and must never load in
  a fixture.** `PLUGIN_OPTIONAL_DIRS` keeps it out of `$(OUTDIR)/plugins`,
  which `VENTURE_PLUGIN_PATH` loads into every test; it wants a registered
  GOLD and credentials no other fixture has. `make plugins` builds it,
  `tests/test-plugin-blizzard.c` loads it by path
  (`VENTURE_TEST_OPTIONAL_PLUGINS`), and an operator adds the directory to
  `plugins.paths`.
- **The commodity market costs 25 even on a 304.** Blizzard charges it
  either way, and the feeds budget is spent before a request goes out and
  never refunded; do not "fix" that into charging only for a 200.
- **`expires_in_min` is seconds, and a minimum.** "min" is minimum, not
  minutes: the store, the batch and the JSON-lines protocol all mean the
  least time a listing has left, in seconds. A bucketed source gives the
  lower bound (WoW LONG is 7200): the sale estimate asks whether a vanished
  listing *could* have expired, and the upper bound counts expiries as
  sales.
- **A request that is not the unit's own answer is
  `VENTURE_FEED_HTTP_UNCONDITIONAL`.** A token, an index, a name lookup:
  otherwise its Last-Modified becomes the unit's If-Modified-Since and the
  next fetch asks the auctions "anything newer than the item database?".
  The every-realm unit keeps each realm's date in its cursor and sends it
  itself, unconditionally.
- **A manual sync sends no If-Modified-Since, and refreezes the source.**
  A test of a conditional fetch needs a scheduled run (switch the source to
  `auto` and wait for the run, as `schedule_and_wait()` does); a token or
  name cache lives as long as the frozen source, not across manual syncs.
- **A provider never registers a currency.** Blizzard's freeze refuses one
  that is not registered with exponent 4 (prices are copper); the operator
  or the demo makes the `currency` record. A worker that created records
  would be writing the database from another thread.
- **A key in a query string never reaches a message.** the-odds-api
  takes its key only as `?apiKey=`; the script names a sport and a status
  in its errors, never the address or the far end's body, and redacts its
  own notes. `test-plugin-examples` greps the run, the log and the store.
- **A crispy script sees the feeds API only with SQLite in its flags.**
  `CFLAGS_PLUGIN_DEPS` carries `-DVENTURE_HAVE_SQLITE=1` for that; it is
  baked into the server, so after changing it touch
  `src/plugin/venture-crispy-host.c`.
- **An exec program holds its own paths.** supplier-csv's `file` setting
  is an editor's to write, so the script itself refuses anything but a
  plain name and compares the realpath against its directory plus `/`.
- **The tsmctl wrapper prints an export whole or not at all.** The
  account-operations lines replace state -- a snapshot removes what it
  does not restate -- so `export.sh` has tsmctl write to a private
  `--file` and copies it out only after exit 0; streaming tsmctl's stdout
  would hand over half an export when ssh drops, and every auction of the
  half-written account would read as gone. Settings reach tsmctl as
  `--option=value` after a pattern check, tsmctl comes from `PATH` (never
  a setting), and where the WoW install is stays tsmctl's own config.
  `test-plugin-examples` drives it with a fake `tsmctl` on `PATH`; `PATH`
  is captured when the plugin loads, so the test sets it before loading
  and puts it back straight after.
