-- requires-table: sequence_steps
-- Additive: journeys (docs/journeys.org). Branch and jump positions on a
-- step, a trigger's identity on an enrollment, the branch taken on a
-- delivery, the machine judgement on a tracking event and a sequence's
-- marketing topic. Existing steps have no branch and no jump, so every
-- existing sequence runs in position order as before; existing opens have
-- no machine reason and keep counting. Triggers start empty.
SELECT branch_on, yes_position, no_position, then_position, end_after FROM sequence_steps WHERE 1 = 0;
SELECT trigger_key, trigger_id FROM sequence_enrollments WHERE 1 = 0;
SELECT branch_result FROM sequence_deliveries WHERE 1 = 0;
