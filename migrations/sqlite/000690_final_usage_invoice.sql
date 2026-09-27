-- requires-table: subscription_events
-- Retain the actual final invoice recorded by a completed cancellation.
-- A terminal event is distinct from the recurring invoice event so billing
-- periods and discount coverage do not count usage as another renewal.
UPDATE subscription_events SET final_invoice_id = (
 SELECT MIN(r.invoice_id) FROM billing_requests r
 WHERE r.subscription_id = subscription_events.subscription_id
   AND r.action IN ('cancel', 'renew') AND r.processed = 1 AND r.invoice_id > 0
   AND NOT EXISTS (SELECT 1 FROM subscription_events e WHERE e.invoice_id = r.invoice_id)
)
WHERE kind = 'cancelled' AND to_status = 'cancelled' AND COALESCE(final_invoice_id, 0) = 0
 AND (SELECT COUNT(*) FROM billing_requests r
      WHERE r.subscription_id = subscription_events.subscription_id
        AND r.action IN ('cancel', 'renew') AND r.processed = 1 AND r.invoice_id > 0
        AND NOT EXISTS (SELECT 1 FROM subscription_events e WHERE e.invoice_id = r.invoice_id)) = 1;
