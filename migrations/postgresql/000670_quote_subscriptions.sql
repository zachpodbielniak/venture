-- A quote's plan line starts a subscription: quote_lines.plan_price_id,
-- the quote's and the start instruction's links, the subscription's
-- quote_id and the action's result. No historical quote is linked to a
-- subscription: which one it led to was never recorded.
DO $$
BEGIN
 IF (EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'quote_lines')
   AND (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND ((table_name = 'quote_lines' AND column_name = 'plan_price_id')
       OR (table_name = 'quotes' AND column_name = 'subscription_id')
       OR (table_name = 'quote_actions' AND column_name = 'result_subscription_id'))) <> 3)
  OR (EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'customer_subscriptions')
   AND (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND ((table_name = 'customer_subscriptions' AND column_name = 'quote_id')
       OR (table_name = 'billing_requests' AND column_name = 'quote_id'))) <> 2) THEN
   RAISE EXCEPTION 'Quote subscription schema is incomplete';
 END IF;
END $$;
