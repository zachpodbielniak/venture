/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

GType
venture_job_status_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_JOB_BOOKED, "VENTURE_JOB_BOOKED", "booked" },
			{ VENTURE_JOB_ON_THE_WAY, "VENTURE_JOB_ON_THE_WAY", "on_the_way" },
			{ VENTURE_JOB_ON_SITE, "VENTURE_JOB_ON_SITE", "on_site" },
			{ VENTURE_JOB_DONE, "VENTURE_JOB_DONE", "done" },
			{ VENTURE_JOB_INVOICED, "VENTURE_JOB_INVOICED", "invoiced" },
			{ VENTURE_JOB_CANCELLED, "VENTURE_JOB_CANCELLED", "cancelled" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureJobStatus", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

static const VentureFieldDecl job_fields[] = {
	VENTURE_FIELD_NAME("title", "Job", "What the job is, the way the customer put it: \"No hot water\""),
	VENTURE_FIELD("number", "Number", "Left blank, the next J- number", VENTURE_FIELD_KIND_STRING,
		VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_ENUM("status", "Status", "Booked, on the way, on site, done, invoiced or cancelled",
		venture_job_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("company-id", "Customer", "Who the work is for", "company",
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("contact-id", "Contact on site", "Who lets the technician in", "contact", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("property-id", "Property", "Which of the customer's places; its address fills the service address", "property", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("service-address", "Service address", "Where the work is, if not the customer's own address"),
	VENTURE_FIELD_TEXT("problem", "Problem", "What is wrong, in the customer's words"),
	VENTURE_FIELD("window-start", "Arrive from", "The start of the arrival window promised to the customer",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("window-end", "Arrive by", "The end of the arrival window",
		VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("quote-id", "Estimate", "The quote this job carries out", "quote", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("invoice-id", "Invoice", "The invoice that bills it", "invoice", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("left-at", "On the way", "When a technician set off", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("arrived-at", "Arrived", "When a technician got there", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("completed-at", "Done", "When the work was finished", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("notes", "Notes", "Gate codes, pets, parts to bring")
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureJob, venture_job, job_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Job", NULL);)

static const VentureFieldDecl technician_fields[] = {
	VENTURE_FIELD_REF("job-id", "Job", NULL, "job", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("user-id", "Technician", "A person who signs in", "user",
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("lead", "Lead technician", "The one the customer deals with", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureJobTechnician, venture_job_technician, technician_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Job technician", NULL);)

static const VentureFieldDecl property_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What the customer calls it: \"Home\", \"14 Oak St rental\""),
	VENTURE_FIELD_REF("company-id", "Customer", "Whose place it is", "company",
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	{ "address", "Address", "Street, town and postcode, the way a technician types it into a map", VENTURE_FIELD_KIND_TEXT, NULL, NULL,
	  VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_SEARCHABLE },
	VENTURE_FIELD_TEXT("access-notes", "Getting in", "Gate code, where the key is, the dog in the yard, where to park"),
	VENTURE_FIELD_REF("contact-id", "Who is there", "The tenant or the person who lets technicians in", "contact", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureProperty, venture_property, property_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Property", "Properties");)

static const VentureFieldDecl equipment_fields[] = {
	VENTURE_FIELD_NAME("name", "What it is", "\"Water heater\", \"Furnace\", \"Upstairs AC\""),
	VENTURE_FIELD_REF("property-id", "Property", "Where it is", "property",
		VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("make", "Make", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("model", "Model", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("serial", "Serial number", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("installed-year", "Year installed", "Gives its age; blank when nobody knows", VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("warranty-until", "Warranty until", NULL, VENTURE_FIELD_KIND_DATE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", "Size, fuel, filter size, what was last done")
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureEquipment, venture_equipment, equipment_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Equipment", "Equipment");)
