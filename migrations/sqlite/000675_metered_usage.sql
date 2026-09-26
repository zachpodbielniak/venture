-- Metered prices and usage reports: plan_prices gains usage_unit,
-- unit_amount and included_units, and usage_records is created, from the
-- field tables. Existing prices keep an empty unit, which is what a flat
-- price is; no usage is invented for any subscription.
CREATE TEMP TABLE venture_metered_usage_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_metered_usage_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'plan_prices')
 OR ((SELECT COUNT(*) FROM pragma_table_info('plan_prices')
      WHERE name IN ('usage_unit', 'unit_amount_amount', 'unit_amount_currency', 'included_units')) = 4
  AND (SELECT COUNT(*) FROM pragma_table_info('usage_records')
      WHERE name IN ('subscription_id', 'quantity', 'occurred_at', 'idempotency_key')) = 4) THEN 1 ELSE 0 END;
DROP TABLE venture_metered_usage_guard;
