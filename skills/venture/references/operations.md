# Operating an install: backups, federation, hosted workspaces, maintenance

Read this for scheduled backups, verification and restore drills,
federation and offline working copies, hosted (tenant) workspace
administration, operator-only server commands, and HTTP transport limits.
Users, configuration and providers are in [platform.md](platform.md).
Sources: `docs/backup.org`, `docs/backup-retention.org`,
`docs/federation.org`, `docs/hosted-workspaces.org`,
`docs/tenant-operations.org`, `docs/tenant-recovery.org`,
`docs/integration-key-maintenance.org`, `docs/configuration.org`.

## Backups (module `backup`)

A `backup_schedule` is an ordinary record: `scope` (`organization` -- a
version 4 accounting snapshot of its own organization -- or `installation`
-- the whole database), `schedule` (`daily` or five UTC cron fields; empty
for manual), `retention` (0 = `backup.retention`, 7), `destination` (empty
= `backup.directory` under the state directory), `verify`. There is no
background thread: something must call the sweep -- a rule
(`venture->backups_run("ORG")`, 0 for all) or cron.

```bash
venturectl create backup_schedule name=Nightly scope=installation schedule=daily verify=true
venturectl backup run 1 [as_of=DATE]          # write it now
venturectl backup verify 7                    # restore into an empty database and tie it out
venturectl backup restore-drill [run_id=7] [organization_id=1] [name="Q3 drill"]
venturectl list backup_run scope=series       # the series-store copies of an installation run
```

Every attempt is a `backup_run` (`status` running/succeeded/failed/pruned,
`sha256`, `size`, `path`). A drill is a `backup_run` of kind `drill` naming
its `source_run_id`, keeping the verdict and report; drills never count
toward retention and are never pruned. An installation backup also copies
every series store as its own `backup_run` (scope `series`) unless
`series.include_in_backup` is false ([feeds.md](feeds.md)). Accounting
packs (version 4) are in [ledger-operations.md](ledger-operations.md).

**Operator backup retention** is `tools/venture-tenantctl` (not
`venturectl` or an HTTP action): `backup-list`, authenticated
`backup-enroll --archive FILE --key-file FILE`, `retention-plan --days 30`,
`retention-execute --plan UUID`, `retention-recover`, `backup-retire --copy
COPY-UUID`. Each takes the tenant slug; writes require `--reason`; use
`--root` before the command. Review the plan's exact registered copy ids
before execution. Holds block expiry; offboarding starts an additional
retention period. A pending journal requires recovery, which records
missing files and preserves survivors without another unlink. A retained
file that left its registered path (moved offsite) makes plan and execute
refuse with exit 2 naming its copy id and path; `backup-retire` tombstones
that entry as `retired` and refuses while the file is still present. Never
remove the catalog to bypass a refusal. No offsite or erasure claim follows
from local archive deletion. Read `docs/backup-retention.org`, including
the original-ledger transfer gap for restoration to a new host.

## Federation and offline working copies (module `federation`, opt-in)

Federation is opt-in. `federation_peer` and `federation_grant` are
owner-only generic records; use `describe` first. Peer keys are public
Ed25519 base64 pins verified out of band, never private keys. Federation
uses its own `federation.origin`, which may differ from the web URL.
Requests and responses are signed and bound (TLS termination alone is not
identity). The assistant cannot administer federation trust. Grants
enumerate exact record UUIDs and fields -- sharing is never inferred from an
organization, a parent or a reference; sensitive fields are excluded.

```sh
venturectl federation '{"action":"identity"}'
venturectl federation '{"action":"remote","peer_id":1,"operation":{"action":"list"}}'
venturectl federation '{"action":"pull_collection","peer_id":1,"collection":"joint_business","offset":0}'
venturectl federation '{"action":"pull","peer_id":1,"type":"venture","uuid":"UUID"}'
venturectl federation '{"action":"edit","id":1,"version":1,"fields":{"description":"Offline work"}}'
venturectl federation '{"action":"sync","id":1}'
venturectl federation '{"action":"resolve","id":1,"version":5,"field":"description","keep_local":false}'
```

Collection pulls return at most ten results, `next_offset` and `more`;
continue while `more` is true and inspect per-record errors. Pull
imports/merges without pushing; sync pushes conflict-free changes with an
expected remote version. Edits require the local replica version. A
conflict blocks that record until resolved; choosing remote can accept a
removed/revoked field. Never update `federation_replica` through generic
CRUD (refused). Copies stay usable during outages but are not
authoritative local accounting rows; new source objects and attachments are
not created offline. Federation is platform-only in hosted mode: tenant
CLI/API calls are refused and retained replicas never reconnect
automatically; explicit local operator maintenance may invoke the service,
but that lends no authority to timers. See `docs/federation.org`.

## Hosted workspaces

Hosted administration uses declared actions on control records -- inspect
parameters through the schema and never create or update these rows
directly: `tenant_workspace.set_state`, `tenant_membership.set_membership`,
`tenant_invitation.invite` (type-level), `tenant_membership.invite_recovery`
and `tenant_support_grant.revoke`. They need an interactive session
(`--session-file`, [cli.md](cli.md)). `invite_recovery` is a privately
delivered one-time recovery for an explicitly reviewed, quarantined
ordinary member: it preserves the user id and member role, and does not
activate a suspended workspace or grant platform authority. In hosted mode
record types classified as platform are refused to tenants.

**Operator maintenance is the server binary, not `venturectl`**:
`venture --tenant-admin USER --tenant-password-file FILE|-` requires
`--tenant-reason`, and an existing identity additionally `--tenant-recover`.
Use stopped-workspace `--tenant-revoke-credentials --tenant-reason REASON`
before restored authority is activated (it suspends and quarantines every
restored login capability). `--tenant-status` and `--tenant-state` expose
the lifecycle; `--tenant-support USER` (with `--tenant-support-organization`,
`--tenant-support-seconds`, `--tenant-support-write`,
`--tenant-support-private`, `--tenant-support-emergency`) issues an audited,
scoped support capability. Never pass passwords in argv. See
`docs/hosted-workspaces.org`.

**Platform workspace lifecycle** is `tools/venture-tenantctl`, a local
trusted-operator tool: it provisions isolated stopped workspaces and
supports status, stop/start, offline state changes, encrypted
export/restore, bounded maintenance upgrades and retained offboarding. It
never provisions Lightsite. Success exits 0; refusals and failures exit 2.
Read `docs/tenant-operations.org` for private password/key files,
immutable identity, maintenance locks, restore quarantine, explicit
administrator recovery, and offboarding versus erasure.

## Integration master key (offline)

Server-binary operator commands, not `venturectl` actions. Stop the
workspace, then `venture --config FILE --check-integration-key` with its
current private environment key. Rotate with `venture --config FILE
--rotate-integration-key PRIVATE_FILE` (owned, mode 600/400, single-link,
canonical base64 of 32 bytes); update the environment secret, check again,
restart. Retain old keys for old backups. A lost commit response requires
checking both candidates separately while stopped; never blindly retry. For
hosted workspaces supply `--tenant-reason`; maintenance takes its process
lease first, does not migrate or start providers, and cannot be combined
with other tenant flags. See `docs/integration-key-maintenance.org`.

## HTTP transport limits

Every route, generic record writes included, gets the same early limits.
413: body over `server.max_request_size_mb` (32). 503 with Retry-After: the
aggregate receive budget (`server.max_buffered_request_mb`, 64) is full.
408: a parsed but incomplete request past `server.request_timeout` (60 s;
it bounds reception and idle keep-alive, not business execution). An
incomplete TLS/header or a saturated listener
(`server.max_connections`, 128) can close without an HTTP response.
Rejected partial bodies never reach a handler. Do not blindly retry a write
whose response was lost after dispatch: read its retained identity first.
Changes need a restart; HTTP/2 must terminate at the gateway.
