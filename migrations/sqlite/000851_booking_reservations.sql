-- requires-table: booking_reservations
-- Holds and management capabilities are private service-owned working copies.
SELECT page_id, activity_id, starts_at, ends_at, expires_at, secret FROM booking_reservations WHERE 1 = 0;
SELECT capacity, public_origin FROM booking_pages WHERE 1 = 0;
