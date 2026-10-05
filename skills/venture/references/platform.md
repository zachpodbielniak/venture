# Running the platform: users, access, configuration, providers, printers

Read this for users and passwords, API tokens, organization membership,
second factor and OIDC sign-in, organization AI providers, the server
binary and its configuration, the documentation site, and receipt
printers. Backups, federation, hosted workspaces and operator maintenance
are in [operations.md](operations.md). Sources: `docs/configuration.org`,
`docs/orgaccess.org`, `docs/security.org`, `docs/oidc.org`,
`docs/ai-organizations.org`, `docs/printing.org`, `docs/docs-site.org`.

## Users, roles, tokens

- On first start the server creates `owner` with a random password printed
  once in its log (`podman logs venture-dev | grep -A3 'owner account'`);
  `--owner-password` sets it on a first run. It is stored hashed and cannot
  be recovered.
- `/users` (owner only) adds, changes roles, deactivates and resets
  passwords. From the CLI a password is accepted on create (hashed) --
  `venturectl create user username=alice display_name=Alice role=editor
  password='a long enough one'` -- a new account is active unless
  `active=false`. A user's password otherwise has no CLI path: it is set on
  `/account` (current password required) or reset by the owner; setting a
  hash directly is what hashing exists to prevent. The last active owner
  cannot be demoted or deactivated. Password policy is length only
  (`security.password_min_length`, 8). Signing out ends every session of
  the account.
- Global roles `owner`, `admin`, `editor`, `viewer`, `service` (see
  [system.md](system.md)). Tokens: `/account/tokens` or `POST
  /api/v1/tokens` (`{"name":...}`) with a session; shown once; carry the
  minter's role and membership snapshot; act as `API token #N`. A token
  cannot be minted by an editor -- the demo's colleagues comment through
  `--session-file` for that reason.
- `venturectl user mfa reset USER` (an id or username; owner only, audited)
  turns off a user's second factor -- the break-glass path.

## Organization membership (module `orgaccess`)

Read `docs/orgaccess.org` for the role matrix. `organization_membership`,
`team` and `team_membership` use generic CRUD; one membership per user and
organization, deleted rows included -- reactivate the existing one.
Tokens intersect mint-time memberships with current authority; new grants
never widen an old token; legacy non-admin tokens without a snapshot need
rotation. Missing membership gives empty results or 404; a refused
in-organization write gives 403. `journal post ID` returns a confirmation
for an organization editor -- pending until finance approves. An outside
accountant: global `viewer` user plus an `organization_membership` with
role `accountant` per client organization; they read and export the books
only and land on `/books`.

## Second factor and OIDC

- `mfa` module: TOTP enrolment with a QR code, recovery codes, and the
  `mfa_policy` "require MFA for admins" organization setting; sign-in then
  passes `/login/mfa`. A script should use a token, not a session.
- `oidc` (opt-in): explicit identity linking on `/account/oidc` and
  `/organizations/ID/settings/oidc`; issuers allowlisted by the operator
  (`oidc.allowed_issuers`). Existing local passwords, roles and MFA remain
  authoritative. Do not create identity/provider records through generic
  CRUD or infer a local user from the provider's email. Credential inputs
  are write-only; a rotated or disabled provider invalidates its old
  sessions. Provider sign-in does not mint global authority, and API tokens
  keep their local authorization behaviour.

## Organization AI providers (module `ai_providers`)

Use the organization AI settings page (`/organizations/ID/settings/ai`) to
choose disabled, organization-owned or explicitly granted platform service
separately for chat, coding and embeddings. No missing or failing private
connection falls back to platform AI. Read `docs/ai-organizations.org`
before configuring provider actions; generic record writes cannot
manufacture grants or overwrite service-owned usage evidence
(`ai_usage`). Platform credentials stay operator-only even in their billing
organization (`/settings/ai/platform`; `ai_platform_offer` actions
`grant`, `disable`).

## The server binary and configuration

```bash
venture --generate-config > ~/.config/venture/config.yaml
venture -c config.yaml -d sqlite:///srv/venture.db --state-dir /srv/venture -p 8747
venture --list-modules          # resolve modules without starting
venture --migrate               # apply migrations and exit
venture --disable-module crm    # repeatable; also --no-ai, --no-plugins, --no-automation
```

- Layering, each overriding the last field by field: compiled defaults,
  `/etc/venture/config.yaml`, `$XDG_CONFIG_HOME/venture/config.yaml`,
  `--config`, a compiled `config.c` beside the last file (crispy,
  `venture_configure()`), `VENTURE_*` environment variables (the setting
  uppercased with underscores; lists comma-separated), command-line options.
- **Secrets never live in the file**: every secret setting names an
  environment variable (`security.session_secret_env`,
  `database.password_env`, ...). Without a session secret sessions end at
  every restart.
- `GET /api/v1/settings` (and `/settings`) shows every resolved setting,
  its overriding variable and whether a secret is present -- read-only,
  because a later layer could override a write.
- Refused at startup: `security.require_auth: false` on a non-loopback
  address, half-configured TLS, a port out of range, an unknown database
  scheme, a `postgres://` URI without libpq, a module whose requirement is
  off, an invalid feeds origin.
- Sections: server, database, security, locale (`default_currency`,
  timezone, fiscal start), ai (`policy`, `confirmation_ttl`,
  `confirmation_limit`), automation, plugins (`paths`, `allow_crispy`,
  `allow_exec`), ui (`look` classic|industrial, `theme`, `accent`),
  logging, modules, federation, mail, kb, series, feeds, backup, billing,
  the opt-in switches, ocr, oidc, imap/calendar/connectors allowlists,
  hosted, docs, printing.
- Ports: 8747 default/container, 8748 `just start`, 8749 `make demo`.
  Containers: `Containerfile`, `podman-compose up --build`
  (`docs/containers.org`).

## The documentation site

Every instance serves the rendered docs read-only at `/docs` (no session;
`modules.docs: false` turns it off; `docs.site_dir` says where).
`venturectl docs build [source=DIR] [output=DIR]
[renderer=auto|emacs|builtin]` renders `docs/*.org` to a static site
without a server (refusing on a broken link or an unlisted document); `make
docs-site` does the same in a build tree.

## Receipt printers

- `venturectl printers [list]` lists configured names and the default;
  state is `unknown` until explicitly queried -- listing does not probe.
  `printers status NAME` reads printer and paper status; `printers test
  NAME` prints a test page (both admin/owner).
- `venturectl print payment ID [PRINTER]` or `print invoice ID [PRINTER]`
  prints now, using the configured default when PRINTER is omitted; IDs
  must be positive integers; read access to the record is required.
- Printers are configured on the server (`printing.default`,
  `printing.printers` with `name`, `host`, port 9100). Never pass a
  host/port as a printer name; unknown names and an unconfigured default
  are refused; responses never expose a host.
- Printing is not a record write and cannot use `--stage`. A send failure
  may have delivered part of the receipt: inspect the paper before retrying
  (failed writes are never retried automatically).
