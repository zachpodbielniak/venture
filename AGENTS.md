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
- **Coding runs get the only background thread, and it must not touch the
  database.** Writing a record emits `entity-saved`, whose automation handler
  enters podomation, which runs a nested main loop on the *default* context —
  driving that from a second thread is a context-ownership failure, not
  merely a data race. Progress crosses back as plain data and is applied on
  the main thread.
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
  Coding runs hold the only background thread and may not touch the database,
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

Seven git submodules under `deps/`, all linked statically, all treated as the
canonical copies: `yaml-glib`, `htmx-glib`, `ai-glib`, `orm-glib`,
`podomation`, `crispy`, `mail-glib`. Fixing a bug in one and pushing it upstream is
expected and has happened several times.

Fedora build packages are printed by `make list-deps`, plus `libetpan-devel`
and `libgudev-devel` for podomation. That pair matters more than it looks:
podomation resolves its whole dependency set in one `pkg-config` call, so one
missing package empties the flags and the build fails on a missing `glib.h`.

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
  ordinary update and stages like one.

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
  board, the inbox and `POST /api/v1/sla/sweep`, never from a timer:
  coding runs hold the only background thread and may not touch the
  database.
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
- **The enum values are checked against `venturectl describe`, not
  guessed.** Four of them were wrong on the first run: a campaign is
  `running` not `active`, an idea is `researching` not `exploring`, a
  build trigger is `webhook`/`rule` not `push`/`schedule`, and
  `venture_type` is a registered type (`books`, `etsy`, `newsletter`),
  not free text.

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
- **Never put a class name a test greps for inside venture.js.** The
  script is inlined into every page, so `"notice negative"` in a JS string
  made a dashboard test find an error notice that was not on the page.
  Build dynamic notices as DOM nodes, and assert on markup
  (`<div class="notice negative">`), not on two words.
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

Every database feature ships paired, append-only SQL in `migrations/sqlite/` and `migrations/postgresql/`, meaningful upgrade/restart/failure tests, and docs in the same change. Read `docs/migrations.org` before editing persistent fields or storage behavior. Keep the GObject field table authoritative; the SQL expresses backfills and backend-specific invariants, and runs after additive schema reconciliation but before seeds. Never edit an applied script or manage transactions/history inside it. Test representative old data and affected disabled-module configurations; do not infer historical accounting events from current status. `OrmMigrator` validates checksums and unknown versions before schema reconciliation and applies each SQL batch atomically. Build embeds the complete script history into the server. Run DEBUG build/tests and ShellCheck for generator changes.

## Federation

- Federation defaults off and uses `federation.origin`, independently of `server.base_url`. Named peers require exact HTTPS origins and verified Ed25519 pins; global access is only explicit global read grants. Neither mode implicitly shares records.
- `/federation/v1/*` uses application signatures rather than local sessions. Responses must be signed and bound to the request, including discovery's cryptographically random caller nonce: TLS termination alone is not the peer's identity. Local administration still requires ordinary roles.
- Record types opt in through class metadata; grants enumerate exact UUIDs and fields. Never infer sharing from an organization, a parent record or a reference. Sensitive fields and credential-bearing URIs remain excluded.
- Replicas are durable isolated working copies, not local accounting rows. Preserve unresolved three-way conflicts and local versions across every network call; never mutate a merge base via `json_node_copy()` because JSON-GLib shares nested objects.
- Reconnect runs on the main context, never a database worker. Peer or grant revocation is checked again on each request. Test the actual HTTPS path with `test-federation`, including outages, restart, replay, response proofs and local authentication boundaries.
