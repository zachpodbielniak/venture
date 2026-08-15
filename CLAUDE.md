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
- **Chat threads are per-user, not per-entity.** The chat routes must filter
  on the caller's `user-id` and report NOT_FOUND (never FORBIDDEN) on a
  mismatch, and `chat_thread`/`chat_message` stay owner-only in
  `venture_web_require_for_type()`. `tests/test-auth.c` pins this.
- **The JSON wire format uses underscores.** Properties are `invoice-id` in
  C and `invoice_id` on the wire; a POST body with dashed keys is silently
  ignored field by field — the record saves and the values just aren't
  there.
- **An automation reload is a rebuild.** Never parse new rules into a
  running engine: pods hold timer and breaker state, and stacking a second
  generation beside the first doubles every side effect.
- **`make DEBUG=1 test` does not relink the server binary.** After editing
  `data/static/*` verify `build/debug/venture` is newer than
  `build/debug/venture-assets.h`, or the browser serves last hour's JS
  while the tests pass against this hour's.

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
