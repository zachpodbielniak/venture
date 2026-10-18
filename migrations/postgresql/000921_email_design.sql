-- requires-table: mail_templates
-- Additive: a template's block design and the version it shows. Existing
-- templates have no design, so their text and HTML bodies stay exactly as
-- written; nothing is designed for them. The brand, hosted image, section
-- and version tables are new and start empty.
SELECT design, design_version FROM mail_templates WHERE 1 = 0;
