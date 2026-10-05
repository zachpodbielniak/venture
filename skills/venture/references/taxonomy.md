# Taxonomy: categories, locations, tags, custom fields, venture types

Read this when filing records into trees, adding a field without writing C,
or creating a venture whose type declares extra fields. Sources:
`docs/taxonomy.org`, `docs/custom-fields.org`, `docs/extending.org`.

## Categories and locations

- `category` (core, always on): `name`, `parent_id`, `applies_to` (a record
  type name such as `product`, or blank for any), `position`, `color`,
  `description`. `location` (sales module): `name`, `parent_id`, `kind`
  (free text: warehouse, bin, character, bank, guild...), `description`,
  `active` -- **set `active=true` yourself**; a new one is inactive
  otherwise. Locations also carry `data_source_id`/`external_ref`/
  `mirror_state` when the market data mirror made them ([feeds.md](feeds.md)).
- Build a tree top-down: create the parent, read its id, then the child
  with `parent_id=`. A child's `applies_to` must equal its parent's.
- Refused (exit 8): a loop ("would close a loop"), a parent in another
  organization, more than 32 levels, an `applies_to` that is not a record
  type, a parent grouping a different type, and changing `applies_to` on a
  category that has children.
- `product.category_id` and `inventory_item.location_id` are the
  references; the old `category`/`subcategory`/`location` text fields
  still exist and are not kept in step. A product cannot take a category
  whose tree groups another type ("groups expense records, not product").
  Any reference to `category` on any type -- including custom reference
  fields and an `alert_rule`'s category (judged as an instrument's) -- is
  held to the tree's `applies_to`.
- Paths ("Materials / Herbs") are computed, not a field: there is nothing
  to `get` or `update`. Rename the parent and every path follows. Pickers
  show the node's own name.
- A location that holds money is a *holding* (an `account` with
  `location_id`) -- see [currencies.md](currencies.md).

## Tags

`tags` on `product`, `inventory_item`, `sale` (and tickets, listings,
sessions, goals, trades...) is one comma-separated string, found by
`search=`.

```sh
venturectl create category name=Materials applies_to=product
venturectl create category name=Herbs parent_id=1 applies_to=product
venturectl update product 12 category_id=2 tags=herb,farmable
venturectl create location name="Alt 1" kind=character active=true
venturectl list product search=farmable
```

Reports group by tree: `group_by=category` with `category_depth=0` rolls
up to the top level (`listing_performance`, `session_performance`,
`report aggregate ... group_by=product_id.category_id`), and
`category_id=N` options include everything beneath N.

## Custom fields (module `custom_fields`)

Three records: `accounting_custom_field` (`record_type`, `name`, `kind`,
`required`, `options` JSON), `accounting_layout` (`record_type`,
`field_order` JSON array), and `custom_field_value` (a derived index --
writing it directly is refused). Kinds: `string`, `text`, `integer`,
`double`, `boolean`, `enum` (choices as a JSON string array in `options`),
`money`, `date`, `datetime`, `reference` (target in `options`:
`{"target":"category"}`).

```sh
venturectl fields define record_type=vendor_bill name=po_number kind=string required=true
venturectl fields layout record_type=vendor_bill field_order='["number","po_number"]'
venturectl fields value record_type=vendor_bill record_id=1 name=po_number value=PO-42
venturectl fields define record_type=sale name=shelf kind=reference options='{"target":"category"}'
venturectl fields define record_type=product name=weight_kg kind=double
```

- Values live in the owning record's `attributes`: `fields value` PATCHes
  that object (`PATCH /api/v1/TYPE/ID` with `{"attributes":{"NAME":V}}`;
  `null` removes one). Empty clears an optional field and fails a required
  one. A value write bumps the parent's version -- re-read before saving it.
- Names must be identifiers and may not shadow a built-in property. A
  `reference` value follows the built-in rule: a positive id, checked when
  written (missing, deleted or module-off target refused), a kept value left
  alone. It has no GObject property, so the form's query-string prefill
  (`?company_id=4`) skips it. `double` is a finite non-money decimal.
- Definitions and layouts: `/settings/fields` (editors may add them).
  `report aggregate` can group and sum by a custom field's name.

## Declarative venture types

A venture's `venture_type` names a declarative type -- a YAML file in
`data/venture-types/`, or a plugin's. Shipped: `books` (imprint,
primary_retailer, royalty_rate, isbn_block, editor), `etsy` (shop_name,
shop_section, fulfilment, listing_fee, primary_material), `newsletter`
(platform, cadence, paid_tier_price, free_to_paid_target),
`virtual_economy` (world -- required, region, currency_code, faction,
handle, marketplace: `auction_house|player_trade|vendor|platform_store|mixed`)
and `general` (kind: `household|hobby|club|project|personal|other`,
purpose, members: integer >= 0). `GET /api/v1/venture-types` (or
`/api/v1/venture-types/NAME`) lists each type's fields -- `describe
venture` does **not**, because they live in the attribute bag.

- Write them with `attributes.NAME=value`, in the same command as the rest:

  ```sh
  venturectl create venture name="Silverfen AH" venture_type=virtual_economy \
      attributes.world="Evermoor Online" attributes.marketplace=auction_house
  ```

  Over REST they are the nested `attributes` object; a PATCH merges, `null`
  removes one (`fields value record_type=venture record_id=ID name=N
  value=V` PATCHes one too).
- **The type's rules are checked at the save** (exit 8, validation):
  `required` fields present, enum values within `choices`,
  `integer`/`double` text that parses as a number within `min`/`max`.
  Creating a `virtual_economy` venture without `attributes.world` is
  refused -- so do not create it bare and patch the attribute on later.
- **Only what the save writes is checked.** Creating a venture, or changing
  its `venture_type`, checks every declared field; otherwise only the
  attributes whose value changes. A venture stored before a rule existed
  stays editable.
- **An unregistered `venture_type` is refused when written**, naming the
  registered ones -- unless no types are loaded at all (plugins off).
  Keeping an old, since-removed type is allowed.
- A new type is a YAML file, no rebuild: see [plugins.md](plugins.md).

`make demo` seeds that example as a **second organization** ("Evermoor
Trading", book currency `GOLD`); pass `organization_id=ID` to its reports
(`listing_performance`, `session_performance`, `recipe_margin`,
`goal_progress`, `goal_materials`, `aggregate`), or they answer for the
default organization and read nothing.

`docs/examples/game-economy.org` builds a whole game economy -- currency,
organization, venture, trees, stock, prices, sessions, recipes, listings, a
goal and a dashboard -- with commands that run top to bottom.
