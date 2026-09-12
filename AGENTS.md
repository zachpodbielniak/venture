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
- **`/api/v1/health` is the only authenticated-by-nothing route besides the
  webhook**, because a container healthcheck runs before anybody has
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
- **The webhook is the only unauthenticated route, and the HMAC is its
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

Six git submodules under `deps/`, all linked statically, all treated as the
canonical copies: `yaml-glib`, `htmx-glib`, `ai-glib`, `orm-glib`,
`podomation`, `crispy`. Fixing a bug in one and pushing it upstream is
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
- **Templates are written in the export format.** A template is what an
  export of the dashboard it makes would be, and `test-dashboard` imports
  every one, so a template naming a field that does not exist fails the
  suite rather than a user.
