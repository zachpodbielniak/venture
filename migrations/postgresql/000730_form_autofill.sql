-- A question may name the autofill token the browser should use. New and
-- empty by default; a question without one behaves as before. This pins
-- the column the renderer reads. An absent table is a module never on.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'form_fields')
   AND NOT EXISTS (SELECT 1 FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'form_fields'
     AND column_name = 'autocomplete') THEN
   RAISE EXCEPTION 'Form question autofill column is missing';
 END IF;
END $$;
