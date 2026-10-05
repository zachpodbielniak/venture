-- Form versions: what a form asked when it was published, and the version
-- each response answered. Reconciliation makes them before this runs;
-- forms and versions ship together, so nothing needs a backfill. This pins
-- the columns the door, the response page and the summary read. An absent
-- table is a module never switched on, and passes.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'form_versions')
   AND (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'form_versions'
     AND column_name IN ('form_id', 'number', 'definition', 'published_at')) <> 4 THEN
   RAISE EXCEPTION 'Form version columns are incomplete';
 END IF;
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'forms')
   AND (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'forms'
     AND column_name IN ('published_version_id', 'published_number')) <> 2 THEN
   RAISE EXCEPTION 'Form publishing columns are incomplete';
 END IF;
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'form_submissions')
   AND (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'form_submissions'
     AND column_name IN ('version_id', 'version_number')) <> 2 THEN
   RAISE EXCEPTION 'Form response version columns are incomplete';
 END IF;
END $$;
