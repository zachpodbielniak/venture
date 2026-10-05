# Market observations, listings, recipes and crafting

Read this for prices seen from named sources, listings that end sold,
partly sold, expired or cancelled, and recipes (bills of materials) and
crafting at FIFO cost. Feeds-driven market data is in [feeds.md](feeds.md)
and [market-data.md](market-data.md). Sources: `docs/market.org`,
`docs/production.org`, `docs/inventory.org`.

Refusals below are validation errors: **exit 8** (422), unless noted.

## Market: price observations and listings (module `market`, requires `sales`)

Check `describe listing` and `describe price_observation` for the wire
names and enums.

- `price_observation`: `product_id`, `source` (free text, matched
  **exactly** everywhere: `market value` != `Market value`; may not start
  with `series:`), `price` (money, not negative), `volume` (optional
  integer), `observed_at`, `location_id`, `notes`. Nothing here moves money.
- `listing`: `product_id`, `inventory_item_id`, `channel`, `quantity` (>=
  1), `quantity_sold`, `unit_price`, `deposit`, `fees`, `listed_at`
  (required), `closed_at`, `outcome` (`open` default, `sold`, `partial`,
  `expired`, `cancelled`), `sale_id`, `tags`, `notes`, `venue_id`
  (refused while marketdata is off), `expires_at`, `location_id` (the
  account or place that posted it), `bid` (per unit; not above
  `unit_price` unless that is 0), `external_id` (unique per organization,
  deleted listings included). `data_source_id` and `mirror_state` are the
  position mirror's; writing them is refused. A listing creates no sale and
  moves no stock.
- The save refuses a contradiction instead of guessing: `sold` with some
  but not all units counted (use `partial`), `partial` with none or all,
  `expired`/`cancelled` with units sold, a deposit or fee in another
  currency than `unit_price`, `closed_at` on a listing that was always open,
  `closed_at` or `expires_at` before `listed_at`, a `bid` in another
  currency or above the price, and an `external_id` another listing holds.
- It fills in: `outcome=sold` with `quantity_sold` 0 becomes all units; an
  ended listing with no `closed_at` gets now; moving back to `open` clears
  `closed_at`. A given `closed_at` is never overwritten.

```bash
venturectl create listing product_id=12 channel="auction house" quantity=20 \
    unit_price="0.1500 GOLD" deposit="0.0120 GOLD" listed_at=2026-03-02T10:00:00Z
venturectl update listing 7 outcome=partial quantity_sold=12
venturectl update listing 8 outcome=sold          # quantity_sold filled for you
```

Reports:

- `listing_performance` -- `group_by=product|category|channel|location`,
  `category_depth` (category only), `venture_id`, `organization_id`; the
  period bounds `listed_at`. One row per group **and currency**.
  `sale_rate` = units sold on *closed* listings / units on closed listings:
  open listings are left out, partial counts its sold units, cancelled
  counts as unsold, and a group with nothing closed has an empty rate (not
  0%). `units_closed` is the denominator. `group_by=location` gives a sale
  rate per character (each mirrored account is a location).
- `price_history` -- `product_id` (required; another organization's product
  is exit 3), `source`, `bucket=day|week|month` (UTC, ISO weeks). Columns
  `min`, `avg` (half to even, not volume-weighted), `max`, `volume`,
  `observations`, per bucket, source and currency.

```bash
venturectl report listing_performance this_month group_by=channel
venturectl report listing_performance 2026 group_by=category category_depth=0
venturectl report price_history last_30_days product_id=12 source="market value" bucket=week
```

An unknown `group_by` or option is exit 2.

## Production: recipes and crafting (module `production`, requires `sales`; suggests `market`, `goods`)

Check `describe recipe`, `describe recipe_component`, and the `craft`
action's parameters (`GET /api/v1/schema/recipe` -> `actions`).

- `recipe`: `name`, `venture_id`, `output_product_id` (required),
  `output_quantity` (batch size, >= 1), `category_id`, `active`, `notes`.
  **Pass `active=true`**: a boolean has no default through the API, and an
  inactive recipe has no Craft, refuses the action and is left out of
  `recipe_margin`.
- `recipe_component`: `recipe_id`, `product_id`, `quantity` (>= 1),
  `reusable`, `notes`. `reusable=true` marks a tool or catalyst: it must be
  on hand but is not used up (needed once per craft, not per batch).
  Leaving it out means **consumed**.
- The save refuses: no output product, a batch below 1, a component that is
  the recipe's own output, a second line for the same product (change the
  existing line's quantity instead), and any reference into another
  organization.

Crafting is the `craft` action; `act` types each argument from the
schema (`times` and `location_id` are integers):

```bash
venturectl create recipe name="Healing Potion" output_product_id=14 output_quantity=3 active=true
venturectl create recipe_component recipe_id=3 product_id=11 quantity=2
venturectl create recipe_component recipe_id=3 product_id=13 quantity=1 reusable=true
venturectl act recipe 3 craft times=5
venturectl act recipe 3 craft times=5 location_id=2 occurred_at=2026-03-14T18:00:00Z
venturectl --stage act recipe 3 craft times=20    # approval crafts, recounting stock then
venturectl list inventory_txn reference=recipe:3  # everything the recipe made or used
```

- One transaction: every consumed component leaves as a `production`
  inventory transaction (quantity x times), the output arrives as one, and
  the made units carry the consumed FIFO cost exactly (split into a
  one-minor-unit-higher layer and a floor layer, so no cent is lost). Any
  refusal writes nothing. No journal is posted (inventory to inventory). It
  returns the output's `inventory_txn`.
- Inputs costed in two currencies (GOLD dust, TICKET tokens) are **not**
  refused: the output gets one set of cost layers per currency sharing a
  lot (`lot_txn_id`), and a later sale of a made unit gives up its share of
  each. Such a transaction has no `unit_cost`; its `notes` say what each
  currency came to. Stock typed in by hand (no layer) is used as uncosted.
- Refusals say what to do: `Short of <product>` (unless its item allows
  negative stock), a reusable component not on hand (`allow_negative` does
  **not** apply to tools), a product kept in several places ("name the
  location_id to use"), no inventory item for the output ("create an
  inventory item for it there (product_id=... location_id=...)"), an
  inactive recipe, no components.
- `location_id` means exactly that location for every component and the
  output, not its children. The craft never creates the output's inventory
  item. A craft never issues through cost of goods sold.

Report `recipe_margin` -- `price_source` (exact; needs the market module,
refused without it; or `series:<basis>[@<venue or group>]` from the feeds,
never falling back to observations), `currency` (only prices observed in
it count; left out, the book currency's price wins wherever the product was
seen in it, else the newest in any; not a code is refused), `as_of`,
`venture_id`, `category_id` (and everything beneath it), `organization_id`.
One row per active recipe: `cost`, `value`, `profit`, `margin`,
`cost_per_unit`, `craftable_now`, `priced_by`, `note`. Market on: latest
observed prices only; a product never priced is named in `note` and its
figures are blank, **not zero**. Market off: item unit cost (else product
cost) for inputs, list price for the output. Two currencies in one recipe:
a note and no money figures.

```bash
venturectl report recipe_margin all price_source="market value"
venturectl report recipe_margin all category_id=4 as_of=2026-03-01
venturectl report recipe_margin all currency=TICKET
venturectl report recipe_margin all price_source=series:min@realm-a
```

Craft arbitrage (each input at its cheapest venue against the output's best
venue) is the `craft_arbitrage` report and the `transform` scan
([arbitrage-scan.md](arbitrage-scan.md)).
