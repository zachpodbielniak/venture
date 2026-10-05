# Sessions of effort and goals

Read this for time-boxed runs of effort (a farming route, a market day, a
shift) and what they yielded, and for measurable goals with steps and a
shopping list. Sources: `docs/sessions.org`, `docs/goals.org`.

Refusals are validation errors (**exit 8**) unless noted.

## Sessions (module `sessions`, requires only `core`; suggests `sales`, `market`)

Check `describe session` and `describe session_yield`.

- `session`: `name`, `venture_id`, `activity` (free text, one spelling per
  kind of run -- the report groups by the exact string), `category_id`,
  `location_id`, `started_at` (required), `ended_at` (empty = still open),
  `minutes`, `cost` (money), `tags`, `notes`, `posted_at` (technical).
- **`minutes` is derived**: both times set -> the difference; only
  `started_at` and `minutes` -> `ended_at` is filled in; only `started_at`
  -> open, minutes 0. A `minutes` that disagrees with the two times is
  refused ("is 5 but the times give 40; change the end instead") -- but only
  when it *changed*, because a form posts the stored value back. An end
  before the start, negative minutes and a run over a year are refused.
  Writing `posted_at` by hand is silently restored.
- `session_yield`: **goods** (`product_id` + `quantity` >= 1, optional
  `unit_value`, optional `inventory_item_id`) **or money** (`amount` > 0),
  never both, never neither. `unit_value` is a valuation, not a cost.
  `inventory_txn_id`, `journal_id` and `holding_txn_id` are set by posting
  only (writing them is refused). Goods need the sales module; money does
  not.
- A **posted** yield (any of those three set) cannot change product,
  quantity, amount, stock or session, and cannot be deleted; nor can a
  session with posted yields. `unit_value` and `notes` stay editable.
  Correct stock with an adjustment, money with an expense or a
  `holding_txn` adjustment.

Posting is the `post` action on a session; it takes no arguments:

```bash
venturectl create session name="Elwynn loop" activity=herbing location_id=2 \
    started_at=2026-03-01T10:00:00Z ended_at=2026-03-01T10:40:00Z cost="0.5000 GOLD"
venturectl create session name="Mine run" activity=mining started_at=2026-03-02T19:00:00Z minutes=55
venturectl create session_yield session_id=8 product_id=11 quantity=38 unit_value="0.1200 GOLD"
venturectl create session_yield session_id=8 amount="12.3400 GOLD"
venturectl act session 8 post
venturectl list inventory_txn reference=session:8
```

- One transaction: each unposted goods yield arrives as a positive
  `production` inventory transaction (reference `session:<id>`, dated the
  session's end) with a **zero-cost** layer (nothing was paid; the session's
  `cost` is not spread over the units either) and is stamped with it and the
  stock it landed in; each unposted money yield lands in the holding at the
  session's location ([currencies.md](currencies.md)) as a journal (posted
  currency, `journal_id`) or a holding movement (memo, `holding_txn_id`);
  the session gets `posted_at`. Any refusal writes nothing. No location, or
  the ledger off: the money yield stays unposted for a later post.
- **Idempotent**: posting again posts only yields added since; nothing new
  is a success that changes nothing. Retrying is safe. Stageable.
- Stock: the yield's `inventory_item_id`, else the one item for the product
  at exactly the session's `location_id` (anywhere when none). Several
  places -> refused, naming them ("name the location_id to use"; set the
  session's location or the yield's stock); none -> "No stock of
  <product>" with what to create. Sales off -> refused naming the switch.

Report `session_performance` -- `group_by` (`activity` default,
`category`, `location`, `venture`; `location` refused with sales off),
`category_depth` (category/location only), `price_source` (exact; needs
the market module), `currency` (which observations count), `as_of` (value
every yield at that date instead of its session's end), `venture_id`,
`organization_id`; the period bounds `started_at`. One row per group **and
currency**: `sessions`, `open`, `hours` (finished sessions), `units`,
`value` (goods), `amount` (money yields), `cost`, `net`, `value_per_hour`,
`net_per_hour` (finished sessions over their hours), `priced_by`, `note`.
**`sessions`/`open`/`hours`/`units` repeat on each currency row of a group
-- never sum them down the column.** Goods are valued at `unit_value`, else
the latest observation from `price_source` (market on) or the list price
(market off); goods with no value are named in `note` and value, net and
the rates are blank, **not zero**.

```bash
venturectl report session_performance this_month group_by=activity price_source="market value"
venturectl report session_performance 2026 group_by=category category_depth=0
```

## Goals (module `goals`, requires only `core`; suggests `production`, `market`)

Check `describe goal` and `describe goal_step`.

- `goal`: `name`, `venture_id`, `parent_id` (a larger goal; no loops, same
  organization), `category_id`, `metric`, `unit`, `start_value`,
  `current_value`, `target_value` (doubles), `due_on` (date), `status`
  (`active` default, `paused`, `achieved`, `abandoned`), `achieved_at`,
  `tags`, `notes`.
- **`target_value` must differ from `start_value`** (0 to 0 is refused as
  no target). It may be **below** the start (a weight to lose): progress is
  `(current - start) / (target - start)`, never `current / target`.
- `achieved_at` follows `status`: stamped when it becomes `achieved` and the
  date is empty, kept when given, cleared when the status leaves
  `achieved`, refused when typed on a goal that is not achieved.
  (`goal_step.done_at` follows `done` the same way.)
- **Nothing updates a goal for you.** Reaching the target does not mark it
  achieved; ticking a step does not move `current_value`. Update both
  yourself (`goal_progress` notes "mark it achieved").
- `goal_step`: `goal_id` (required, same organization), `position` (lowest
  first), `name`, `from_value`/`to_value` (optional; must not run opposite
  to the goal), `recipe_id` (production module only; same organization),
  `repetitions` (**crafts/batches, not units**; >= 0, 0 = unsaid), `done`,
  `done_at`, `notes`.

```bash
venturectl create goal name="Alchemy 300" metric=level start_value=1 current_value=1 \
    target_value=300 due_on=2026-12-31
venturectl create goal_step goal_id=3 position=1 name="Minor Healing Potion" \
    from_value=1 to_value=25 recipe_id=7 repetitions=20
venturectl update goal_step 11 done=true
venturectl update goal 3 current_value=25
```

Report `goal_progress` -- `status` (a nick or several, comma separated;
every status by default; unknown refused), `category_id` (and beneath),
`venture_id`, `as_of` (the moment the pace is measured to -- it does not
read an old `current_value`), `organization_id`. One row per goal, by path
("Alchemy 300 / Alchemy 150"): `start`, `current`, `target`, `percent` (a
fraction, unclamped), `remaining` (in the goal's direction; negative = past
it), `steps_done`/`steps_total`, `due_on`, `days_left` (negative =
overdue), `forecast` (straight line from `start_value` at creation to
`current_value` at `as_of`; active goals with progress only), `status`,
`note`.

Report `goal_materials` -- `goal_id` (with its sub-goals; every active or
paused goal by default), `venture_id`, `price_source` (market module only;
`series:` allowed), `currency`, `include_on_hand` (`true` default /
`false`), `as_of`, `organization_id`. Reads steps **not done** with a
recipe and `repetitions` > 0: consumed components x repetitions summed per
product; **reusable components once, at the largest single step's need**
(never summed); less stock on hand in **every** location; `to_acquire`
priced at the latest observation (market on) or recorded cost (off). One
`Total` row per currency; a product with no price is named and its cost
blank -- **not zero** -- and the totals become "Total of priced lines".
Gross: what an earlier step makes is not netted. **Refused while
production is off** (`goal_progress` keeps working).

```bash
venturectl report goal_progress all status=active,paused
venturectl report goal_materials all goal_id=3 price_source="market value"
venturectl report goal_materials all include_on_hand=false
```

A dashboard `progress` widget on `goal` should set
`options.start_field=start_value` beside `target_field=target_value`, or a
downward goal reads as done before it starts.
