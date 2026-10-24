-- requires-table: activities
-- Additive: a missed call waits for the inbox (docs/telephony.org). Every
-- existing call keeps its outcome and is not marked for the inbox; the new
-- missed outcome is only written by the phone system's call log.
SELECT call_inbox_ready FROM activities WHERE 1 = 0;
