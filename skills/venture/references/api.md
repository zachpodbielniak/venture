# The raw REST API

Read this to call VENTURE over HTTP directly (curl, a script, another
service) or to understand what a `venturectl` verb does on the wire. The
module-specific routes are indexed in [api-routes.md](api-routes.md); the
record rules every write obeys are in [records.md](records.md). Source:
`docs/api.org`.

## Base, auth and format

- Base path `/api/v1`, JSON in and out (`Content-Type: application/json`).
  Default server `http://127.0.0.1:8747` (`just start` uses 8748, `make
  demo` 8749). Both `/api/v1/sale` and `/api/v1/sales` name one resource.
- `GET /api/v1/health` needs no credentials and says what the *build* can
  do: `version`, `database`, `ai`, `staged_writes`, `account_logins`.
- **Bearer token**: `Authorization: Bearer vk_...`. A token carries the role
  of whoever minted it (there is no scope parameter) and a snapshot of their
  memberships; it acts as `API token #N`. Mint one at *Your account -> API
  tokens* (`/account/tokens`), or with a session:

```bash
curl -s -c jar --data-urlencode username=owner --data-urlencode "password=$PW" "$B/login" -o /dev/null
curl -s -b jar -H 'Content-Type: application/json' -d '{"name":"books-agent"}' "$B/api/v1/tokens"
# {"id": 3, "token": "vk_...", "note": "This is the only time the token is shown..."}
```

- **Session cookie** `venture_session` (HttpOnly, SameSite=Lax) works on
  the API too; that is what `venturectl --session-file` sends. Sign-in may
  require a second factor (TOTP) -- a script should use a token.
- Organization: a token's active organization is the default one. Pass
  `organization_id=N` (query string, or in the JSON body for Trading
  routes and type-level actions) to work in another. Records are always
  filed under the organization in their body (`organization_id`), else the
  default.

## Generic record routes (every type)

| Verb | Path | Does | venturectl |
|---|---|---|---|
| GET | `/api/v1/schema`, `/schema/:type` | types, fields, actions | `types`, `describe TYPE` |
| GET | `/api/v1/:type?filters` | list | `list TYPE k=v ...` |
| GET | `/api/v1/:type/:id` | one (also a deleted one) | `get TYPE ID` |
| POST | `/api/v1/:type` | create (201) | `create TYPE f=v ...` |
| PATCH | `/api/v1/:type/:id` | change named fields only | `update TYPE ID f=v` |
| PUT | `/api/v1/:type/:id` | replace | -- |
| DELETE | `/api/v1/:type/:id` | soft delete (`{"deleted":true,"recoverable":true}`) | `delete TYPE ID` |
| POST | `/api/v1/:type/:id/restore` | undelete (never staged) | `restore TYPE ID` |
| POST | `/api/v1/:type/bulk` | `{"ids":[...],"changes":{...}}` or `{"ids":[...],"delete":true}`, one transaction, <= 500 ids, not stageable | `bulk TYPE 1,2,3 f=v` |
| POST | `/api/v1/:type/:id/actions/:action` | a declared action; ID 0 for type-level | `act TYPE ID ACTION k=v` |

```bash
H="Authorization: Bearer $VENTURE_TOKEN"; B=${VENTURE_SERVER:-http://127.0.0.1:8747}
curl -s -H "$H" "$B/api/v1/sale?period=2026-Q1&order=-occurred_at&limit=20" | jq '.total, .records[].gross.formatted'
curl -s -H "$H" -H 'Content-Type: application/json' \
     -d '{"description":"Cover art","amount":"250.00 USD","venture_id":3,"occurred_at":"2026-03-14"}' "$B/api/v1/expense"
curl -s -X PATCH -H "$H" -H 'Content-Type: application/json' -d '{"notes":"paid by card","version":4}' "$B/api/v1/expense/18"
```

## Lists: filters and reserved names

A list answers `{"total": N, "count": n, "records": [...]}` (total ignores
paging). `field=value` is equality; `field__op=value` names an operator:
`eq ne lt lte gt gte like ilike in not_in is_null not_null between`.

- Reserved, never filters: `limit`, `offset`, `page`, `search` (free text
  over searchable fields), `order` (field, `-field` descending,
  comma-separated), `include_deleted=1`, `period` (`this_month`, `2026-Q1`,
  `all`... -- see [reports.md](reports.md)) and `period_field` (which date
  field the period bounds).
- Any other name must be a real field (wire spelling) or the list is a 400
  that lists the available fields -- the fastest way to find a name.
- `in`/`not_in` take a comma list (`status__in=todo,triage`); `is_null` /
  `not_null` take `1`. `like`/`ilike` use SQL `%` (URL-encode it as
  `%25`). **`between` cannot be expressed in a query string** (it needs two
  values and the string gives one) -- use `__gte` and `__lt`.
- **Money filters compare integer minor units** (`gross__gte=10000` is
  100.00 USD); decimals are not parsed. Use `report aggregate` for totals.
- Deleted rows are excluded unless `include_deleted=1`, even for a filter
  on `deleted_at`. Values are always bound parameters: injection is
  structurally impossible.

## Bodies

`PATCH` applies only the members present; `PUT` replaces. Keys use
**underscores**; a dashed key is ignored field by field with a 200. Money
takes `"12.34"`, `"12.34 EUR"`, `"$12.34"`, coin forms, or the object;
dates take ISO 8601 or `YYYY-MM-DD`; enums take the nick in any case;
references take ids; `attributes` is merged on PATCH (`null` removes a
key). Include `"version": N` to make the write conditional (409 if stale).
Sensitive fields in a body are ignored and omitted from every response.
Responses carry money as `{"amount","currency","exponent","formatted"}`.

## Errors

One shape: `{"error": "validation", "message": "Description: is required", "code": 5}`.

| `error` (`code`) | HTTP | venturectl exit | Meaning |
|---|---|---|---|
| `invalid_argument` (1), `serialization` | 400 | 2 | unusable input, unknown field or option |
| `unauthenticated` (6) | 401 | 5 | no or bad credentials |
| `permission_denied` (7) | 403 | 5 | role or type gate |
| `not_found` (2) | 404 | 3 | no such record, route, or a module that is off; also "not yours" |
| `already_exists` (3), `conflict` (4) | 409 | 4 | uniqueness, stale version, stale approval, queue full |
| `validation` (5), `balance` (19) | 422 | 8 | refused by a validator, reference check or service |
| `ai_confirmation_required` | 202 | 0 | staged, awaiting approval |
| `unsupported` | 501 | 6 | not in this build |
| `network`, `timeout` | 502, 504 | 7 | a far end did not answer |
| anything else | 500 | 1 | -- |

A 409 on a write means somebody changed the record since you read it:
re-read, re-apply, retry -- never force it. Transport limits answer before
any handler: 413 (body over `server.max_request_size_mb`, 32), 503 with
`Retry-After` (receive budget full), 408 (slow request), or a closed
connection. Do not blindly retry a write whose response was lost -- read
its identity (`external_id`, the list) first.

## Staging a write

Add `?stage=1` to any POST, PUT, PATCH or DELETE (and to actions marked
`stageable`, `/links`, billing, bill, lead-convert and journal routes) and
the change is proposed instead of made: **202** with
`{"status":"awaiting_approval","staged":true,"confirmation":{"id","summary","action","type","record_id","origin","diff","expires_at"},"note":...}`.

```bash
curl -s -X PATCH -H "$H" -H 'Content-Type: application/json' -d '{"notes":"x"}' "$B/api/v1/expense/18?stage=1"
curl -s -H "$H" "$B/api/v1/confirmations"                    # what waits
curl -s -X POST -H "$H" "$B/api/v1/confirmations/ID/approve"  # or /reject
```

- `?stage=y` (anything but a recognised value) is a 400, not "no". An older
  build without `staged_writes` in health *ignores* the parameter and
  applies the write -- check health first.
- Approval needs the editor role *and* the type's own role (a forge stays
  owner-only). It applies the staged object through the same save; if the
  record moved meanwhile it is 409 naming the fields and the card is
  dropped -- re-read and stage again. A change already made identically
  succeeds as a no-op. Rejecting is audited.
- Cards expire after `ai.confirmation_ttl` (3600 s); at most
  `ai.confirmation_limit` (200) wait, past which staging is 409.
- Not stageable: `restore`, `bulk`, publishing a release, non-stageable
  actions (they say so). An organization viewer's create is staged even
  without `?stage=1`; an editor member's journal post is too.

## Reports

`GET /api/v1/reports` lists every report with its declared parameters;
`GET /api/v1/reports/:name?period=2026&organization_id=1&...` runs one and
answers `{"title","period":{start,end,label},"metrics":[...],"columns":[...],"rows":[...]}`
(plus notes). `?format=csv` returns the CSV rendering. An undeclared option
is ignored by some reports and refused by others -- see [reports.md](reports.md).

## Webhooks both ways

Outbound deliveries carry `X-Venture-Event`, `X-Venture-Delivery` and
`X-Venture-Signature: sha256=<hex HMAC-SHA256 of the exact body>`
([automation.md](automation.md)). Inbound `/hooks/forge/:id` is verified the
same way against the forge's webhook secret (a forge with none is refused);
`/webhooks/stripe/:connection` verifies `Stripe-Signature`.
