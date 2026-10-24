# Data model: customers, outreach and mail

The CRM, leads, activities, pipelines, sequences, marketing, outreach,
attribution, territories, mail and calendars. Conventions are in
[data-model.md](data-model.md); usage is in [crm.md](crm.md),
[outreach.md](outreach.md) and [mail.md](mail.md).

## crm (requires core) -- report `pipeline`

- `company` -- an outside business: customer, prospect, supplier,
  marketplace or partner (`kind` [customer|prospect|supplier|platform|partner|other]).
  **name**, `legal_name`, `email`, `phone`, `website`, address fields
  (`address_state/county/city` pick sales tax), `owner_user_id -> user`,
  `team_id -> team`, `venture_id`, `default_price_list_id`, `campaign_id`,
  `source`, `tax_exempt` + reason/number, `dunning_policy_id`,
  `dunning_opt_out`, `dunning_paused_until`, `merged_into_id`. Distinct
  from an *organization* (which is you). *actions* `log_call (s)`,
  `consent_marketing`, `collect_retainer`.
- `contact` -- a person, belonging to a company by reference:
  **name**, `company_id`, `email`, `phone`, `role`, `owner_user_id`,
  `team_id`, `subscribed`, `merged_into_id`. *actions* `log_call (s)`,
  `consent_marketing`.
- `interaction` -- history: `kind`
  [note|email|call|meeting|message|purchase|support|outreach], `subject`,
  `body`, `occurred_at`, `outbound`, contact/company/deal/lead.
- `deal` -- **name**, `value`, `probability`, `stage`
  [lead|qualified|proposal|negotiation|won|lost] (legacy) and
  `pipeline_id`/`stage_id` (moved only by `deal move`), `company_id`,
  `contact_id` (same company), `owner_user_id`, `territory_id`,
  `expected_close_at`, `next_step`/`next_action_at`, `loss_reason_id`,
  `committed`. *action* `handoff (s)` (to a client project).

## leads (requires crm)

`lead` (**name**, `company_name`, `email`, `source`, `campaign_id`,
`status` [new|working|qualified|unqualified|converted|recycled], `score`
(+ `score_manual`), `owner`, `routing_rule_id` (service-set),
`recycle_until`, `converted_*_id` (service-set); *actions* `log_call (s)`,
`consent_marketing`), `lead_form` (public capture: `public_token`,
`fields`, `honeypot`, `on_duplicate`), `lead_assignment_rule`,
`lead_routing_rule` (`position`, `conditions`, `action`
[assign_user|round_robin|assign_venture], `assign_to`, `team_id`),
`lead_scoring_rule` (`conditions`, `points`), `lead_score_history`
(service-only). Reports `lead_sources`, `lead_response_time`,
`leads_recycled_due`, `lead_routing`, `lead_scoring`.

## activities (requires crm) -- reports `worklist`, `calls`

`activity` -- planned work and logged calls: **subject**, `kind`
[task|call|meeting|email|followup], `status` [planned|done|cancelled]
(done only via `activity complete`), `owner`, `due_at`, `starts_at`/
`ends_at`, `remind_at`, `recurrence` [none|daily|weekly|monthly],
`related_type/related_id`, contact/company/deal/lead, call fields
(`call_direction`, `call_duration`, `call_outcome`
[unknown|reached|voicemail|no_answer|callback], recording, transcript,
external replay key, follow-up). *action* `log_call (s)`. `activity_type`.

## pipelines (requires crm) -- reports `stage_duration`, `funnel`, `forecast`, `loss_reasons`, `overdue_deals`

`pipeline` (`kind` [sales|renewal|partnership|custom], `default`),
`pipeline_stage` (`position`, `kind` [open|won|lost], `probability`,
`required_fields`, `rotting_days`), `deal_stage_entry` (history),
`loss_reason`, `deal_line` (products and prices a quote is built from).

## sequences (requires crm) -- reports `sequence_performance`, `sequence_failures`, `sequence_engagement`

`sequence` (exit rules, send window, `weekdays`, `tracking`, `goal`),
`sequence_step` (`channel` [email|call_task|sms_task|wait], delays,
subject/body), `sequence_enrollment` (`status`
[active|paused|completed|exited|failed]; created via `sequence enroll`,
progress service-owned), `sequence_delivery` (`state`
[pending|sent|failed|skipped]), `suppression` (`reason`
[unsubscribed|bounced|complained|manual]), `sequence_link`,
`sequence_tracking_event` (`kind` [open|click], `machine_reason`),
`sequence_trigger` (journey triggers: `kind` [form_submitted|tag_added|
deal_stage_changed|quote_not_accepted|invoice_paid|booking_made], `tag`,
`form_id`, `deal_stage`, `days`; `feed_cursor` derived). Steps add
`channel` branch with `branch_on` [none|opened|clicked|replied],
`yes_position`, `no_position`, `then_position`, `end_after`; an enrollment
a trigger made carries `trigger_id` and a unique `trigger_key`. See
`docs/journeys.org`.

## marketing (requires mail, sequences, leads) -- report `marketing_performance`

`marketing_list` (`mode` [static|segment], `target`
[contact|company|lead], `filters`), `marketing_member` (exactly one of
contact/company/lead), `marketing_consent` (evidence: `source`,
`evidence`, `evidence_key`, `recorded_at`; *action* `withdraw`),
`marketing_send` (`state` [draft|preview|approved|paused|cancelled|complete];
*actions* `preview`, `approve`, `pause`, `resume`, `cancel`, `run` --
none stage), `marketing_recipient` (frozen per-recipient copy; *action*
`feedback`; evidence stamps `bounced_at`, `opened_at`, `clicked_at`,
`unsubscribed_at`, `delivered_at`, `replied_at`, `complained_at`,
`machine_opened_at`), `marketing_event` (`machine_reason` when an
observation was judged a machine's), `marketing_sending_domain` (owner/admin
setting: `domain`, `dkim_selector`, `warmup_started_on`, `ramp_schedule`),
`marketing_topic` (what the preference page offers), `marketing_preference`
(service-owned per address: `topics_off`, `frequency` [any|weekly|monthly]).
A send adds `topic_id`, A/B (`variant_subject`, `variant_text_body`,
`variant_html_body`, `test_percent`, `winner_metric` [opens|clicks],
`winner_wait_hours`; `winner` derived), local time (`local_time`,
`local_hour`, `local_date`) and `resend_of_id`/`resend_after_days`
(*action* `resend_non_openers`). Contacts and companies have *actions*
`add_tag`/`remove_tag`; contacts and organizations a `timezone`.

## outreach, ideas (requires sales / core) -- reports `campaigns`, `ideas`

`campaign` (`status` [draft|scheduled|running|paused|completed|cancelled],
`channel`, `budget`, `spend`, `revenue`, counts), `newsletter`,
`subscriber` (`status` [pending|active|unsubscribed|bounced|complained]),
`post` (`status` [draft|review|scheduled|published|archived]). `idea`
(`status` [captured|researching|validated|rejected|promoted],
`opportunity`, `confidence`, `effort` -- ranked opportunity x confidence /
effort, `promoted_venture_id`), `research_note`.

## attribution (requires leads, integrations) -- report `attribution`

`attribution_site` (`origin`, `external_site_id`/`external_tenant_id`,
`lead_form_id`, `consent_policy`, `campaign_map`, `connection_id`,
lookback/retention days; owner/admin only), `attribution_visitor`
(*action* `retention_sweep (0)`), `attribution_touch`,
`attribution_submission`, `attribution_binding` -- service-owned evidence.

## sales_performance (requires orgaccess, leads, pipelines) -- report `sales_attainment`

`sales_territory` (`team_id`), `sales_quota` (`metric`, `target`,
`starts_at`/`ends_at`, owner or team), `sales_credit` and
`sales_assignment` (immutable evidence; correct the source deal instead).

## crm_import, dedupe

`crm_import` (HubSpot/Zoho/Salesforce migration: `source`, `state`,
`manifest`, `report`; *actions* `import`, `activate`, `rollback`),
`crm_import_row`. `duplicate_candidate` (`kind` company|contact,
`record_a`/`record_b`, `score`, `reasons`, `status`; *actions*
`scan (0)`, `merge (s)`, `dismiss`).

## mail, mail_sync, calendar

- mail (requires core): `mail_message` -- the durable outbox: `to`,
  `subject`, bodies, `attachments`, `related_type/related_id`, `state`
  (incl. `uncertain`, `dead`), `attempts`, `last_error`, `message_id`,
  `idempotency_key`, `connection_id/connection_version`. `mail_template`
  (`design`: block JSON; with one, `text_body`/`html_body` are rebuilt
  from it on every save; `design_version`). Designed email (owner/admin
  write, docs/email-design.org): `email_brand` (one per organization:
  `logo_image_id`, `*_color` as `#rrggbb`, `font` sans/serif/rounded/
  system/mono, `website_url`, `footer_address`, `logo_ask`),
  `hosted_image` (made only by upload; `token`, sniffed `content_type`,
  `width`/`height`), `email_section` (shared blocks, cycles refused),
  `email_design_version` (written only by the design service).
- mail_sync (requires mail, crm, leads): `mail_account` (IMAP mailbox;
  `address`, `imap_host`, `folders`, `capture_address`,
  `private_owner_id`; sync state fields are the sweep's), `mail_inbound`
  (filed message, read-only; `skip_reason` on a stub),
  `mail_unmatched_sender` (`dismissed`).
- calendar (requires activities, crm, leads): `calendar_account` (CalDAV;
  `private_owner_id` immutable), `calendar_event` (synced, read-only; `planner_event_id` on a pushed planner event's copy),
  `booking_page` (`slug`, `owner`, `duration_minutes`, `buffer_minutes`,
  `timezone`, `availability` JSON, `horizon_days`; serves `/book/<slug>`).

## Phone and text (`telephony` module; docs/telephony.org)

Requires `crm`, `activities`, `integrations` and `marketing`.
`telephony_account` (a phone line: `provider` 3cx or voipms, `base_url`,
`phone_number`, `connection_id` to the encrypted integration binding,
`webhook_sources`, `media_hosts`; written only by the telephony service),
`telephony_extension` (`user_id`, `extension`; one per person per
organization) and `text_message` (`contact_id`, `direction`, `body`,
`state` queued/held/sent/delivered/failed/received, `hold_until`,
`provider_message_id`, `message_key` replay identity, `origin`, `keyword`,
`auto_reply`, `inbox_ready`; service-written). Calls are ordinary
`activity` rows through `log_call` (outcome `missed` and
`call_inbox_ready` added); text consent is `marketing_consent` with
`purpose` text and `phone`; a `marketing_send` with `channel` text is a
text campaign; sequence step channel `text` sends a real text. The
organization carries `telephony_country`, `telephony_quiet_hours`
(empty is 21:00-08:00 recipient time, `off` for none) and
`telephony_recording_links` (off).

## One inbox (`inbox` module; docs/inbox.org)

Requires `crm` and `activities`. `inbox_conversation` (one per contact:
`contact_id`, `status` open/waiting/closed, `owner_id`, `last_message_at`,
`last_inbound_at`, `last_inbound_channel`, `channels`; written only by the
inbox service, never a message body), `inbox_chat_widget` (a web chat:
public `token`, `greeting`, `allowed_origins`, `active`),
`inbox_chat_session` (a visitor's chat: `contact_id`, what they gave,
`token_hash` of their link) and `inbox_chat_message` (`session_id`,
`contact_id`, `inbound`, `body`, `author`, `sent_at`). The thread is read
from each channel's own records through registered sources (email
interactions, text messages, call activities, chat messages).

## jobs (requires crm; suggests quotes, invoicing)

See [jobs.md](jobs.md) for the workflow and refusal rules.

- `job`: customer work order; `title`, `number`, `company_id`, `contact_id`,
  `property_id`, `service_address`, `problem`, `window_start/window_end`,
  `quote_id`, `invoice_id`, `maintenance_plan_id`, `notes`;
  `status` [booked|on_the_way|on_site|done|invoiced|cancelled]. Actions:
  `assign` (`user_id`, `lead`), `on_the_way`, `arrive`, `complete`,
  `invoiced` (optional `invoice_id`), `cancel`. None is stageable.
- `job_technician`: `job_id`, `user_id`, `lead`; one assignment per user/job.
- `property`: customer's service address; `name`, `company_id`, `address`,
  `access_notes`, `contact_id`, `notes`.
- `equipment`: `name`, `property_id`, `make`, `model`, `serial`,
  `installed_year`, `warranty_until`, `notes`.
- `maintenance_plan`: `name`, `company_id`, `property_id`, `equipment_id`,
  `visit`, `every_months`, `next_visit`, `ended_at`, `notes`.
  Action `book` creates the visit job and advances the due date atomically.
