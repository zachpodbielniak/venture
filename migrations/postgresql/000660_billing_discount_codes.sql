-- A start instruction may name a discount by the code the customer quoted:
-- billing_requests.discount_code, from the field table. Nothing is
-- backfilled: every earlier start named its discount by id or took none,
-- and a code was never recorded, so none is invented. The guard refuses an
-- installation whose billing tables exist without the column.
DO $$
BEGIN
 IF EXISTS (SELECT 1 FROM information_schema.tables
     WHERE table_schema = current_schema() AND table_name = 'billing_requests')
   AND NOT EXISTS (SELECT 1 FROM information_schema.columns
     WHERE table_schema = current_schema() AND table_name = 'billing_requests'
     AND column_name = 'discount_code') THEN
   RAISE EXCEPTION 'Billing discount code schema is incomplete';
 END IF;
END $$;
