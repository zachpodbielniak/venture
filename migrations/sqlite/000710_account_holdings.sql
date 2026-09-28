-- An account with a location is a holding, and a holding may not go below
-- zero unless allow_negative says so. Every account written before the
-- column existed carries no location, so the floor never judges it; say
-- "not allowed" in the row rather than leaving it NULL, so the column holds
-- exactly the two values the ledger reads. No balance or journal changes.
UPDATE accounts SET allow_negative = 0 WHERE allow_negative IS NULL;
