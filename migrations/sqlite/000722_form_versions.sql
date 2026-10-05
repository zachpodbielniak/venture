-- Form versions: what a form asked when it was published, frozen, and the
-- version each response answered. Reconciliation makes the table and the
-- columns before this runs. Forms and versions ship in the same release,
-- so no released install holds a response without a version to backfill;
-- the code reads one without a version against the first. This pins the
-- columns the door, the response page and the summary read. An absent
-- table is a module never switched on, and passes.
CREATE TEMP TABLE venture_form_versions_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_form_versions_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'form_versions')
 OR (SELECT COUNT(*) FROM pragma_table_info('form_versions')
  WHERE name IN ('form_id', 'number', 'definition', 'published_at')) = 4 THEN 1 ELSE 0 END;
INSERT INTO venture_form_versions_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'forms')
 OR (SELECT COUNT(*) FROM pragma_table_info('forms')
  WHERE name IN ('published_version_id', 'published_number')) = 2 THEN 1 ELSE 0 END;
INSERT INTO venture_form_versions_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'form_submissions')
 OR (SELECT COUNT(*) FROM pragma_table_info('form_submissions')
  WHERE name IN ('version_id', 'version_number')) = 2 THEN 1 ELSE 0 END;
DROP TABLE venture_form_versions_guard;
