-- Additive metadata creates empty service-owned event and delivery tables.
-- Never manufacture outcomes from historical source records.
SELECT event, occurred_at, source_type, source_id, source_version
FROM business_events WHERE 1 = 0;
CREATE INDEX IF NOT EXISTS idx_business_events_replay
ON business_events (organization_id, id);
CREATE UNIQUE INDEX IF NOT EXISTS idx_business_event_delivery_obligation
ON business_event_deliveries (event_id, webhook_id);
CREATE INDEX IF NOT EXISTS idx_business_event_delivery_due
ON business_event_deliveries (delivered, next_attempt_at, id);
