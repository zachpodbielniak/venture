# Data model: market, production, sessions, goals, feeds, arbitrage

The generic "venture of any kind" features -- a market of observed prices
and listings, recipes and crafting, sessions of effort, goals -- and the
market data stack (feeds, the series store, venues and instruments,
alerts, arbitrage). Conventions are in [data-model.md](data-model.md);
usage is in [market-production.md](market-production.md),
[sessions-goals.md](sessions-goals.md), [feeds.md](feeds.md),
[market-data.md](market-data.md), [accounts.md](accounts.md),
[arbitrage.md](arbitrage.md) and [arbitrage-scan.md](arbitrage-scan.md).

## market (requires sales; suggests marketdata) -- reports `listing_performance`, `price_history`

- `price_observation` -- a price seen: **product_id**, **source** (free
  text matched *exactly*: `market value` != `Market value`; may not start
  with `series:`), **price**, `volume`, **observed_at**, `location_id`.
  Moves no money.
- `listing` -- goods offered for sale: **product_id**, **unit_price**,
  **listed_at**, `inventory_item_id`, `channel`, `quantity` (>= 1),
  `quantity_sold`, `deposit`, `fees`, `bid`, `closed_at`, `expires_at`,
  `outcome` [open|sold|partial|expired|cancelled], `sale_id`, `venue_id`,
  `location_id` (who posted it), `external_id` (unique per organization,
  deleted included). `data_source_id` and `mirror_state` belong to the
  position mirror. Creates no sale and moves no stock.

## production (requires sales; suggests market, goods) -- report `recipe_margin`

- `recipe` -- **name**, **output_product_id**, `output_quantity` (batch,
  >= 1), `venture_id`, `category_id`, `active` (pass `true`; inactive
  recipes cannot craft and leave `recipe_margin`). *action* `craft (s)`
  (`times`, `location_id`, `occurred_at`).
- `recipe_component` -- `recipe_id`, `product_id`, `quantity` (>= 1),
  `reusable` (a tool: needed on hand, not consumed; default consumed).

## sessions (requires core; suggests sales, market) -- report `session_performance`

- `session` -- a time-boxed run of effort: **name**, **started_at**,
  `ended_at` (empty = open), `minutes` (derived from the times), `activity`
  (exact string; the report groups by it), `cost`, `venture_id`,
  `category_id`, `location_id` (where money yields land), `tags`,
  `posted_at` (post's alone). *action* `post (s)`.
- `session_yield` -- goods (`product_id` + `quantity`, optional
  `unit_value`, `inventory_item_id`) or money (`amount`), never both.
  `inventory_txn_id`, `journal_id`, `holding_txn_id` are set by posting.

## goals (requires core; suggests production, market) -- reports `goal_progress`, `goal_materials`

- `goal` -- **name**, `metric`, `unit`, `start_value`, `current_value`,
  `target_value` (doubles; target != start; may be below), `due_on`,
  `status` [active|paused|achieved|abandoned], `achieved_at` (follows
  status), `parent_id -> goal`, `category_id`, `venture_id`.
- `goal_step` -- **goal_id**, **name**, `position`, `from_value`/
  `to_value`, `recipe_id`, `repetitions` (crafts, not units), `done`,
  `done_at`.

## feeds (requires core; opt-in `feeds.enabled`)

- `data_source` -- **name**, **provider** (`http_json`, `csv`,
  `file_jsonl`, `push`, or a plugin's: `blizzard_auctions`, `odds_api`,
  `supplier_csv`, `tsmctl`), `settings` (YAML text; no credentials),
  `schedule` (`auto`/empty, `hourly`, `manual`, or five cron fields),
  `enabled`, `track` [all|known], `currency` (the source's money),
  `venue_namespace`, `instrument_namespace`, `min_value`. Admin writes.
  *actions* `sync`, `test` (`unit`), `purge_history` (admin).
- `data_source_run` -- one recorded run (server-written): `status`
  [ok|partial|failed|deferred], `trigger` [schedule|manual|automation|push],
  `units`, `rows`, `requests`, `bytes`, quota, `error`, `notes`.
- Everything the source returns lives in its series store, not records.

## marketdata (requires market; suggests feeds) -- reports `market_deals`, `venue_index`, `watchlist`, `accounts`, `account_holdings`, `external_pnl`

- `venue` -- where trading happens: **name**, `kind`
  [other|marketplace|auction_house|bookmaker|exchange|supplier|store],
  `namespace`/`key`/`external_ref` (unique per organization incl. deleted),
  `group_key` (a region/realm group), `currency`, `fee_model` (`percent`,
  `commission`, `none`, or a plugin's) and `fee_params` (YAML),
  `transfer_cost`, `transfer_hours`, `location_id` (whose purse its money
  moves through), `account_id`, `data_source_id`.
- `instrument` -- a tradeable thing: **name**, `kind`
  [other|item|outcome|event|asset|sku], `namespace`/`key`/`external_ref`,
  `product_id` (what lets reports price a product from feeds),
  `parent_id` (an outcome's event), `category_id`, `attrs`.
- `watchlist` (`group_key`), `watchlist_entry` (`instrument_id`,
  `target_buy`, `target_sell`).
- `venue_group` -- **name**, `venues` (venue keys or names, comma
  separated; a name matches one part of a connected realm's name),
  `notes`. Picked as `venue_group=ID` on browse, find, deals and the
  instrument page; `venue_group=characters` is the realms the push
  sources' characters are on, with no record.
- `alert_rule` -- **name**, `kind` (below, above, pct_vs_reference,
  spread, out_of_stock, back_in_stock, shortage, spike, undercut,
  entry_match, position_expiring, inbound_expiring, account_stale,
  collect_ready), `basis`, one scope (`watchlist_id`/`instrument_id`/
  `category_id`), `venue_id` or `group_key`, `threshold` (money) or
  `threshold_number`, `pattern`, `window_hours`, `cooldown_minutes`,
  `notify_username`, `enabled`. *action* `evaluate` (`record`).
- `alert_hit` -- server-written: `rule_id`, `observed_at`, `message`,
  `observed`/`reference`, `subject`, `account_key`, `listing_id`.

## arbitrage (requires marketdata, ledger; suggests production, goods) -- reports `arbitrage_performance`, `arbitrage_scan`, `craft_arbitrage`, `external_books`

- `arbitrage_trade` -- **name**, `strategy` (exact string), `status`
  [planned|open|closed|abandoned] (closed/abandoned by actions only),
  `opened_at`/`closed_at` (derived), `expected` (JSON), `close_journal_id`,
  `venture_id`, `data_source_id`/`external_ref` (recorded flips).
  *actions* `close (s)`, `reopen (s)`, `abandon (s)`, `record (0,s)`,
  `record_flips (0,s)`.
- `arbitrage_leg` -- **trade_id**, `venue_id`, `location_id`, `kind`
  [buy|sell|fee|transfer|stake|payout|refund|write_off], `status`
  [planned|executed|failed|cancelled], `instrument_id`,
  `inventory_item_id`, `quantity`, `unit_price`, `amount`, `fees`,
  `occurred_at`, `cost`/`cost_detail`/`inventory_txn_id` (execute's).
  *action* `execute (s)`.
- `arbitrage_strategy` -- a scan preset: `strategy`, `data_source_id`,
  `buy_venues`, `sell_venues`, `options` (YAML of other filters).
- `external_posting` -- the books' memory of an external ledger day
  (server-written, never deleted): `kind` [day|opening], `day`, sales,
  purchases, income, expenses, `capital`, `fingerprint`, `journal_id`.
  *action* `post_ledger (0,s)`.

Related core/ledger types used throughout: `currency` (a game's GOLD at
exponent 4, tickets as memo), `location` (characters, banks, realms,
logins), `account` with `location_id` (a purse), `exchange_rate`.
