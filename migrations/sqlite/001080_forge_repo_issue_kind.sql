-- requires-table: forge_repos
-- A forge repository says what an issue it accepts from upstream is filed
-- as: forge_repos.issue_kind, internal (your own project work) or external
-- (a support request). Reconciliation adds the column from the field table.
-- No backfill: an existing repository reads NULL, which is the enum's zero
-- value, internal -- the default a repository's own issues should have.
SELECT issue_kind FROM forge_repos WHERE 1 = 0;
