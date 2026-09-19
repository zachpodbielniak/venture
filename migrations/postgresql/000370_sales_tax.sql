-- Sales-tax jurisdiction and rule tables come from the sales_tax module
-- field tables, and the company, product, invoice-line and credit columns
-- from additive metadata. Verify the pair is present together, or absent
-- when the module is disabled.
CREATE TEMP TABLE venture_sales_tax_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 2)));
INSERT INTO venture_sales_tax_upgrade_guard SELECT COUNT(*) FROM information_schema.tables
 WHERE table_schema = current_schema() AND table_type = 'BASE TABLE'
 AND table_name IN ('tax_jurisdictions', 'tax_rules');
DROP TABLE venture_sales_tax_upgrade_guard;
-- The address, exemption and frozen-rate columns live on core record types,
-- so they are present whatever the sales_tax switch says. Each table is
-- checked on its own because invoicing and receivables can be disabled
-- separately: an absent table is accepted, a present one must carry them.
-- Nothing is backfilled. An existing line keeps jurisdiction 0 and rate 0,
-- which reads as "no jurisdiction was in force when this was issued" and
-- lands the line on the return's unassigned row rather than inventing a
-- jurisdiction for tax that was frozen before any rule existed.
DO $$
DECLARE
  t TEXT;
  c TEXT;
  wanted TEXT[];
BEGIN
  FOREACH t IN ARRAY ARRAY['companies', 'products', 'invoice_lines', 'customer_credits'] LOOP
    CONTINUE WHEN NOT EXISTS (SELECT 1 FROM information_schema.tables
      WHERE table_schema = current_schema() AND table_name = t);
    wanted := CASE t
      WHEN 'companies' THEN ARRAY['tax_exemption_number', 'address_state', 'address_county', 'address_city']
      WHEN 'products' THEN ARRAY['tax_exempt']
      WHEN 'invoice_lines' THEN ARRAY['tax_jurisdiction_id', 'tax_rate_scaled', 'tax_exempt']
      ELSE ARRAY['tax_jurisdiction_id']
    END;
    FOREACH c IN ARRAY wanted LOOP
      IF NOT EXISTS (SELECT 1 FROM information_schema.columns
          WHERE table_schema = current_schema() AND table_name = t AND column_name = c) THEN
        RAISE EXCEPTION 'sales tax requires %.%', t, c;
      END IF;
    END LOOP;
  END LOOP;
END $$;
