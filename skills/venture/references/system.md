# The system as a whole

Read this to answer "how does VENTURE work", "why does it refuse X", or
"where would Y happen". It is the map; the other guides are the territory.
The deeper sources are `docs/architecture.org`, `docs/modules.org`,
`docs/orgaccess.org` and the "Things that are easy to get wrong" section of
`AGENTS.md` in the repository.

## What it is

An ERP and CRM in one, written in C on GLib/GObject, for one operator (or a
small team) running a *portfolio* of ventures rather than one company: a
book imprint, an Etsy shop, a newsletter, a household, a game-economy
trade. It keeps the books (double-entry journals, statements, close),
receivables and payables, inventory at FIFO cost, a CRM with pipelines and
sequences, a support desk, a software factory, market data and arbitrage,
dashboards and reports -- and an AI assistant that may read freely but
writes only through approval.

## One idea: everything derives from the field table

A record type is a field table (name, kind, flags, reference target, help
text, enum) plus a registration. From that one declaration VENTURE builds
the database table, REST CRUD, JSON/YAML serialisation, the list page, the
form, the assistant's and MCP's tool schema, the audit diff, the webhook
events and the `venturectl` verbs. Consequences you can rely on:

- A type added by a plugin works everywhere the day it registers -- there
  is no per-type route, CLI verb or tool to wait for.
- `venturectl describe TYPE` and `GET /api/v1/schema/TYPE` are generated
  from the source; they cannot drift, and prose (including this skill) can.
- If a record type looks like it needs a hand-written endpoint, it is
  usually a *service* operation (posting, settlement, billing) exposed as a
  record action or a module route instead. See [records.md](records.md).

## One writer, many doors

| Door | Who uses it | Goes through |
|---|---|---|
| Web UI (HTMX pages, `/e/TYPE/ID`) | people | session cookie, the same repository |
| REST API `/api/v1/*` | scripts, agents | bearer token or session |
| `venturectl` | shell, agents | the REST API, never the database |
| `venturectl mcp` | outside AI agents | the REST API; writes staged by default |
| In-process assistant (`/ui/chat`) | people via chat | the repository, under `ai.policy` |
| Automations (podomation pods) | rules | the repository as the automation actor |
| Webhooks in (`/hooks/forge/:id`, Stripe, Lightsite) | other systems | HMAC/signature, then services |

Every door ends in `venture_database_save()`: the same validators,
reference checks, optimistic concurrency, period guards, access policy and
audit entry. There is no "admin bypass" path and no direct-SQL door for
business data. The CLI deliberately never opens the database.

## Process model and threads

`venture` is one process, one main loop, one database connection behind a
recursive lock (SQLite or PostgreSQL through orm-glib). Background threads
exist but **none of them touches the database**: the coding-run thread, the
mailer's and bank feed's transport tasks, and the feeds worker (its own
main context, the series stores' writers, provider parsing and exec
plugins). Results cross back to the main thread as plain data and are
written there. Practical effects:

- A feed sync, a push, an outbound webhook and an SMTP delivery all
  *queue* and return; their outcome is a record written later
  (`data_source_run`, `webhook_delivery`, `mail_message.state`).
- Long work is bounded and explicit: sweeps take `limit=`, KB indexing is
  synchronous on the request, the SLA sweep runs when the board opens or
  when called -- nothing heavy runs on a hidden timer.

## Modules: every feature is a switch

About 80 built-in modules (plus plugins' own) group record types, reports,
pages and services. A module may only *require* modules registered before
it, so there are no cycles; one whose requirement is off is refused at
startup naming both. Turning a module off hides its types from REST, the
schema, forms, search, the assistant, MCP and the CLI at once (404 naming
the module); nothing is deleted, and turning it back on restores the rows.

- Ask a server: `venturectl modules` (`-f json` for types, reports and the
  reason a module is off). Ask a config: `venture --list-modules`.
- Opt-in modules (off by default): `feeds`, `federation`, `stripe`,
  `payroll`, `bankfeed`, `commerce`, `group`, `ocr`, `oidc`. Switch with
  `modules: {NAME: true}`, `VENTURE_MODULE_NAME=true`, or the legacy
  `NAME.enabled` (both must be true where both exist).
- The full dependency table, module by module with its types and reports,
  is in [data-model.md](data-model.md).

## Organizations (entities) scope everything

An *organization* is a business or personal sphere you keep books for
(sole proprietor, LLC, household, a game guild...). Every record carries
`organization_id`; organizations may nest (`parent_id`) for *display*
roll-up only -- membership and filing never inherit. Each has a book
currency (`default_currency`). A *venture* is one thing you do inside an
organization (`venture_id` on most business records); a *company* is an
outside business you deal with (customer, supplier, platform).

- A token's "active organization" is always the **default** one
  (`is_default=true`). Generic `list`, `report`, the market/feeds/arbitrage
  verbs and most sweeps answer for it unless given `organization_id=N`.
  `get TYPE ID` reads a record wherever it lives (subject to access).
- In the browser the sidebar picker (cookie `venture_entity`) decides; a
  dashboard opens in the organization it is filed under until you pick.

## Roles and access

Global roles: `owner`, `admin`, `editor`, `viewer`, `service`. Organization
membership roles (`organization_membership`): `owner`, `admin`, `editor`,
`finance`, `sales`, `support`, `viewer`, `accountant`. Global owners and
admins see everything; everybody else needs an active membership in the
record's exact organization, and:

- *Financial* types (every type of a module that requires `finance`) need
  organization `finance`, `admin` or `owner`; an editor member may only
  *propose* a journal post. `accountant` reads and exports the books only.
- `sales`/`support`/`viewer` members see assigned records (owner, team,
  or a record under an assigned venture). A viewer's create becomes a
  proposal (202) rather than a write.
- Owner-only on the generic routes: `user`, `api_token`, `forge`,
  `webhook`, `federation_peer`, `federation_grant`, and the personal
  `chat_thread`, `chat_message`, `notification`, `watch`, `comment` (their
  own routes filter to the caller). Admin-only: `forge_rule`,
  `plugin_config`, writing `currency` or a `data_source`. Mailbox and
  calendar connectors are delegated by organization administrators. A 403
  on these is the role, not a bug.
- Refusals hide existence: a record you may not read is 404, never 403;
  an in-organization write your role forbids is 403.
- A token carries its minter's role and a snapshot of their memberships at
  mint time, intersected with current authority -- a later promotion never
  widens an old token. Its actor string is `API token #N`, never its name.

## Writes that wait: staging and approval

Any write can be *proposed* instead of applied: REST `?stage=1`,
`venturectl --stage`, MCP by default, the assistant under the default
`confirm_writes` policy (deletes always). A proposal is a
`VentureConfirmation` in one queue (`GET /api/v1/confirmations`), holding
the record with the change applied in memory plus a diff; approval applies
exactly that object through the same save, so it cannot differ from what
was shown, and a record changed meanwhile makes approval a 409 that drops
the card. Cards live `ai.confirmation_ttl` (3600 s), at most
`ai.confirmation_limit` (200) at once. Some operations cannot wait in the
queue (publishing a release, a bulk edit, a macro, a craft-less multi-record
operation) and say so. Separately, an organization can require a *second
person* for posting/paying (`accounting_approval_rule`) -- see
[ledger.md](ledger.md).

## The audit trail, notifications, webhooks, automation

- Every applied change writes an `audit_entry` (actor, `actor_kind`,
  diff, `approved_by`, `request_id`); nobody can write or edit one.
- The inbox (`notification`) is fed from the audit signal only: mentions,
  assignments, watched changes (`watch`), SLA clocks, budgets, runs,
  alerts. Webhooks out POST `TYPE.created|updated|deleted` signed with
  HMAC-SHA256. Automations (podomation pods in `automations.pod`) react to
  `on_created/on_updated/on_deleted` and schedules. See
  [automation.md](automation.md).

## Money, currencies and time

Money is never a double: integer minor units + currency code + exponent,
half-to-even rounding, cross-currency arithmetic refused. Currencies may be
user-defined (`GOLD`, `POINTS`) with a book treatment (`valued`,
`separate_book`, `memo`); reports keep one figure per currency, book
currency first. Calendar dates are stored as midnight UTC; period
boundaries are UTC. See [currencies.md](currencies.md).

## Outside the main database

- **Series stores**: one SQLite file per market data source,
  `<state_dir>/series/<source-uuid>/store.db`, written only by the feeds
  worker; read through the `market`, `accounts` and `arbitrage` verbs and
  reports, never `list`. See [feeds.md](feeds.md).
- **Encrypted integration bindings** (`integration_connection`): provider
  credentials sealed under the integration master key; never in records.
- **State directory**: database (SQLite default), `automations.pod`,
  backups, KB sources, compiled config cache.

## Extending, federating, hosting

Plugins arrive as YAML venture types, crispy C scripts, native `.so` files
or exec programs (`plugins.allow_exec`) and add types, reports, providers,
strategies, handlers and pages ([plugins.md](plugins.md)). Federation
shares explicit records and fields with pinned Ed25519 peers; hosted mode
runs isolated tenant workspaces administered by local operator tools
([operations.md](operations.md)). Users, configuration and providers are in
[platform.md](platform.md).
