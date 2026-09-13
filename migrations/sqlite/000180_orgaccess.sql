-- Keep the access schema present when orgaccess is disabled during upgrade.
CREATE TABLE IF NOT EXISTS organization_memberships (
 id INTEGER PRIMARY KEY AUTOINCREMENT, uuid TEXT NOT NULL UNIQUE,
 organization_id INTEGER, created_at TEXT, updated_at TEXT, deleted_at TEXT,
 version INTEGER, attributes TEXT, user_id INTEGER NOT NULL, role TEXT, active INTEGER
);
CREATE TABLE IF NOT EXISTS teams (
 id INTEGER PRIMARY KEY AUTOINCREMENT, uuid TEXT NOT NULL UNIQUE,
 organization_id INTEGER, created_at TEXT, updated_at TEXT, deleted_at TEXT,
 version INTEGER, attributes TEXT, name TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS team_memberships (
 id INTEGER PRIMARY KEY AUTOINCREMENT, uuid TEXT NOT NULL UNIQUE,
 organization_id INTEGER, created_at TEXT, updated_at TEXT, deleted_at TEXT,
 version INTEGER, attributes TEXT, user_id INTEGER NOT NULL, team_id INTEGER NOT NULL, active INTEGER
);
CREATE UNIQUE INDEX IF NOT EXISTS uq_organization_membership ON organization_memberships (user_id, organization_id);
CREATE UNIQUE INDEX IF NOT EXISTS uq_team_membership ON team_memberships (user_id, team_id);
-- Preserve the existing install owner's authority without granting editors access.
INSERT INTO organization_memberships
 (uuid, organization_id, user_id, role, active, version, created_at, updated_at)
SELECT lower(hex(randomblob(4))) || '-' || lower(hex(randomblob(2))) || '-4' || substr(lower(hex(randomblob(2))), 2) || '-8' || substr(lower(hex(randomblob(2))), 2) || '-' || lower(hex(randomblob(6))), o.id, u.id, 'owner', 1, 1, strftime('%Y-%m-%dT%H:%M:%SZ', 'now'), strftime('%Y-%m-%dT%H:%M:%SZ', 'now')
FROM users u CROSS JOIN organizations o
WHERE u.role = 'owner' AND u.deleted_at IS NULL AND o.deleted_at IS NULL
AND NOT EXISTS (SELECT 1 FROM organization_memberships m
 WHERE m.user_id = u.id AND m.organization_id = o.id);
