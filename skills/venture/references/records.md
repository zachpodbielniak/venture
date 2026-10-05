# Records: the shape every type shares

Read this before creating, updating, deleting or explaining any record.
Every rule here holds for every type, from every door (web, REST,
`venturectl`, MCP, assistant, automation), because they all end in one save.

## The spine

Every record carries these besides its declared fields:

| Field | Meaning |
|---|---|
| `id` | surrogate key; `0` in a reference means "none" |
| `uuid` | stable external identity, safe to share |
| `organization_id` | owning organization; scopes every query and report |
| `created_at`, `updated_at` | timestamps (UTC, ISO 8601) |
| `version` | optimistic-concurrency counter, bumped on every save |
| `deleted_at` | soft deletion stamp; `null` while live |
| `attributes` | a JSON bag: venture-type fields and custom fields live here |

Responses add `type` and `display_name`. No field may be named `id`,
`uuid`, `created_at`, `updated_at` or `version` -- that is why a release's
version is `number`.

## Field kinds and how to write them

`describe TYPE` prints the kind; write values like this:

| Kind | Write | Read back |
|---|---|---|
| `string`, `text` | plain text (`text` is long-form) | string or `null` |
| `integer`, `double` | `42`, `1.5` | number |
| `boolean` | `true`/`false` | boolean -- **there is no default through the API**: pass `active=true` yourself where it matters (recipes, locations) |
| `date`, `datetime` | `2026-03-14` or ISO 8601 `2026-03-14T10:00:00Z` | ISO 8601; a bare date is midnight UTC |
| `enum` | the nick (`issue_type=bug`), any case, `-` or `_` | the nick; never a number |
| `reference` | the target's `id` | integer (`0` = none) |
| `money` | `12.34`, `"12.34 EUR"`, `"$12.34"`, `"12g 34s 56c GOLD"`, or `{"amount":1234,"currency":"USD","exponent":2}` | `{"amount":1234,"currency":"USD","exponent":2,"formatted":"12.34 USD"}` |
| `json` | a JSON **string** (`denominations='[...]'`) | JSON |

- **Money** is integer minor units. Use `.formatted` for display and
  `.amount` for arithmetic; never sum `.formatted`. A bare amount is
  read in the record's own currency (its `currency` field, else a
  referenced document's, else the organization's book currency) -- never a
  silent "USD".
- **A filter on a money field compares minor units**:
  `gross__gte=10000` is 100.00 in a two-decimal currency; `gross__gte=100`
  is 1.00, and a decimal like `269.01` is not parsed as money. Filter
  amounts in minor units, or use `report aggregate`.
- **Wire names use underscores** (`invoice_id`); C and `/api/v1/schema`
  print dashes (`invoice-id`). A dashed key -- or any misspelt field name --
  in a create or update is ignored field by field and the save still
  succeeds (exit 0): the classic silent no-op. Only list *filters* refuse an
  unknown name. Copy names from `describe`.

## References are checked when written, not when kept

Writing a reference to a row that does not exist, is soft-deleted, or
belongs to a module that is off is refused (validation, exit 8) naming the
field and the id. A reference a save does not change is left alone, so a
record pointing at a since-deleted row stays editable. There are no SQL
foreign keys; the check is in the save. Look ids up first:

```bash
REPO=$(venturectl -f json list forge_repo name__eq=zach/venture | jq -r '.records[0].id')
venturectl create ticket title="It crashes" issue_type=bug repo_id="$REPO"
```

Some references must also share a parent (`VENTURE_COLUMN_FLAG_SAME_PARENT`,
"attention of"): a deal's contact must belong to the deal's company. A
category reference is held to its tree's `applies_to` ([taxonomy.md](taxonomy.md)).

## Soft delete and restore

`delete` stamps `deleted_at`; the row stays, reports over past periods still
reconstruct, and `restore` clears the stamp (not stageable; applies at
once). A deleted record still answers `GET /api/v1/TYPE/ID` (with
`deleted_at` set) but leaves lists unless `include_deleted=1` -- and a
filter like `deleted_at__not_null=1` finds nothing without it. Unique keys
(`external_id`, an instrument's `external_ref`, a membership) count deleted
rows: restore the old one rather than creating a duplicate. A merged CRM
record answers 301 to its survivor. Deleting a source document never
deletes its journal or holding movements ([ledger.md](ledger.md)).

## Versions and conflicts

Send `version` in a PATCH/PUT body to make the write conditional: a stale
one is 409 `conflict` ("changed by someone else since you loaded it").
Omit it and the write applies to whatever is current. Derived writes bump
the version too (a ticket's first-reply stamp, logged hours, a budget's
warning), so re-read before saving an object you held across them. A save
whose diff is empty changes nothing and still succeeds.

## Who owns which fields

Many state fields are a service's, not yours. Writing them through the
generic routes is refused or ignored, and the refusal names the service:

- Invoice `status`/`paid_at`/`workflow_state` come from settlement; a
  journal's `state` from posting; a subscription's `status` from billing;
  a lead's `converted` status and ids from `lead convert`; deal stages from
  `deal move`; a session's `posted_at` and yields' stamps from `post`; an
  arbitrage trade's `closed`/`abandoned` from its actions; factory dates
  (`released_at`, `resolved_at`...) are stamped when their status moves
  and only filled when empty.
- **Evidence types refuse generic writes entirely** (403 at the route):
  `audit_entry`, `forge_run`, `webhook_delivery`, `ledger_entry`,
  `data_source_run`, `external_posting`, `mail_inbound`,
  `federation_replica`. Others are refused by their validators or services:
  `alert_hit`, `dunning_event`, `calendar_event`, `lead_score_history`,
  `custom_field_value`, `comment` (use `comments`), `holding_txn` with a
  source. Read them; change their source instead.

## Sensitive fields

Fields flagged sensitive (password hashes, token secrets, webhook secrets,
forge tokens, TINs, sealed settings) are omitted from every response, log,
export, webhook body and the assistant -- omitted, not nulled. Naming one in
a create or update is **ignored, not an error**, and the rest of the
payload applies. Each credential has its own door -- there is no generic
"write this sensitive field" command, because a password must be hashed
and a forge token must not be, and one command for both would get one of
them wrong: a user's password is
accepted on `create user ... password=...` and set on `/account` (hashed);
forge credentials go through `forge settings` (JSON on stdin); a webhook
secret through `webhook secret ID`; feed credentials on `/feeds/ID/credentials`;
provider accounts on `/organizations/ID/settings/*`. Never put a secret in
argv. A URI carrying a password is shown redacted.

## Actions: operations that are not a field write

A record action is a declared business operation on a type: `post` a
journal, `craft` a recipe, `close` a trade, `sweep` a dunning policy.
Discover them from the schema (`describe` does **not** print them):

```bash
curl -s -H "Authorization: Bearer $VENTURE_TOKEN" "$VENTURE_SERVER/api/v1/schema/journal" \
  | jq '.actions[] | {name, type_level, stageable, roles, params: [.parameters[].name]}'
venturectl act journal 42 reverse occurred_at=2026-09-13 memo="Correction"
venturectl --stage act journal 0 create_and_post 'journal={...}'
```

- `act` types each argument from the schema; an unknown parameter is exit
  2, an action the schema does not offer is exit 3.
- **Type-level** actions run at ID `0` (sweeps, batch creates, `record`).
  One with no subject parameter is judged in the organization its
  `organization_id` parameter names; a member who is not a global
  owner/admin must pass it (else 422 "runs in one organization").
- `stageable` actions take `--stage`/`?stage=1`; the assistant and MCP
  always stage generated action tools. Non-stageable ones (close
  checklists, cutover, retainers, marketing sends, mail, printing) apply
  at once. The full list per type is in the `data-model-*.md` files.

## Links, comments, watches

- `record_link` joins any two records with a kind and its inverse
  (`blocks`/`blocked_by`, `depends_on`/`required_by`, `parent_of`/`child_of`,
  `causes`/`caused_by`, `produces`/`produced_by`, `references`/`referenced_by`,
  `supersedes`/`superseded_by`, `related`, `duplicates`): `venturectl link
  TYPE ID TYPE ID kind=K note=T`, `links TYPE ID`, unlink with `delete
  record_link ID`. Refused: a missing end, a self-link, a pair linked twice.
- Discussion comments exist on every type that has not opted out (tickets
  use `ticket_comment`); use `comments add`, never `create comment`
  ([desk.md](desk.md)). `watch TYPE ID` puts changes in your inbox.

## Attributes, custom fields, venture types

`attributes` holds fields a type does not declare in C: a declarative
venture type's fields (`attributes.world=...` on a `virtual_economy`
venture, enforced at the save) and operator-defined custom fields
(`fields define ...`). Write them in the same request as the record. See
[taxonomy.md](taxonomy.md).
