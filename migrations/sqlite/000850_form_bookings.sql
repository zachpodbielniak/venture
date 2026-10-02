-- requires-table: form_fields
-- The metadata migrator installs optional booking references independently.
SELECT booking_page_id, booking_name_field, booking_email_field FROM form_fields WHERE 1 = 0;
SELECT booking_id FROM form_submissions WHERE 1 = 0;
