/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_JOB_RECORDS_H
#define VENTURE_JOB_RECORDS_H
G_BEGIN_DECLS

/**
 * VentureJobStatus:
 * @VENTURE_JOB_BOOKED: on the schedule, nobody on the way yet
 * @VENTURE_JOB_ON_THE_WAY: a technician has set off
 * @VENTURE_JOB_ON_SITE: a technician is at the address
 * @VENTURE_JOB_DONE: the work is finished
 * @VENTURE_JOB_INVOICED: the work is billed: the job names its invoice
 * @VENTURE_JOB_CANCELLED: called off before it was done
 *
 * Where a job is in its day. Appended, never inserted: stored jobs read
 * back by value.
 */
typedef enum {
	VENTURE_JOB_BOOKED,
	VENTURE_JOB_ON_THE_WAY,
	VENTURE_JOB_ON_SITE,
	VENTURE_JOB_DONE,
	VENTURE_JOB_INVOICED,
	VENTURE_JOB_CANCELLED
} VentureJobStatus;

/**
 * venture_job_status_get_type:
 *
 * Returns: the #GType of #VentureJobStatus
 */
GType venture_job_status_get_type(void) G_GNUC_CONST;

/**
 * VentureJob:
 *
 * A piece of work at a customer's address: what is wrong, when the
 * technicians are expected, where it has got to, and the estimate and the
 * invoice it belongs with.
 */
#define VENTURE_TYPE_JOB (venture_job_get_type())
VENTURE_DECLARE_ENTITY(VentureJob, venture_job, JOB)

/**
 * VentureJobTechnician:
 *
 * One user sent to a job. A job can have several; one may be the lead.
 */
#define VENTURE_TYPE_JOB_TECHNICIAN (venture_job_technician_get_type())
VENTURE_DECLARE_ENTITY(VentureJobTechnician, venture_job_technician, JOB_TECHNICIAN)

G_END_DECLS
#endif
