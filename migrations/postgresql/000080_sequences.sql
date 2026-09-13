-- The field tables own schema creation, including when a module is enabled later.
DO $sequence_checks$
BEGIN
 IF to_regclass('suppressions') IS NOT NULL AND to_regclass('uq_suppressions_organization_email') IS NULL THEN
  RAISE EXCEPTION 'Missing organization-scoped suppression uniqueness';
 END IF;
 IF to_regclass('sequence_deliveries') IS NOT NULL AND to_regclass('uq_sequence_deliveries_organization_delivery_key') IS NULL THEN
  RAISE EXCEPTION 'Missing durable sequence delivery uniqueness';
 END IF;
 IF to_regclass('sequence_enrollments') IS NOT NULL AND to_regclass('uq_sequence_enrollments_organization_active_key') IS NULL THEN
  RAISE EXCEPTION 'Missing active sequence enrollment uniqueness';
 END IF;
END;
$sequence_checks$;
