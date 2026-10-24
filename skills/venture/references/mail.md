# Mail and calendars: the outbox, SMTP, inbound IMAP, CalDAV, booking pages

Read this to send or retry mail, diagnose a message stuck as `uncertain`
or `dead`, sync mailboxes onto contact timelines, or sync calendars.
Sources: `docs/mail.org`, `docs/connectors.org`, `docs/calendar.org`.

## The outbox (module `mail`)

Every message -- an invoice sent, a dunning step, a sequence email, a trial
reminder, a test -- is a durable `mail_message` row first, delivered later.

```bash
venturectl mail send to=ana@example.com subject="Your invoice" body="Hello" [--html FILE]
venturectl mail test to=me@example.com          # immediately tests real SMTP
venturectl mail deliver --limit 50              # submit due rows
venturectl mail list state=uncertain            # acceptance unknown
venturectl mail retry 88                        # deliberate resend, same Message-ID
```

- States include `failed` (will retry), `dead` (gave up; read `last_error`
  and decide), and `uncertain` (the relay may or may not have accepted it).
  **Never automatically retry uncertain rows**: a retry may mail the
  customer twice. The outbox commits `sent` before writing the CRM timeline.
- Mail actions reject `--stage`; propose an enqueue with `--stage create
  mail_message ...` when approval is required.
- Subscription notices are queued inside the billing instruction and keyed
  for once-only delivery ([billing.md](billing.md)).

### Previews and test sends

```bash
venturectl mail preview invoice 41      # what Send would queue, with checks, as JSON
venturectl mail preview reminder 41     # the overdue step the sweep would send today
venturectl mail preview message 88      # a row already in the outbox
```

- Kinds: `invoice`, `quote`, `reminder`, `receipt`, `campaign` (a
  `marketing_recipient` id), `sequence` (a `sequence_delivery` id),
  `portal-invitation`, `account-welcome`, `account-password-change`. Other
  mail (notices, bookings, forms, report packs) previews once queued, as
  `message`. Alerts send no email.
- Owner or administrator of the record's organization only; the page is
  `/mail/preview/KIND/ID` (three widths, light and dark, plain text,
  checks) with *Send me a test*: one copy to the signed-in user's own
  address, `[TEST]` subject, private links replaced, through the outbox.
- Checks: `plain-text`, `image-alt`, `links` (syntax always; reachability
  only with `?reachability=1`), `size` (warns above about 102 KB),
  `unsubscribe` (required for campaigns and sequences), `source` (sender
  notes). Private bearer links never appear in a preview or a test send.
- `email-design` previews a `mail_template` with a design. Designed email
  (docs/email-design.org): `/email-design` (builder, owner/admin),
  `POST /email-design/images` (upload, sniffed, 5 MB), public
  `GET /i/ORG/TOKEN.EXT`; actions on `mail_template`: `design_email`
  (type-level: `brief`, `organization_id`, optional `template_id`),
  `edit_design` (`instruction`), `undo_design`, `rebuild_designs`. The AI
  never invents a price, percentage, date or "free": they must be in the
  brief or the records, or the sentence is removed / the design refused.
- A sender's enqueue path must call its preview builder; a second
  composition beside it is how a preview and the real mail drift apart.

## Who sends it: organization SMTP accounts

Outbound `mail test`, `mail send` and `mail deliver` use the explicit
business organization's SMTP binding. Missing configuration never falls
back to installation credentials. An organization owner/admin configures it
at `/organizations/ID/settings/mail`; the operator must first permit the
relay in `mail.allowed_endpoints` (deny by default). Password inputs are
write-only. A delivery retains `connection_id` and `connection_version`
before SMTP: a retry can use rotated credentials for the same connection,
but replacing an account does not move old attempts to it. The settings
page's test sends only its selected test message and shows the retained
delivery evidence. `mail.receipts` (default true) mails a customer a
receipt with its PDF when a payment is recorded. A company whose
`external_id` is `lightsite:organization:…` is skipped: the hosted
application sends that owner's receipt. An existing outbox row keeps its
original identity across that change. Quote acceptance confirmations are
queued by the quote service ([receivables.md](receivables.md)).

## Inbound mail (module `mail_sync`)

```bash
venturectl mail sync [organization_id=N] [limit=N]   # bounded IMAP sweep of active accounts
venturectl mail sync account_id=N                    # one readable account now, ignoring backoff
venturectl list mail_unmatched_sender dismissed=false
venturectl mail contact 5                            # unmatched sender -> contact, backfills its mail
venturectl mail dismiss 5                            # keep the address as an ignore entry
```

- The sweep covers every active `mail_account` in the organization that is
  not backing off; the report's `skipped` counts accounts in backoff or
  mid-sync elsewhere, and `deferred: true` means a message or time budget
  ran out -- run it again to continue. `contact`/`dismiss` take the
  unmatched sender's id, not a contact id.
- `mail_account` is assigned by organization administrators and uses an
  explicit encrypted binding in connector settings (`secret_env` is unused
  historical metadata; the operator allowlists hosts in
  `imap.allowed_endpoints`). A positive `private_owner_id` imports private
  messages and attachments without CRM capture; zero is shared business
  mail. Its `consecutive_failures`, `next_attempt_at`, `last_error`,
  `cursors` and `sync_lease_until` are the sweep's -- do not write them.
  Five failed syncs ending in a refused login set `active=false`: fix the
  credentials, then `update mail_account ID active=true`.
- Generic writes to `mail_inbound` are refused; `mail_unmatched_sender` is
  ordinary CRM data. Read what the sweep filed with `list mail_inbound`
  and `list mail_unmatched_sender dismissed=false`. A `mail_inbound` row with `skip_reason` is a stub
  filed after repeated failures or read truncated for size. Mail to the
  account's `capture_address` lands in the capture inbox
  ([payables.md](payables.md)).

## Calendars (module `calendar`)

`calendar sync [organization_id=N] [limit=N]` runs the bounded two-way
CalDAV sweep over every active `calendar_account`: dated calls and meetings
go up as VEVENTs, events made on the calendar come back as planned
meetings, removals cancel rather than delete, and a change on both sides is
settled by last-modified with the loser noted on the activity timeline. It
refuses `--stage`. `calendar_account` uses an explicit encrypted connector
binding (`secret_env` unused; origins allowlisted in
`calendar.allowed_origins`); a positive `private_owner_id` selects private,
read-only imports without shared activity mirroring or export; ownership
is immutable. Generic writes to `calendar_event` are refused.

`booking_page` (`slug`, `owner`, `duration_minutes`, `buffer_minutes`,
IANA `timezone`, `availability` JSON of weekday to `HH:MM-HH:MM` windows,
`horizon_days`) is ordinary editor data and serves the public
`/book/<slug>` page, which books a contact and a meeting. The planned
activities also export as `/api/v1/activities.ics`; the money calendar as
`/money/calendar.ics`.
