-- requires-table: notifications
-- The inbox recorded who caused each notification the same way the audit
-- log did, and wrote the same name at the head of its title ("token:x
-- assigned you: ...", "token:x updated ..."). Both are rewritten to
-- "API token #<id>" by 000640's rule: a name two tokens share becomes
-- "API token" rather than a guess. Only the title's leading actor is
-- touched, so a record label that happens to contain the name is left
-- alone. substr() rather than LIKE: SQLite's LIKE ignores case and
-- PostgreSQL's does not, and the two backends must rewrite the same rows.
UPDATE notifications
SET title = 'API token #' || CAST((SELECT t.id FROM api_tokens t WHERE 'token:' || t.name = notifications.actor) AS TEXT) || substr(title, length(actor) + 1)
WHERE substr(actor, 1, 6) = 'token:'
  AND substr(title, 1, length(actor)) = actor
  AND (SELECT COUNT(*) FROM api_tokens t WHERE 'token:' || t.name = notifications.actor) = 1;
UPDATE notifications
SET title = 'API token' || substr(title, length(actor) + 1)
WHERE substr(actor, 1, 6) = 'token:'
  AND substr(title, 1, length(actor)) = actor;
UPDATE notifications
SET actor = 'API token #' || CAST((SELECT t.id FROM api_tokens t WHERE 'token:' || t.name = notifications.actor) AS TEXT)
WHERE substr(actor, 1, 6) = 'token:'
  AND (SELECT COUNT(*) FROM api_tokens t WHERE 'token:' || t.name = notifications.actor) = 1;
UPDATE notifications SET actor = 'API token' WHERE substr(actor, 1, 6) = 'token:';
