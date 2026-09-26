-- An API token's name is what its owner typed, and it was written into the
-- audit log as the actor of every change the token made and the approver
-- of every staged change it applied -- a log the viewer role reads. Both
-- are now "API token #<id>"; rewrite the names already recorded the same
-- way. A name two tokens share cannot say which token it
-- was, so it becomes "API token" without a number rather than a guess.
UPDATE audit_entries
SET actor = 'API token #' || (SELECT t.id FROM api_tokens t WHERE 'token:' || t.name = audit_entries.actor)
WHERE actor LIKE 'token:%'
  AND (SELECT COUNT(*) FROM api_tokens t WHERE 'token:' || t.name = audit_entries.actor) = 1;
UPDATE audit_entries SET actor = 'API token' WHERE actor LIKE 'token:%';
UPDATE audit_entries
SET approved_by = 'API token #' || (SELECT t.id FROM api_tokens t WHERE 'token:' || t.name = audit_entries.approved_by)
WHERE approved_by LIKE 'token:%'
  AND (SELECT COUNT(*) FROM api_tokens t WHERE 'token:' || t.name = audit_entries.approved_by) = 1;
UPDATE audit_entries SET approved_by = 'API token' WHERE approved_by LIKE 'token:%';
