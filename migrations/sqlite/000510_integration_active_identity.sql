-- Metadata builds the partial unique index before this checkpoint. Never
-- choose one account or delete history to repair ambiguous active bindings.
CREATE TEMP TABLE venture_active_binding_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_active_binding_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'integration_connections')
 OR EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'index' AND name = 'uq_integration_connections_organization_provider_when_enabled') THEN 1 ELSE 0 END;
DROP TABLE venture_active_binding_guard;
