-- Every record can carry a discussion: the comments table comes from the
-- comment type's field table, and reconciliation creates it with one index
-- per column. A record page reads its discussion by both halves of the
-- subject at once -- "every comment on invoice 12, oldest first" -- so the
-- pair gets an index of its own, with the id that orders the thread. No
-- comment existed before this release, so nothing is backfilled.
CREATE INDEX IF NOT EXISTS idx_comments_subject_thread
    ON comments (subject_type, subject_id, id);
