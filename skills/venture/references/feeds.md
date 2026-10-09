# Market data feeds and the series store

Read this to set up or diagnose a `data_source`, sync or push data, or
understand what the position mirror writes. The records and price oracle
built on top are in [market-data.md](market-data.md); the operator's
accounts in [accounts.md](accounts.md). Sources: `docs/market-data.org`,
`docs/plugins.org`, `docs/examples/*-feed.org`,
`docs/examples/wow-operations.org`.

## How it fits

A `data_source` names a provider; the feeds worker thread fetches it (on a
learned schedule, behind an origin allowlist, a budget, deadlines and byte
caps) into **one SQLite file per source** --
`<state_dir>/series/<source-uuid>/store.db`, outside the main database:
latest row per venue and instrument with region median and deal price,
hourly and daily history, odds, sale estimates from vanished listings, and
the operator's accounts. The worker never touches the database; each run
comes back to the main thread as a `data_source_run`. **`list` cannot read
the store** -- the `market`, `accounts` and `arbitrage` verbs and the
market reports do. The marketdata module promotes what you care about
(venues, instruments) into ordinary records.

## Switching it on

The module is **off** unless the operator sets `feeds.enabled: true` (and
`modules.feeds`). With it off every `/api/v1/feeds` route is NOT_FOUND, so
the `feeds` verbs exit 3; the `market` and `arbitrage` verbs and market
reports still answer, with `available: false` or no rows and a note -- not
an error -- and evaluating an alert rule is refused. Operator settings
(`docs/configuration.org`): `feeds.allowed_origins` (exact, deny by
default), `feeds.file_roots`, `feeds.max_response_mb`, `max_push_mb` (32),
`request_timeout`, `max_records_per_run` (500), `run_window_minutes` (15),
`series.hourly_days` (14), `daily_days` (0 = forever), `max_store_mb`,
`include_in_backup`, `stale_minutes` (120: when the Trading pages call a
price stale), and `plugins.allow_exec` (false).

## A data source

`provider` (`http_json`, `csv`, `file_jsonl`, `push`, or a plugin's),
`settings` (YAML text), `schedule` (`auto` -- also what empty means: learn
when each venue updates -- `hourly`, `manual`, or five cron fields),
`enabled`, `track` (`all` or `known`), `currency` (the source's money),
`venue_namespace`, `instrument_namespace`, `min_value`. **Creating or
changing a source needs an administrator**: it decides which outside host
the server calls.

- **Credentials never go in `settings`**: the save refuses a setting the
  provider marks sensitive (`token`, `api_key`). Set them on
  `/feeds/ID/credentials`; templates use `{secret:token}`. A credential is
  only sent to the origins its settings named when it was set.
- An address must be on `feeds.allowed_origins` and a file under
  `feeds.file_roots`; a run says so when not. Redirects are never followed.
- **A sync never waits.** `feeds sync ID` queues and answers
  `{"status":"queued","data_source_id":ID}`; `--wait` is the CLI polling
  the source's runs once a second and printing the run -- exit 1 when it
  `failed`; after five minutes exit 7 (the sync stays queued; `feeds runs
  ID` shows it later). `act data_source ID test [unit=U]` fetches one unit
  and reports what came back, writing nothing; `act data_source ID
  purge_history` (administrator) deletes the store's history -- irreversible.
- `data_source_run` is written by the server only (generic writes 403):
  `status` ok/partial/failed/deferred (a spent quota window), `trigger`,
  `rows`, `requests`, `error`, `notes`. Scheduled passes within
  `feeds.run_window_minutes` share one run.

```bash
venturectl feeds sync 1 --wait organization_id=2
venturectl feeds runs 1 organization_id=2       # newest first: status, units, rows, error
venturectl feeds due organization_id=2          # every unit and when it is checked next (null: never on its own)
```

## Push sources

A `push` source is filled from outside and never scheduled (a `feeds sync`
of it fails its run). `feeds push ID FILE|-` POSTs JSON lines to
`/api/v1/feeds/ID/push` (`Content-Type: application/x-ndjson`) and answers
`{"status":"queued","push_id":...}` at once.

```bash
tsmctl export --format venture | venturectl feeds push 4 - --wait organization_id=2
```

- Pushing needs an editor. Refused with **exit 4**: a source whose provider
  is not `push`, that is switched off, or that already has four pushes not
  yet stored (wait for those runs). Feeds off is exit 3; a body past
  `feeds.max_push_mb` is 413.
- Every push is a run of its own (trigger `push`). `--wait` holds the
  request until the run is written (at most two minutes) and prints it; a
  `failed` run (a malformed line, named) exits 1; an answer without the run
  (too slow, or two other pushes already being waited on) prints the queued
  answer and exits 7 with the push still queued.
- Money in the JSON-lines protocol is a decimal **string**; a JSON number is
  refused. `docs/plugins.org` defines the protocol (`file_jsonl` reads the
  same vocabulary). Market kinds ignore unknown members; the
  account-operations kinds (`login`, `account`, `account_snapshot`,
  `balance`, `holding`, `position`, `inbound`, `txn`) refuse one. A `login`
  (`key`; `name`, `kind` game_account|platform_account|other, `group`,
  `attrs`) is a credential -- tsmctl sends one per WoW account folder; an
  `account`'s `login` names it (absent/null keeps the stored login, `""`
  clears it). Their money is in the **data source's** currency (set it; a
  `balance` in another is refused and noted). An `account_snapshot`
  replaces the covered kinds of that one account and must come before its
  rows. Ledger `txn` rows upsert on `id`: re-sending is safe.
- A refused `balance`, `holding`, `position` or `inbound` line (too many
  decimal places, the wrong currency) takes that kind out of its account's
  snapshot: what the store had is kept, not swept, and the run's notes say
  so. Fix the line and push again. `GET /api/v1/health` reports
  `account_logins: true` on a build whose push reads logins.

## The position mirror

After every run the marketdata module **mirrors** a source's accounts into
`location` records (kind `character`/`shared`/`guild`/`other`, inside a
`group` location per realm -- per realm *per login*, inside a `login`
location, for an account that names a login; a place a person moved is
never moved back) and its open positions into `listing` records (`list
listing data_source_id=ID outcome=open`; `external_id` is `<source
uuid>:<position id>`). A gone position is closed from the ledger: sold,
partial, expired, or cancelled after `mirror_grace_hours` (default 48) with
no ledger row. The run's `notes` say what it did, including positions "not
mirrored: their items have no product" -- link the instrument (`update
instrument ID product_id=N`) or set `create_products: true` and
`products_venture_id: N` in the source's settings -- and "their items'
products are deleted" (restore it or link another; the mirror never makes a
replacement). **Never `update listing ID data_source_id=` or
`mirror_state=`**: refused. Editing a mirrored listing's price or outcome
is fine and sticks; an outcome you set makes the listing yours. Settings:
`mirror_positions`, `auto_promote_accounts` (both default true),
`account_namespace` (share places between two sources of the same
characters), `mirror_max_writes` (500; every record a pass writes, promoted
instruments and venues included), `mirror_grace_hours`. Automatic promotion
never restores something a person deleted.

## Backups of the stores

`backup run SCHEDULE_ID` on an installation schedule also copies every
store (unless `series.include_in_backup` is false), but answers before the
copies finish: each is its own `backup_run` with `scope` `series`,
`parent_run_id` and `store_uuid`, `running` until the worker answers. Check
with `venturectl list backup_run scope=series`; a `failed` one says why and
did not fail the database copy. Restoring one is a manual, server-stopped
file copy (`docs/backup.org`, "Restoring a store").

## Providers from plugins

`blizzard_auctions` (WoW auction houses; optional plugin, needs a
registered `GOLD` at exponent 4 and `client_secret` on the credentials
page), `odds_api` (`units` are sport keys, `api_key` on the credentials
page), `supplier_csv` and `tsmctl` (exec) -- see [plugins.md](plugins.md).
Read a source's provider with `get data_source ID` before guessing its
settings. A `blizzard_auctions` source has two more actions. `act
data_source ID import_recipes professions="Alchemy, Inscription"
skill_tier="Khaz Algar" max_recipes=500 create_products=true venture_id=N`
reads those professions' recipes, that expansion only, into `recipe`
records filed under `recipe` categories (profession / tier / section); a
`result` ending `cursor=P/T/O` means more remain -- pass it back as
`cursor=...`; without `create_products` recipes whose items have no product
are skipped and named. `act data_source ID set_venue_fees [cut_percent=5]
[duration_hours=48] [replace=true]` gives every venue of the store a record
with the `wow_auction` fee model (kept where one is set), so scans and
Crafting count the cut and deposit.

## Traps across feeds, market and arbitrage, in the order they bite

- **Which organization.** The `feeds` and `market` verbs and `arbitrage
  scan|record|export` answer for the **active organization** -- with a
  token, always the default one -- unless told `organization_id=N`. It is an
  option on `market browse|deals|venues|instrument|alerts|quote` and the
  arbitrage scan verbs, and a trailing word after the positional ones
  elsewhere: `feeds sync ID --wait organization_id=N`, `feeds runs ID
  organization_id=N`, `feeds due organization_id=N`, `market promote SOURCE
  venue KEY organization_id=N`, `market watchlist [ID] organization_id=N`,
  `market alerts evaluate RULE_ID [--dry-run] organization_id=N` (anything
  else there is exit 2). A member is answered; anybody else NOT_FOUND (exit
  3). A source, list, rule or scan row of another organization than the one
  asked about is NOT_FOUND too -- so a second organization's source asked
  about without `organization_id` reads exactly like a missing one ("No such
  data source in organization 1"). `organization_id=two` is exit 2.
  `arbitrage calc` and `registries` read no organization, and `arbitrage
  close|reopen|abandon|execute` act on a record that already has one:
  `organization_id=` there is "Unknown action parameter" (exit 2).
- **Wire names use underscores**, and `describe` is the truth:
  `data_source_id`, `group_key` (not `group`), `venue_namespace`,
  `instrument_namespace`, `fee_model`, `fee_params`, `transfer_cost`,
  `threshold_number`, `cooldown_minutes`, `buy_venues`, `trade_id`. (The
  `market` verbs take the *pages'* query names `source`/`group`/`category`
  as well as the report spellings.)
- **Money names its currency.** A bound on a scan or a deal --
  `min_value="10.00 GOLD"`, `min_profit=`, `max_capital=`, `total_stake=`
  -- is refused bare, never read as dollars. The calculators are the other
  way round: `--stake 100`, `fixed=`, `transfer=` and a flip's buy and sell
  prices are read in the install's default currency, so name it there too.
  A leg's bare `amount=100` is read in its venue's currency. Ratios are
  plain percent strings (`min_roi=15`, `max_pct=80`); `min_confidence` is a
  0-1 fraction.
- **`series:` is a price source, not a record source.**
  `price_source=series:min@realm-a` prices from the stores; an
  observation's `source` may not start with `series:`; a number basis
  (`quantity`, `sale_rate`, `sold_per_day`) is refused as a price.
- **`track: known`** stores only the instruments in the settings'
  `instruments` list and the `instrument` records filed under the source --
  promote or create the instrument first, or a run stores nothing for it.
- **Exec plugins need `plugins.allow_exec: true`** (operator config,
  default off): with it off an exec plugin does not load at all -- missing
  from `plugins list`, its provider name unknown -- and a source frozen while
  it was off fails its sync saying so. Nothing from the CLI turns it on.
