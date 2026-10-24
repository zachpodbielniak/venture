-- requires-table: marketing_consents
-- Additive: text consent is marketing consent evidence keyed by number,
-- and a campaign may go by text (docs/telephony.org). No existing email
-- consent becomes text consent, and every existing send stays email.
SELECT phone FROM marketing_consents WHERE 1 = 0;
SELECT channel FROM marketing_sends WHERE 1 = 0;
