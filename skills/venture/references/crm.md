# Customers: companies, contacts, leads, deals, activities, health, duplicates

Read this to capture or qualify leads, move deals, plan and log activities
and calls, check customer health, merge duplicates, set territories and
quotas, or migrate from another CRM. Outreach (sequences, marketing,
attribution) is in [outreach.md](outreach.md); mail and calendars in
[mail.md](mail.md). Sources: `docs/leads.org`, `docs/pipelines.org`,
`docs/activities.org`, `docs/dedupe.org`, `docs/crm-import.org`,
`docs/sales-performance.org`, `docs/reporting.org`.

## The shape

*Companies* are the outside businesses you deal with (customers, prospects,
suppliers, marketplaces, partners) -- distinct from *organizations*, which
are the businesses you are. Contacts belong to companies by reference;
deals, interactions and tickets carry both. Every record's page lists
everything that points at it (derived from the references). Company,
contact, deal, ticket and venture carry `owner_user_id` and an optional
`team_id` -- ownership decides what sales/support members see, but grants
no role. A deal's contact must belong to the deal's company ("attention
of"); an empty parent on either end is not a mismatch.

## Leads (module `leads`)

Use `describe lead` before capture or qualification. Public capture is
`POST /f/:token` (a `lead_form`; JSON or form-encoded, honeypot, rate
limits, duplicate policy).

```bash
venturectl lead convert 31 deal=yes [company_id=ID] [contact_id=ID]
venturectl --stage lead convert 31
venturectl lead reassign 31 owner=alice        # omit owner to rerun the matching rules
venturectl leads reroute 31 ; venturectl leads rescore 31
venturectl update lead 31 status=recycled unqualified_reason="Budget next year" recycle_until=2027-01-15
```

- `convert` requires a qualified lead and creates or links CRM records
  atomically; failures roll back every new record; `--stage` proposes it.
  Never set `status=converted` or the conversion ids with generic updates.
  A new deal keeps the lead's owner, source and campaign; existing linked
  company/contact records keep their values. Staged reassignment is refused.
- `leads reroute ID` clears the owner and evaluates `lead_routing_rule`
  records again in `position` order (writing "Lead routed" or "No rule
  matched" to the timeline); `leads rescore ID` clears `score_manual` and
  applies the `lead_scoring_rule` formula. Both take no other arguments,
  refuse a converted lead and refuse `--stage`; each is also spelled `lead
  reroute` / `lead rescore`.
- Rules are ordinary records: `create lead_routing_rule position=N
  'conditions=source=web' action=assign_user|round_robin|assign_venture
  ...` and `create lead_scoring_rule 'conditions=...' points=N` (`describe
  lead_routing_rule` for exact enums). A malformed condition or an action
  missing its target is refused at the create. `round_robin` needs the
  `orgaccess` module for teams. Never set `routing_rule_id` by hand, and
  never create a `lead_score_history` row: both are refused.
- Reports `lead_sources`, `lead_response_time`, `leads_recycled_due`,
  `lead_routing`, `lead_scoring` (`band_size=N`, default 25).

## Deals and pipelines (module `pipelines`)

```bash
venturectl describe pipeline_stage ; venturectl list pipeline_stage pipeline_id=1
venturectl deal move 12 4 "Sent the proposal"      # ID STAGE_ID [NOTE]
venturectl deal quote 12                            # quote from deal_line records
```

Fill required deal fields (`pipeline_stage.required_fields`) and a loss
reason before moving to a lost stage. `update deal` cannot change either
stage field or the closing timestamp. Reports `stage_duration`, `funnel`,
`forecast`, `loss_reasons`, `overdue_deals` filter with `pipeline_id=N` and
`owner=USERNAME`. A deal's weighted value is value x probability, a closed
outcome overriding the stored probability.

## Activities and calls (module `activities`)

```bash
venturectl activity list mine|overdue|today
venturectl activity complete 55 outcome="Agreed a trial"
venturectl report worklist organization_id=1        # current UTC week per owner
```

`activity complete` completes a planned activity, writes interaction
history and advances recurrence atomically. Generic `create`/`update
activity` edit the plan; a generic `status=done` is refused. (`activity
TYPE ID` is different: a record's timeline.) **Structured calls**: the
generated `log_call` action on `company`, `contact`, `lead` or a planned
call `activity` (`describe TYPE`/schema for typed parameters:
`call_direction`, `call_duration`, `call_occurred_at`, `call_outcome`,
optional follow-up). The actual occurrence and structured outcome belong to
the historical call; free-text outcome stays narrative; an optional
follow-up is created in the same transaction; a verified CRM relation is
required. External source/ID pairs give replay identity for adapters --
changed payloads conflict rather than duplicating history. `report calls`
counts historical calls once (`activity_churn` is a different metric).

## Customer health (module `customer_health`)

`report customer_health PERIOD [band=red|amber|green] [owner=USERNAME]
[sort=[-]column]` lists every customer company with last touch, open
deals, overdue invoices and days, open tickets and SLA breaches, dunning
step and trailing-12-month revenue, banded against the organisation's
`headline_setting` thresholds (`health_touch_days` 30,
`health_overdue_days` 15, `health_open_tickets` 3; zero means the default).
An unknown band or sort column is exit 2. `customers health-sweep
[as_of=DATE] [organization_id=N] [limit=N]` creates one planned `check in:
<company>` activity for each red company's owner and answers
`{"created": N}`; it never duplicates an open one. Needs `customer_health`
and `activities`.

## Duplicates (module `dedupe`)

`dedupe scan kind=company|contact [organization_id=N]` proposes
`duplicate_candidate` rows (exact normalised email/phone/website, same
email domain with a similar name, or a similar name) and merges nothing;
rerunning updates the same rows and drops pairs that stopped matching.
`dedupe merge ID survivor=N` folds the other record into the survivor in
one transaction: every reference naming the loser is re-pointed, empty
survivor fields are filled, the loser is soft-deleted with
`merged_into_id`, and the old id answers 301 to the survivor. Refused
across organizations, onto itself, or when the loser has issued
invoices/bills in a currency the survivor's issued documents do not use.
`dedupe dismiss ID` closes a proposal; `--stage dedupe merge` proposes it.

## Territories and quotas (module `sales_performance`)

Generic records: read `describe sales_territory`, `describe sales_quota`
and `describe lead_routing_rule` before configuring their
organization/team references. `report sales_attainment` shows captured
booked sales, targets and current pipeline by recipient, currency and
quota period -- not posted accounting revenue. `sales_credit` and
`sales_assignment` are service evidence; correct the source deal instead.

## Migrating from another CRM (module `crm_import`)

HubSpot, Zoho CRM or Salesforce CSV exports, mirroring the accounting
cutover: a mapped manifest, a preview that refuses before writing, an
import in one transaction, a row per source id (reruns are idempotent), and
a rollback that removes exactly what the import created. No vendor API, no
attachments, no accounting records.

```bash
venturectl crm preview manifest.json        # {"source":"hubspot","files":{"companies":"companies.csv",...},"stage_map":{...},"currency":"USD","owner_map":{...}}
venturectl crm import manifest.json         # or: crm import ID (a previewed batch)
venturectl crm activate 3                   # accept; closes the door on rollback
venturectl crm rollback 3
```

Objects: companies, contacts, deals, notes, emails, calls, meetings, tasks;
list unsupported ones (attachments) under `unsupported`. Generic writes to
`crm_import`/`crm_import_row` are refused. See `docs/crm-import.org`.
