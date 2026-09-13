-- Existing organizations acquire the documented quote validity default.
-- Organizations are core records, including when quotes is disabled.
UPDATE organizations SET quote_valid_days = 30 WHERE quote_valid_days IS NULL;
