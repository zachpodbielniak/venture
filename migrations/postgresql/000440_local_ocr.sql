-- Property metadata owns the additive job/batch tables; no attachment or
-- previously reviewed document text is changed during upgrade.
CREATE TEMP TABLE venture_ocr_upgrade_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_ocr_upgrade_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM information_schema.tables WHERE table_schema=current_schema() AND table_name='ocr_jobs')
 OR (SELECT COUNT(*) FROM information_schema.columns WHERE table_schema=current_schema() AND table_name='ocr_jobs' AND column_name IN
 ('document_id','source_hash','completed_pages','text','applied_hash','reviewed'))=6 THEN 1 ELSE 0 END;
DROP TABLE venture_ocr_upgrade_guard;
