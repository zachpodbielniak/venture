-- Additive cutover tables come from the module field tables. Verify the
-- pair is present, or absent when the module is disabled.
CREATE TEMP TABLE cutover_upgrade_check (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO cutover_upgrade_check(valid)
SELECT CASE WHEN COUNT(*) IN (0, 2) THEN 1 ELSE 0 END
FROM sqlite_master WHERE type = 'table' AND name IN
('accounting_cutovers', 'accounting_cutover_rows');
DROP TABLE cutover_upgrade_check;
