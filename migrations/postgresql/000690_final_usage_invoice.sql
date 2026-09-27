-- requires-table: subscription_events
-- Retain the actual final invoice recorded by a completed cancellation.
-- A terminal event is distinct from the recurring invoice event so billing
-- periods and discount coverage do not count usage as another renewal.
--
-- What is inferred, and from what: a cancelled event's final usage invoice
-- is the invoice a processed cancel or renew request for the same
-- subscription produced that no subscription event already names. Only an
-- event with exactly one such invoice is filled; two candidates cannot say
-- which was the final one, so the event keeps none rather than a guess.
-- The guard skips only an absent table, which never held a row: startup
-- reconciles the tables of a disabled module too, so billing switched off
-- after use still gains final_invoice_id and is backfilled here.
-- billing_requests reached master with subscription_events, so where one
-- table exists the other does too.
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
