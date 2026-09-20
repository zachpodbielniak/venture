-- Metadata adds connection_id and scoped uniqueness before this ledger step.
-- Legacy rows remain unbound; never infer a historical account from current keys.
DO $$
DECLARE table_name_value TEXT;
BEGIN
 FOREACH table_name_value IN ARRAY ARRAY['stripe_price_links', 'stripe_customer_links', 'stripe_checkouts', 'stripe_events', 'processor_payouts', 'processor_disputes'] LOOP
  IF EXISTS (SELECT 1 FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = table_name_value) AND NOT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = table_name_value AND column_name = 'connection_id') THEN
   RAISE EXCEPTION 'Stripe account binding metadata is incomplete';
  END IF;
 END LOOP;
END $$;
