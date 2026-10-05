# Dashboards and widgets

Read this to build, change, export or read a dashboard. Module
`dashboards` (requires only core). Source: `docs/dashboards.org`.

A dashboard is a named page with a grid of widgets; a widget is a small
declarative question -- "how many incidents are open", "this month's P&L"
-- answered from a record type or a report. Both are ordinary record types
(`dashboard`, `dashboard_widget`), so CRUD, audit and staging apply. Each
widget kind writes its JSON and its HTML from one loop, so the page, the
API and the assistant see one answer; a widget that cannot be answered
renders an `error` in place and the rest of the page still works.

## Commands and routes

```bash
venturectl dashboards                       # what exists
venturectl dashboard factory                # every widget evaluated, as a table
venturectl -f json dashboard factory        # the whole answer
venturectl dashboard export factory > factory.json
venturectl dashboard import factory.json [organization_id=N]   # or - for stdin
venturectl dashboard create work [organization_id=N]           # from a template
venturectl dashboard templates; venturectl dashboard kinds
```

`GET /api/v1/dashboards`, `/dashboards/:slug` (`?data=0` skips evaluation,
`?organization_id=N` scopes), `/dashboards/:slug/export`, `POST
/dashboards/import` (`?organization_id=N`), `POST /dashboards/from-template`
(`{"template":"factory","organization_id":2}`), `GET /widget-kinds`, `GET
/dashboard-templates`; the generic routes on `dashboard` and
`dashboard_widget`. Pages: `/dashboards`, `/dashboards/:slug[/edit]`,
`/overview` (always the built-in page), `/` (the home dashboard, else the
headline cards, else the overview).

**Templates** (`dashboard create NAME`): `today` (the daily questions;
the demo's home), `factory`, `reporting`, `progress`, `work`, `operations`
(the operator's game accounts -- file it under the organization whose
accounts it shows), `overview`. A definition is plain JSON (no ids or
owners) -- keep it in git. Importing a taken slug suffixes a number; every
widget is checked before anything is written.

## The dashboard

`name`, `slug` (its address, unique among live dashboards), `purpose`
[overview|reporting|work] (sorting only), `layout`
[one_column|two_columns|three_columns|four_columns], `home` (only one;
marking one unmarks the rest), `personal` (owner only), `venture_id`.

**Scope**: a widget never scopes itself. The page decides -- the sidebar
pick (cookie `venture_entity`, `all` included), else the organization the
dashboard is filed under (its `organization_id`); the API uses
`?organization_id=` alone; the assistant reads across every entity. So file
a business's dashboard under that business's organization. `{me}` in a
filter is the viewer's username, `{user_id}` their id.

## Widget kinds (26)

| Kind | Shows | Reads |
|---|---|---|
| `count` | records matching a filter (optionally in a period on `field`) | `entity_type`, `filter`, `period`, `field` |
| `list` | newest records with chosen columns | `entity_type`, `filter`, `order`, `limit`, `columns` |
| `breakdown` | counts per value of an enum field, as bars | `entity_type`, `field`, `filter` |
| `upcoming` | due within N days on a date field, overdue first | `entity_type`, `field`, `filter`, `limit`, `options` `{"days":14}` |
| `record`, `links` | one record's fields; its links | `entity_type`, `record_id`, `columns` |
| `metric` | one report figure with change vs previous period | `report_name`, `period`, `field` (metric key), `options` |
| `report` | a whole report (tiles and/or table) | `report_name`, `period`, `limit`, `options` (`tiles`, `table`, the report's own) |
| `chart` | a report's rows as bars | `report_name`, `period`, `field` (value), `columns` (label), `limit` |
| `activity`, `confirmations` | audit trail; staged changes waiting | `entity_type`, `limit` |
| `note`, `actions`, `search` | text (`-` bullets, links); buttons `Label \| /path` per line; a search box | `body`, `entity_type` |
| `sum` | total of a money/number field per currency | `entity_type`, `field`, `filter`, `period`, `options.date_field` |
| `progress` | a value against a target field as a bar | `entity_type`, `field`, `record_id` or `filter`, `options.target_field`, `start_field`, `date_field` |
| `environments`, `milestone` | running releases; a milestone's done/planned (factory) | `filter`, `record_id` |
| `watchlist`, `market_alerts`, `source_health` | market data (marketdata / feeds) | `record_id`, `limit` |
| `opportunities` | top arbitrage opportunities from a preset or scan options | `record_id`, `options`, `limit` |
| `accounts_attention`, `accounts_summary`, `holdings_value`, `external_pnl` | the operator's accounts; each takes `options.login` | `options` (`data_source_id`, `basis`, ...), `limit`, `period` |

## Rules a widget is held to at the save

- The kind, record type (even one whose module is off -- a widget outlives
  the switch) and report must exist; `options` must be a JSON object.
- A filter is written exactly as on a list URL
  (`status=open&priority__in=high,urgent`); an order is a field with `-`
  for descending; columns are wire names (sensitive ones never shown).
- **`report`, `chart` and `metric` widgets run their report in the page's
  organization** -- never put `organization_id` or `venture_id` in their
  `options`; the save refuses it. Their `options` pass only the report's
  *declared* parameters (what `GET /api/v1/reports` lists), type-checked;
  `tiles`/`table` are the report kind's own switches:

  ```sh
  venturectl create dashboard_widget dashboard_id=4 kind=report \
      report_name=holdings period=all options='{"currency": "TICKET", "tiles": false}'
  ```

- `sum`/`progress` add up with the `aggregate` report's accumulator, money
  per currency, and their `period` needs `options.date_field` (no date
  field is an error in place, never an all-time figure). Their fields are
  checked while the type is visible. A `progress` widget on `goal` should
  set `options.start_field=start_value` beside `target_field=target_value`,
  or a downward goal reads as done before it starts.
- `opportunities` options are the scan's own names, checked at the save.

## The grid

A widget's place is `grid_col`, `grid_row`, `grid_width`, `grid_height`
(zero = wherever fits). The layout is resolved on every render: an
overlapping or off-edge placement flows instead of overlapping, so nothing
is ever lost. `PATCH /api/v1/dashboard_widget/:id` sets the four numbers
(range-checked only); the editor's `POST /dashboards/:slug/widgets/:id/place`
(`col`, `row`, `width`, `height`) is the checked placement and answers 409
naming the widget holding a cell; dropping a card on one of the same size
swaps them (`.../swap`). On a narrow screen cards stack and the editor will
not drag.

## Planned activity widgets

`list`/`count` on `entity_type=activity` with
`filter=status=planned&owner={me}`; `upcoming` with `field=due_at` and
`options={"days":14}`; `metric` of `worklist` with
`field=activities_overdue|activities_today`.
