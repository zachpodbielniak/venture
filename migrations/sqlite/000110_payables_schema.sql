-- Payables is additive. Verify the metadata-created schema, including a
-- never-enabled module, without inventing historical approval/payment events.
CREATE TEMPORARY TABLE payables_schema_check (ok INTEGER NOT NULL CHECK (ok = 1));
INSERT INTO payables_schema_check (ok)
SELECT CASE WHEN (SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name IN ('vendor_bills', 'vendor_bill_lines', 'bill_payments', 'bill_payment_allocations', 'vendor_credits', 'vendor_bill_events', 'bill_refunds')) = 0 THEN 1
            WHEN (SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name IN ('vendor_bills', 'vendor_bill_lines', 'bill_payments', 'bill_payment_allocations', 'vendor_credits', 'vendor_bill_events', 'bill_refunds')) = 7 AND (SELECT COUNT(*) FROM pragma_table_info('vendor_bills') WHERE name IN ('organization_id', 'number', 'bill_date', 'due_date', 'currency', 'status')) = 6 THEN 1
            ELSE 0 END;
DROP TABLE payables_schema_check;
