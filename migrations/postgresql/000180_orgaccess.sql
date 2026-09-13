-- Keep the access schema present when orgaccess is disabled during upgrade.
CREATE TABLE IF NOT EXISTS organization_memberships (
 id BIGSERIAL PRIMARY KEY, uuid TEXT NOT NULL UNIQUE,
 organization_id BIGINT, created_at TEXT, updated_at TEXT, deleted_at TEXT,
 version BIGINT, attributes TEXT, user_id BIGINT NOT NULL, role TEXT, active BOOLEAN
);
CREATE TABLE IF NOT EXISTS teams (
 id BIGSERIAL PRIMARY KEY, uuid TEXT NOT NULL UNIQUE,
 organization_id BIGINT, created_at TEXT, updated_at TEXT, deleted_at TEXT,
 version BIGINT, attributes TEXT, name TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS team_memberships (
 id BIGSERIAL PRIMARY KEY, uuid TEXT NOT NULL UNIQUE,
 organization_id BIGINT, created_at TEXT, updated_at TEXT, deleted_at TEXT,
 version BIGINT, attributes TEXT, user_id BIGINT NOT NULL, team_id BIGINT NOT NULL, active BOOLEAN
);
CREATE UNIQUE INDEX IF NOT EXISTS uq_organization_membership ON organization_memberships (user_id, organization_id);
CREATE UNIQUE INDEX IF NOT EXISTS uq_team_membership ON team_memberships (user_id, team_id);
-- Preserve the existing install owner's authority without granting editors access.
INSERT INTO organization_memberships
 (uuid, organization_id, user_id, role, active, version, created_at, updated_at)
SELECT md5(random()::text || clock_timestamp()::text)::uuid::text, o.id, u.id, 'owner', TRUE, 1, to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'), to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"')
FROM users u CROSS JOIN organizations o
WHERE u.role = 'owner' AND u.deleted_at IS NULL AND o.deleted_at IS NULL
AND NOT EXISTS (SELECT 1 FROM organization_memberships m
 WHERE m.user_id = u.id AND m.organization_id = o.id);
