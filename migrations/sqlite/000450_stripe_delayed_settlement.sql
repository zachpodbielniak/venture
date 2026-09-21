-- requires-table: stripe_checkouts
-- Metadata installs the per-invoice active-attempt index before this backfill.
-- Ambiguous historical open sessions fail the entire migration, never pick one.
UPDATE stripe_checkouts SET active = CASE WHEN status IN ('open', 'initiated', 'processing', 'exception') THEN TRUE ELSE FALSE END;
