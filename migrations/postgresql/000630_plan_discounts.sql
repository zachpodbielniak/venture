-- Plans offer discounts: a plan_discounts table, the discount a subscription
-- was started with and how many invoices it has covered, and the start
-- instruction's discount and skip-trial choice. Existing subscriptions keep
-- no discount: none was ever agreed, and nothing is invented for them.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'customer_subscriptions')
   AND ((SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'customer_subscriptions'
     AND column_name IN ('discount_id', 'discount_periods_used')) <> 2
   OR NOT EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'plan_discounts')
   OR (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'billing_requests'
     AND column_name IN ('discount_id', 'skip_trial')) <> 2) THEN
   RAISE EXCEPTION 'Plan discount schema is incomplete';
 END IF;
END $$;
