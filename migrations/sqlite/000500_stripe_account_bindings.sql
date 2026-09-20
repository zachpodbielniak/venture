-- Metadata adds connection_id and scoped uniqueness before this ledger step.
-- Legacy rows remain unbound; never infer a historical account from current keys.
CREATE TEMP TABLE venture_stripe_binding_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_stripe_binding_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'stripe_price_links') OR EXISTS (SELECT 1 FROM pragma_table_info('stripe_price_links') WHERE name = 'connection_id') THEN 1 ELSE 0 END;
INSERT INTO venture_stripe_binding_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'stripe_customer_links') OR EXISTS (SELECT 1 FROM pragma_table_info('stripe_customer_links') WHERE name = 'connection_id') THEN 1 ELSE 0 END;
INSERT INTO venture_stripe_binding_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'stripe_checkouts') OR EXISTS (SELECT 1 FROM pragma_table_info('stripe_checkouts') WHERE name = 'connection_id') THEN 1 ELSE 0 END;
INSERT INTO venture_stripe_binding_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'stripe_events') OR EXISTS (SELECT 1 FROM pragma_table_info('stripe_events') WHERE name = 'connection_id') THEN 1 ELSE 0 END;
INSERT INTO venture_stripe_binding_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'processor_payouts') OR EXISTS (SELECT 1 FROM pragma_table_info('processor_payouts') WHERE name = 'connection_id') THEN 1 ELSE 0 END;
INSERT INTO venture_stripe_binding_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'processor_disputes') OR EXISTS (SELECT 1 FROM pragma_table_info('processor_disputes') WHERE name = 'connection_id') THEN 1 ELSE 0 END;
DROP TABLE venture_stripe_binding_guard;
