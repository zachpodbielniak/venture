-- A quote's plan line starts a subscription: quote_lines.plan_price_id,
-- the quote's and the start instruction's links, the subscription's
-- quote_id and the action's result. All come from the field tables. No
-- historical quote is linked to a subscription: which one it led to was
-- never recorded, and guessing from customer and price would invent it.
CREATE TEMP TABLE venture_quote_subscription_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_quote_subscription_guard
SELECT CASE WHEN
 (NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'quote_lines')
  OR ((SELECT COUNT(*) FROM pragma_table_info('quote_lines') WHERE name = 'plan_price_id') = 1
   AND (SELECT COUNT(*) FROM pragma_table_info('quotes') WHERE name = 'subscription_id') = 1
   AND (SELECT COUNT(*) FROM pragma_table_info('quote_actions') WHERE name = 'result_subscription_id') = 1))
 AND (NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'customer_subscriptions')
  OR ((SELECT COUNT(*) FROM pragma_table_info('customer_subscriptions') WHERE name = 'quote_id') = 1
   AND (SELECT COUNT(*) FROM pragma_table_info('billing_requests') WHERE name = 'quote_id') = 1))
 THEN 1 ELSE 0 END;
DROP TABLE venture_quote_subscription_guard;
