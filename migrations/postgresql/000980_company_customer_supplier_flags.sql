-- requires-table: companies
-- A company is a customer, a supplier, or both: the two flags join the
-- single relationship. A company already filed as a customer or a supplier
-- is flagged as one, so the Customers and Suppliers lists start out holding
-- exactly what the relationship said. Nothing else is inferred.
UPDATE companies SET customer = TRUE WHERE kind = 'customer' AND customer IS NOT TRUE;
UPDATE companies SET supplier = TRUE WHERE kind = 'supplier' AND supplier IS NOT TRUE;
UPDATE companies SET customer = FALSE WHERE customer IS NULL;
UPDATE companies SET supplier = FALSE WHERE supplier IS NULL;
