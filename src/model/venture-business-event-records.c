/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static const VentureFieldDecl event_fields[] = {
	VENTURE_FIELD("event", "Event", "Semantic business transition", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("occurred-at", "Recorded at", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("source-type", "Source type", NULL, VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("source-id", "Source", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("source-version", "Source version", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NOT_NULL)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureBusinessEvent, venture_business_event, event_fields,
	venture_entity_class_set_working_copy(VENTURE_ENTITY_CLASS(klass));
	venture_entity_class_set_commentable(VENTURE_ENTITY_CLASS(klass), FALSE);)

static const VentureFieldDecl delivery_fields[] = {
	VENTURE_FIELD_REF("event-id", "Event", NULL, "business_event", VENTURE_COLUMN_FLAG_NOT_NULL | VENTURE_COLUMN_FLAG_INDEXED),
	/* A numeric identity retains the obligation even if its subscription is removed. */
	VENTURE_FIELD("webhook-id", "Webhook", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("delivered", "Accepted", NULL, VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("attempts", "Attempts", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("next-attempt-at", "Next attempt", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("status-code", "Last response status", NULL, VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureBusinessEventDelivery, venture_business_event_delivery, delivery_fields,
	venture_entity_class_set_working_copy(VENTURE_ENTITY_CLASS(klass));
	venture_entity_class_set_commentable(VENTURE_ENTITY_CLASS(klass), FALSE);)
