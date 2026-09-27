-- A plan price can carry a tax rate: plan_prices.tax_code_id, from the field
-- table. Existing prices keep none, so their invoices go on being taxed by
-- the customer's address exactly as before; no rate is invented for them.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'plan_prices')
   AND NOT EXISTS (SELECT 1 FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'plan_prices'
     AND column_name = 'tax_code_id') THEN
   RAISE EXCEPTION 'Plan price tax schema is incomplete';
 END IF;
END $$;
