-- requires-table: companies
-- A company is a customer, a supplier, or both: the two flags join the
-- single relationship. A company already filed as a customer or a supplier
-- is flagged as one, so the Customers and Suppliers lists start out holding
-- exactly what the relationship said. Nothing else is inferred.
UPDATE companies SET customer = 1 WHERE kind = 'customer' AND (customer IS NULL OR customer = 0);
UPDATE companies SET supplier = 1 WHERE kind = 'supplier' AND (supplier IS NULL OR supplier = 0);
UPDATE companies SET customer = 0 WHERE customer IS NULL;
UPDATE companies SET supplier = 0 WHERE supplier IS NULL;
