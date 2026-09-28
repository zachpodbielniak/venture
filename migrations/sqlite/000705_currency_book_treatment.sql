-- A currency defined before book treatment could be chosen was valued:
-- converted into the book currency when a rate existed, kept in its own
-- books when none did. Say so in the row rather than leaving it NULL, so
-- the column holds exactly the three values the ledger reads and no
-- existing journal's meaning moves. Posted journals are not touched.
UPDATE currencies SET book_treatment = 0 WHERE book_treatment IS NULL;
