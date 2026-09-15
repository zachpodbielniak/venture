-- Additive metadata creates last_output when the reporting module is enabled.
-- Do not fabricate output for historical timestamp-only runs.
CREATE TEMP TABLE venture_report_output_guard (column_count INTEGER CHECK (column_count IN (0, 1)));
INSERT INTO venture_report_output_guard SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'report_packs' AND column_name = 'last_output';
DROP TABLE venture_report_output_guard;
