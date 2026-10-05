# Automation, notifications and webhooks

Read this when something should happen on a schedule or in reaction to a
record changing, when somebody should be told, or when an outside system
should hear about a change. Sources: `docs/automation.org`,
`docs/webhooks.org`, `docs/workdesk.org`.

## Nothing heavy runs on a hidden timer

Sweeps are explicit and bounded. If a thing should happen regularly, a
rule (below) or an outside cron must call it:

| Job | Call it with |
|---|---|
| deliver queued mail | `mail deliver --limit N` / handler `mail_deliver` |
| fetch mailboxes | `mail sync` / `mail_sync` |
| recurring documents | `recurring run --as-of DATE` / `recurring_run` |
| invoice reminders | `dunning sweep as_of=DATE` / `dunning_sweep`; legacy `collections run` / `collections_run` |
| subscription renewals | `billing renew --as-of DATE` |
| depreciation | `assets run-period YYYY-MM` / `assets_run_period` |
| SLA breaches | `POST /api/v1/sla/sweep` (also runs when the board or inbox opens) |
| bank feeds, commerce orders | `bankfeed sync ID` / `bankfeed_sync`; `commerce import` / `commerce_import` |
| report packs, backups | `report_packs_run`; `backup run SCHEDULE_ID` / `backups_run` |
| a market data source | `feeds sync ID` / `feeds_sync("ID")` (sources with a schedule run themselves on the feeds worker) |
| calendar | `calendar sync` |

## Rules: the embedded podomation engine (module `automation`)

Rules are pods in podomation's DSL in one file -- `automations.pod` under
the state directory (`automation.pods_file` moves it). `/automations`
shows engine status and pods, an editor with live diagnostics from the
real parser, and the loaded module reference. Viewing needs admin; saving
and reloading need owner (rules can run inline crispy and bash). Saving
validates first; a reload **rebuilds** the engine from the file (never
stacks rules beside running ones). You may edit the file directly and press
Reload or restart.

- Vocabulary: podomation's modules (`cron`, `timer`, `log`, `http`,
  `ntfy`, `discord`, ~100 more) plus VENTURE's `venture` module. Check the
  loaded list -- podomation's docs call the cron module `cron_event`, but it
  registers as `cron`; a rule using the wrong name parses and then refuses
  to load ("unknown source module").
- The `venture` module's **events** are `on_created`, `on_updated`,
  `on_deleted` for every record from every door, plus registered ones
  (`feed_synced`, `feed_failed` with `data_source_id`, `run_id`,
  `organization_id`, `status`, `rows`, `error`). **Handlers**: `query`,
  `count`, `report`, `create`, `low_stock`, `assets_run_period`,
  `mail_deliver`, `mail_sync`, `recurring_run`, `collections_run`,
  `bankfeed_sync`, `commerce_import`, `report_packs_run`, `dunning_sweep`,
  `backups_run`, `feeds_sync`, plus plugins'.
- Writes by a rule are audited as the automation actor. Nothing written
  from inside a handler raises events, which stops a rule triggering itself.
  Handlers are synchronous on the main thread; an exec plugin's handler
  blocks the server up to its manifest `timeout`.
- The DSL passes every argument as text, and `create` takes `field=value`
  pairs (not JSON). `data/examples/automations.pod` in the repository
  holds worked examples; the test suite parses each one.

```text
pod monthly_hosting = cron->new("0 9 1 * *");
monthly_hosting->on_schedule => venture->create("expense",
        "description=VPS hosting", "vendor=Hosting Co", "amount=12.00",
        "category=software");

pod alerts = venture->new();
alerts->on_created where event->type == "alert_hit"
        => ntfy->publish("trading", "{event->label}", "Market alert");

pod feeds = venture->new();
feeds->feed_failed => ntfy->publish("trading", "Source {event->data_source_id} failed: {event->error}", "Feed failed");
```

For writing and debugging the DSL itself, use the `podomation-dsl` skill
if it is installed.

## The inbox, watches and notifications

The inbox is fed by the audit signal only, so every door tells the same
people: a comment `@mention` (only people who may read the record), a
ticket assigned to you (an assignee is a username), a change to a record
you `watch`, an SLA clock, a budget warning, a coding run finishing, an
alert hit (`notify_username`), a webhook switched off. The system actor's
own derived writes (first-reply stamp, logged hours) are not told twice.

```bash
venturectl inbox [--all]          # unread by default
venturectl inbox read 12          # or: inbox read all
venturectl watch ticket 7         # unwatch ticket 7 to stop
```

`GET /api/v1/inbox` (`?unread=0` for all) answers
`{"unread":N,"notifications":[...]}`; `POST /api/v1/inbox/read`
`{"id":N}` (0 = all unread); `POST /api/v1/watch`
`{"type","id","watch":true}`; `GET /api/v1/watching/:type/:id`. Inbox and
watch rows are personal: somebody else's is not found, never forbidden; a
token with no user has no inbox.

## Webhooks out (module `webhooks`)

A `webhook` is a URL, a comma-separated list of event patterns and a
signing secret; every audited change matching it is POSTed
asynchronously (a write never waits on somebody else's server) and kept as
a `webhook_delivery`. Owner-only everywhere; the assistant has no webhook
tool.

- **Events** are `TYPE.created|updated|deleted` for every type (a plugin's
  too). Patterns: `*`, `ticket.*`, `ticket.created`, a comma list; blank
  means everything. `ticket.*` does not match `ticket_link.created`. A
  market alert is `alert_hit.created`; each feed run is a
  `data_source_run.created` -- subscribe narrowly when a source polls often.
- **Never published**: `webhook`, `webhook_delivery`, `notification`,
  `watch`, `audit_entry`, chat, the KB's derived rows, federation's
  bookkeeping, `api_token`, `user`.
- **Body**: `{"event","delivery","occurred_at","actor","webhook":{id,name},"record":{type,id,label,url},"data"}`
  -- `data` is the whole record (never sensitive fields) only with
  `include_record=true`.
- **Signing**: headers `X-Venture-Event`, `X-Venture-Delivery`,
  `X-Venture-Signature: sha256=<hex HMAC-SHA256 of the exact body bytes>`.
  Verify over the raw body in constant time. A webhook with no secret is
  sent unsigned (the page says so).
- **Failures**: ten consecutive failures switch it off and tell owners and
  admins; there is no retry (the delivery id lets a receiver decide; the
  delivery log shows what was missed). Re-enable with `update webhook ID
  active=true`.

```bash
venturectl create webhook name="Ops chat" url=https://hooks.example.com/venture events="ticket.*,invoice.updated" active=true
venturectl webhook secret 3        # shown once
venturectl webhook test 3          # sends webhook.test and waits for the answer
venturectl webhooks                # active, signed, failures, last deliveries
```

A test against this very install works (the test waits in a nested main
loop). Turning the module off stops every delivery without deleting
anything.
