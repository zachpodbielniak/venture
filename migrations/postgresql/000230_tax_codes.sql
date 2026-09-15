-- Tax codes and dated exchange rates come from field tables.
DO $$
DECLARE n INTEGER;
BEGIN
  SELECT COUNT(*) INTO n FROM information_schema.tables
    WHERE table_schema = current_schema()
      AND table_name = 'tax_codes';
  IF n NOT IN (0, 1) THEN
    RAISE EXCEPTION 'tax_codes schema must be complete or absent';
  END IF;
  SELECT COUNT(*) INTO n FROM information_schema.tables
    WHERE table_schema = current_schema()
      AND table_name = 'exchange_rates';
  IF n NOT IN (0, 1) THEN
    RAISE EXCEPTION 'exchange_rates schema must be complete or absent';
  END IF;
END $$;
