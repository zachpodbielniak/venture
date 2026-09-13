-- Billing is additive. Verify its organization-scoped identity indexes when
-- enabled; an install with no billing tables is a supported upgrade too.
-- No historical subscription or financial events are invented.
CREATE TEMP TABLE billing_schema_check (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO billing_schema_check
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'plans')
 OR EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'uq_plans_organization_code') THEN 1 ELSE 0 END;
INSERT INTO billing_schema_check
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'customer_subscriptions')
 OR EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'uq_customer_subscriptions_organization_external_id') THEN 1 ELSE 0 END;
DROP TABLE billing_schema_check;
