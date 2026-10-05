# The software factory, forges, coding runs and the harness

Read this for milestones, releases, builds, environments, deployments and
incidents; for connecting a Forgejo/Gitea forge and letting an agent work
tickets; for budgets and mission control; and for the agent harness.
Sources: `docs/factory.org`, `docs/forge.org`, `docs/harness.org`,
`docs/workdesk.org` (runs, budgets).

## The loop as records (module `factory`)

A ticket is planned into a `milestone`, built by CI (every workflow run
arrives through the forge webhook as a `build`), shipped in a `release`
(changelog drafted from its tickets, published to the forge), deployed to
an `environment` (`deployment`), and -- when something breaks -- raised as
an `incident` pointing at the release and deployment and forward at the
ticket that fixes it. Dates follow statuses on every writer: an incident's
started/resolved, a deployment's deployed, a milestone's completed, a
release's released, a build's started/finished are stamped when the status
moves and only when empty -- a status moved without its date is a record no
report sees, so never stamp them by hand.

```bash
venturectl factory                     # milestones, releases, builds, environments, open incidents
venturectl factory actions             # what needs somebody, most pressing first -- start here for "what next"
venturectl factory briefing            # the same as a few paragraphs (AI)
venturectl release readiness 4         # checks that pass/warn/fail, ready, score -- advice, never a gate
venturectl release changelog 4 [--replace]   # draft from tickets marked fixed in it
venturectl release deploy 4 2 "Rolled out"   # record it live in environment 2 (records, does not deploy)
venturectl release notes 4 [AUDIENCE]  # user-facing notes (AI); nothing written
venturectl release publish 4 [--prerelease]  # cut the tag on the forge -- cannot be undone here
venturectl environment 2 rollback "Bad migration"
venturectl milestone 3 forecast        # on_track, at_risk, overdue, stalled... at the last 28 days' pace
venturectl build 9 ticket ; venturectl build 9 triage
venturectl incident 2 ticket ; venturectl incident 2 postmortem
```

- "What needs you", readiness and forecasts are arithmetic over the records,
  the same every time; the AI only narrates. The AI drafts (notes,
  postmortem, build triage, briefing) write nothing -- a draft reaches a
  record only as a person's own save.
- **Publishing cannot be staged** (it creates a tag on another system); MCP
  offers it only with `--apply-writes`. A changelog draft is an ordinary
  update and stages; so does recording a deployment. Rollback (two writes)
  and a build's ticket (a ticket and a link) cannot be held by the queue.
- `release deploy` records a succeeded deployment naming the release's
  latest green build; a yanked release is refused. `rollback` marks the
  current deployment `rolled_back` and records the release before it
  (`supersedes` link); refused when there is nothing to go back to. A
  rollback counts as a change failure in `report delivery`.
- "Open" incidents are `open` or `mitigated`; `postmortem` means fixed and
  being written up. Cancelled tickets did not ship (no changelog, no lead
  time). A finished build stays finished unless a re-run starts later.
- Forgejo sends `action_run_success`/`action_run_failure` (Gitea sends
  `workflow_run`); both become builds. A forge release is matched by id,
  then tag, then version; one published there becomes a release record
  (deleted there: yanked here).
- Reports: `releases`, `lead_time`, `incidents`, `delivery` (DORA's four
  keys beside what the coding runs did and cost).

## Wiring up a forge (module `forge`)

```bash
# 1. The server, and where git lives -- often a different host
venturectl create forge name="Home" kind=forgejo organization_id=1 \
    base_url=https://git.example.com clone_base_url=git@git-ssh.example.com active=true
# 2. Both credentials in an encrypted organization binding (JSON on stdin, never argv)
FORGE=$(venturectl -f json list forge name__eq=Home | jq -r '.records[0].id')
venturectl forge settings "$FORGE" < protected-forge-settings.json
# 3. A repository
venturectl create forge_repo name=owner/project forge_id="$FORGE" \
    default_branch=main branch_prefix=venture/ push_issues=true accept_issues=true active=true
# 4. A rule: bugs anywhere on this forge get an agent and a draft PR
venturectl create forge_rule name="Bugs" forge_id="$FORGE" issue_type=bug enabled=true \
    runner=agent outcome=draft_pr trigger=on_create max_runs_per_day=10
# 5. Check what a ticket would resolve to
venturectl list forge_rule forge_id__eq="$FORGE"
```

**Forge settings** are one JSON operation on stdin. Create:
`{"operation":"configure","connection_id":0,"version":0,"settings":{"token":"SUPPLY_PRIVATELY","webhook_secret":"SUPPLY_32_OR_MORE_RANDOM_BYTES_PRIVATELY"}}`.
The response contains only connection metadata. Rotation repeats
`configure` with both credentials and the exact current `connection_id`
and `version`; `test` and `disconnect` take that identity without
settings; `import` with both identity numbers zero verifies, encrypts and
transactionally clears legacy plaintext (old backups may still hold it; a
failed import changes nothing). The forge must name an explicit
organization; account/origin changes require disconnect first. Settings
remain an owner/admin capability -- organization membership does not
authorize arbitrary forge origins or host execution. `forge set-token` and
`forge set-secret` are retired and refuse; `forge verify ID` records which
account the token belongs to (configure already verifies). Install the
separately generated webhook secret on the forge (`/hooks/forge/:id`,
HMAC-checked; a forge with no secret is refused).

- **Rules resolve most-specific-first**: repository+type, repository
  catch-all, forge+type, forge catch-all, then nothing. **Nothing is a
  legitimate answer** -- a repository nobody enrolled does not get an AI
  because a rule elsewhere was written generously. A disabled rule falls
  through to a broader one. To suppress work for one repository, give it an
  *enabled* rule that does nothing -- `trigger=manual outcome=none` (add
  `require_approval=true` to be sure): the search stops there. (There is no
  `ai_enabled` field any more; older notes that say "a rule with the AI
  switched off" mean this.) `forge_rule` is admin-only; `forge` is
  owner-only.
- **Coding runs are off until `forge.runs_enabled: true`.** Everything else
  (linking, issues both ways, builds) works without it. A CLI runner needs
  its command in `forge.cli_allowed_commands`.
- Tickets gain `repo_id`; `ticket_link` carries the issue, branch and pull
  request. Web routes: `/tickets/:id/link|branch|work`, `/tickets/:id/runs`,
  `/runs/:id/cancel`.

## Runs, budgets, mission control

```bash
venturectl runs [--state running]      # every coding run: state, model, tokens, cost; totals
venturectl budgets                     # agent budgets and their spend this window
venturectl -f json list forge_run state__eq=failed | jq -r '.records[].failure_reason'
```

`forge_run` is read-only evidence. An `agent_budget` (`limit` per
`period`, `warn_percent`, `hard_stop`, optionally per repository) is asked
before a run starts; the run's own `cost` is summed, nothing stores a
running total, and a warning is sent once per window. A run that says
`interrupted` was cut off by a restart and is never resumed automatically.

Troubleshooting: "rejected this install's access token" -- rotate both
credentials; "not allowed to ..." -- token scope (`write:issue`,
`write:repository`); a webhook that does nothing -- secret mismatch, the
repository lacks *Accept issues*, or the name is not exactly `owner/repo`;
a 401 delivery -- the active binding is missing, revoked or signed
differently; tickets not on the board -- the repository has no venture, so
they landed in the default organization; a run failing at once with a git
error -- git missing or the clone base unreachable (cloning from the API
host is the usual mistake); an SSH clone hanging -- the server needs a key
and `known_hosts`.

## The agent harness (`/harness`)

A coding agent kept open in a workspace, driven a turn at a time by a
person -- distinct from the assistant panel (`/assistant` lists the chat
harness's commands). Each session uses its organization's explicit
`coding` AI binding ([platform.md](platform.md)); no global fallback. It
works in a directory under `forge.workspace_roots` (empty by default --
requires `forge.runs_enabled` and the root configured), in a checkout of a
`forge_repo` it clones under the state directory, or with no workspace.
Sessions (`agent_session`, `agent_turn`) are rebuilt from records every
turn, so they survive a restart.
