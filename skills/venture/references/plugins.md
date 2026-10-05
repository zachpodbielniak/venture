# Plugins and extending VENTURE

Read this to list what is loaded, add a venture type, provider, report or
record type, or explain why a plugin's feature is missing. Sources:
`docs/extending.org` (the tour), `docs/plugins.org` (the loader, manifests,
the JSON-lines protocol, a tutorial per runtime), `docs/data-model.org`
(adding a record type), `docs/automation.org`.

## What is loaded

```bash
venturectl plugins list        # owner only; the answer names server paths
venturectl arbitrage registries   # strategies, fee models, export formats plugins added
curl -s -H "Authorization: Bearer $VENTURE_TOKEN" "$VENTURE_SERVER/api/v1/venture-types"
```

`plugins list` shows each loaded plugin's `name`, `kind`, `runtime`
(`native`, `crispy`, `exec`, `declarative` for a venture-type YAML) and
`provides` (`data_source_provider`, `automation_handler`, ...). "No plugins
are loaded." is an answer, not an error; `plugins` takes no other word than
`list` (exit 2). A provider an exec plugin registers is an ordinary
`data_source` `provider`. Plugins load from the configured directories
(`plugins.paths`, `VENTURE_PLUGIN_PATH` colon-separated in a build tree);
`--no-plugins` and `modules.plugins: false` load none (then no venture
types are registered and any `venture_type` is accepted).

## Five ways in, in increasing power

1. **A declarative venture type** -- a YAML file in
   `plugins.venture_type_paths` (shipped in `data/venture-types/`): `name`,
   `label`, `description`, `fields` (`name: kind` or a mapping with
   `type`, `label`, `required`, `choices`, `min`, `max`, `max_length`,
   `pattern`, `indexed`, `technical`), `metrics`. No code, no rebuild. Its
   fields live in a venture's `attributes` and are enforced at the save
   ([taxonomy.md](taxonomy.md)).
2. **A podomation rule** in `automations.pod` -- the DSL, no plugin at all
   ([automation.md](automation.md)).
3. **An exec plugin** -- any program beside a `*.plugin.yaml` manifest,
   spoken to in versioned JSON lines on stdin/stdout. **Off unless the
   operator sets `plugins.allow_exec: true`**; with it off the plugin does
   not load (and a source using its provider fails its run, saying so).
   Run with an argv array (never a shell), a fresh process group, the
   environment cleared to PATH/LANG/HOME plus the manifest's `env` names,
   settings and secrets on stdin only (never argv or environment), secret
   values redacted from everything handed back, and the manifest `timeout`.
   Money in the protocol is a decimal string.
4. **A crispy `.c` script** -- compiled on demand by crispy and cached by
   content hash (`plugins.allow_crispy`, on by default); `plugins/scripts/
   odds-api.c` is the reference.
5. **A native `.so`** -- `venture_plugin_register()` on the context;
   `plugins/blizzard-auctions/` (optional, in `plugins-optional`, enabled by
   adding that directory to `plugins.paths`) is the reference.

A manifest (`*.plugin.yaml`):

```yaml
name: supplier-csv                 # lower-case, digits, . _ -
runtime: exec                      # optional for native, crispy, declarative
entry: fetch.sh                    # relative to this file; must stay inside its directory
protocol: 1                        # required for exec
exec: {args: [--format, jsonl], env: [SUPPLIER_REGION], timeout: 30}
provides:
  - kind: data_source_provider
    name: supplier_csv
```

Any other top-level key is refused. A manifest's `provides` is judged whole
before anything registers, and a failed load takes back every name it added
(providers, fee models, export formats, strategies, handlers, reports,
posting rules, runtimes); record types, modules and actions cannot be
unregistered, so a plugin registers those last. A name is never registered
twice -- a plugin cannot replace a built-in.

## What a plugin can add

Record types (and with them REST, forms, CLI, assistant tools, webhooks --
for free), modules (a plugin module is a switch like any other), reports,
record actions, save validators, ledger posting sources, automation
handlers and events, data source providers, arbitrage strategies, fee
models and export formats, dashboard widget kinds, web pages and a sidebar
row under "Plugins" (`venture_context_add_web_extension()`; a built-in
route always wins a path both serve), and its own configuration
(`plugin_config` records, admin-only). The bundled example plugin adds the
`example` module (`subscription` type, `subscriptions` report).

## Adding a record type in C

A field table and a registration -- that is the whole type:

```c
static const VentureFieldDecl venture_widget_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What it is called"),
	VENTURE_FIELD_REF("venture-id", "Venture", NULL, "venture", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD_MONEY("price", "Price", "What it sells for")
};
VENTURE_DEFINE_ENTITY(VentureWidget, venture_widget, venture_widget_fields)
```

Field kinds: `string text integer double money boolean date datetime enum
reference json`. Flags: `NOT_NULL`, `UNIQUE` (`UNIQUE_ORGANIZATION` counts
deleted rows), `INDEXED`, `IMMUTABLE`, `SENSITIVE`, `SEARCHABLE`,
`TRANSIENT`, `RETAIN_REFERENCE`, `TECHNICAL`, `SAME_PARENT`. A field may not
be named `id`, `uuid`, `created-at`, `updated-at` or `version`. Adding a
field is a non-event (the column appears on the next start); removing or
retyping one is a deliberate migration. Every type belongs to exactly one
module. Never hand-write a form, route, serialiser or SQL for a type --
that is what the design exists to avoid.

## Reference plugins

| Plugin | Runtime | Provides |
|---|---|---|
| `blizzard-auctions` | native, optional | `blizzard_auctions` provider (WoW auction houses; GOLD at exponent 4; recipe import), `wow_auction` fee model, `tsm` export |
| `odds-api` | crispy | `odds_api` provider (bookmakers' odds; key in the query string, redacted) |
| `supplier-csv` | exec | `supplier_csv` provider (a CSV in its own directory) |
| `tsmctl` | exec | `tsmctl` provider running `tsmctl export --format venture` |

Worked examples: `docs/examples/wow-auction-house-feed.org`,
`odds-api-feed.org`, `dropship-supplier-feed.org`, `wow-operations.org`.
