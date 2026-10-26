-- requires-table: deal_contacts
-- Additive: contact roles on deals (docs/pipelines.org). The table starts
-- empty and nothing is backfilled: an existing deal's own contact stays
-- the fallback recipient until somebody is marked primary on it.
SELECT deal_id, contact_id, role, "primary", note FROM deal_contacts WHERE 1 = 0;
