-- Metadata owns schema. Existing CRM rows do not imply marketing consent.
CREATE TEMP TABLE venture_marketing_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_marketing_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'marketing_members') OR EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'uq_marketing_members_organization_member_key') THEN 1 ELSE 0 END;
INSERT INTO venture_marketing_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'marketing_consents') OR EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'uq_marketing_consents_organization_evidence_key') THEN 1 ELSE 0 END;
INSERT INTO venture_marketing_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'marketing_recipients') OR EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'uq_marketing_recipients_organization_recipient_key') THEN 1 ELSE 0 END;
INSERT INTO venture_marketing_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'marketing_events') OR EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'uq_marketing_events_organization_event_key') THEN 1 ELSE 0 END;
DROP TABLE venture_marketing_guard;
