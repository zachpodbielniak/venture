# Finding opportunities: the arbitrage scan, presets, calculators, exports

Read this to look for flips, deals, craft arbitrage, surebets and back/lay
pairs in the market data, to record a scan row as a trade, or to run the
calculators. Trades and their posting are in [arbitrage.md](arbitrage.md);
the which-organization and money traps are in [feeds.md](feeds.md). Source:
`docs/arbitrage.org` ("Finding opportunities").

Everything here is read-only until recorded.

## The scan

Report `arbitrage_scan` -- opportunities **now** (the period is ignored)
by one `strategy`: `spread` (default; cross-venue flips, `buy_sources=N`
lists the N cheapest suppliers per item, 1-10), `deal` (under the group's
deal price), `transform` (a recipe's inputs at their cheapest venues;
`recipe_id`, `recipe_category_id` = a category and beneath it, `units` =
batches; a venue the store marks `region_wide` -- a commodity market -- is
open whatever `buy_venues`/`sell_venues` say), `cover` (surebets; `total_stake`),
`back_lay`, or a plugin's.

- Filters: `preset_id`, `data_source_id`, `buy_venues`, `sell_venues`
  (comma-separated **venue keys**), `venue_group` (`characters` or a
  `venue_group` id: both sides only at its venues, each connected realm
  whole across sources; a `buy_venues` outside it is set aside with a
  note; an unknown group is 404), `group_key`, `category_path`, `kind`,
  `instrument`, `units`, `sell_basis` (`min`, `market`, `sale_avg`,
  `region_median`, `region_market`, `bid`; `region_market` is another
  source's region market value -- TSM's, at venue `region-<group>` --
  joined on the item key only within the same `instrument-namespace`, so
  leave the TSM source in the scan), `min_profit`, `max_capital`, `total_stake`
  (**money names its currency**: `"10.00 GOLD"`; a bare amount is
  refused), `min_roi`, `min_sale_rate`, `max_buy_pct`, `share` (**percent
  strings**, `"15"` = 15%), `min_confidence` (0-1), `max_age_hours`, `sort`
  (`profit` default, `roi`, `roi_per_day`, `annualized`, `ev`,
  `confidence`), `top` (1-500). **An unknown option is refused by name.**
- A row with something unquoted (no price, a venue whose fee model is not
  loaded) has blank figures and names it in `missing`/"Unquoted" -- never
  zero. A venue with no record charges nothing and warns. Mixed currencies
  convert only through an `exchange_rate` (never inverted), else the row is
  skipped and a note says so.
- `cover` and `back_lay` leave out an event that has started (its
  `commence_time` attribute at or before now) and say so in a note; an
  event with no `commence_time` is kept and noted -- bound it with
  `max_age_hours`.
- Report `craft_arbitrage` -- per recipe, each input at its cheapest venue
  (a shopping list; a reusable input is bought once), the output's best
  venue and the profit: `recipe_id`, `units`, `data_source_id`,
  `buy_venues`, `sell_venues`, `venue_group`, `group_key`, `sell_basis`,
  `max_age_hours`.
- Each row's `buy`/`sell` side carries `taken_at`, `age_seconds` and
  `stale` (older than `series.stale_minutes`); the CLI table shows
  `buy stale`/`sell stale`. A `transform` row has them per input, on its
  `sell`, and flat as `buy_*` (the oldest input) and `sell_*`, plus
  `margin` (net / gross) and a warning when the sell venue has no fee model.

## Crafting and the flip planner

- `GET /api/v1/arbitrage/crafting` / `venturectl arbitrage crafting
  [option=value ...]` -- every recipe priced by `transform`, losses kept:
  `recipe_category_id`, `buy_realm` (empty: cheapest realm per reagent),
  `sell_realm` (empty: the best), `venue_group`, `units`, `sell_basis`,
  `max_age_hours`, `share`, `recipe_id`, `data_source_id`, and the
  filters `character` (account keys `Name-Realm`, comma separated or one
  parameter each), `profession`, `expansion` (category names, any case),
  `only_known` (1/0; default 1 once any recipe has a knower), `min_profit`
  and `max_cost` (a bare number is in the craft's currency), `min_margin`
  (percent), `min_sold_per_day`, `recipe_list` (recipe list ids, comma
  separated or one parameter each: their union; turns `only_known`'s
  default off; an unknown id is 400, another organization's 404);
  anything else is refused (exit 2). Every
  chosen recipe is priced -- not the scan's 200 by name; past 5000 it
  refuses and says narrow -- profit first, unpriced last (`unpriced`), and
  a bound leaves an unpriced recipe out and counts it in
  `excluded.unpriced`. Realms are named as Deals names them; an unknown one
  is NOT_FOUND. Rows add `recipe_category`, `expansion`, `known_by`,
  `knowers` (`{key, rank}`, rank like "Khaz Algar 65/100" from the
  account's attrs); the answer has `realm_choices`, `categories`,
  `character_choices`, `profession_choices`, `expansion_choices`,
  `recipes`, `options` (the scan question "Add to plan" sends back),
  `recipe_list_choices` (`{id, name, entries}`) and, asked,
  `recipe_list_filter`; every row has `recipe_lists` (`[{id, name,
  entry_id}]`, the lists it is on).
- **Recipe lists** (`recipe_list` + `recipe_list_entry`, arbitrage
  module; "favourites", "bag crafts"): put a recipe on one with
  `POST /api/v1/arbitrage/recipe-lists/add` `{"recipe_id": N, "list":
  "Name"}` (a name found ignoring case, else made; a second add is a 200
  that writes nothing) or `create recipe_list_entry recipe_list_id=..
  recipe_id=..` (once per list, validated); take it off with
  `.../remove {"entry_id": N}` or `delete recipe_list_entry ID`.
  `GET /api/v1/arbitrage/recipe-lists[/ID]` lists them / prices one (the
  Crafting answer), and `arbitrage crafting recipe_list=ID` is the same.
  **It computes nothing itself** -- quote its figures, they are the
  strategy's.
- **The plan is planned `arbitrage_trade`s**, not a record type: "Add to
  plan" (Deals, Arbitrage, Crafting; `POST /arbitrage/plan/add`) is the
  record path. `GET /api/v1/arbitrage/plan` / `arbitrage plan` regroups
  their planned legs: `buy` (per venue, an item once, the dearest price as
  the most to pay), `sell` (per venue, item and price), `fees`, `totals`
  per currency (`outlay`, `gross`, `cut`, `deposit`, `listing_loss`,
  `revenue`, `profit` -- never across currencies). Past 500 planned trades
  it refuses (CONFLICT). `reprice=1` re-asks each trade's stored
  `expected.question`: `ok`, `stale`, `unprofitable`, `unquoted`, `gone`,
  `unknown` (recorded before questions were kept). `arbitrage plan export
  csv|tsm [-o FILE]`, `arbitrage plan remove TRADE_ID` (only a trade
  nothing executed).
- **Presets** are `arbitrage_strategy` records (`name`, `strategy`,
  `data_source_id`, `buy_venues`, `sell_venues`, `options` as YAML of the
  other filters); the save refuses a misspelt filter.
- **Fee models**: a venue's `fee_model` (`percent`, `commission`, `none`,
  or a plugin's, e.g. the Blizzard plugin's `wow_auction`) and `fee_params`
  YAML are checked when written: `percent` takes `cut_percent`,
  `fixed_per_unit`, `fixed_per_order`, `min_fee`, `deposit_percent`,
  `deposit_basis`, `deposit_refundable`, `buy:`; `commission` takes
  `rate_percent`. A plugin's name written while the plugin is not loaded is
  refused.

## The verbs

```bash
venturectl report arbitrage_scan strategy=spread group_key=eu min_profit="10.00 GOLD" min_roi=15 top=20
venturectl report craft_arbitrage recipe_id=4 units=10
venturectl arbitrage scan spread group_key=eu min_profit="10.00 GOLD" min_roi=15 top=20
venturectl arbitrage record 1 spread group_key=eu min_profit="10.00 GOLD" min_roi=15 top=20
venturectl --stage arbitrage record name="Peacebloom flip" strategy=spread legs=@legs.json organization_id=1
venturectl arbitrage calc surebet 2.10 2.05 --stake "100.00 USD"
venturectl arbitrage export shopping_list transform recipe_id=4 units=10 -o list.txt
venturectl arbitrage crafting buy_realm=Thrall sell_realm=Thrall recipe_category_id=12
venturectl arbitrage plan reprice=1
venturectl arbitrage plan export tsm -o shopping.txt
venturectl arbitrage registries       # strategies, fee models, export formats, scan option names
venturectl arbitrage help
```

- `arbitrage scan STRATEGY [option=value ...]` asks the same question
  through `/api/v1/arbitrage/scan`; the table numbers its rows.
- `arbitrage record N STRATEGY [same options]` records row N: the CLI
  re-asks the question, takes the row's `key` and `narrow`, and the server
  re-runs the scan and performs `record` with the plan -- a moved
  opportunity is exit 3 "no longer there", a row number past the end is
  exit 3 too. **Pass exactly the options the scan had**, or row N is a
  different row. `arbitrage record KEY [options]` takes a key from `-f
  json` output instead. `--stage` proposes it (202 + confirmation);
  planning still promotes the legs' venues and instruments. The server
  never records from a JSON opportunity a client posted.
- `arbitrage scan|record|export` take `organization_id=N` like the
  `market` verbs; a row recorded with it is filed there. They **refuse**
  `venture_id`, `as_of`, `key`, `format` and `calc`, which the routes would
  ignore or drop (exit 2). Every other option travels as text; the server
  refuses by name one that neither the scan nor the chosen strategy
  declares, and a plugin strategy's own options get through.
- `arbitrage export csv|shopping_list|<plugin's, e.g. tsm> [STRATEGY]
  [options] [-o FILE]` writes the scan in a registered format, raw bytes to
  stdout or FILE.

## The calculators

`arbitrage calc surebet ODDS... --stake AMOUNT` (odds as bare words; a
negative American price would read as a flag, so write `odds="+150 -120"
format=american`), `calc back-lay BACK LAY --stake X [commission=5]
[back_commission=0]`, `calc flip BUY SELL [units=N] [cut=5] [fixed=AMOUNT]
[deposit=15] [refundable=true] [sale_rate=40] [sold_per_day=12]
[share=100] [transfer=AMOUNT] [transit_hours=0]`. They run on the server
(`GET /api/v1/arbitrage/calc`).

- Give the stake once (`--stake` or `stake=`); a flip takes none (its
  capital is the buy price). **A bare `--stake 100` is read in the
  install's default currency**, unlike a scan bound -- name it.
- Stakes are rounded to the currency's minor unit; `residual` is what the
  rounding left. A surebet's `payout` is the **least** rounded payout (what
  any result is sure to pay) and `profit` that less the stakes;
  `payout_ideal`/`profit_ideal` are the unrounded T/S and T(1/S - 1). A
  back-lay's `worst` rounds against you (winnings down, liability up);
  `ideal` is unrounded. **Quote the sure figures as what a person gets**,
  never an `_ideal` one.

The `opportunities` dashboard widget runs the same scan from a preset or
options ([dashboards.md](dashboards.md)).
