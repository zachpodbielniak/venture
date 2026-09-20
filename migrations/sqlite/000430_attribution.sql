-- Metadata owns schema; existing CRM rows never imply anonymous analytics consent.
CREATE TEMP TABLE venture_attribution_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_attribution_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'attribution_sites') OR EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'uq_attribution_sites_organization_site_key') THEN 1 ELSE 0 END;
INSERT INTO venture_attribution_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'attribution_touches') OR EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'uq_attribution_touches_organization_event_key') THEN 1 ELSE 0 END;
INSERT INTO venture_attribution_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'attribution_submissions') OR EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'uq_attribution_submissions_organization_submission_key') THEN 1 ELSE 0 END;
INSERT INTO venture_attribution_guard SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'attribution_bindings') OR EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'uq_attribution_bindings_organization_binding_key') THEN 1 ELSE 0 END;
DROP TABLE venture_attribution_guard;
