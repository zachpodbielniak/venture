-- Field-table reconciliation creates these organization-scoped indexes.
-- A disabled module legitimately has no tables; never invent enrollment data.
CREATE TEMP TABLE sequence_invariant_check (valid INTEGER CHECK (valid = 1));
INSERT INTO sequence_invariant_check
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type='table' AND name='suppressions')
 OR EXISTS (SELECT 1 FROM sqlite_master WHERE type='index' AND name='uq_suppressions_organization_email') THEN 1 ELSE 0 END;
INSERT INTO sequence_invariant_check
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type='table' AND name='sequence_deliveries')
 OR EXISTS (SELECT 1 FROM sqlite_master WHERE type='index' AND name='uq_sequence_deliveries_organization_delivery_key') THEN 1 ELSE 0 END;
INSERT INTO sequence_invariant_check
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type='table' AND name='sequence_enrollments')
 OR EXISTS (SELECT 1 FROM sqlite_master WHERE type='index' AND name='uq_sequence_enrollments_organization_active_key') THEN 1 ELSE 0 END;
DROP TABLE sequence_invariant_check;
