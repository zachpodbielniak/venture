-- requires-table: stripe_authorizations
-- A saved card's brand, last four and expiry are read from the verified
-- provider evidence at verification, and a card change is a second Setup
-- that names the permission it replaces. Reconciliation adds the columns
-- from the field table. An earlier card's last four are already in its
-- label, written from the same evidence ("card ending 4242"); its brand
-- and expiry were never kept, so they stay unknown rather than invented.
-- A bank account's label is not a card and is left alone.
UPDATE stripe_authorizations SET method_last4 = substr(method_label, 13)
WHERE method_label LIKE 'card ending ____' AND (method_last4 IS NULL OR method_last4 = '');
