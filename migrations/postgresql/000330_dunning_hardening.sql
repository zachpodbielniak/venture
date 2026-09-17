-- Reminder hardening: the escalation owner and the sweep's answer on the
-- policy, the exact balance a reminder rendered on its event, and the
-- promise-to-pay pause on invoices and customers. Every column comes from
-- additive metadata; nothing historical is invented. Legacy events keyed by
-- step position keep their keys, because the sweep reads offset_days for
-- recorded-ness. Verify each group is wholly present or wholly absent, the
-- latter when its module is disabled.
CREATE TEMP TABLE venture_dunning_hardening_guard (
 policy_columns INTEGER CHECK (policy_columns IN (0, 2)),
 event_columns INTEGER CHECK (event_columns IN (0, 3)),
 invoice_columns INTEGER CHECK (invoice_columns IN (0, 2)),
 company_columns INTEGER CHECK (company_columns IN (0, 2)));
INSERT INTO venture_dunning_hardening_guard SELECT
 (SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'dunning_policies' AND column_name IN ('escalation_owner', 'last_sweep')),
 (SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'dunning_events' AND column_name IN ('rendered_balance_amount', 'rendered_balance_currency', 'rendered_balance_exponent')),
 (SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'invoices' AND column_name IN ('dunning_paused_until', 'dunning_pause_reason')),
 (SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'companies' AND column_name IN ('dunning_paused_until', 'dunning_pause_reason'));
DROP TABLE venture_dunning_hardening_guard;
