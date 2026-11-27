-- requires-table: saved_views
-- A saved view keeps the LQL filter of the list it was saved from:
-- saved_views.lql, beside the query string it always kept (docs/lql.org,
-- "Filter bars"). Reconciliation adds the column from the field table.
-- No backfill: a view saved before has no LQL, reads NULL, and opens at
-- its query string exactly as it did.
SELECT lql FROM saved_views WHERE 1 = 0;
