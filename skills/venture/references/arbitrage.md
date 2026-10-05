# Arbitrage trades, legs and the books

Read this to record, execute, close, reopen or abandon an arbitrage trade,
or to explain how one posts. Finding opportunities (the scan, presets,
calculators, exports) is in [arbitrage-scan.md](arbitrage-scan.md). Source:
`docs/arbitrage.org`; worked examples `docs/examples/arbitrage-*.org`.

Module `arbitrage` (requires `marketdata` and `ledger`; suggests
`production` and `goods`). Check `describe arbitrage_trade` and `describe
arbitrage_leg`. Financial: an organization **finance** member records and
closes trades; an editor member is refused (exit 5). A second-actor rule
wraps execute, close, reopen, abandon and record in one proposal.

## The records

- `arbitrage_trade`: `name`, `strategy` (spread, transform, deal, cover,
  back_lay, flip or a plugin's -- an exact string; the report groups by it),
  `venture_id`, `status` (planned, open, closed, abandoned), `opened_at`,
  `closed_at` (both derived), `expected` (technical JSON, e.g.
  `{"profit":["40 GOLD"]}`), `close_journal_id` (technical), `tags`,
  `notes`. **`closed`/`abandoned` are set only by the `close`/`abandon`
  actions and left only by `reopen`** -- writing them is refused (exit 8).
- `arbitrage_leg`: `venue_id`, `trade_id` (required), `kind` (buy, sell,
  fee, transfer, stake, payout, refund; `write_off` is the abandon
  action's), `status` (planned, executed, failed, cancelled -- **only
  executed posts**), `instrument_id`, `inventory_item_id` (buy/sell only),
  `location_id` (whose holding the money moves through), `quantity`,
  `unit_price`, `amount`, `fees`, `occurred_at`, `cost`/`cost_detail` and
  `inventory_txn_id` (technical, set by execute). **A bare amount ("100")
  is read in the venue's currency** (the book currency when the venue names
  none); an amount in another currency than the venue's is refused.
  Negative only on a transfer (arriving). Fees in the amount's currency. A
  unit price x quantity fills an empty amount.

## Posting

- Saving a leg `status=executed` posts it: Dr arbitrage positions / Cr the
  venue's cash for money out, the reverse for money in, fees to arbitrage
  fees. Saving it unchanged posts nothing; changing an executed leg
  reposts; cancelling it reverses. The venue's cash is its location's
  holding (so the holding floor applies, from the leg's date), else its
  account, else the organization's cash.
- **A leg with `inventory_item_id` is executed only by `act arbitrage_leg ID
  execute`** (setting `status=executed` is refused): a buy is received paid
  from the venue's cash at an exact split of the amount, a sell is issued
  at FIFO cost into the position (units from a multi-currency craft carry
  each currency's share). **An executed stock leg cannot be changed or
  cancelled** -- record a counter-leg. An executed leg of a closed or
  abandoned trade is frozen until `reopen`.
- Accounts: control-map classifications `arbitrage_positions`,
  `arbitrage_gains`, `arbitrage_fees`; unmapped they are made as
  `<org>:1460`, `<org>:4960`, `<org>:6960` (only writers make them; reading
  a trade never creates an account).

```bash
venturectl act arbitrage_leg 12 execute occurred_at=2026-03-05T10:00:00Z
venturectl act arbitrage_trade 4 close closed_at=2026-03-07T10:00:00Z
venturectl act arbitrage_trade 4 reopen
venturectl act arbitrage_trade 4 abandon goods=write_off      # or goods=keep (default)
venturectl --stage act arbitrage_trade 0 record organization_id=1 name="Peacebloom flip" \
    strategy=spread expected='{"profit":["40 GOLD"]}' \
    legs='[{"kind":"buy","venue_id":3,"amount":"100","status":"executed"},
           {"kind":"sell","venue_id":4,"amount":"150","status":"executed"}]'
```

The same as verbs (typed from the action schemas; `--stage` on all five):
`arbitrage execute LEG_ID [occurred_at=T]`, `arbitrage close|reopen|abandon
TRADE_ID [closed_at=] [goods=keep|write_off] [abandoned_at=]`, and
`arbitrage record name=N legs=JSON|@FILE [strategy=S] [venture_id=N]
[expected=JSON|@FILE] [notes=T] [organization_id=N]`. An unknown parameter
is exit 2.

- `close` moves what is left on the positions account to arbitrage gains,
  **one journal per currency section, never added together**, and is
  refused before the last executed leg's date. `reopen` reverses it.
  `abandon goods=write_off` writes unsold stock off at cost (a `write_off`
  leg) before closing; `goods=keep` leaves it in stock.
- `record` (type-level, stageable) creates the trade and its legs in one
  transaction and executes the legs marked `"status":"executed"` in order.
  A leg naming anything but kind, status, venue_id, location_id,
  instrument_id, inventory_item_id, quantity, unit_price, amount, fees,
  occurred_at and notes is refused. A member must pass `organization_id`.
  `location_id` moves the leg's money through that place's holding (the
  character who bought or sold) instead of the venue's.

## How trades show up

Report `arbitrage_performance` -- finished (closed or abandoned) trades in
the period they finished; `group_by` (`strategy` default, `venue_pair`,
`instrument`, `month`), `strategy` (exact), `venture_id`,
`organization_id`. One row per group **and currency**, book currency first:
`trades`, `wins`, `hit_rate`, `realised`, `capital`, `roi`, `fees`,
`avg_hold_days` (group-wide, repeats per row), `expected`, `slippage` (only
trades whose snapshot named that currency).

```bash
venturectl report arbitrage_performance this_month group_by=venue_pair strategy=spread
```

The operational `pnl`, `ventures` and `monthly` reports add "Arbitrage
gains", "Less arbitrage fees" and "Arbitrage result" for trades finished in
the period ([reports.md](reports.md)); legs are neither sales nor expenses,
so nothing is counted twice. Flips recorded from an external ledger are
closed trades of strategy `flip` ([accounts.md](accounts.md)).
