# VENTURE

An ERP/CRM hybrid in C on GLib/GObject, for running a portfolio of small
business ventures. Read `README.org` first, then `docs/index.org`.

## Build and test

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
- **`make DEBUG=1 test` does not relink the server binary.** After editing
  `data/static/*` verify `build/debug/venture` is newer than
  `build/debug/venture-assets.h`, or the browser serves last hour's JS
  while the tests pass against this hour's.

## The CLI, and its skill

`venturectl` is generic over record types: `list`, `get`, `create`,
`update`, `delete`, `restore`, `describe`, `report`, `health`, with the type
as an argument. A record type added today already works, and no subcommand
should ever be added per type.

**The agent-facing guide is `.claude/skills/venturectl/SKILL.md`, and it is
maintained with the code.** When a command, a flag, an exit code or one of
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
