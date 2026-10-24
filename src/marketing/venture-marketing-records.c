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
GType venture_marketing_winner_metric_get_type(void)
{
	static gsize type = 0;
	if (g_once_init_enter(&type)) {
		static const GEnumValue values[] = { { 0,"MARKETING_WINNER_OPENS","opens" }, { 1,"MARKETING_WINNER_CLICKS","clicks" }, { 0,NULL,NULL } };
		g_once_init_leave(&type, g_enum_register_static("VentureMarketingWinnerMetric", values));
	}
	return type;
}
GType venture_marketing_frequency_get_type(void)
{
	static gsize type = 0;
	if (g_once_init_enter(&type)) {
		static const GEnumValue values[] = { { 0,"MARKETING_FREQUENCY_ANY","any" }, { 1,"MARKETING_FREQUENCY_WEEKLY","weekly" },
			{ 2,"MARKETING_FREQUENCY_MONTHLY","monthly" }, { 0,NULL,NULL } };
		g_once_init_leave(&type, g_enum_register_static("VentureMarketingFrequency", values));
	}
	return type;
}
GType venture_marketing_channel_get_type(void)
{
	static gsize type = 0;
	if (g_once_init_enter(&type)) {
		static const GEnumValue values[] = { { 0,"MARKETING_CHANNEL_EMAIL","email" }, { 1,"MARKETING_CHANNEL_TEXT","text" }, { 0,NULL,NULL } };
		g_once_init_leave(&type, g_enum_register_static("VentureMarketingChannel", values));
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
	UNIQUE("evidence-key","Consent evidence identity"),
	/* Text consent (docs/telephony.org): purpose text, keyed by number. */
	VENTURE_FIELD("phone","Normalized number","E.164, for text consent",VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_INDEXED)
};
VENTURE_DEFINE_ENTITY(VentureMarketingConsent, venture_marketing_consent, consent_fields)
static const VentureFieldDecl send_fields[] = {
	REF("survey-form-id", "Personal survey form", "form"),
	VENTURE_FIELD_NAME("name","Name",NULL), REF("list-id","Audience list","marketing_list"),
	STR("subject","Subject"), VENTURE_FIELD_TEXT("text-body","Text body",NULL), VENTURE_FIELD_TEXT("html-body","HTML body",NULL),
	VENTURE_FIELD("tracking","Track observations","Requires organization marketing tracking permission",VENTURE_FIELD_KIND_BOOLEAN,VENTURE_COLUMN_FLAG_NONE),
	INT("interval-seconds","Minimum seconds between recipients"),
	VENTURE_FIELD_ENUM("state","State",NULL,venture_marketing_send_state_get_type,VENTURE_COLUMN_FLAG_INDEXED),
	DATE("previewed-at","Previewed at"), DATE("approved-at","Approved at"), STR("approved-by","Approved by"),
	INT("eligible-count","Eligible recipients"), INT("excluded-count","Excluded records"),
	DATE("next-enqueue-at","Next enqueue"), DATE("next-delivery-at","Next submission"),
	REF("last-mail-id","Latest outbox identity","mail_message"), STR("public-base","Frozen public origin"),
	/* Audiences and journeys (docs/marketing.org): the topic a person can
	 * turn off, an A/B test of subject or content, a send at each
	 * recipient's own local hour, and a resend to the first send's
	 * non-openers. The winner and its stamps are the service's to write. */
	VENTURE_FIELD_REF("topic-id","Topic","Recipients who turned this topic off on the preference page are left out","marketing_topic",VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("variant-subject","B subject","A/B test: the second subject; empty keeps the first",VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("variant-text-body","B text body","A/B test: the second text; empty keeps the first"),
	VENTURE_FIELD_TEXT("variant-html-body","B HTML body","A/B test: the second HTML; empty keeps the first"),
	VENTURE_FIELD("test-percent","Test share","A/B test: percent of the audience split between A and B before the rest get the winner; 0 is no test",VENTURE_FIELD_KIND_INTEGER,VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("winner-metric","Winner by","A/B test: opens or clicks, people not machines",venture_marketing_winner_metric_get_type,VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("winner-wait-hours","Winner after","A/B test: hours after the test arms are queued before the winner is picked; 4 when empty",VENTURE_FIELD_KIND_INTEGER,VENTURE_COLUMN_FLAG_NONE),
	STR("winner","Winning arm"), DATE("winner-at","Winner picked at"), DATE("test-sent-at","Test arms queued at"),
	VENTURE_FIELD("local-time","Send at local time","Each recipient gets it at the chosen hour in their own time zone",VENTURE_FIELD_KIND_BOOLEAN,VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("local-hour","Local hour","0 to 23, in the recipient's time zone (the contact's, else the organization's, else UTC)",VENTURE_FIELD_KIND_INTEGER,VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("local-date","Local day","The day to send on; empty is the next time that hour comes round after approval",VENTURE_FIELD_KIND_DATE,VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("resend-of-id","Resend of","The audience is that send's recipients with no human open","marketing_send",VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("resend-after-days","Resend after","Days after the first message before someone counts as a non-opener",VENTURE_FIELD_KIND_INTEGER,VENTURE_COLUMN_FLAG_NONE),
	/* A text campaign (docs/telephony.org): the text body goes by text
	 * message to people with text consent, through the telephony send path. */
	VENTURE_FIELD_ENUM("channel","Channel","email, or text for a text message campaign",venture_marketing_channel_get_type,VENTURE_COLUMN_FLAG_NONE)
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
	UNIQUE("recipient-key","Retained audience identity"),
	/* Deliverability evidence (docs/deliverability.org). opened-at stays
	 * "first open not judged a machine's", so rows from before the
	 * heuristic keep counting as they always did. */
	DATE("delivered-at","Provider reported delivered at"), DATE("replied-at","First reply at"),
	DATE("complained-at","Complaint at"), DATE("machine-opened-at","First machine-judged open"),
	/* A/B arm (a, b or rest) and the second arm's frozen content, which a
	 * rest recipient takes if B wins; and the local-time due moment. */
	STR("variant","A/B arm"), STR("variant-subject","Rendered B subject"),
	VENTURE_FIELD_TEXT("variant-text-body","Rendered B text",NULL), VENTURE_FIELD_TEXT("variant-html-body","Rendered B HTML",NULL),
	SECRET("variant-private-text-body","Private B delivery text"), SECRET("variant-private-html-body","Private B delivery HTML"),
	SECRET("variant-links","Original B tracking destinations"),
	DATE("due-at","Due at")
};
VENTURE_DEFINE_ENTITY(VentureMarketingRecipient, venture_marketing_recipient, recipient_fields)
static const VentureFieldDecl event_fields[] = {
	REF("recipient-id","Recipient","marketing_recipient"), STR("kind","Evidence kind"),
	INT("link-position","Link position"), DATE("occurred-at","Occurred at"),
	VENTURE_FIELD_TEXT("source","Evidence source",NULL), UNIQUE("event-key","Evidence identity"),
	STR("machine-reason","Judged a machine's because")
};
VENTURE_DEFINE_ENTITY(VentureMarketingEvent, venture_marketing_event, event_fields)
/* A sending domain is a setting: it decides how much every campaign may
 * send and which DKIM key the check looks for, so only an owner or
 * administrator changes it (read by venture_access_policy_can()). */
static const VentureFieldDecl sending_domain_fields[] = {
	VENTURE_FIELD("domain","Domain","The domain in your campaigns' From address, for example example.com",VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD("dkim-selector","DKIM selector","The name your mail provider gave the DKIM key: the part before ._domainkey",VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("warmup-started-on","Warm-up started","Day one of this domain's sending; empty is the day the first campaign message was accepted",VENTURE_FIELD_KIND_DATE,VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("ramp-schedule","Daily limits","day:limit pairs, for example 1:50,3:100,30:0 (0 is no limit); empty is the default; off turns the warm-up off",VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("notes","Notes",NULL,VENTURE_FIELD_KIND_TEXT,VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureMarketingSendingDomain, venture_marketing_sending_domain, sending_domain_fields,
	g_type_set_qdata(G_TYPE_FROM_CLASS(klass), g_quark_from_static_string("venture-access-manager-write"), GINT_TO_POINTER(1));)
/* What a person can turn off on the preference page. */
static const VentureFieldDecl topic_fields[] = {
	VENTURE_FIELD_NAME("name","Name","What people choose, for example Product news"),
	VENTURE_FIELD_TEXT("description","Description","Shown beside the choice on the preference page"),
	VENTURE_FIELD("active","Offered","Shown on the preference page",VENTURE_FIELD_KIND_BOOLEAN,VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY(VentureMarketingTopic, venture_marketing_topic, topic_fields)
/* One address's choices, written only from its preference page or by an
 * operator recording what the person asked for. Absent is "everything". */
static const VentureFieldDecl preference_fields[] = {
	UNIQUE("email","Normalized address"),
	STR("topics-off","Topics turned off"),
	VENTURE_FIELD_ENUM("frequency","How often","At most one marketing message a week or a month; any is no limit",venture_marketing_frequency_get_type,VENTURE_COLUMN_FLAG_NONE),
	DATE("chosen-at","Chosen at"),
	VENTURE_FIELD_TEXT("source","Recorded from",NULL)
};
VENTURE_DEFINE_ENTITY(VentureMarketingPreference, venture_marketing_preference, preference_fields)
