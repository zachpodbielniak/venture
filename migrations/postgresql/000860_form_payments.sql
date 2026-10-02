-- requires-table: form_payments
-- Metadata installs exact prices and private pending settlement records.
SELECT payment_enabled, payment_name_field, payment_email_field FROM forms WHERE 1 = 0;
SELECT form_id, product_id, unit_price_amount, unit_price_currency, unit_price_exponent, choice_field, quantity_field FROM form_prices WHERE 1 = 0;
SELECT invoice_id, checkout_id, reservation_id, nonce_hash, payload, state FROM form_payments WHERE 1 = 0;
SELECT invoice_id FROM form_submissions WHERE 1 = 0;
