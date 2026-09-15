-- Goods is additive. Field tables create purchase, receipt, cost layer and
-- sales-order records. Both a disabled fresh module and a complete existing
-- module are valid. Never invent receiving or match events from bill status.
CREATE TEMP TABLE venture_goods_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 8)));
INSERT INTO venture_goods_upgrade_guard SELECT COUNT(*) FROM sqlite_master
 WHERE type = 'table' AND name IN (
	'purchase_orders', 'purchase_order_lines', 'goods_receipts', 'goods_receipt_lines',
	'inventory_cost_layers', 'sales_orders', 'sales_order_lines', 'fulfillments');
DROP TABLE venture_goods_upgrade_guard;
