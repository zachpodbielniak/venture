# The operator's accounts: where to log in, inventory, trading P&L, books

Read this when a data source reports the operator's *own* accounts -- game
characters and banks fed by tsmctl, seller accounts -- and the question is
where to log in next, what is held and worth, what trading made, or how to
get that ledger into the books. Sources: `docs/market-data.org` ("The
Accounts pages", "The external ledger in the books"),
`docs/arbitrage.org` ("Flips from an external ledger"),
`docs/examples/wow-operations.org`.

## The pages and verbs

A source that reports accounts (a `push` source fed by tsmctl, say) has
four pages under Trading > Your accounts (`/accounts`,
`/accounts/SOURCE/KEY`, `/accounts/inventory`, `/accounts/pnl`), each with
a JSON twin (`/api/v1/accounts...`) and an `accounts` verb:

```bash
venturectl accounts attention organization_id=2           # where to log in next
venturectl accounts list by_login=true organization_id=2  # summary, logins, accounts
venturectl accounts show 2 "Drgold-Thorium Brotherhood" organization_id=2
venturectl accounts inventory group_by=login min_value="100.00 GOLD" organization_id=2
venturectl accounts inventory dead=true dead_days=30 organization_id=2
venturectl accounts pnl period=last_90_days group_by=week organization_id=2
venturectl accounts help
```

- `accounts [list|attention]` options: `source`, `group` (realm), `login`,
  `by_login`, `basis`, `expiring_hours`, `mail_days`, `stale_days`, `sort`,
  `dir`. The answer: `summary` (money on hand, inventory value, listed,
  mail, 30-day net, `logins`), `logins` (a card per login with its sums;
  "No login" last), `attention`, and `accounts` (one row each, with
  `login`/`login_name`). `attention` prints just the places and reasons,
  under a heading per login.
- `accounts show SOURCE_ID KEY [basis=] [ledger=N]`: one account --
  listings against the market (`undercut`, `vs_market_pct`, `urgency`),
  mail, holdings by place valued, the newest ledger rows. KEY is the
  account's key in the store; a `/` in it is fine.
- `accounts inventory`: everything held, per item across accounts, valued
  (`rows`, `totals`, `portfolio_value`; `by_login` with `group_by=login`);
  options `source`, `account`, `login`, `group_by=login`, `place`,
  `category`, `search`, `min_value`, `dead`, `dead_days`, `basis`, `sort`,
  `dir`, `page`, `per_page`.
- `accounts pnl`: the source's own trading ledger summed (`totals`,
  `buckets`, `top_items`) and the `flips`; options `source`, `period`,
  `group_by=week|month|account|instrument|venue|source|login`, `account`,
  `login`, `venue`, `instrument`, `label`, `top`.
- Ignored accounts: `account_ignore` records (`kind=character|realm`,
  `key`) leave characters, or every account on a realm, out of `list`,
  `attention`, `inventory` and `pnl` (and "My characters" on the market
  pages); each answer counts them (`summary.ignored` / `ignored`) and
  `show_ignored=true` includes them, marked `ignored` on the overview.
- **Professions are account attrs by convention**: `profession:<Name>`
  (skill), `profession_max:<Name>`, `profession_secondary:<Name>` (true),
  `profession_tiers:<Name>` ("Classic 300/300; Khaz Algar 65/100", one
  string). Overview rows carry `professions` (`{name, skill, max,
  secondary, crafting_url, tiers: [{label, rank, max, text,
  crafting_url}]}`; a malformed tier is `{text}` alone) and `crafting_url`;
  the page links each into `/arbitrage/crafting?character=KEY&profession=P&expansion=E`.
  Ignore one with `venturectl create account_ignore name=Medivh kind=realm
  key=Medivh`, or the buttons on its account page.

## What the answers mean

- **Where to log in next** (`attention`): one row per realm per login (an
  account's group; a shared bank is its own place), grouped a login at a
  time ("Log in to Main -> Thornmere"; `login` on each row), most urgent
  first within and across -- listings already expired, listings and mail
  about to expire, then money or items waiting in the mail, then accounts
  not seen in `stale_days` (14). Thresholds: `expiring_hours` (12, 1-720),
  `mail_days` (3, 1-60), `stale_days` (14). Each row has `title` ("Log in
  to Thorium Brotherhood"), `severity` (`overdue`, `soon`, `waiting`,
  `stale`) and `reasons[]` with `account_name`, `kind`, `text`.
- **Valuation** (`basis=`): `conservative` (default: the lower of the
  region's sale average and the realm's market value), `market`, `min`,
  `historical`, `region_market`, `region_sale_avg`; anything else exit 2.
  Unpriced items count for nothing and are noted. `min_value` must name the
  **source's** currency (`"100.00 GOLD"`); another or none is exit 2. Game
  currencies (`place=currency`) are left out unless asked for by place.
- **Logins**: `login=KEY` (the login's key, e.g. tsmctl's folder
  `ZAKMANN`) narrows `list`, `attention`, `inventory` and `pnl`;
  `group_by=login` / `by_login=true` group by it. The cards add up to the
  summary; a shared warband or guild bank is "No login", counted once. A
  `login` that matches nothing is an empty answer, not an error -- read
  `login_choices` (or `summary.logins`) for the keys.
- **Trading P&L** is the source's ledger, not the books. Sales are after
  the venue's cut; `net` = sales + income - purchases - expenses, per
  currency, integers. Flips match buys to later sales FIFO; `open_units` is
  bought and not yet sold, `held` what the accounts hold now.
- Reports: `accounts`, `account_holdings` (**not** `holdings`, which is the
  ledger's report of what each location holds in the books) and
  `external_pnl` (the period is its window). Options: `data_source_id`,
  `group_key`, `login`, `group_by` (`login`), `basis`, `expiring_hours`,
  `mail_days`, `stale_days`, `sort` / `account_key`, `place`,
  `category_path`, `min_value`, `dead_days`, `top` / `account_key`, `venue`,
  `instrument`, `source`. `report listing_performance group_by=location`
  gives a sale rate per character.
- Dashboard kinds `accounts_attention`, `accounts_summary`,
  `holdings_value`, `external_pnl` (each takes `options.login`); the
  `operations` template puts them together -- `dashboard create operations
  organization_id=N`, filed under the organization whose accounts it shows,
  or it opens in the default one and shows nothing.

## Getting accounts in

A `push` source takes what tsmctl sends (`feeds push ID FILE --wait
organization_id=N`, [feeds.md](feeds.md)); the optional `tsmctl` exec
provider (`plugins/exec/tsmctl`, needs `plugins.allow_exec`) pulls the same
lines by running `tsmctl export --format venture`, with settings
`accounts`, `sources`, `market`, `since`, `currency`, `include_internal`,
`offline`. The source must have a `currency`. The mirror turns accounts into
locations and positions into listings ([feeds.md](feeds.md)).
`docs/examples/wow-operations.org` is the whole setup.

## Into the books (opt-in, per source)

The source's `books` setting picks one door: `none` (default), `daily`
(`accounts post`, or `post_to_books: true` to post after every push) or
`trades` (`accounts record-flips`). The other verb is refused (exit 1); a
dry run works in any mode. **Always `dry_run=true` first** and read
`days_by_status` / `flips`. `books_from: 2026-09-01` starts the books there.

```bash
venturectl accounts post 2 dry_run=true organization_id=2            # from= until= account=
venturectl accounts post 2 organization_id=2                         # --stage proposes it
venturectl accounts record-flips 2 min_profit="5.0000 GOLD" limit=50 dry_run=true organization_id=2
venturectl report external_books data_source_id=2 organization_id=2
```

- **Daily journals** (`accounts post`, the type-level `act external_posting
  0 post_ledger data_source_id=N`): one journal per account per day -- sales
  Cr `trading_sales`, purchases Dr `trading_purchases`, other income and
  expenses, the net through the character's purse; an opening from the
  first balance seen; `trading_capital` (equity) for gold the ledger never
  showed arriving (mail from an alt). A day that changed is reversed and
  posted again by the next `post`; purged days and days in a closed period
  are kept. **Never delete an `external_posting` record** (refused). The
  answer is the data source with the pass in `result` (JSON text:
  `days_by_status`, `days[]`, `capital`, `notes`).
- **Flips** (`accounts record-flips`, `act arbitrage_trade 0 record_flips
  data_source_id=N`): each sale matched FIFO to earlier buys becomes one
  closed `arbitrage_trade` (strategy `flip`), legs at the buyer's and
  seller's places, `external_ref` `<uuid>:<sale>:<n>`; running it again
  records only what is new. Never `update` a recorded flip's `expected`,
  `external_ref` or `data_source_id`: refused, because the next run
  subtracts what `expected` says it took. Options `from`, `until`,
  `instrument`, `min_profit`, `limit`, `dry_run`.
- `report external_books data_source_id=N` shows each day: `posted`,
  `unposted`, `changed`, `left_out` (flips recorded)...
- Both are financial: an organization's finance member may, an editor
  member may not. With a second-actor rule on posting, the automatic pass
  does not post and notes why on the run.
