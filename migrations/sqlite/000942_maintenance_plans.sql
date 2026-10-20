-- requires-table: maintenance_plans
-- Additive: visits a customer has bought on a schedule, and the plan a
-- job was booked from. The table comes from its field table; this checks
-- it carries it.
SELECT name, company_id, property_id, equipment_id, visit, every_months, next_visit, ended_at, notes FROM maintenance_plans WHERE 1 = 0;
SELECT maintenance_plan_id FROM jobs WHERE 1 = 0;
