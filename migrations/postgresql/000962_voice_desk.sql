-- requires-table: voice_desks
-- Voice answering is entity metadata. Verify the desk the line reads.
SELECT mode, services, service_area, booking_rules FROM voice_desks WHERE 1 = 0;
