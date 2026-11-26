-- A user can carry a profile picture: users.avatar_version is the digest
-- of its renditions, which live under <state_dir>/avatars/<user id>/ and
-- never in the database. Reconciliation adds the column from the field
-- table. Every existing account has no picture, which is what an empty
-- value already says, so there is nothing to backfill.
SELECT avatar_version FROM users WHERE 1 = 0;
