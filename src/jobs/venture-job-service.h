/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_JOB_SERVICE_H
#define VENTURE_JOB_SERVICE_H
G_BEGIN_DECLS

/**
 * venture_jobs_install:
 * @database: the database
 *
 * Adds the save checks for jobs and their technicians: the next J- number
 * for a job saved without one, a customer and contact in the job's
 * business, an arrival window that ends after it starts, an estimate and
 * an invoice for the same customer, an invoice before "invoiced", and one
 * row per technician per job, for an active user. Idempotent.
 */
void venture_jobs_install(VentureDatabase *database);

/**
 * venture_jobs_actions_register:
 * @database: the database
 *
 * Registers the buttons that move a job through its day: on_the_way,
 * arrive, complete, invoiced (naming the invoice, or taking the
 * estimate's) and cancel. Each is offered only where it applies and
 * stamps when it happened.
 */
void venture_jobs_actions_register(VentureDatabase *database);

G_END_DECLS
#endif
