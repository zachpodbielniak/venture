-- requires-table: documents
-- The attachment service keeps form downloads behind response authority.
SELECT form_upload_id FROM documents WHERE 1 = 0;
