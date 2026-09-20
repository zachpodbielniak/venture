-- Reconcile metadata without fabricating ownership of prior wins.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'sales_credits')
   AND (SELECT COUNT(*) FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'sales_credits'
     AND column_name IN ('credit_key', 'deal_id', 'owner_user_id', 'team_id', 'value_amount', 'value_currency', 'value_exponent', 'credited_at', 'reverses_id')) <> 9 THEN
   RAISE EXCEPTION 'Sales credit metadata is incomplete';
 END IF;
END $$;
