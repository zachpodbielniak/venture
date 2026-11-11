-- requires-table: referral_programs
-- The program states the refund window in days. Reconciliation adds
-- the column from the field table. 0 revokes nothing. No backfill.
SELECT refund_window_days FROM referral_programs WHERE 1 = 0;
