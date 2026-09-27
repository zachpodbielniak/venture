-- A quote line may be taxed by a rate record, as an invoice line is: the
-- tax_code_id column comes from the field table. Existing lines keep their
-- whole tax percent and no rate record, because none was ever chosen for
-- them; nothing is inferred from the percent. This only checks the column
-- is there wherever quotes are.
CREATE TEMP TABLE venture_quote_tax_code_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_quote_tax_code_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'quote_lines')
 OR EXISTS (SELECT 1 FROM pragma_table_info('quote_lines') WHERE name = 'tax_code_id')
 THEN 1 ELSE 0 END;
DROP TABLE venture_quote_tax_code_guard;
