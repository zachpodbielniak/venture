-- requires-table: job_technicians
-- Additive jobs: work at a customer's address and the technicians sent.
-- The tables come from their field tables; this checks they carry them.
SELECT title, number, status, company_id, contact_id, service_address, problem, window_start, window_end, quote_id, invoice_id, left_at, arrived_at, completed_at, notes FROM jobs WHERE 1 = 0;
SELECT job_id, user_id, lead FROM job_technicians WHERE 1 = 0;
