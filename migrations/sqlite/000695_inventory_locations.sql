-- requires-table: inventory_items
-- Give every distinct free-text inventory location a location record, and
-- point each inventory item at it.
--
-- What is inferred, and from what: one top-level location per distinct
-- (organization, trimmed location text) among inventory items, deleted
-- items included so a restored item finds its place. Nothing else about a
-- location can be read from a string, so each is a top-level, active place
-- with no kind. The text column is left exactly as it was.
--
-- The guard skips only an absent inventory_items table, which never held a
-- row to take a location from. Sales switched off after use still has that
-- table (startup reconciles a disabled module's existing tables) but no
-- locations table -- reconciliation never creates a hidden type's table --
-- so it is created here with the columns its field table declares, and the
-- backfill runs now rather than never.
CREATE TABLE IF NOT EXISTS locations (
 id INTEGER PRIMARY KEY AUTOINCREMENT, uuid TEXT NOT NULL UNIQUE,
 organization_id INTEGER, created_at TEXT, updated_at TEXT, deleted_at TEXT,
 version INTEGER, attributes TEXT, name TEXT NOT NULL, parent_id INTEGER,
 kind TEXT, description TEXT, active INTEGER
);
INSERT INTO locations
 (uuid, organization_id, created_at, updated_at, version, name, parent_id, active)
SELECT lower(hex(randomblob(4))) || '-' || lower(hex(randomblob(2))) || '-4' || substr(lower(hex(randomblob(2))), 2) || '-8' || substr(lower(hex(randomblob(2))), 2) || '-' || lower(hex(randomblob(6))),
 s.organization_id, strftime('%Y-%m-%dT%H:%M:%SZ', 'now'), strftime('%Y-%m-%dT%H:%M:%SZ', 'now'), 1, s.name, 0, 1
FROM (SELECT DISTINCT organization_id, TRIM(location) AS name FROM inventory_items
      WHERE location IS NOT NULL AND TRIM(location) <> '') s
WHERE NOT EXISTS (SELECT 1 FROM locations l
 WHERE COALESCE(l.organization_id, 0) = COALESCE(s.organization_id, 0)
   AND l.name = s.name AND COALESCE(l.parent_id, 0) = 0 AND l.deleted_at IS NULL);
UPDATE inventory_items SET location_id = (
 SELECT MIN(l.id) FROM locations l
 WHERE COALESCE(l.organization_id, 0) = COALESCE(inventory_items.organization_id, 0)
   AND l.name = TRIM(inventory_items.location) AND COALESCE(l.parent_id, 0) = 0
   AND l.deleted_at IS NULL
)
WHERE COALESCE(location_id, 0) = 0 AND location IS NOT NULL AND TRIM(location) <> '';
