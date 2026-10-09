# Troubleshooting: start here

A symptom-first index across the whole system. Find the row, run the first
check, then read the guide it points at.

## Method

Five habits that settle most problems before a theory is needed:

1. **Ask the server what it is.** `venturectl health` (version, backend,
   `staged_writes`), on the port you think it is (8747 default/container,
   8748 `just start`, 8749 `make demo`). A healthy but wrong server is the
   most expensive mistake there is.
2. **Ask whether the module is on.** `venturectl modules` (`-f json` gives
   the reason a module is off). A type or route of an off module is 404.
3. **Read the declaration, never guess.** `venturectl describe TYPE` for
   field names, kinds and enum values; `GET /api/v1/schema/TYPE` for
   actions; `GET /api/v1/reports` for report options.
4. **Ask which organization.** A token's lists, reports and market verbs
   answer for the default organization; pass `organization_id=N`.
5. **Read the exit code and the record's history.** Exit codes are in
   [cli.md](cli.md), error slugs in [api.md](api.md); `venturectl activity
   TYPE ID` and `list audit_entry target_type=TYPE target_id=ID` show who
   changed what, and whether it was staged (`approved_by`).

## Connecting and authority

| Symptom | First check | Guide |
|---|---|---|
| exit 7, connection refused | `venturectl -s URL health`; the port | [cli.md](cli.md) |
| exit 5 / 401 "Sign in to continue" | `VENTURE_TOKEN` set, not revoked/expired, minted on *this* server | [api.md](api.md) |
| 403 on `user`, `api_token`, `forge`, `webhook`, `comment`, `notification` | owner-only types; `forge_rule`, `plugin_config`, `currency` writes are admin | [system.md](system.md) |
| 404 for a whole type or route | `venturectl modules` -- the module is off | [system.md](system.md) |
| 404 for a record that exists | another organization, or not readable by your role (never 403) | [system.md](system.md) |
| editor member refused on invoices/journals | financial types need organization finance/admin/owner | [system.md](system.md) |
| `--session-file` refused | origin mismatch, file not 600/single-link, combined with a token | [cli.md](cli.md) |
| `--stage` refused by a verb | only some verbs read it; elsewhere it would be ignored | [cli.md](cli.md) |
| `--dry-run` refused for `assets run-period` | use `dry_run=true` | [ledger-operations.md](ledger-operations.md) |
| non-ASCII text refused in argv | pass it on stdin (`-`) or use a UTF-8 locale | [cli.md](cli.md) |

## Writes that do not do what you meant

| Symptom | First check | Guide |
|---|---|---|
| create/update succeeds but a value is missing | dashed or misspelt field name, or a sensitive field -- all ignored silently | [records.md](records.md) |
| exit 8 "points at X #N, which does not exist" | wrong or soft-deleted target; its module off | [records.md](records.md) |
| exit 4 / 409 conflict | stale `version`; a unique key held by a *deleted* row | [records.md](records.md) |
| "requires a date within its configured fiscal periods" / closed period | `list fiscal_period`; pick a date inside an open period | [ledger.md](ledger.md) |
| "status=paid is derived from allocations" | record a `payment` instead | [receivables.md](receivables.md) |
| "Use the ... service" on a status field | the field is a service's; use its verb or action | [records.md](records.md) |
| "permission denied" after a proposal was saved | a second-actor rule: another account repeats the command | [ledger.md](ledger.md) |
| a staged change never happened | nobody approved; it expired after an hour | [api.md](api.md) |
| approval answers 409 and the card vanished | the record moved; re-read and stage again | [api.md](api.md) |
| `?stage=1` applied the write | the server predates staging (`staged_writes` missing from health) | [api.md](api.md) |
| an action is "not offered" (exit 3) | wrong name or the record is not eligible; read `/schema/TYPE` actions | [records.md](records.md) |
| type-level action 422 "runs in one organization" | pass `organization_id=N` | [records.md](records.md) |
| holding refused "holds 12 ... and this takes 50" | the balance on the document's date; date the income first, or `allow_negative` | [currencies.md](currencies.md) |
| amount "ambiguous" or in the wrong currency | name the code; a bare amount is the record's/book currency | [currencies.md](currencies.md) |
| Stripe, tax filing or Shopify refuse a currency | they take ISO money only | [currencies.md](currencies.md) |

## Lists and reports that look wrong

| Symptom | First check | Guide |
|---|---|---|
| a list or report is empty for a second organization | add `organization_id=N` | [system.md](system.md) |
| 400 "has no field X" on a list | a filter name; reserved names are `limit offset page search order include_deleted period period_field` | [api.md](api.md) |
| a money filter matches too much | money filters compare minor units | [records.md](records.md) |
| `__between` refused | use `__gte` and `__lt` | [api.md](api.md) |
| deleted rows missing even with `deleted_at__not_null` | add `include_deleted=1` | [records.md](records.md) |
| `report aggregate` counts everything | name `date_field` | [reports.md](reports.md) |
| a report option silently ignored | not a declared parameter; `--from/--to` are not report options | [reports.md](reports.md) |
| two figures where one was expected | one row per currency, book first -- never add them | [reports.md](reports.md) |
| a figure blank rather than zero | unpriced/unquoted is blank by design; read `note`/`missing` | [market-production.md](market-production.md) |
| a dashboard widget shows the wrong organization | file the dashboard under it, or pick it in the sidebar | [dashboards.md](dashboards.md) |

## Nothing happened on its own

| Symptom | First check | Guide |
|---|---|---|
| mail stays queued | nothing delivers on a timer: `mail deliver` or a rule | [mail.md](mail.md) |
| a message is `uncertain` | never retry automatically; decide deliberately | [mail.md](mail.md) |
| outbound mail refused | the organization's SMTP binding and `mail.allowed_endpoints` | [mail.md](mail.md) |
| reminders never sent | `dunning sweep`, then `mail deliver`; read `last_sweep` | [billing.md](billing.md) |
| a rule loads nothing ("unknown source module") | the module's registered name (`cron`, not `cron_event`) | [automation.md](automation.md) |
| a webhook stopped | ten failures switched it off; read `webhooks` and its deliveries | [automation.md](automation.md) |
| SLA breaches not marked | `POST /api/v1/sla/sweep`, or open the board | [desk.md](desk.md) |
| no inbox entry for a mention | the person may not read the record, or is inactive | [desk.md](desk.md) |
| a mailbox stopped syncing | five refused logins set `active=false` | [mail.md](mail.md) |

## Factory, forge and agents

| Symptom | First check | Guide |
|---|---|---|
| "Coding runs are turned off" | `forge.runs_enabled: true` | [factory.md](factory.md) |
| forge webhook does nothing / 401 | secret and binding, *Accept issues*, exact `owner/repo` | [factory.md](factory.md) |
| no rule applies to a ticket | resolution order; nothing is a legitimate answer | [factory.md](factory.md) |
| publish refused under MCP | it cannot be staged; `--apply-writes` or a person | [factory.md](factory.md) |
| assistant cannot do X | `ai.policy`; deletes always stage; no webhook/token tools | [desk.md](desk.md) |

## Market data, trading, accounts

| Symptom | First check | Guide |
|---|---|---|
| `feeds` verbs exit 3 | `feeds.enabled`, or the source is in another organization | [feeds.md](feeds.md) |
| "No such data source in organization 1" | add `organization_id=N` | [feeds.md](feeds.md) |
| push refused, exit 4 | not a push source, switched off, or four pushes in flight | [feeds.md](feeds.md) |
| a run stores nothing | `track: known` without the instrument; origins/file roots | [feeds.md](feeds.md) |
| a series store only grows | `feeds upkeep-status ID`: `size.auto_vacuum` `none` needs one `feeds upkeep ID rebuild=1`; `last.error`; `series.daily_days`/`idle_days` 0 keep forever | [feeds.md](feeds.md) |
| `/metrics` 404 | `metrics.access` is `off`, or `loopback` and the request came through a proxy (forwarding header) | [operations.md](operations.md) |
| `/metrics` 403 with a token | its `scopes` must include `metrics` (or it is the owner's) | [operations.md](operations.md) |
| provider name unknown | an exec plugin with `plugins.allow_exec` off | [plugins.md](plugins.md) |
| positions not mirrored | instruments without products; `create_products` | [feeds.md](feeds.md) |
| `market quote` `found: false` | currency mismatch, several groups, nothing observed | [market-data.md](market-data.md) |
| alert hits written by a test | `market alerts evaluate` writes; use `--dry-run` | [market-data.md](market-data.md) |
| craft refused | "Short of", tools not on hand, "name the location_id" | [market-production.md](market-production.md) |
| recipe missing from `recipe_margin` | `active=true` | [market-production.md](market-production.md) |
| session refused for minutes | change the end, not the minutes | [sessions-goals.md](sessions-goals.md) |
| a goal never "achieved" | nothing updates a goal for you | [sessions-goals.md](sessions-goals.md) |
| `arbitrage record N` "no longer there" | pass exactly the scan's options; the market moved | [arbitrage-scan.md](arbitrage-scan.md) |
| scan bound refused | money must name its currency | [arbitrage-scan.md](arbitrage-scan.md) |
| `accounts post` refused (exit 1) | the source's `books` mode | [accounts.md](accounts.md) |
| trade edits refused | closed/abandoned only by actions; executed stock legs are final | [arbitrage.md](arbitrage.md) |

## Transport and operations

| Symptom | First check | Guide |
|---|---|---|
| 413, 503 with Retry-After, 408, or a dropped connection | HTTP limits; do not blindly retry a write | [operations.md](operations.md) |
| a backup "succeeded" but stores are `running` | series copies finish later as their own runs | [operations.md](operations.md) |
| startup refused | the validator's message: a module requirement, auth on a public address, an origin | [platform.md](platform.md) |
| sessions end at every restart | no session secret in the environment | [platform.md](platform.md) |

When the cause is VENTURE's own (a stale document, a verb that disagrees
with its usage line), say so plainly and point at the source, `--help`
or the schema -- the generated ground truths win over prose.
