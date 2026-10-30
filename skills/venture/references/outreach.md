# Outreach: sequences, marketing sends, consent, attribution, campaigns

Read this to enroll contacts in follow-ups, send an approved bulk mail,
record or withdraw consent, or attribute leads and revenue to sources.
Transactional mail and its delivery are in [mail.md](mail.md). Sources:
`docs/sequences.org`, `docs/marketing.org`, `docs/attribution.org`.

## Follow-up sequences (module `sequences`)

Use `describe sequence`, `describe sequence_step` and `describe
sequence_enrollment` before configuring a journey.

```bash
venturectl sequence enroll 2 contact_id=40 enrollment_reason="Demo request"
venturectl --stage sequence enroll 2 contact_id=40      # or: --stage create sequence_enrollment sequence_id=2 contact_id=40
venturectl sequence run --as-of 2026-09-14T13:00:00Z organization_id=1
venturectl sequence status 17
```

- Approval rechecks suppression and duplicate enrollment at application
  time. Generic enrollment edits are refused: pause, resume, exit and
  recording a goal are `POST /api/v1/sequence_enrollment/:id/pause|resume|
  exit|goal`.
- `sequence run` processes due steps for one organization (an ISO
  timestamp with zone). Email steps atomically create pending
  `sequence_delivery` and `mail_message` rows. Run `mail deliver` for that
  organization to submit them. Repeated sweeps recover older pending steps
  and preserve the same outbox identity across restarts. Only committed
  SMTP acceptance marks a sequence delivery sent.
- Import a contact's reply with `mail sync`; it exits reply-sensitive
  enrollments and cancels later pending deliveries, including a final email
  still queued after the step list completed. The outbox rechecks eligibility
  immediately before submission. Pauses defer a queued message; they do not
  spend its retry budget.
- Tracking (opt-in per sequence and organization): `/t/o/:token.gif` opens
  (at most one per delivery per UTC day), `/t/c/:token/:n` clicks.
  Reports `sequence_performance`, `sequence_failures`,
  `sequence_engagement`. One-click stop is public `GET`/`POST
  /sequences/u/:token`: GET only draws the form; POST requires
  `List-Unsubscribe=One-Click` and writes an organization suppression.

## Marketing sends (module `marketing`)

Use `describe marketing_list`, `describe marketing_member`, `describe
marketing_send` and `describe marketing_recipient`. Records are ordinary;
consent and delivery evidence are service-owned.

1. **Consent first.** An organization editor records explicit permission:
   `act contact ID consent_marketing source="Signed preference form"
   evidence="Requested marketing email" evidence_key=FORM_ID
   occurred_at=2026-09-20T10:00:00Z` (company and lead have the same
   action). Use the real evidence timestamp, never a fabricated one.
   Existing CRM records never imply permission.
2. **The audience.** A static list with member rows naming exactly one
   `contact_id`, `company_id` or `lead_id` (a company means its own primary
   mailbox), or a segment: `mode=segment target=contact filters='name=Alice'`
   (ordinary typed filters; no organization/history/pagination override).
3. **The send.** Create a `marketing_send` referencing the list, then `act
   marketing_send ID preview` (configure an HTTPS `server.base_url` first).
   Review `list marketing_recipient send_id=ID` and the frozen
   counts/content, then `act marketing_send ID approve`. Preview is
   immutable: new copy, filters or recipients need a new draft.
4. **Running it.** `act marketing_send ID run limit=100` examines a bounded
   audience, queues at most one due recipient and attempts that exact
   organization-bound outbox message; repeat for progress (default
   interval 60 s) -- it does not start an unbounded job. `pause`, `resume`,
   `cancel` preserve identities. Uncertain SMTP acceptance blocks progress
   until deliberately resolved through the outbox retry workflow; retry may
   duplicate a delivery and is never automatic. None of these actions stage.

- Current marketing permission for one address is
  `GET /api/v1/marketing_consent/current` (viewer; `purpose=marketing`;
  `Cache-Control: no-store`). `act marketing_consent ID withdraw` suppresses the address and stops
  applicable queued campaigns and sequences; transactional mail is
  independent. Unsubscribe links are private capabilities sent only in
  mail: GET shows a confirmation, POST performs an idempotent
  organization-scoped withdrawal. A new consent row cannot clear a retained
  suppression. Never request or expose private body/token fields.
- Relay feedback: `act marketing_recipient ID feedback kind=hard_bounce
  source="Reviewed DSN 5.1.1" event_key=DSN_ID occurred_at=TIMESTAMP`;
  `temporary_bounce` does not suppress, `complaint` does.
- Tracking needs both organization `marketing_tracking=true` and send
  `tracking=true` before preview. `report marketing_performance` uses
  approval cohorts and current outcomes; acceptance is not inbox delivery,
  and observed opens/clicks are not proof of reading.

### Deliverability (`docs/deliverability.org`)

- Bounces and complaints also arrive by themselves: DSNs and abuse reports
  in a synced shared mailbox are read on sync, and a provider can post to
  `POST /hooks/deliverability/ORG/generic` signed with the organization's
  secret (`X-Venture-Timestamp`, `X-Venture-Signature` = hex
  HMAC-SHA256 of `timestamp + "\n" + body`, 300 s window). Set the secret
  with `POST /api/v1/marketing/feedback-secret {"organization_id": N}`
  (owner/admin; shown once). No secret means every delivery is refused.
- Replies are counted by the sequence reply rule (an inbound interaction
  for the contact) onto the latest accepted campaign message within 30 days.
- Domain check: `GET /api/v1/marketing/domain-health?organization_id=N&domain=D&selector=S`
  (owner/admin) gives SPF, DKIM and DMARC as pass/weak/missing/error with
  what to change. It reads public DNS only.
- Warm-up: `marketing_sending_domain` (`domain`, `dkim_selector`,
  `warmup_started_on`, `ramp_schedule` = `day:limit` pairs, empty for the
  default 50 rising to no limit at day 30, `off`). A run that reaches the
  day's limit waits for the next UTC day; that is not an error.
- One send: `GET /api/v1/marketing/sends/ID/report` (also
  `report marketing_performance` with option `send_id`): delivered,
  opened, machine_opened, clicked, replied, unsubscribed, bounced,
  soft_bounced, complained and `links` (the click map). Machine-judged
  opens are never in `opened`; Apple privacy proxies cannot be detected,
  so `opened` is an upper bound.

### Audiences and journeys (`docs/marketing.org`, `docs/journeys.org`)

- Segment filters beyond fields: `tag=vip`, `opened_within_days=N`,
  `clicked_within_days`, `replied_within_days`, `bought_within_days`,
  `booked_within_days` (contacts only; people's opens, not machines').
- Tags: `act contact ID add_tag tag=vip` / `remove_tag`.
- Preference page `/marketing/u/<token>`: topics, weekly/monthly, stop all.
  A send with `topic_id` leaves out `topic_turned_off` and `frequency`.
- A/B: `test_percent` + B fields; arms on the send report (`arms`, `winner`).
- Local time: `local_time=true local_hour=9 [local_date=YYYY-MM-DD]`.
- Resend: `act marketing_send ID resend_non_openers days=3 subject=...`
  makes a draft; preview, approve and run it as usual.
- Journeys: branch steps and `sequence_trigger` records; feed triggers run
  with `sequence run`.

## First-party source attribution (module `attribution`)

`describe attribution_site` exposes the organization-owned site
configuration; only organization owners/admins may change it. Set its exact
HTTPS `origin`, verified `external_tenant_id`/`external_site_id`,
`lead_form_id`, `consent_policy`, and `active=true`; `campaign_map` maps
bounded UTM labels to same-organization campaign ids. Lookback/retention
default to 30 days; a changed configuration needs fresh analytics
permission. Pair/rotate/disconnect write-only signing credentials on the
organization's Lightsite form settings page, then name that
`connection_id` on the site. No secrets in records or CLI arguments. Site
identities cannot be deleted: deactivate them so existing withdrawal
capabilities keep working. No Lightsite provisioning is performed here.

```bash
venturectl report attribution 2026-09 organization_id=1 model=first [details=true]
venturectl act attribution_visitor 0 retention_sweep organization_id=1 limit=100
```

`attribution` groups source records by source/campaign, measure and
currency (`model=last` for last touch; `details=true` lists contributing
record identities). Read `period_basis` and `evidence`: new leads,
conversions, first applied-cash customers, won deal value, issued net and
applied cash have different dates and denominators. Imported `cac_*` and
`campaigns_*` metrics are the existing reports under the same scope, not
extra attributed revenue. Currency buckets are never added. Legacy CRM
source is labelled; no touch or consent is inferred. `venture_id` and
`as_of` are refused. Analytics withdrawal is independent of marketing
permission: it stops tracking and removes visitor linkage, while coarse
business acquisition and separate email-choice evidence remain; generic
CRUD cannot manufacture or remove that evidence. The retention sweep (owners/admins) redacts expired private
observations and forgets expired capability hashes. The signed contract
and browser consent methods: `docs/attribution.org`.

## Campaigns and newsletters (module `outreach`)

`campaign` (`status` running, not "active"; `budget`, `spend`, `revenue`),
`newsletter`, `subscriber`, `post` are ordinary records; sales and expenses
name a `campaign_id` so `report campaigns` can attribute revenue and
return, and `cac` pro-rates campaign spend.
