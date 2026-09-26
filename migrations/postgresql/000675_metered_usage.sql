-- Metered prices and usage reports: plan_prices gains usage_unit,
-- unit_amount and included_units, and usage_records is created, from the
-- field tables. Existing prices keep an empty unit, which is what a flat
-- price is; no usage is invented for any subscription.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'plan_prices')
   AND ((SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'plan_prices'
     AND column_name IN ('usage_unit', 'unit_amount_amount', 'unit_amount_currency', 'included_units')) <> 4
   OR (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'usage_records'
     AND column_name IN ('subscription_id', 'quantity', 'occurred_at', 'idempotency_key')) <> 4) THEN
   RAISE EXCEPTION 'Metered usage schema is incomplete';
 END IF;
END $$;
