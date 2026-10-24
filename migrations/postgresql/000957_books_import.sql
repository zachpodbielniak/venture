-- Books migration tables come from the books_import module field tables:
-- one batch per backup and one row per source document or record it
-- became. Verify the pair is present together, or absent when the module
-- is disabled.
CREATE TEMP TABLE venture_books_import_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 2)));
INSERT INTO venture_books_import_upgrade_guard SELECT COUNT(*) FROM information_schema.tables
 WHERE table_schema = current_schema() AND table_type = 'BASE TABLE'
 AND table_name IN ('books_imports', 'books_import_rows');
DROP TABLE venture_books_import_upgrade_guard;
