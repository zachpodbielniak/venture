/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_HEADLINE_PRIVATE_H
#define VENTURE_HEADLINE_PRIVATE_H
#include "venture.h"

/**
 * venture_headline_install_validators:
 * @database: the repository receiving classification validators
 *
 * Applies category defaults only on creation, preserving subsequent choices,
 * and keeps the headline settings to one row per organisation.
 */
void venture_headline_install_validators(VentureDatabase *database);

/**
 * venture_headline_register_reports:
 * @registry: the report registry
 *
 * Registers cac, churn, ltv and ltv_cac.
 */
void venture_headline_register_reports(VentureReportRegistry *registry);
#endif
