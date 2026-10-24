-- requires-table: marketing_recipients
-- Additive: deliverability evidence on a campaign recipient (delivered,
-- replied, complained, first machine-judged open) and on an observation
-- (why it was judged a machine's). Existing rows have none of it: nothing
-- is inferred for mail sent before the provider feed, the DSN reader or
-- the reply count existed, and earlier opens keep counting as opens. The
-- sending domain table is new and starts empty, so every organization
-- starts on the default warm-up measured from its first accepted campaign
-- message.
SELECT delivered_at, replied_at, complained_at, machine_opened_at FROM marketing_recipients WHERE 1 = 0;
