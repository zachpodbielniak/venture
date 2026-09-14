-- Additive cutover tables come from the module field tables.
DO $$
DECLARE n INTEGER;
BEGIN
  SELECT COUNT(*) INTO n FROM information_schema.tables
    WHERE table_schema = current_schema()
      AND table_name IN ('accounting_cutovers', 'accounting_cutover_rows');
  IF n NOT IN (0, 2) THEN
    RAISE EXCEPTION 'cutover schema must be complete or absent';
  END IF;
END $$;
