-- Plans offer discounts: a plan_discounts table, the discount a subscription
-- was started with and how many invoices it has covered, and the start
-- instruction's discount and skip-trial choice. All come from the field
-- tables; existing subscriptions keep no discount, because none was ever
-- agreed, and nothing is invented for them.
CREATE TEMP TABLE venture_discount_upgrade_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_discount_upgrade_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'customer_subscriptions')
 OR ((SELECT COUNT(*) FROM pragma_table_info('customer_subscriptions')
      WHERE name IN ('discount_id', 'discount_periods_used')) = 2
  AND EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'plan_discounts')
  AND (SELECT COUNT(*) FROM pragma_table_info('billing_requests')
      WHERE name IN ('discount_id', 'skip_trial')) = 2) THEN 1 ELSE 0 END;
DROP TABLE venture_discount_upgrade_guard;
