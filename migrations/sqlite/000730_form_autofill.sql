-- A question may name the autofill token the browser should use. The
-- column is new and empty by default: a question without one takes the
-- token its kind or its lead mapping implies, which is what every question
-- did before, so there is nothing to backfill. This pins the column the
-- renderer reads. An absent table is a module never switched on.
CREATE TEMP TABLE venture_form_autofill_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_form_autofill_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'form_fields')
 OR (SELECT COUNT(*) FROM pragma_table_info('form_fields') WHERE name = 'autocomplete') = 1
 THEN 1 ELSE 0 END;
DROP TABLE venture_form_autofill_guard;
