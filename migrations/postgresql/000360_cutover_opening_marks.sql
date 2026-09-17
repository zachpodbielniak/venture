-- Migrated invoices, bills and credits carry opening_at from additive field
-- metadata. Each table is verified independently because receivables and
-- payables can be disabled on their own: an absent table is accepted, a
-- present one must have the column. Existing rows keep NULL, meaning "issued
-- in VENTURE"; no historical document is reclassified as an opening balance.
DO $$
DECLARE t TEXT;
BEGIN
  FOREACH t IN ARRAY ARRAY['invoices', 'vendor_bills', 'customer_credits', 'vendor_credits'] LOOP
    IF EXISTS (SELECT 1 FROM information_schema.tables
        WHERE table_schema = current_schema() AND table_name = t)
      AND NOT EXISTS (SELECT 1 FROM information_schema.columns
        WHERE table_schema = current_schema() AND table_name = t AND column_name = 'opening_at') THEN
      RAISE EXCEPTION 'cutover opening marks require %.opening_at', t;
    END IF;
  END LOOP;
END $$;
