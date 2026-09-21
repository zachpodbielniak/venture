-- Metadata creates the optional tables. Retained wins have no historical
-- owner/value evidence to invent; the report exposes their uncaptured count.
CREATE TEMP TABLE venture_sales_upgrade_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_sales_upgrade_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'sales_credits')
 OR (SELECT COUNT(*) FROM pragma_table_info('sales_credits')
 WHERE name IN ('credit_key', 'deal_id', 'owner_user_id', 'team_id', 'value_amount', 'value_currency', 'value_exponent', 'credited_at', 'reverses_id')) = 9 THEN 1 ELSE 0 END;
DROP TABLE venture_sales_upgrade_guard;
