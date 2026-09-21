-- Property metadata owns the additive job/batch tables; no attachment or
-- previously reviewed document text is changed during upgrade.
CREATE TEMP TABLE venture_ocr_upgrade_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_ocr_upgrade_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type='table' AND name='ocr_jobs')
 OR (SELECT COUNT(*) FROM pragma_table_info('ocr_jobs') WHERE name IN
 ('document_id','source_hash','completed_pages','text','applied_hash','reviewed'))=6 THEN 1 ELSE 0 END;
DROP TABLE venture_ocr_upgrade_guard;
