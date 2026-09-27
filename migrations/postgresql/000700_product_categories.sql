-- requires-table: products
-- Build a product category tree from the free-text category and
-- subcategory columns, and point each product at its place in it.
--
-- What is inferred, and from what: one top-level category (applies to
-- product) per distinct (organization, trimmed category), and one child of
-- it per distinct trimmed subcategory seen with that category. A product
-- points at its subcategory's row when it has one and at its category's
-- otherwise. Deleted products are included so a restored product keeps its
-- place. A subcategory with no category names no parent and is left as
-- text. The text columns are left exactly as they were.
--
-- The categories table is core and always exists. The guard skips only an
-- absent products table, which never held a row to take a category from;
-- sales switched off after use still has products (startup reconciles a
-- disabled module's existing tables, category_id included) and is
-- backfilled now.
INSERT INTO categories
 (uuid, organization_id, created_at, updated_at, version, name, parent_id, applies_to)
SELECT md5(random()::text || clock_timestamp()::text)::uuid::text,
 s.organization_id, to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'), to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'), 1, s.name, 0, 'product'
FROM (SELECT DISTINCT organization_id, TRIM(category) AS name FROM products
      WHERE category IS NOT NULL AND TRIM(category) <> '') s
WHERE NOT EXISTS (SELECT 1 FROM categories c
 WHERE COALESCE(c.organization_id, 0) = COALESCE(s.organization_id, 0)
   AND c.name = s.name AND c.applies_to = 'product'
   AND COALESCE(c.parent_id, 0) = 0 AND c.deleted_at IS NULL);
INSERT INTO categories
 (uuid, organization_id, created_at, updated_at, version, name, parent_id, applies_to)
SELECT md5(random()::text || clock_timestamp()::text)::uuid::text,
 s.organization_id, to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'), to_char(CURRENT_TIMESTAMP AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'), 1, s.name, s.parent_id, 'product'
FROM (SELECT DISTINCT p.organization_id, TRIM(p.subcategory) AS name,
       (SELECT MIN(c.id) FROM categories c
        WHERE COALESCE(c.organization_id, 0) = COALESCE(p.organization_id, 0)
          AND c.name = TRIM(p.category) AND c.applies_to = 'product'
          AND COALESCE(c.parent_id, 0) = 0 AND c.deleted_at IS NULL) AS parent_id
      FROM products p
      WHERE p.category IS NOT NULL AND TRIM(p.category) <> ''
        AND p.subcategory IS NOT NULL AND TRIM(p.subcategory) <> '') s
WHERE s.parent_id IS NOT NULL AND NOT EXISTS (SELECT 1 FROM categories c
 WHERE c.parent_id = s.parent_id AND c.name = s.name AND c.deleted_at IS NULL);
UPDATE products SET category_id = COALESCE(
 (SELECT MIN(child.id) FROM categories child JOIN categories top ON top.id = child.parent_id
  WHERE COALESCE(top.organization_id, 0) = COALESCE(products.organization_id, 0)
    AND top.name = TRIM(products.category) AND top.applies_to = 'product'
    AND COALESCE(top.parent_id, 0) = 0 AND top.deleted_at IS NULL
    AND child.name = TRIM(products.subcategory) AND child.deleted_at IS NULL
    AND products.subcategory IS NOT NULL AND TRIM(products.subcategory) <> ''),
 (SELECT MIN(top.id) FROM categories top
  WHERE COALESCE(top.organization_id, 0) = COALESCE(products.organization_id, 0)
    AND top.name = TRIM(products.category) AND top.applies_to = 'product'
    AND COALESCE(top.parent_id, 0) = 0 AND top.deleted_at IS NULL))
WHERE COALESCE(category_id, 0) = 0 AND category IS NOT NULL AND TRIM(category) <> '';
