-- Privacy for forms: sensitive questions, answers kept apart, a privacy
-- notice and retention. New, empty columns; nothing is inferred. This pins
-- them. An absent table is a module never switched on.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'form_fields')
   AND NOT EXISTS (SELECT 1 FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'form_fields' AND column_name = 'sensitive') THEN
   RAISE EXCEPTION 'Form question sensitivity column is missing';
 END IF;
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'form_submissions')
   AND (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'form_submissions'
     AND column_name IN ('sensitive_answers', 'anonymised_at')) <> 2 THEN
   RAISE EXCEPTION 'Form response privacy columns are incomplete';
 END IF;
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'forms')
   AND (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'forms'
     AND column_name IN ('privacy_url', 'retention_days', 'retention_action')) <> 3 THEN
   RAISE EXCEPTION 'Form privacy columns are incomplete';
 END IF;
END $$;
