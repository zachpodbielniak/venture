-- An organization may choose where its profit and loss comes from:
-- pnl_basis 'ledger' reads the posted books, empty keeps the sale and
-- expense records. Additive field metadata; existing rows keep NULL, so
-- every report reads exactly as before.
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM information_schema.columns
      WHERE table_schema = current_schema() AND table_name = 'organizations' AND column_name = 'pnl_basis') THEN
    RAISE EXCEPTION 'profit and loss basis requires organizations.pnl_basis';
  END IF;
END $$;
