-- requires-table: equipments
-- Additive: a customer's properties, the equipment at each, and the
-- property a job is at. The tables come from their field tables; this
-- checks they carry them.
SELECT name, company_id, address, access_notes, contact_id, notes FROM properties WHERE 1 = 0;
SELECT name, property_id, make, model, serial, installed_year, warranty_until, notes FROM equipments WHERE 1 = 0;
SELECT property_id FROM jobs WHERE 1 = 0;
