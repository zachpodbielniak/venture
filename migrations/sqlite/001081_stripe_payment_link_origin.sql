-- requires-table: stripe_payment_links
-- A payment link has an origin: shared (Create payment link) or email (the
-- Pay now button Send puts in the invoice email). The one-enabled-link
-- uniqueness is now per invoice and origin, so the two coexist.
-- Reconciliation adds the origin column and the new index, but does not
-- drop the old one-per-invoice index, whose name it no longer generates:
-- left in place it would still refuse a send while a shared link is
-- enabled. Drop it by name. Every link from before origins is a shared
-- link, which is also what a NULL origin reads as; say so in the row.
DROP INDEX IF EXISTS uq_stripe_payment_links_organization_invoice_id_when_enabled;
UPDATE stripe_payment_links SET origin = 'shared' WHERE origin IS NULL OR origin = '';
