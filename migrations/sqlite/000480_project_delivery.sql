-- Metadata adds nullable sales links. Do not invent historical agreements.
CREATE TEMP TABLE venture_delivery_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_delivery_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'client_projects')
 OR (SELECT COUNT(*) FROM pragma_table_info('client_projects') WHERE name IN ('quote_id', 'deal_id', 'owner', 'delivery_status', 'delivery_note')) = 5 THEN 1 ELSE 0 END;
DROP TABLE venture_delivery_guard;
