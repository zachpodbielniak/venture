-- requires-table: deal_competitors
-- Metadata adds the records and optional loss attribution without guessing history.
SELECT lost_to_competitor_id FROM deals WHERE 1 = 0;
SELECT name, website, notes FROM competitors WHERE 1 = 0;
CREATE UNIQUE INDEX IF NOT EXISTS uq_deal_competitor_live
 ON deal_competitors (organization_id, deal_id, competitor_id)
 WHERE deleted_at IS NULL;
