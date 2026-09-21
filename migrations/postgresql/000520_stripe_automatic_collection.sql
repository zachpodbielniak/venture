-- requires-table: stripe_checkouts
-- Historical hosted sessions carry no reusable collection permission.
UPDATE stripe_checkouts SET channel = 'hosted' WHERE channel IS NULL OR channel = '';
