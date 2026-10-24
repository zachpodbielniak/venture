-- Invoice lines may name their income and discount accounts, and an expense
-- its expense account, from additive field metadata. Each table is checked
-- on its own because receivables and money can be disabled separately: an
-- absent table is accepted, a present one must carry the columns. Existing
-- rows keep NULL, which posts exactly as before.
DO $$
DECLARE pair TEXT[];
BEGIN
  FOREACH pair SLICE 1 IN ARRAY ARRAY[['invoice_lines', 'income_account_id'],
      ['invoice_lines', 'discount_account_id'], ['expenses', 'expense_account_id']] LOOP
    IF EXISTS (SELECT 1 FROM information_schema.tables
        WHERE table_schema = current_schema() AND table_name = pair[1])
      AND NOT EXISTS (SELECT 1 FROM information_schema.columns
        WHERE table_schema = current_schema() AND table_name = pair[1] AND column_name = pair[2]) THEN
      RAISE EXCEPTION 'line and expense accounts require %.%', pair[1], pair[2];
    END IF;
  END LOOP;
END $$;
