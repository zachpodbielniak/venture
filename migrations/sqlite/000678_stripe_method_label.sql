-- requires-table: stripe_authorizations
-- Which saved method a permission charges is read from the verified
-- provider evidence at verification. Earlier permissions keep an empty
-- label -- the evidence retained for them holds no card digits -- and the
-- page says "the saved payment method" rather than inventing some.
UPDATE stripe_authorizations SET method_label = NULL WHERE method_label = '';
