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

/**
 * VentureProperty:
 *
 * A place a customer has work done: their home, or each rental a landlord
 * owns. It carries the address, how to get in (gate code, key, dog in the
 * yard) and who is there. Equipment and jobs are filed under it.
 */
#define VENTURE_TYPE_PROPERTY (venture_property_get_type())
VENTURE_DECLARE_ENTITY(VentureProperty, venture_property, PROPERTY)

/**
 * VentureEquipment:
 *
 * A thing at a property that gets serviced: a water heater, a furnace, an
 * air conditioner. Make, model, serial, the year it went in (so its age),
 * and its warranty.
 */
#define VENTURE_TYPE_EQUIPMENT (venture_equipment_get_type())
VENTURE_DECLARE_ENTITY(VentureEquipment, venture_equipment, EQUIPMENT)

/**
 * VentureMaintenancePlan:
 *
 * A visit a customer has bought on a schedule: a yearly furnace tune-up at
 * their home. It names the property (and the equipment, when it is one
 * thing), what each visit is, how many months apart, and when the next is
 * due. Booking it makes the job and moves the due date on; how it is
 * billed (a subscription, an invoice per visit) is separate.
 */
#define VENTURE_TYPE_MAINTENANCE_PLAN (venture_maintenance_plan_get_type())
VENTURE_DECLARE_ENTITY(VentureMaintenancePlan, venture_maintenance_plan, MAINTENANCE_PLAN)

G_END_DECLS
#endif
