-- requires-table: webhooks
-- Existing subscriptions keep ordinary audit notifications until opted in.
UPDATE webhooks SET business_events = FALSE WHERE business_events IS NULL;
