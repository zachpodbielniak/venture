-- requires-table: notifications
-- The inbox recorded who caused each notification the same way the audit
-- log did; a token's name there is rewritten to "API token #<id>" too.
UPDATE notifications
SET actor = 'API token #' || CAST((SELECT t.id FROM api_tokens t WHERE 'token:' || t.name = notifications.actor) AS TEXT)
WHERE actor LIKE 'token:%'
  AND (SELECT COUNT(*) FROM api_tokens t WHERE 'token:' || t.name = notifications.actor) = 1;
UPDATE notifications SET actor = 'API token' WHERE actor LIKE 'token:%';
