-- The owner's first and last name as given at a Lightsite sign-up, kept on
-- the receipt: the user keeps only the joined display name, and a linked
-- member keeps their own. Reconciliation adds the columns from the field
-- table. Receipts written before this release have no names; nothing is
-- guessed from a display name. No backfill.
SELECT first_name, last_name FROM tenant_signups WHERE 1 = 0;
