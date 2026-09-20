-- Metadata adds nullable sales links. Do not invent historical agreements.
CREATE TEMP TABLE venture_delivery_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_delivery_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM information_schema.tables
 WHERE table_schema = current_schema() AND table_name = 'client_projects')
 OR (SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = current_schema()
 AND table_name = 'client_projects' AND column_name IN ('quote_id', 'deal_id', 'owner', 'delivery_status', 'delivery_note')) = 5 THEN 1 ELSE 0 END;
DROP TABLE venture_delivery_guard;
