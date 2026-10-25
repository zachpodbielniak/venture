-- requires-table: bank_accounts
-- Planning is opt-in; existing accounts retain their behavior.
UPDATE bank_accounts SET payoff_enabled = FALSE WHERE payoff_enabled IS NULL;
UPDATE bank_accounts SET payoff_apr_bps = 0 WHERE payoff_apr_bps IS NULL;
