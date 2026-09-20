/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#define STR(n,l) VENTURE_FIELD(n,l,NULL,VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_NONE)
#define INT(n,l) VENTURE_FIELD(n,l,NULL,VENTURE_FIELD_KIND_INTEGER,VENTURE_COLUMN_FLAG_NONE)
#define DATE(n,l) VENTURE_FIELD(n,l,NULL,VENTURE_FIELD_KIND_DATETIME,VENTURE_COLUMN_FLAG_INDEXED)
#define SECRET(n,l) VENTURE_FIELD(n,l,NULL,VENTURE_FIELD_KIND_TEXT,VENTURE_COLUMN_FLAG_SENSITIVE)
#define REF(n,l,t) VENTURE_FIELD_REF(n,l,NULL,t,VENTURE_COLUMN_FLAG_INDEXED)
#define UNIQUE(n,l) VENTURE_FIELD(n,l,NULL,VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION)
#define SUBJECT(n,l,t) VENTURE_FIELD_REF(n,l,"Original approved source; retained through CRM merge",t,VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_RETAIN_REFERENCE)
#define SUBJECT_FIELDS SUBJECT("contact-id","Contact","contact"), SUBJECT("company-id","Company","company"), SUBJECT("lead-id","Lead","lead")

GType venture_marketing_list_mode_get_type(void)
{
	static gsize type = 0;
	if (g_once_init_enter(&type)) {
		static const GEnumValue values[] = { { 0,"MARKETING_LIST_STATIC","static" }, { 1,"MARKETING_LIST_SEGMENT","segment" }, { 0,NULL,NULL } };
		g_once_init_leave(&type, g_enum_register_static("VentureMarketingListMode", values));
	}
	return type;
}
GType venture_marketing_target_get_type(void)
{
	static gsize type = 0;
	if (g_once_init_enter(&type)) {
		static const GEnumValue values[] = { { 0,"MARKETING_TARGET_CONTACT","contact" }, { 1,"MARKETING_TARGET_COMPANY","company" }, { 2,"MARKETING_TARGET_LEAD","lead" }, { 0,NULL,NULL } };
		g_once_init_leave(&type, g_enum_register_static("VentureMarketingTarget", values));
	}
	return type;
}
GType venture_marketing_send_state_get_type(void)
{
	static gsize type = 0;
	if (g_once_init_enter(&type)) {
		static const GEnumValue values[] = { { 0,"MARKETING_SEND_DRAFT","draft" }, { 1,"MARKETING_SEND_PREVIEW","preview" }, { 2,"MARKETING_SEND_APPROVED","approved" }, { 3,"MARKETING_SEND_PAUSED","paused" }, { 4,"MARKETING_SEND_CANCELLED","cancelled" }, { 5,"MARKETING_SEND_COMPLETE","complete" }, { 0,NULL,NULL } };
		g_once_init_leave(&type, g_enum_register_static("VentureMarketingSendState", values));
	}
	return type;
}
static const VentureFieldDecl list_fields[] = {
	VENTURE_FIELD_NAME("name","Name",NULL),
	VENTURE_FIELD_ENUM("mode","Audience mode",NULL,venture_marketing_list_mode_get_type,VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("target","Segment record type",NULL,venture_marketing_target_get_type,VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("filters","Saved search filters","URL-encoded record filters; organization and pagination overrides are refused")
};
VENTURE_DEFINE_ENTITY(VentureMarketingList, venture_marketing_list, list_fields)
static const VentureFieldDecl member_fields[] = {
	REF("list-id","Static list","marketing_list"), SUBJECT_FIELDS,
	UNIQUE("member-key","Derived membership identity")
};
VENTURE_DEFINE_ENTITY(VentureMarketingMember, venture_marketing_member, member_fields)
static const VentureFieldDecl consent_fields[] = {
	SUBJECT_FIELDS, STR("email","Normalized address"), STR("purpose","Purpose"),
	REF("withdraws-id","Withdrawn permission","marketing_consent"),
	STR("withdraws-uuid","Withdrawn permission UUID"),
	VENTURE_FIELD_TEXT("source","Permission source",NULL),
	VENTURE_FIELD_TEXT("evidence","Permission evidence",NULL),
	DATE("recorded-at","Recorded at"), DATE("withdrawn-at","Withdrawn at"),
	UNIQUE("evidence-key","Consent evidence identity")
};
VENTURE_DEFINE_ENTITY(VentureMarketingConsent, venture_marketing_consent, consent_fields)
static const VentureFieldDecl send_fields[] = {
	VENTURE_FIELD_NAME("name","Name",NULL), REF("list-id","Audience list","marketing_list"),
	STR("subject","Subject"), VENTURE_FIELD_TEXT("text-body","Text body",NULL), VENTURE_FIELD_TEXT("html-body","HTML body",NULL),
	VENTURE_FIELD("tracking","Track observations","Requires organization marketing tracking permission",VENTURE_FIELD_KIND_BOOLEAN,VENTURE_COLUMN_FLAG_NONE),
	INT("interval-seconds","Minimum seconds between recipients"),
	VENTURE_FIELD_ENUM("state","State",NULL,venture_marketing_send_state_get_type,VENTURE_COLUMN_FLAG_INDEXED),
	DATE("previewed-at","Previewed at"), DATE("approved-at","Approved at"), STR("approved-by","Approved by"),
	INT("eligible-count","Eligible recipients"), INT("excluded-count","Excluded records"),
	DATE("next-enqueue-at","Next enqueue"), DATE("next-delivery-at","Next submission"),
	REF("last-mail-id","Latest outbox identity","mail_message"), STR("public-base","Frozen public origin")
};
VENTURE_DEFINE_ENTITY(VentureMarketingSend, venture_marketing_send, send_fields)
static const VentureFieldDecl recipient_fields[] = {
	REF("send-id","Send","marketing_send"), SUBJECT_FIELDS,
	REF("consent-id","Consent evidence","marketing_consent"),
	STR("email","Normalized recipient"), STR("eligibility","Preview decision"),
	STR("subject","Rendered subject"), VENTURE_FIELD_TEXT("text-body","Rendered text",NULL), VENTURE_FIELD_TEXT("html-body","Rendered HTML",NULL),
	SECRET("private-text-body","Private delivery text"), SECRET("private-html-body","Private delivery HTML"),
	SECRET("unsubscribe-url","Private unsubscribe URL"),
	VENTURE_FIELD("unsubscribe-hash","Unsubscribe token hash",NULL,VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_SENSITIVE),
	VENTURE_FIELD("tracking-hash","Tracking token hash",NULL,VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_SENSITIVE),
	SECRET("links","Original tracking destinations"),
	REF("mail-id","Outbox message","mail_message"),
	VENTURE_FIELD("state","Campaign queue state","Once enqueued, the linked outbox message and performance report are authoritative for SMTP outcome",VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_NONE),
	DATE("opened-at","First observed open"), DATE("clicked-at","First observed click"),
	DATE("bounced-at","Hard bounce evidence at"), DATE("unsubscribed-at","Unsubscribed at"),
	UNIQUE("recipient-key","Retained audience identity")
};
VENTURE_DEFINE_ENTITY(VentureMarketingRecipient, venture_marketing_recipient, recipient_fields)
static const VentureFieldDecl event_fields[] = {
	REF("recipient-id","Recipient","marketing_recipient"), STR("kind","Evidence kind"),
	INT("link-position","Link position"), DATE("occurred-at","Occurred at"),
	VENTURE_FIELD_TEXT("source","Evidence source",NULL), UNIQUE("event-key","Evidence identity")
};
VENTURE_DEFINE_ENTITY(VentureMarketingEvent, venture_marketing_event, event_fields)
