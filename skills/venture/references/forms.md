# Forms: questions, responses, bookings and payments

Read this to build, publish or embed a form, read its answers, or take
bookings, payments, uploads or Lightsite submissions through one.

## Questions and responses

Module `forms` (requires only `core`; suggests `leads`, `mail` and `marketing`). Check
`venturectl describe form`, `describe form_field`, `describe form_submission`.
URL prefill is opt-in per question (`allow_prefill=true`); invalid values are
omitted and final answers revalidate. Personal links use a published
`contact_field=name|email|phone|role|website|address` mapping. Owners can call
`act form ID personal_links contact_ids='[3,8]' origin=https://forms.example days=7`.
The nonstageable result contains private bearer URLs, valid for 1–30 days.
Do not put them in shared notes or logs. `one_per_link`/`one_per_contact` limit
retained bound responses. Consent, sensitive and repeated questions cannot
be prefilled. Marketing sends and email sequence steps accept `survey_form_id`;
links live in private delivery bodies. See `docs/forms.org` for the adapter
contract and expiration behavior.

Question labels/help and page-break headings/intros accept `{earlier_key}`;
`success_message` can refer to any non-repeated question. `{group.count}`
inserts a validated repeat-row count; same-row members can refer to earlier
members. Escape literal braces as `{{` and `}}`. Unknown, forward and
sensitive references are refused at save, including changes that would break
an existing template. Hidden/invalid answers substitute empty text. Piping
never evaluates expressions or HTML. Publish after changing question text.

Double opt-in requires `double_opt_in=true`, `optin_email_field` naming a
required non-sensitive email, `public_origin=https://forms.example`, and a
required `consent` question with `marketing_consent=true`. Optionally set
`optin_list_id` to a static marketing list, then publish. Mail, marketing and
contacts must be enabled. Initial input is a private `form_pending` working
copy; do not create/update it through CRUD. Only the email confirmation POST
creates contact, permission, membership and final response. Links expire in
24 hours; resubmission resends at most three times, 60 seconds apart. Retention
reports `expired_signups`; owner export/erasure includes them. Never log or
paste confirmation capabilities into shared records.

For repeated questions, inspect `describe form_group`, create a group with
`form_id`, stable `key`, `label`, `min_rows` and `max_rows`, then set member
questions' `group_id`. Keep members consecutive on one page and publish.
Maximum 0 means 10; the cap is 50. Public JSON accepts arrays of row objects
under the group key; HTML uses `attendee[0][name]`. Responses and owner
exports retain row arrays, with sensitive members in `sensitive_answers`.
The summary counts rows for repeated questions. Add/remove are private draft
edits, not responses. See `docs/forms.org` for same-row rules and limits.

Use `describe form_rule` before editing conditional questions. A rule has
`form_id`, `action=show|hide|require|jump|end`, `target_key`, and a `conditions`
JSON array of `{field,operator,value}`. Conditions use a closed comparison
vocabulary (see `docs/forms.org`), never expressions. Question targets must
follow their sources; jumps only go forward. Publish freezes rules as well as
questions. Hidden and skipped answers are discarded by the server.

For multiple pages, insert a `form_field kind=page_break` between questions by
`position`; its label/help introduce the next page. Publish refuses empty pages.
Forms can opt into saved drafts with `allow_resume=true`, `public_origin=https://forms.example.com`
and `resume_days=7` (maximum 30). These are ordinary `form` fields. Public Save
issues a private single-use link; Resume loads the newest published version and
asks new required questions and consent again. Do not read or distribute draft
capabilities through generic record tools. `forms` owner export/erasure includes
the optional resume inbox; retention purges expired drafts and cancels queued mail.

`form draft_minutes=60` controls unfinished-response lifetime (0 also means 60).
`form_draft` is service-owned: do not create/update it. Its sensitive answers
are absent from generic reads. Retention sweep removes expired drafts within
its limit and reports `expired_drafts`; owner export/erasure includes them.

- `form`: `name` (internal), `title`/`description`/`submit_label`/
  `success_message` (public), `state` (`draft` default, `live`, `closed`),
  `opens_at` (inclusive), `closes_at` (exclusive), `response_limit`,
  `unique_email_field` (a required email question key; blank permits repeats),
  `redirect_url` (http/https only),
  `allowed_origins` (one `https://host[:port]` per line; empty = any),
  `hourly_limit`, `min_fill_seconds`, `create_lead`, `lead_source`,
  `campaign_id`, `on_duplicate` (`merge`/`create`/`reject`),
  `confirmation_field` (an email question's key), `confirmation_subject`,
  `confirmation_message`. `public_token` and `slug` are made on the first
  save; clearing `public_token` issues a new one (old embeds stop working).
- `form_field` ("Form question"): `form_id`, `key`, `label`, `kind`
  (`short_text` default, `long_text`, `email`, `phone`, `url`, `number`,
  `date`, `single_choice`, `multiple_choice`, `checkbox`, `rating`,
  `hidden`, `consent`), `required`, `position`, `help`, `placeholder`, `choices`,
  `min_value`/`max_value`, `min_length`/`max_length`, `pattern`,
  `default_value`, `maps_to` (`name`, `email`, `phone`, `company_name`,
  `website`, `notes`).
- **`key` is fixed once saved and never reused on that form, even after
  the question is deleted** (its answers keep it). Lowercase letters,
  digits and `_`, starting with a letter.
- **`choices` are lines; each gets a stable id on save** (`Dark blue` →
  `dark_blue | Dark blue`). Answers store the id: relabel freely, but keep
  the id unless you mean a new choice.
- **Nothing is public until published.** `venturectl act form ID publish`
  freezes the questions as the next `form_version`; the public door serves
  that version. Editing questions afterwards changes only the draft until
  you publish again. Versions are read-only. Roll back by setting the
  form's `published_version_id` to an earlier version of the same form.
- `form_submission` ("Form response") **cannot be created with `create`** —
  only the form's public address makes one. Its answers are fixed; you may
  update `reviewed` and `notes`. `answers` is JSON text keyed by question
  key; `summary` is the readable version; `mapping_note` says why a lead or
  confirmation was not made.

```sh
venturectl create form name="Website contact" title="Contact us"
venturectl create form_field form_id=3 key=email label=Email kind=email required=true position=10
venturectl create form_field form_id=3 key=topic label=Topic kind=single_choice \
    choices="Sales
Support" position=20
venturectl update form 3 state=live create_lead=true
venturectl act form 3 publish
venturectl list form_submission form_id=3
venturectl report form_summary all form_id=3
```

Privacy settings and actions:

- `form.privacy_url`, `retention_days`, `retention_action` (`anonymise` or
  `purge`). `venturectl act form 0 sweep_retention organization_id=1 limit=100`
  processes a bounded batch; it is not a timer.
- `form_field.sensitive=true` keeps answers out of generic JSON, pages,
  AI tools, audit content, webhooks and notifications. Sensitive questions
  cannot map to leads or marketing permission, or have a default answer.
- `kind=consent` records the published wording, version and time. It cannot
  have `default_value`. `marketing_consent=true` explicitly maps checked
  permission to the lead captured by `create_lead` and its name/email
  mappings. The marketing module must be enabled; suppression is never
  cleared. Check `mapping_note` for a refused follow-up.
- `venturectl act form 0 erase_person organization_id=1 email=person@example.com`
  is owner-only and erases matching responses, drafts, opt-in copies and files
  across forms in that organization, including soft-deleted rows. Replace
  `email` with `contact_id=42` to use personal-link bindings even after the
  contact loses its email. Queued private mail content is erased; delivery
  identities remain cancelled. Sending/uncertain/sent mail and independent
  CRM, booking and financial records retain their own lifecycle.
- `export_person` is an owner-only access-request export containing sensitive
  answers. It accepts the same email or contact selector; unfinished files
  export metadata rather than bearer claims. It cannot be staged, so generated assistant and MCP action tools
  refuse it. A human owner performs this outside those tools; never put its
  payload into AI context or an approval card.

Public addresses (no session): `/pub/form/TOKEN` (hosted page and where
answers are posted), `/pub/form/TOKEN/fragment`, `/pub/form/TOKEN/schema`,
`/pub/forms.js`. The hosted link and every embed variant a form's page
offers are built on `server.base_url` -- set it behind a reverse proxy,
or they carry the listening address and port; only unconfigured local
development uses that fallback on purpose. Public booking and form
pages send `Referrer-Policy: same-origin`, so a browser post keeps the
origin hosted validation checks; a post from a foreign origin is still
refused. A draft, closed, full, never-published or unknown form is
the same 404. `form_summary` rows carry a `versions` column; a question
whose kind or scale changed between versions is split.

## Languages

For multilingual forms, create `form_translation` records with `form_id`,
`language`, `text_key` and plain `text`, then publish the form. Keys include
`field.KEY.label`, `field.KEY.help`, `choice.KEY.ID`, `form.success_message`
and `message.required`. Read `docs/forms.org` for the catalog. Partial
translations fall back to the form's `default_language`; choice IDs never
change. `venturectl report form_summary all form_id=3 language=fr` changes
report labels while counting responses from every language. Public `?lang=fr`
or the loader's `data-venture-form-lang` selects the language before header
negotiation. Signed state pins the language through pages and opt-in emails.

## Form assessments

Use `describe form_field` before setting `scoring`: choice points are JSON
keyed by stable choice IDs, for example `{"choices":{"yes":5,"no":0},"correct":["yes"]}`.
Number/rating scoring uses inclusive, non-overlapping `ranges` with `min`,
`max`, and integer `points`. Sensitive questions cannot be scored. Enable
`quiz_enabled`, create `form_result_band` records with stable `key`, `form_id`,
`minimum`, `maximum`, `message` and optional HTTPS `redirect_url`, then publish.
`show_answer_key` is opt-in and applies only after completion. Never write
response score/result properties: the server computes and freezes them.
`score_to_lead` adds the trusted `assessment_score` attribute to ordinary lead
capture before the existing scoring rules run; it preserves manual overrides.
`form_summary` shows score distributions separately for each version.

## Booking questions

A `form_field` with `kind=booking` names an existing `booking_page_id`.
Configure the form's HTTPS `public_origin` and required short-text/email
questions first; `booking_name_field` and `booking_email_field` default to
`name` and `email`. One booking question per form, without repeating groups,
prefill, defaults or sensitive storage. Publish after changing the binding.
The public schema exposes current `slots`; final submit rechecks capacity.
`booking_page.capacity=0` means one seat. `form_submission.booking_id` is a
service-owned link to the meeting. Do not write `booking_reservation` records:
they are private working copies. Signed management links go through the private
outbox body; opening one never cancels or reschedules a booking.

## Paid forms

Use `form.payment_enabled=true` and `form_price` declarations (`form_id`,
`key`, `name`, `product_id`, exact `unit_price`, optional `choice_field` /
`choice_id` / `quantity_field`), then publish. Read `describe form_price`;
all prices share one currency and quantities are bounded whole counts.
Required payer name/email keys default to `name`/`email`. Stripe Checkout
uses the server-computed invoice, never a posted amount. Paid intake and
newsletter double opt-in are separate forms.

Do not create or edit `form_payment`: it is a private working copy until
settlement creates the ordinary `form_submission` with `invoice_id`. Form
response automation and confirmation wait for settlement. Paid booking holds
last 45 minutes and use cards; ordinary paid forms retain configured ACH.
An owner can run `act form ID reconcile_payment invoice_id=N` after repairing
a failed follow-up. For `paid_needs_booking`, add `booking_start=ISO_TIMESTAMP`
for a replacement arranged with the customer. This action cannot be staged;
use invoice refund/dispute operations for the financial outcome. A payment
retry must reuse its signed intake nonce and original answers, never create a
second invoice. Pending payment answers participate in owner export/erasure;
financial and guest CRM records retain their independent policies.

The `form-results` dashboard template supplies six form statistics widgets.
Set each widget's `record_id` to a form and its `field` to the stable question
key for choice/NPS/rating kinds. `options={"version":2}` narrows before row
limits. NPS requires a 0–10 rating. `form_dropoff` counts retained unfinished
drafts by current page, not all historical visitors; it never exposes draft
answers. `dashboard SLUG` reads the same scoped aggregates as the page.

For a bounded proposed summary, use
`act form ID summarize first_version=1 last_version=3 period=this_month question=comments limit=100`.
Version endpoints are inclusive; omitted last version means the published one.
Only nonsensitive short/long text answers are sent to the organization-bound
toolless model. Theme counts use cited answer IDs and quotes are checked against
the input. Nothing is saved back to responses. Excessive/empty input is refused;
narrow the scope. This action is nonstageable and requires an editor.

Form templates and portability use generic actions:
`act form 0 templates organization_id=1`,
`act form 0 import_definition organization_id=1 template=contact`, and
`act form 12 export_definition format=yaml`. Export returns `result.text`
and `result.definition`; import accepts `definition=<JSON-or-YAML text>` or
one template name. The eight names are contact, feedback, nps, event-signup,
job-application, newsletter, appointment-request and order. Imports stay draft
with fresh tokens; newsletter needs its real `public_origin` before publish.
External record references require `bindings={"product:17":42}` (same-org
existing destination IDs). Responses and signing keys never travel with a
form definition. Appointment/order templates collect requests until an author
adds a booking question or payment prices. See `docs/forms.org`.

File questions use `kind=file`, `file_max_bytes` (default 5 MiB; max 20 MiB),
`file_max_count` (default 1; max 10), and `file_types` (PDF, PNG, JPEG and/or
plain-text MIME names, comma separated). Publish after editing limits.
`form.upload_quota_bytes` and `upload_client_hourly_bytes` default to 100 MiB
and 20 MiB/hour when zero. The documents module must be enabled.

Upload through the public multipart form, not generic record creation.
`form_upload` is a private service-owned working copy. A completed response
contains file name/size/type and document references; sensitive file metadata
is omitted from ordinary reads. Download bytes only through the authenticated
`/forms/uploads/ID` route with response read authority (owner for sensitive
files). Generic document/AI attachment readers refuse these bytes. Erasure and
the forms retention sweep remove their files; neither CLI output nor an AI
summary contains file contents.

## Retrying public form intake

Fetch a fresh public schema/fragment per intentional submission. Retain
`submission_nonce` under the schema's `submission_field` (currently `_vf_payment`)
and retry exactly the same answers after an uncertain final response. A changed
payload under an accepted identity conflicts; do not obtain a new identity merely
to retry. Ordinary forms now keep a private service-owned `form_receipt` in the
acceptance transaction. Never create or edit receipts with generic commands.
Erasure removes their fingerprints and response links but preserves a minimal
consumed-identity marker. Closed/full forms still return the uniform 404.
Copied HTML snippets first open a native confirmation form to obtain a fresh
identity; legacy clients omitting it remain outside the retry guarantee.

For Lightsite user management, `GET /api/v1/account-authority` describes the
credential's own principal. Match origin/workspace/organization and require
`can_manage_sites: true` (or `can_edit_sites` / `can_view_sites` for editing and
reading); a displayed current role alone cannot expand an old
token. A shared provisioning token does not identify a signed-in customer.
See `docs/lightsite-accounts.org`. Staging Mailpit uses normal organization mail
settings and the operator's explicit `mail.plaintext_endpoints` exception;
never put credentials on plaintext SMTP or rely on legacy global mail settings.

## Native Lightsite forms

Authenticated `GET /api/v1/forms/:id/definition` returns published fields,
labels, constraints and stable choice IDs for native rendering, without visitor
capabilities or internal mappings. Read the returned `form_id`/`form_version`.
Lightsite's daemon posts signed complete answers to
`/hooks/lightsite/:site/:connection/forms`, preserving its original
`submission_id` on retries. Origin comes from Venture's paired site, not the
browser payload. This uses ordinary form lead mappings and receipts; it does
not create invoices/quotes. Payments, bookings, uploads and double opt-in use
the hosted workflow. An optional `referral_code` beside `answers` carries the
code the visitor arrived with to the lead the form makes. See `docs/forms.org`
for the exact closed envelope.
