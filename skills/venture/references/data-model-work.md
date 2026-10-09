# Data model: work, knowledge, the factory and the platform pages

Tickets and the desk, knowledge bases, forges and coding runs, the
software factory, dashboards, webhooks, chat and the assistant. Conventions
are in [data-model.md](data-model.md); usage is in [desk.md](desk.md),
[factory.md](factory.md), [dashboards.md](dashboards.md) and
[automation.md](automation.md).

## tickets (requires core; suggests crm, ideas, forge) -- reports `support`, `support_rollup`

- `ticket` -- one shape for internal work and external support. **title**,
  `description`, `kind` [internal|external] (whose problem; decides who
  reads replies), `issue_type` [task|subtask|story|epic|bug|research]
  (what shape the work is; what forge rules key on), `status`
  [triage|todo|in_progress|blocked|review|done|cancelled], `priority`
  [low|normal|high|urgent], `assignee` (a username), `owner_user_id`,
  `team_id`, `contact_id`, `company_id`, `venture_id`, `idea_id`,
  `parent_id -> ticket`, `repo_id -> forge_repo`, `milestone_id`,
  `release_id`, `sprint_id`, `story_points`, `estimate_hours`, `due_at`,
  `resolution`, `tags`. Derived: `resolved_at`, SLA due times
  (`first_response_due_at`, `resolution_due_at`, `first_responded_at`,
  `sla_breached`), `logged_hours`. `satisfaction`
  [unrated|bad|neutral|good] + comment.
- `ticket_comment` -- the ticket's own conversation: `body`, `author`,
  `internal` (a note the requester never sees). Tickets take no generic
  `comment`.
- `ticket_relation` -- a ticket about any record (`subject_type`,
  `subject_id`, label stored); made through the service, not CRUD.
- `sla_policy` (`kind`/`priority` or `all_*`, `first_response_hours`,
  `resolution_hours`), `macro` (canned reply plus `apply_status`,
  `apply_priority`, `assignee`, `add_tags`), `worklog` (`hours`, `note`),
  `sprint` (`status` [planned|active|completed], `starts_on`, `ends_on`,
  `capacity_points`), `routing_rule` (`strategy`
  [round_robin|least_busy|first], `assignees`, matching kind/priority/tag).

## kb (requires core) -- no reports

`knowledge_base` (**name**, **slug**, `instructions`, `embedding_model`
(claimed by the first model to index it), `source_path`, `auto_retrieve`),
`kb_article` (**title**, **kb_id**, `body`, `format`
[org|markdown|text|html|pdf|docx|other], `status`
[published|draft|archived], `source_path`/`source_hash`, `origin_type/
origin_id`), `kb_chunk` and `kb_link` (derived, purged not soft-deleted,
hidden from the related-records walk).

## forge (requires tickets, integrations) -- coding runs

- `forge` -- a Forgejo/Gitea/GitHub/GitLab server (`kind`, `base_url`,
  `clone_base_url`, `bot_username`, `verified_at`, `active`). Owner-only;
  credentials go through `forge settings`, never fields.
- `forge_repo` -- `name` (`owner/repo`), `forge_id`, `default_branch`,
  `branch_prefix`, `push_issues`, `accept_issues`, `workspace_path`.
- `forge_rule` -- what may happen automatically: scope (`repo_id` or
  `forge_id`), `issue_type` or `all_issue_types`, `enabled`, `trigger`
  [manual|on_create|on_todo|on_in_progress], `runner` [agent|cli],
  `outcome` [draft_pr|push_branch|local_branch|none], `provider`, `model`,
  `max_turns`, `timeout_seconds`, `max_runs_per_day`, `require_approval`.
  Admin-only.
- `ticket_link` -- a ticket in a repository: issue number/URL, `branch`,
  pull request, `origin` [forge|venture].
- `forge_run` -- one attempt by a runner (read-only evidence): `state`
  [queued|running|succeeded|failed|cancelled|refused|interrupted], tokens,
  `cost`, `summary`, `log`, `failure_reason`.
- `agent_session`, `agent_turn` -- the agent harness (`/harness`):
  `state` [idle|working|closed|failed], provider, model, workspace.
- `agent_budget` -- `limit` money per `period` [monthly|weekly|all_time],
  `warn_percent`, `hard_stop`, optional `repo_id`.

## factory (requires forge) -- reports `releases`, `lead_time`, `incidents`, `delivery`

`milestone` (`status` [planned|active|completed|cancelled], `due_on`,
`completed_at` derived), `release` (**number** -- the version; `status`
[planned|in_progress|released|yanked], `tag`, `changelog`, `released_at`
derived, `repo_id`, `external_id`), `build` (one CI run from the webhook:
`status` [queued|running|succeeded|failed|cancelled], `trigger`
[manual|webhook|rule|run], `commit`, `ref`, `log_excerpt`), `environment`
(`kind` [development|staging|production|other]), `deployment` (`status`
[pending|in_progress|succeeded|failed|rolled_back], `deployed_at`
derived), `incident` (`severity` [sev1..sev4], `status`
[open|mitigated|resolved|postmortem] -- "open" means open or mitigated,
`release_id`, `deployment_id`, `ticket_id`, `postmortem`).

## dashboards (requires core)

`dashboard` (**name**, `slug`, `purpose` [overview|reporting|work],
`layout` [one_column..four_columns], `home` (one only), `personal`,
`owner_user_id`, `venture_id`), `dashboard_widget` (**dashboard_id**,
**kind**, `span` [normal|wide|full], `title`, `entity_type`, `record_id`,
`report_name`, `period`, `filter`, `order`, `limit`, `field`, `columns`,
`body`, `options` (JSON text), `refresh_seconds`, `grid_col/row/width/height`).

## webhooks (requires core)

`webhook` (**name**, **url**, `format` [venture|gotify|ntfy], `events`
(patterns), `secret` (sensitive), `priority` (pushes, 0-10),
`include_record`, `active`, `failure_count`; a push's token is sealed,
never a field) -- owner-only; and
`webhook_delivery` (`event`, `state` [pending|succeeded|failed],
`status_code`, `duration_ms`, `request_body`, `response_excerpt`) --
read-only evidence.

## ai, chat, automation

- `ai` has no types: the assistant, its tools and `ai.policy`
  (`read_only`, `confirm_writes` default, `autonomous`).
- chat (requires ai): `chat_thread`, `chat_message` (per-user; another
  person's thread is not found) and `ai_skill` (`trigger`, `prompt` with
  `{input}`, `enabled`; a record's trigger shadows a built-in's).
- `automation` has no types: rules live in `automations.pod` under the
  state directory, edited at `/automations` (owner saves).

## Hosted provisioning receipts

`tenant_signup` retains the trusted sign-up request identity and its owner /
organization result for replay. It is service-managed evidence; do not create
or edit receipts through generic CRUD. See `docs/lightsite-accounts.org`.

`tenant_identity` binds a verified issuer/subject to a local workspace member.
Trusted sign-up or authenticated account linking writes it; an email claim
never grants membership. Revocation follows the local member lifecycle.
