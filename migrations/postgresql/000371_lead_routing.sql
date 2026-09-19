-- Routing rule, scoring rule and score history tables come from the leads
-- module field tables; the lead's score_manual and routing_rule_id columns
-- from additive metadata. Verify the three tables are present together, or
-- all absent when the module is disabled.
CREATE TEMP TABLE venture_lead_routing_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 3)));
INSERT INTO venture_lead_routing_upgrade_guard SELECT COUNT(*) FROM information_schema.tables
 WHERE table_schema = current_schema() AND table_type = 'BASE TABLE'
 AND table_name IN ('lead_routing_rules', 'lead_scoring_rules', 'lead_score_histories');
DROP TABLE venture_lead_routing_upgrade_guard;
