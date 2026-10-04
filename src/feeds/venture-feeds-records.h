/*
 * venture-feeds-records.h - Market data sources and their runs
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The feeds module's two record types. A data source says where market
 * data comes from and how often; a run says what one pass over it did.
 * Both are field tables and nothing else -- what the data source's
 * settings must look like is a save validator in venture-feeds-service.c,
 * because it needs the provider registry, and what a run says is written
 * by the service alone (venture_web_type_accepts_writes() refuses the
 * rest). The data itself never comes here: it lives in the series store,
 * one SQLite file per source (docs/market-data.org).
 */

#ifndef VENTURE_FEEDS_RECORDS_H
#define VENTURE_FEEDS_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

#define VENTURE_TYPE_DATA_SOURCE (venture_data_source_get_type())
VENTURE_DECLARE_ENTITY(VentureDataSource, venture_data_source, DATA_SOURCE)

#define VENTURE_TYPE_DATA_SOURCE_RUN (venture_data_source_run_get_type())
VENTURE_DECLARE_ENTITY(VentureDataSourceRun, venture_data_source_run, DATA_SOURCE_RUN)

G_END_DECLS

#endif /* VENTURE_FEEDS_RECORDS_H */
