-- Source rounding is a separate nullable money field from additive metadata.
-- Existing documents retain NULL (no adjustment); no history is recomputed.
DO $$
DECLARE t TEXT;
BEGIN
  FOREACH t IN ARRAY ARRAY['invoices', 'vendor_bills'] LOOP
    IF EXISTS (SELECT 1 FROM information_schema.tables
        WHERE table_schema = current_schema() AND table_name = t)
      AND (SELECT COUNT(*) FROM information_schema.columns
        WHERE table_schema = current_schema() AND table_name = t
          AND column_name IN ('opening_adjustment_amount', 'opening_adjustment_currency', 'opening_adjustment_exponent')) <> 3 THEN
      RAISE EXCEPTION 'cutover opening marks require %.opening_adjustment', t;
    END IF;
  END LOOP;
END $$;
