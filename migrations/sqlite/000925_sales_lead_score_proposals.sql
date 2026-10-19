-- requires-table: lead_score_proposals
-- Additive: AI lead score proposals (docs/leads.org, AI score proposals).
-- A new table beside the rule score; no existing lead's score, manual mark
-- or history is touched, and no proposal is invented for an existing lead.
-- The organization switch arrives off, so nothing proposes until an
-- administrator turns it on.
SELECT lead_id, score, rule_score, reasons, status, proposed_at, decided_by, decided_at FROM lead_score_proposals WHERE 1 = 0;
SELECT ai_lead_scoring FROM organizations WHERE 1 = 0;
