-- requires-table: events
-- Additive: an event's reminder lead time, the occurrence the reminder
-- sweep last told people about, and its budget. Existing events had no
-- reminder and no budget and stay that way; nothing is inferred.
SELECT remind_minutes, reminded_for, budget_amount, budget_currency, budget_exponent FROM events WHERE 1 = 0;
