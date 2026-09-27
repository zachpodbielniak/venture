-- Metadata adds who wrote in and what they wrote to each form submission.
-- Earlier submissions keep them empty: there is nothing trustworthy to copy.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'attribution_submissions')
   AND (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'attribution_submissions'
     AND column_name IN ('sender_name', 'sender_email', 'message')) <> 3 THEN
   RAISE EXCEPTION 'Form submission metadata is incomplete';
 END IF;
END $$;
