-- A quote line may be taxed by a rate record, as an invoice line is: the
-- tax_code_id column comes from the field table. Existing lines keep their
-- whole tax percent and no rate record, because none was ever chosen for
-- them; nothing is inferred from the percent. This only checks the column
-- is there wherever quotes are.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'quote_lines')
   AND NOT EXISTS (SELECT 1 FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'quote_lines'
     AND column_name = 'tax_code_id') THEN
   RAISE EXCEPTION 'Quote line tax code column is missing';
 END IF;
END $$;
