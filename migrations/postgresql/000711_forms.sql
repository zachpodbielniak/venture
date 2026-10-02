-- Forms, their questions and their responses are new tables, made by
-- reconciliation before this runs; nothing existed to backfill. This pins
-- what the code relies on: question keys unique per form, deleted
-- questions included, and the columns a response and the public door
-- read. An absent table is a module never switched on, and passes.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'form_fields')
   AND NOT EXISTS (SELECT 1 FROM pg_indexes
     WHERE schemaname = current_schema() AND tablename = 'form_fields'
     AND indexname = 'uq_form_fields_organization_form_id_key') THEN
   RAISE EXCEPTION 'Form question keys are not unique per form';
 END IF;
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'forms')
   AND (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'forms'
     AND column_name IN ('public_token', 'ticket_key', 'state', 'allowed_origins')) <> 4 THEN
   RAISE EXCEPTION 'Form columns are incomplete';
 END IF;
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'form_submissions')
   AND (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'form_submissions'
     AND column_name IN ('form_id', 'answers', 'summary', 'submitted_at')) <> 4 THEN
   RAISE EXCEPTION 'Form response columns are incomplete';
 END IF;
END $$;
