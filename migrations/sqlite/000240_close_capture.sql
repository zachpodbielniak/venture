-- Close workspace tables come from field tables.
-- Both a disabled fresh module and a complete existing module are valid.
-- Never infer close signoff from a period's current state.
CREATE TEMP TABLE venture_close_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 5)));
INSERT INTO venture_close_upgrade_guard SELECT COUNT(*) FROM sqlite_master
 WHERE type = 'table' AND name IN ('close_workspaces', 'close_tasks', 'close_workpapers',
	'close_discrepancies', 'close_signoffs');
DROP TABLE venture_close_upgrade_guard;
