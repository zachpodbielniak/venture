-- One headline_setting row per organisation. The save validator refuses a
-- second living row, but a restore skips validators, so the database holds
-- the rule too. The table is created here when the headline module is off,
-- so the index exists before the module is first switched on.
CREATE TABLE IF NOT EXISTS headline_settings (
 id INTEGER PRIMARY KEY AUTOINCREMENT, uuid TEXT NOT NULL UNIQUE,
 organization_id INTEGER, created_at TEXT, updated_at TEXT, deleted_at TEXT,
 version INTEGER, attributes TEXT, classic_home INTEGER,
 hourly_rate_amount INTEGER, hourly_rate_currency TEXT, hourly_rate_exponent INTEGER,
 minimum_customer_months INTEGER, activity_days INTEGER
);
-- Every reader has always used the oldest living row (lowest id), so retiring
-- the later duplicates changes no figure and no home page. They remain as
-- soft-deleted evidence rather than being purged.
UPDATE headline_settings SET deleted_at = strftime('%Y-%m-%dT%H:%M:%SZ', 'now')
 WHERE deleted_at IS NULL AND EXISTS (SELECT 1 FROM headline_settings AS older
 WHERE older.organization_id = headline_settings.organization_id
 AND older.deleted_at IS NULL AND older.id < headline_settings.id);
CREATE UNIQUE INDEX IF NOT EXISTS uq_headline_settings_organization
 ON headline_settings (organization_id) WHERE deleted_at IS NULL;
