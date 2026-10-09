# Market data records, the price oracle, alerts and the Trading pages

Read this to promote venues and instruments, price something from the
feeds, browse deals and venues, keep watchlists, or set alert rules. Feeds
and the store are in [feeds.md](feeds.md) (including the
*which-organization* and money traps that apply here too). Source:
`docs/market-data.org`.

## The records (module `marketdata`, on by default; requires `market`)

`venue`, `instrument`, `watchlist`, `watchlist_entry`, `alert_rule`,
`alert_hit` are generic records. An instrument names its store row
(`data_source_id` + `key`) and the `product_id` it is; that link is what
lets reports price a product from feeds. A venue's `group_key` is its
region/realm group; its `fee_model`/`fee_params` feed the scan
([arbitrage-scan.md](arbitrage-scan.md)); its `location_id` is the purse
its money moves through.

- `market promote SOURCE_ID instrument|venue|account KEY` is how a store's
  row becomes a record (it prints the record; its id is `.id`); an account
  becomes a `location`, and the venue it trades on gets that location as
  its `location_id` when it had none. Run it twice and you get the same
  record; a deleted one is restored (automatic promotion after a run never
  restores). Do not `create instrument` with a key a record already has:
  `external_ref` (namespace:key) is unique per organization *including
  deleted rows*, so the save is refused -- restore or promote instead.

## The price oracle

```bash
venturectl market quote 42 basis=market venue=eu organization_id=2
venturectl market quote product=12 basis=region_median currency=GOLD fallback=true
```

`market quote ID|product=ID [basis=B] [venue=KEY|GROUP] [venue_id=N]
[at=DATE] [currency=C] [prefer_currency=C] [fallback=true] [source=S]
[organization_id=N]` answers `{basis, found, price, value, evidence}`.
Bases: `market` (default), `min`, `market_14d`, `historical_60d`,
`region_median`, `region_p33`, `region_market_avg`, `sale_avg`, and the
numbers `sale_rate`, `sold_per_day`, `quantity` (in `value`).

- It never converts currencies. `currency=EUR` with a figure in USD is
  `found: false` and `evidence.note` says why. Nothing observed is `found:
  false`, not an error -- read `evidence.note`.
- Without `venue=`, a group-wide basis (`region_*`, `market`, the 14/60-day
  figures) uses the store's only group; a store with several groups is
  refused -- name one (`venue=eu`). `venue=` is a venue key when the store
  knows one by that name, otherwise a group.
- `fallback=true` lets the newest `price_observation` answer when no store
  does (prices only); `evidence.origin` is then `observation`.
- `recipe_margin` and `goal_materials` take
  `price_source=series:<basis>[@<venue or group>]`, e.g.
  `series:min@realm-a`. It needs `marketdata` and `feeds` on and never
  falls back to observations.

## The Trading pages and their verbs

Each page under Trading (`/market/browse`, `/market/deals`,
`/market/venues`, `/market/i/SOURCE/KEY`, `/market/watchlists`,
`/market/alerts`) has a JSON twin (`/api/v1/market/...`) and a verb:

```bash
venturectl market browse [source=N] [search=T] [category=PATH] [venue=KEY] [group=G] \
    [stock=true] [sort=COL] [dir=asc|desc] [page=N] [per_page=N] [organization_id=N]
venturectl market deals group=eu min_value="10.00 GOLD" max_pct=80 top=20
venturectl market deals venue_group=characters venue=3676 sell_venue=11  # buy at one realm, sell at another
venturectl market deals venue=Farstriders     # any member realm's name picks its whole connected realm
venturectl market venues [source=N] [group=G]
venturectl market instrument 1 2589 units=200        # SOURCE_ID KEY; units= prices a bulk buy
venturectl --format json market instrument 1 2589 range=90d compare=region  # the price history in .history
venturectl market watchlist [ID] organization_id=2   # 'watchlists' is the same verb
venturectl market alerts [count=N]                   # rules and recent hits
venturectl market help                               # every verb, options, examples
```

- The verbs take the **page's** query names (`source`, `group`,
  `category`) and also the report spellings (`data_source_id`,
  `group_key`, `category_path`). Each option is typed before anything is
  sent: `page=two`, `stock=maybe`, `dir=sideways`, an unknown name
  (`limit=5`) are exit 2. `browse` sorts only by `min_price, quantity,
  market_value, region_median, pct_vs_region, sale_rate, sold_per_day,
  deal_price, listings, name, updated, venue` (anything else is a 400,
  exit 2); `per_page` is at most 200 (default 50).
- Deals' `venue_choices` are one per **connected realm** across every
  source (Blizzard keys a connected realm by id, TSM each realm by slug):
  `{name, value, group_key, members, venues: [{data_source_id, venue_key,
  taken_at, age_seconds, stale}]}`, labelled by the realms a source keeps
  alone, e.g. `Cenarius (+ Cairne, Frostmane, ...)`. `venue`/`sell_venue`
  take the label, any member realm's name or any source's key; each row
  says its `realm`. A venue group is read as whole connected realms.
- **A dip or the new normal.** The item page's *Price history* charts the
  lowest price, market value and quantity over `range` (`24h`, `7d`, `14d`
  default, `90d`, `all`; anything else is a 400): hour by hour within
  `series.hourly_days`, day by day beyond, never both on one line. Gaps are
  hours (or days) nothing was stored for. `compare=region` adds the group's
  median as it stood then, `compare=VENUE_KEY` another venue (a 404 if it
  never listed the item). `GET /api/v1/market/history/SOURCE/KEY` answers
  the history alone. Deals and Browse rows carry `trend_7d` (42 four-hour
  lows), `median_7d`, `median_7d_hours` and `vs_median_7d_pct` (-30 = 30%
  under the week's median; null under 12 hours of prices); the CLI tables
  show it as `vs 7d`.
- **Every price says how old it is.** Deals rows: `buy_taken_at`,
  `buy_age_seconds`, `buy_stale`, and `sell_*` likewise (the `sell` object
  unprefixed); an item's `base` and `venues[]` and arbitrage `buy`/`sell`
  sides: `age_seconds`, `stale`. Stale is older than
  `series.stale_minutes` (120) or no time at all; the root says
  `stale_after_seconds`. The pages show "12m"/"3h" under the realm and grey
  a stale price with a "stale" flag. Check it before trusting a "cheapest
  realm": a realm whose feed stopped looks exactly like a cheap one.
- **How fast a deal sells, and what it can really make.** With a sell
  side a Deals row carries `sell_sold_per_day` and `sell_sale_rate` (the
  sell venue's 14-day store estimate: sold ÷ days, and sold ÷ (sold +
  expired) -- a listing that could have run out is expired, never sold;
  a cancellation still reads as a sale), `expected_sales` (sold a day ×
  `horizon_days`, default 7, rounded down) and `realisable_profit` /
  `realisable_units` / `realisable_cost` / `book_units`: the buy venue's
  book walked cheapest first while a unit costs less than the net sale,
  no more units than `expected_sales`. Null when there is no velocity,
  never 0. `min_sold_per_day=N` sells only where at least N go a day
  (else the deal is dropped); `sort=realisable` or `sort=sold`.
  `totals.realisable_profit` sums them.
  `venturectl market deals sort=realisable min_sold_per_day=2 horizon_days=3`.
- **TSM beside the live price.** A Deals row (and an item's page) carries
  `ref` when another source of the organization with the **same
  `instrument-namespace`** knows the item: `region_market`,
  `region_historical`, `region_sale_rate`, `region_sold_per_day` (the
  `region-<group>` venue tsmctl fills), `buy_vs_region_pct`,
  `sell_vs_region_pct`, and `buy_realm_market`/`sell_realm_market` with
  their `_vs_realm_pct` (the connected realm's TSM value). No `ref` at
  all when nothing matches -- absent, not zero. Keys join exactly: gear
  variants (Blizzard's `:m...` modifiers) usually have none.
- `market instrument SOURCE_ID KEY`: KEY is the store key (`herb`,
  `2589:b1234`), not an instrument record id; a key with `/` is fine.
- Tables show money with its currency and leave a missing figure blank,
  then the answer's notes (`-q` drops them); `-f json` is the whole answer
  (notes, echoes); `-f csv` the rows.

Reports reading the same answers (the period does not narrow any of them:
they read the stores as they are):

- `market_deals [data_source_id=N] [venue=KEY] [group_key=G]
  [category_path=PATH] [min_value="10.00 GOLD"] [max_pct=80] [top=N]` --
  instruments in stock at or under their group's deal price (the median of
  the group's lowest prices, or the 33rd percentile with 15+ venues),
  cheapest against the region first. `min_value` compares in its own
  currency only; `max_pct` is a percent of the region median; `top` 1-500
  (50). There is no `limit` option.
- `venue_index [data_source_id=N] [group_key=G]` -- per venue: shares
  cheaper/equal/dearer than the region, price to region, listings, last
  snapshot, the learned update interval and the data's age.
- `watchlist watchlist_id=N` -- the list priced now against its targets; a
  target in another currency is not compared.

## Alert rules and hits

`alert_rule` and `alert_hit` are generic records. Run `describe
alert_rule` for the kind nicks; each kind takes only its own thresholds,
and the others must be left empty or the save is refused:

- `below`/`above`/`spread` take `threshold` (money, e.g. `"1.20 USD"`);
  `pct_vs_reference` (a percent: 80 fires 20% under), `shortage` (units)
  and `spike` (a percent change, negative for a drop) take
  `threshold_number`; `spike` also needs `window_hours` (1-336);
  `entry_match` takes `pattern` (plain text, not a regex); `out_of_stock`,
  `back_in_stock`, `undercut` and `collect_ready` take none.
- The operator's-account kinds read a store's accounts, positions and
  mail: `position_expiring` and `inbound_expiring` take `threshold_number`
  = **hours** ahead (0 < n <= 720; 2 fires on what lapses within two
  hours); `account_stale` takes `threshold_number` = **days** unseen (0 < n
  <= 365; shared and guild accounts are never judged); `collect_ready`
  (mail with money or goods, or positions expired awaiting login) takes
  nothing. Their hits carry `account_key`; `group_key` narrows by the
  *account's* group (a realm).
- Every market kind but `undercut` and `entry_match` needs a scope:
  `watchlist_id`, `instrument_id` or `category_id` (an instrument
  category). The two expiring kinds may take one; `account_stale` and
  `collect_ready` refuse one. `venue_id` or `group_key` narrows, never
  both. `enabled` starts true and `cooldown_minutes` 60;
  `notify_username` puts hits in that person's inbox.
- **The two ways to evaluate a rule default opposite ways.** `act
  alert_rule ID evaluate` only looks; `record=true` writes. `market alerts
  evaluate RULE_ID` **writes** the hits (the cooldown applies; a webhook,
  the inbox and automations fire); `--dry-run` only looks. Neither is
  stageable and both are refused with feeds off or from inside an
  automation handler. Hits are written only by the server -- creating or
  editing an `alert_hit` is refused (403); read them with `list alert_hit
  rule_id=N`.
- A firing is `alert_hit.created` to webhooks and `on_created` to
  automations -- there is no separate alert event ([automation.md](automation.md)).
  Overflow past the per-run cap is a note on the run, never dropped silently.

Dashboard kinds `watchlist`, `market_alerts`, `source_health`
([dashboards.md](dashboards.md)).
