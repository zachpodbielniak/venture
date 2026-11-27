-- requires-table: forge_repos
-- A repository enrolled before forge_repos.issue_kind existed was filing the
-- issues it accepted as external (support requests), and keeps doing so:
-- every such row reads NULL here, so say external in the row. A repository
-- enrolled after this stores its kind (internal by default) when it is
-- created, so NULL never means "new" and this is never what decides one.
-- The column is stored as the enum nick, text.
UPDATE forge_repos SET issue_kind = 'external' WHERE issue_kind IS NULL;
