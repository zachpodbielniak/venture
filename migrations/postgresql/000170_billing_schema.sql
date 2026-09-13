-- Billing is additive; verify organization-scoped identities if enabled.
-- Disabled installations need no billing tables or synthetic history.
DO $billing$
BEGIN
 IF to_regclass('plans') IS NOT NULL AND to_regclass('uq_plans_organization_code') IS NULL THEN
  RAISE EXCEPTION 'Billing plan codes require an organization-scoped unique index';
 END IF;
 IF to_regclass('customer_subscriptions') IS NOT NULL AND to_regclass('uq_customer_subscriptions_organization_external_id') IS NULL THEN
  RAISE EXCEPTION 'Billing subscription identities require an organization-scoped unique index';
 END IF;
END
$billing$;
