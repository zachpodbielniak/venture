-- A start instruction may name a discount by the code the customer quoted:
-- billing_requests.discount_code, from the field table. Nothing is
-- backfilled: every earlier start named its discount by id or took none,
-- and a code was never recorded, so none is invented. The guard refuses an
-- installation whose billing tables exist without the column.
CREATE TEMP TABLE venture_discount_code_upgrade_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_discount_code_upgrade_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'billing_requests')
 OR (SELECT COUNT(*) FROM pragma_table_info('billing_requests') WHERE name = 'discount_code') = 1
 THEN 1 ELSE 0 END;
DROP TABLE venture_discount_code_upgrade_guard;
