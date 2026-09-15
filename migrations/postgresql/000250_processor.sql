-- Processor payout, dispute and exception evidence is created from field tables.
-- Either the complete family exists or none, when stripe is disabled.
CREATE TEMP TABLE venture_processor_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 4)));
INSERT INTO venture_processor_upgrade_guard SELECT COUNT(*) FROM information_schema.tables
 WHERE table_schema = current_schema() AND table_type = 'BASE TABLE'
 AND table_name IN ('processor_payouts', 'processor_payout_items', 'processor_disputes', 'processor_exceptions');
DROP TABLE venture_processor_upgrade_guard;
