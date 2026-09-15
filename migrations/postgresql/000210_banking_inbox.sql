-- Categorization rules and transfers are additive field-table types.
-- Banking remains complete (all evidence tables) or absent when disabled.
DO $$
DECLARE n INTEGER;
BEGIN
  SELECT COUNT(*) INTO n FROM information_schema.tables
  WHERE table_schema = current_schema() AND table_name IN
  ('bank_accounts', 'bank_statements', 'bank_transactions', 'bank_matches', 'reconciliations', 'bank_rules', 'bank_transfers');
  IF n NOT IN (0, 7) THEN
    RAISE EXCEPTION 'banking evidence family is incomplete';
  END IF;
END $$;
