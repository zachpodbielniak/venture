-- Metadata owns schema, including an attribution module enabled after upgrade.
DO $attribution_checks$
BEGIN
 IF to_regclass('attribution_sites') IS NOT NULL AND to_regclass('uq_attribution_sites_organization_site_key') IS NULL THEN
  RAISE EXCEPTION 'Missing attribution identity invariant: attribution_sites';
 END IF;
 IF to_regclass('attribution_touches') IS NOT NULL AND to_regclass('uq_attribution_touches_organization_event_key') IS NULL THEN
  RAISE EXCEPTION 'Missing attribution identity invariant: attribution_touches';
 END IF;
 IF to_regclass('attribution_submissions') IS NOT NULL AND to_regclass('uq_attribution_submissions_organization_submission_key') IS NULL THEN
  RAISE EXCEPTION 'Missing attribution identity invariant: attribution_submissions';
 END IF;
 IF to_regclass('attribution_bindings') IS NOT NULL AND to_regclass('uq_attribution_bindings_organization_binding_key') IS NULL THEN
  RAISE EXCEPTION 'Missing attribution identity invariant: attribution_bindings';
 END IF;
END;
$attribution_checks$;
