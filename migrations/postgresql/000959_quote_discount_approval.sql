-- requires-table: quotes
-- Metadata adds disabled-by-default policy and immutable authors without inventing history.
SELECT author_user_id FROM quotes WHERE 1 = 0;
SELECT quote_discount_approval, quote_max_line_discount_percent, quote_discount_approver FROM organizations WHERE 1 = 0;
