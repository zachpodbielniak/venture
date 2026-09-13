-- Preserve historical refunds without inventing dates or journal events.
-- Property reconciliation supplies the additive columns before this check.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = 'sales')
 AND NOT EXISTS (SELECT 1 FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'sales' AND column_name = 'refunded_at') THEN
  RAISE EXCEPTION 'autojournal requires sales.refunded_at';
 END IF;
 IF EXISTS (SELECT 1 FROM information_schema.tables WHERE table_schema = current_schema() AND table_name = 'posting_profiles')
 AND (SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = current_schema() AND table_name = 'posting_profiles'
 AND column_name IN ('profile_key', 'refunds_account_id', 'expense_categories')) <> 3 THEN
  RAISE EXCEPTION 'autojournal requires profile configuration columns';
 END IF;
END $$;
