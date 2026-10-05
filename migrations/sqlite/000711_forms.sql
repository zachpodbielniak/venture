-- Forms, their questions and their responses are new tables, made by
-- reconciliation from the field tables before this runs; nothing existed
-- to backfill. What this pins is what the code relies on and the schema
-- must provide: a response keyed by question keys that are unique per form
-- (deleted questions included, since their answers keep the key), and a
-- form reached by a capability token. An absent table is a module that has
-- never been on, and passes; a present one missing either fails startup
-- here rather than accepting a duplicate key later.
CREATE TEMP TABLE venture_forms_upgrade_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_forms_upgrade_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'form_fields')
 OR EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'index' AND tbl_name = 'form_fields'
  AND name = 'uq_form_fields_organization_form_id_key') THEN 1 ELSE 0 END;
INSERT INTO venture_forms_upgrade_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'forms')
 OR (SELECT COUNT(*) FROM pragma_table_info('forms')
  WHERE name IN ('public_token', 'ticket_key', 'state', 'allowed_origins')) = 4 THEN 1 ELSE 0 END;
INSERT INTO venture_forms_upgrade_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'form_submissions')
 OR (SELECT COUNT(*) FROM pragma_table_info('form_submissions')
  WHERE name IN ('form_id', 'answers', 'summary', 'submitted_at')) = 4 THEN 1 ELSE 0 END;
DROP TABLE venture_forms_upgrade_guard;
