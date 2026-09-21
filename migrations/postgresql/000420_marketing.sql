-- Metadata owns schema, including a module enabled after this upgrade.
DO $marketing_checks$
BEGIN
 IF to_regclass('marketing_members') IS NOT NULL AND to_regclass('uq_marketing_members_organization_member_key') IS NULL THEN
  RAISE EXCEPTION 'Missing marketing identity invariant: marketing_members';
 END IF;
 IF to_regclass('marketing_consents') IS NOT NULL AND to_regclass('uq_marketing_consents_organization_evidence_key') IS NULL THEN
  RAISE EXCEPTION 'Missing marketing identity invariant: marketing_consents';
 END IF;
 IF to_regclass('marketing_recipients') IS NOT NULL AND to_regclass('uq_marketing_recipients_organization_recipient_key') IS NULL THEN
  RAISE EXCEPTION 'Missing marketing identity invariant: marketing_recipients';
 END IF;
 IF to_regclass('marketing_events') IS NOT NULL AND to_regclass('uq_marketing_events_organization_event_key') IS NULL THEN
  RAISE EXCEPTION 'Missing marketing identity invariant: marketing_events';
 END IF;
END;
$marketing_checks$;
