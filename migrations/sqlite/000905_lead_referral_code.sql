-- requires-table: leads
-- Additive: the code a referrer shared, kept on the lead as it arrived.
-- Existing leads were captured before codes existed and stay empty.
SELECT referral_code FROM leads WHERE 1 = 0;
