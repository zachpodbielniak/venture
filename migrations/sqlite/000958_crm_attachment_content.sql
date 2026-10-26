-- requires-table: documents
-- Reconciliation adds nullable content_base64 from the document field table.
-- Existing URL-only documents keep their metadata and acquire no invented
-- payload. Verify the additive column without rewriting retained documents.
SELECT content_base64 FROM documents WHERE 1 = 0;
