-- requires-table: referral_rewards
-- Additive referral tracking. A reward is once per referral: the
-- organization-scoped unique index on referral_id is what guarantees it.
SELECT name, active, reward_kind, reward_amount_amount, reward_amount_currency, landing_url FROM referral_programs WHERE 1 = 0;
SELECT code, program_id, company_id, contact_id, link, retired FROM referral_codes WHERE 1 = 0;
SELECT status, referrer_company_id, referrer_contact_id, lead_id, company_id, contact_id, code_id, program_id, won_at FROM referrals WHERE 1 = 0;
SELECT status, kind, amount_amount, company_id, referred_company_id, referral_id, credit_id, subscription_id, applied_at, failure FROM referral_rewards WHERE 1 = 0;
