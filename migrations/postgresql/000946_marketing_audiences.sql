-- requires-table: marketing_recipients
-- Additive: audiences (docs/marketing.org). A send's topic, A/B test,
-- local-time and resend settings, a recipient's arm, second arm and due
-- moment, and the time zone on contacts and organizations. Existing sends
-- have no test, no local time and no topic, so they behave exactly as
-- before; existing rows get no arm. Topics and preferences start empty:
-- nobody has turned anything off, which is what "no preference" means.
SELECT topic_id, variant_subject, test_percent, winner, local_time, local_hour, resend_of_id FROM marketing_sends WHERE 1 = 0;
SELECT variant, variant_subject, due_at FROM marketing_recipients WHERE 1 = 0;
SELECT timezone FROM contacts WHERE 1 = 0;
