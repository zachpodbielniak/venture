# The data model: modules and where each type is described

Read this to find which record types exist, which module owns them, what
they depend on, and which file describes them. The live server is the
authority -- these files summarise purpose, key fields (wire spelling),
references and actions; run `venturectl describe TYPE` for every field,
its kind, enum choices and help text, and `GET /api/v1/schema/TYPE` for the
actions and their parameters. Generated against 83 modules / ~285 types at
the skill baseline; a newer server may have more (`venturectl types`,
`venturectl modules`).

How to read the type entries in the `data-model-*.md` files:
`type` -- purpose. **Key fields**; `-> target` marks a reference; `[a|b]`
an enum; *actions* lists record actions (`(0)` = type-level at ID 0,
`(s)` = stageable). Required fields are those `describe` marks `required`.

Every type also has the spine (`id`, `uuid`, `organization_id`,
`created_at`, `updated_at`, `version`, `deleted_at`, `attributes`) -- see
[records.md](records.md).

## Module map

Bottom-up: a module only requires modules above it. "(opt-in)" modules are
off by default. A module nobody configures is on.

| Module | Requires | Types in |
|---|---|---|
| `core` | -- | [core](data-model.md) |
| `custom_fields` | core | [core](data-model.md) |
| `sales` | core | [revenue](data-model-revenue.md) |
| `finance` | sales | [books](data-model-books.md) |
| `ledger` | finance | [books](data-model-books.md) |
| `crm` | core | [crm](data-model-crm.md) |
| `invoicing` | finance, crm, ledger | [revenue](data-model-revenue.md) |
| `receivables` | finance, invoicing | [revenue](data-model-revenue.md) |
| `outreach` | sales | [crm](data-model-crm.md) |
| `ideas` | core | [crm](data-model-crm.md) |
| `kb` | core | [work](data-model-work.md) |
| `tickets` | core | [work](data-model-work.md) |
| `federation` (opt-in) | core | [core](data-model.md) |
| `webhooks` | core | [work](data-model-work.md) |
| `ai` | core | [work](data-model-work.md) |
| `chat` | ai | [work](data-model-work.md) |
| `automation` | core | [work](data-model-work.md) |
| `plugins` | core | [core](data-model.md) |
| `orgaccess` | core | [core](data-model.md) |
| `integrations` | orgaccess | [core](data-model.md) |
| `forge` | tickets, integrations | [work](data-model-work.md) |
| `factory` | forge | [work](data-model-work.md) |
| `dashboards` | core | [work](data-model-work.md) |
| `periods` | finance | [books](data-model-books.md) |
| `reconciliation` | ledger | [books](data-model-books.md) |
| `assets` | ledger, periods | [books](data-model-books.md) |
| `quotes` | crm, invoicing | [revenue](data-model-revenue.md) |
| `billing` | invoicing, receivables | [revenue](data-model-revenue.md) |
| `projects` | invoicing, receivables | [revenue](data-model-revenue.md) |
| `mail` | core | [crm](data-model-crm.md) |
| `leads` | crm | [crm](data-model-crm.md) |
| `activities` | crm | [crm](data-model-crm.md) |
| `payables` | finance, ledger, crm | [revenue](data-model-revenue.md) |
| `supplier_portal` | payables | [revenue](data-model-revenue.md) |
| `banking` | ledger | [books](data-model-books.md) |
| `bankfeed` (opt-in) | banking | [books](data-model-books.md) |
| `pipelines` | crm | [crm](data-model-crm.md) |
| `sequences` | crm | [crm](data-model-crm.md) |
| `marketing` | mail, sequences, leads | [crm](data-model-crm.md) |
| `autojournal` | ledger | [books](data-model-books.md) |
| `statements` | ledger, periods | [books](data-model-books.md) |
| `cutover` | ledger | [books](data-model-books.md) |
| `setup` | ledger, periods | [books](data-model-books.md) |
| `recurring` | invoicing, payables, ledger, periods, receivables, mail | [books](data-model-books.md) |
| `close` | periods, ledger, statements | [books](data-model-books.md) |
| `capture` | finance | [books](data-model-books.md) |
| `ocr` (opt-in) | capture | [books](data-model-books.md) |
| `claims` | finance, ledger | [books](data-model-books.md) |
| `payroll` (opt-in) | finance, ledger | [books](data-model-books.md) |
| `accounting` | finance | [books](data-model-books.md) |
| `backup` | ledger | [books](data-model-books.md) |
| `tax_filing` | finance, periods | [books](data-model-books.md) |
| `goods` | payables, ledger, sales | [revenue](data-model-revenue.md) |
| `budgets` | statements | [books](data-model-books.md) |
| `equity` | ledger, setup | [books](data-model-books.md) |
| `group` (opt-in) | statements | [books](data-model-books.md) |
| `mail_sync` | mail, crm, leads | [crm](data-model-crm.md) |
| `dunning` | receivables, mail | [revenue](data-model-revenue.md) |
| `headline` | receivables, leads | [revenue](data-model-revenue.md) |
| `sales_tax` | receivables | [books](data-model-books.md) |
| `pnl_cuts` | receivables, payables | [revenue](data-model-revenue.md) |
| `customer_health` | headline, activities | [revenue](data-model-revenue.md) |
| `calendar` | activities, crm, leads | [crm](data-model-crm.md) |
| `money_calendar` | receivables, payables | [revenue](data-model-revenue.md) |
| `crm_import` | crm, pipelines, activities, leads | [crm](data-model-crm.md) |
| `dedupe` | crm | [crm](data-model-crm.md) |
| `docs` | core | [core](data-model.md) |
| `mfa` | orgaccess | [core](data-model.md) |
| `attribution` | leads, integrations | [crm](data-model-crm.md) |
| `commerce` (opt-in) | invoicing, receivables, integrations | [revenue](data-model-revenue.md) |
| `stripe` (opt-in) | receivables, integrations | [revenue](data-model-revenue.md) |
| `oidc` (opt-in) | orgaccess, mfa, integrations | [core](data-model.md) |
| `ai_providers` | orgaccess, integrations | [core](data-model.md) |
| `sales_performance` | orgaccess, leads, pipelines | [crm](data-model-crm.md) |
| `market` | sales | [trading](data-model-trading.md) |
| `production` | sales | [trading](data-model-trading.md) |
| `sessions` | core | [trading](data-model-trading.md) |
| `goals` | core | [trading](data-model-trading.md) |
| `feeds` (opt-in) | core | [trading](data-model-trading.md) |
| `marketdata` | market | [trading](data-model-trading.md) |
| `arbitrage` | marketdata, ledger | [trading](data-model-trading.md) |

The `example` module (type `subscription`, report `subscriptions`) comes
from the bundled example plugin and exists only where that plugin loads.

## Core (`core`, always on)

- `organization` -- a legal or personal entity you keep books for. **name**,
  `kind` [personal|sole_proprietor|llc|s_corp|c_corp|partnership|nonprofit|trust],
  `default_currency` (the book currency; never a memo currency),
  `parent_id -> organization` (display roll-up only), `is_default`,
  `fiscal_year_start_month`, `quote_valid_days`, `tax_id` (sensitive),
  `sequence_tracking`, `marketing_tracking`, `active`.
- `venture` -- one thing you do. **name**, `venture_type` (a registered
  declarative type), `status` [idea|planning|building|active|paused|winding_down|archived],
  `priority`, `idea_id -> idea`, `owner_user_id -> user`, `team_id -> team`,
  `target_revenue`, `monthly_goal`, `started_at`/`ended_at`, `attributes`.
- `document` -- a file or link (`path` is service-owned), `venture_id`,
  `expense_id`, `private_owner_id -> user`, `extracted_text`; *actions*
  `ocr_extract (s)`, `ocr_review (s)` (ocr module).
- `record_link` -- any record to any record: `source_type/source_id`,
  `kind` (16 kinds with inverses), `target_type/target_id`, `note`.
- `comment` -- a discussion entry on any record: `subject_type`,
  `subject_id`, `parent_id -> comment` (one level), `body` (markdown),
  `author` (set from the caller), `mentions`. Use `venturectl comments`.
- `currency` -- a unit of account beyond ISO: `code`, `name`, `kind`
  [virtual|points|commodity|other], `exponent` 0-6, `symbol`,
  `symbol_position`, `denominations` (JSON string), `book_treatment`
  [valued|separate_book|memo]. Admin writes. See [currencies.md](currencies.md).
- `category` -- a tree node any type can be filed under (`parent_id`,
  `applies_to`). See [taxonomy.md](taxonomy.md).
- `user` -- `username`, `display_name`, `email`, `role`
  [owner|admin|editor|viewer|service], `active`, `timezone`; *action*
  `mfa_reset` (owner). `api_token` -- `name`, `role`, `prefix`,
  `expires_at`, `user_id`, `membership_snapshot`. Both owner-only.
- `audit_entry` -- read-only trail: `action`
  [create|update|delete|login|logout|tool_call|confirm|reject|automation|export],
  `actor`, `actor_kind` [system|user|ai|automation|plugin|import],
  `target_type/target_id`, `diff`, `approved_by`, `request_id`, `source`.
- `saved_view` (a named list URL: `entity_type`, `query`, `board`,
  `personal`, `pinned`), `watch` (a user following a record),
  `notification` (one thing told to one user: `kind`
  [mention|assigned|watched|sla|budget|run|system|alert|reply], `read_at`)
  -- the last two are personal and owner-only on generic routes.
- `plugin_config` -- `name`, `config` (YAML/JSON text) for a plugin; admin.
- Hosted control rows (`tenant_workspace`, `tenant_membership`,
  `tenant_invitation`, `tenant_support_grant`, `tenant_event`) -- changed
  only through their actions (`set_state`, `set_membership`,
  `invite_recovery`, `invite (0)`, `revoke`); see [operations.md](operations.md).

## Access, identity and providers

- `orgaccess`: `organization_membership` (`user_id`, `role`
  [viewer|owner|admin|editor|finance|sales|support|accountant], `active`;
  one per user and organization, deleted rows included -- reactivate),
  `team`, `team_membership`, `accounting_approval_rule` (`action`,
  `require_second_actor`) and `accounting_approval` (a pending
  second-person proposal: `proposer`, `approver`, `state`).
- `integrations`: `integration_connection` -- an organization's encrypted
  provider binding (`provider`, `account_id`, `environment`, `enabled`,
  `credential_revision`); configured on `/organizations/ID/settings/*`;
  *action* `adopt_commerce_invoice`.
- `mfa`: `user_mfa`, `mfa_recovery_code`, `mfa_policy`
  (`require_mfa_for_admins`). `oidc` (opt-in): `oidc_identity` (`issuer`,
  `subject`, `user_id`) -- linked through settings pages, never CRUD.
- `ai_providers`: `ai_configuration` (per purpose: chat, coding,
  embeddings; `mode`), `ai_platform_offer` (*actions* `grant`, `disable`),
  `ai_grant`, `ai_usage_period`, `ai_usage` (service-owned evidence).
- `federation` (opt-in): `federation_peer` (`origin`, `public_key`
  Ed25519 pin), `federation_grant` (`record_type`, `record_uuid`, `fields`,
  `write_fields`, `collection`), `federation_replica` (service-owned
  working copy). Owner-only. See [operations.md](operations.md).
- `plugins`, `docs` have no types: the plugin loader and declarative venture
  types, and the read-only `/docs` site.
- `custom_fields`: `accounting_custom_field`, `accounting_layout`,
  `custom_field_value` -- see [taxonomy.md](taxonomy.md).
