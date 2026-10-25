-- An organization may choose where its profit and loss comes from:
-- pnl_basis 'ledger' reads the posted books, empty keeps the sale and
-- expense records. Additive field metadata; existing rows keep NULL, so
-- every report reads exactly as before.
CREATE TEMP TABLE venture_pnl_basis_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_pnl_basis_guard (valid)
SELECT CASE WHEN EXISTS (SELECT 1 FROM pragma_table_info('organizations') WHERE name = 'pnl_basis') THEN 1 ELSE 0 END;
DROP TABLE venture_pnl_basis_guard;
