-- requires-table: inventory_cost_layers
-- Venture's stock runs on the Libreventory engine (deps/libreventory).
-- Reconciliation adds inventory_cost_layers.stock_layer_id from the field
-- table: the engine layer each row mirrors. The engine makes its own
-- tables, prefixed stock_, when the database opens. Moving the stock into
-- them needs the engine's rules, so it is C, after the seeds:
-- venture_stock_engine_migrate() gives every inventory item its open lots
-- and links each open layer row. Nothing is backfilled here; a layer used
-- up before the engine keeps 0, which means "no engine layer".
SELECT stock_layer_id FROM inventory_cost_layers WHERE 1 = 0;
