-- Late fees: the three late-fee tables come from the late_fees module's
-- field tables, and the invoice and customer override and pause columns
-- from additive metadata. Nothing historical is invented: no fee is ever
-- charged for an occurrence before late fees were switched on. Verify each
-- group is wholly present, or wholly absent when the module is disabled.
CREATE TEMP TABLE venture_late_fees_guard (
 table_count INTEGER CHECK (table_count IN (0, 3)),
 invoice_columns INTEGER CHECK (invoice_columns IN (0, 6)),
 company_columns INTEGER CHECK (company_columns IN (0, 5)));
INSERT INTO venture_late_fees_guard SELECT
 (SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = current_schema() AND table_name IN ('late_fee_settings', 'late_fee_schedules', 'late_fees')),
 (SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'invoices' AND column_name IN ('invoice_type',
  'late_fee_schedule_id', 'late_fee_exempt', 'late_fees_paused', 'late_fees_paused_until', 'late_fees_pause_reason')),
 (SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'companies' AND column_name IN (
  'late_fee_schedule_id', 'late_fee_exempt', 'late_fees_paused', 'late_fees_paused_until', 'late_fees_pause_reason'));
DROP TABLE venture_late_fees_guard;
