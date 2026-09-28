-- Privacy for forms: sensitive questions, answers kept apart from the rest,
-- a privacy notice, and how long responses are kept. Every column is new
-- and empty: no question was sensitive and no form had a retention period
-- before this, so nothing is inferred. This pins the columns that keep a
-- sensitive answer out of everything but its record. An absent table is a
-- module never switched on.
CREATE TEMP TABLE venture_form_privacy_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_form_privacy_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'form_fields')
 OR (SELECT COUNT(*) FROM pragma_table_info('form_fields') WHERE name = 'sensitive') = 1 THEN 1 ELSE 0 END;
INSERT INTO venture_form_privacy_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'form_submissions')
 OR (SELECT COUNT(*) FROM pragma_table_info('form_submissions')
  WHERE name IN ('sensitive_answers', 'anonymised_at')) = 2 THEN 1 ELSE 0 END;
INSERT INTO venture_form_privacy_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'forms')
 OR (SELECT COUNT(*) FROM pragma_table_info('forms')
  WHERE name IN ('privacy_url', 'retention_days', 'retention_action')) = 3 THEN 1 ELSE 0 END;
DROP TABLE venture_form_privacy_guard;
