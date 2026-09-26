/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#define STR(n,l) VENTURE_FIELD(n,l,NULL,VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_NONE)
#define INT(n,l) VENTURE_FIELD(n,l,NULL,VENTURE_FIELD_KIND_INTEGER,VENTURE_COLUMN_FLAG_NONE)
#define DATE(n,l) VENTURE_FIELD(n,l,NULL,VENTURE_FIELD_KIND_DATETIME,VENTURE_COLUMN_FLAG_INDEXED)
#define SECRET(n,l) VENTURE_FIELD(n,l,NULL,VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_SENSITIVE)
#define REF(n,l,t) VENTURE_FIELD_REF(n,l,NULL,t,VENTURE_COLUMN_FLAG_INDEXED)
#define PRIVATE_REF(n,l,t) VENTURE_FIELD_REF(n,l,"Private anonymous linkage",t,VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_SENSITIVE)
#define RETAIN(n,l,t) VENTURE_FIELD_REF(n,l,"Original attribution evidence; retained through CRM merge",t,VENTURE_COLUMN_FLAG_INDEXED | VENTURE_COLUMN_FLAG_RETAIN_REFERENCE)
#define UNIQUE(n,l) VENTURE_FIELD(n,l,NULL,VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION)
#define SUBJECTS RETAIN("lead-id","Captured lead","lead"), RETAIN("contact-id","Captured contact","contact"), RETAIN("company-id","Captured company","company")
#define TOUCHES RETAIN("first-touch-id","First touch","attribution_touch"), RETAIN("last-touch-id","Last touch","attribution_touch"), STR("first-source","First-touch source"), STR("last-source","Last-touch source"), REF("first-campaign-id","First-touch campaign","campaign"), REF("last-campaign-id","Last-touch campaign","campaign"), STR("first-evidence","First-touch evidence"), STR("last-evidence","Last-touch evidence")
static const VentureFieldDecl site_fields[] = {
	VENTURE_FIELD_NAME("name","Name",NULL), STR("origin","Allowed HTTPS site origin"),
	STR("external-site-id","Lightsite site identity"), STR("external-tenant-id","Lightsite tenant identity"),
	REF("lead-form-id","Capture field mapping","lead_form"), REF("connection-id","Lightsite account","integration_connection"),
	VENTURE_FIELD("campaign-map","UTM campaign mapping","JSON object of external campaign labels to same-organization campaign IDs",VENTURE_FIELD_KIND_JSON,VENTURE_COLUMN_FLAG_NONE),
	STR("consent-policy","Anonymous analytics consent policy version"),
	STR("marketing-policy","Optional email permission policy version"),
	VENTURE_FIELD_TEXT("marketing-statement","Approved email permission statement",NULL),
	INT("lookback-days","First/last-touch lookback days"), INT("retention-days","Anonymous analytics retention days"),
	VENTURE_FIELD("active","Accept capture and analytics",NULL,VENTURE_FIELD_KIND_BOOLEAN,VENTURE_COLUMN_FLAG_NONE),
	UNIQUE("site-key","Derived external site identity")
};
VENTURE_DEFINE_ENTITY(VentureAttributionSite, venture_attribution_site, site_fields)
static const VentureFieldDecl visitor_fields[] = {
	REF("site-id","Site","attribution_site"),
	VENTURE_FIELD("token-hash","Private visitor capability hash",NULL,VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_UNIQUE | VENTURE_COLUMN_FLAG_SENSITIVE),
	SECRET("session-key","Private current session identity"), INT("site-version","Accepted site configuration"), STR("consent-policy","Accepted analytics policy"),
	STR("consent-source","Anonymous consent source"), DATE("consented-at","Analytics consent received"),
	DATE("expires-at","Analytics permission expires"), DATE("withdrawn-at","Analytics withdrawn"), DATE("redacted-at","Analytics identity removed"),
	DATE("last-seen-at","Latest accepted observation"), INT("event-count","Accepted observation count"), INT("submission-count","Accepted form count"),
	DATE("rate-window-at","Current observation window"), INT("rate-count","Observations in window")
};
VENTURE_DEFINE_ENTITY(VentureAttributionVisitor, venture_attribution_visitor, visitor_fields)
static const VentureFieldDecl touch_fields[] = {
	REF("site-id","Site","attribution_site"), PRIVATE_REF("visitor-id","Consenting visitor","attribution_visitor"),
	SECRET("session-key","Private session identity"), DATE("occurred-at","Server receipt time"),
	SECRET("path","Private page path without query or fragment"), SECRET("referrer-origin","Referring origin without path"),
	STR("source","Observed source"), STR("medium","Observed medium"), STR("campaign-label","Observed campaign label"),
	REF("campaign-id","Verified campaign","campaign"), VENTURE_FIELD("event-key","Observation replay identity",NULL,VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_SENSITIVE), SECRET("payload-hash","Exact observation digest")
};
VENTURE_DEFINE_ENTITY(VentureAttributionTouch, venture_attribution_touch, touch_fields)
/*
 * A form somebody filled in on a site. Who wrote in and what they wrote
 * come first, because that is what a person opens one to read; the
 * attribution evidence follows, and the replay and configuration
 * identities that make capture idempotent are machinery.
 */
#define TECH_INT(n,l) VENTURE_FIELD(n,l,NULL,VENTURE_FIELD_KIND_INTEGER,VENTURE_COLUMN_FLAG_TECHNICAL)
static const VentureFieldDecl submission_fields[] = {
	VENTURE_FIELD("sender-name","Name","Who wrote in, as they typed it",VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("sender-email","Email","The address they gave",VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_SEARCHABLE | VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("message","Message","What they wrote, exactly as submitted"),
	RETAIN("contact-id","Contact","contact"), RETAIN("lead-id","Lead","lead"), RETAIN("company-id","Company","company"),
	DATE("submitted-at","Received"), REF("site-id","Site","attribution_site"),
	STR("marketing-status","Email permission"), REF("marketing-consent-id","Permission record","marketing_consent"),
	STR("marketing-policy","Permission asked for"),
	VENTURE_FIELD_TEXT("marketing-statement","Permission wording shown",NULL), DATE("marketing-requested-at","Permission given at"),
	TOUCHES, PRIVATE_REF("visitor-id","Consenting visitor","attribution_visitor"),
	TECH_INT("lookback-days","Attribution window (days)"),
	VENTURE_FIELD("submission-key","Submission replay identity",NULL,VENTURE_FIELD_KIND_STRING,VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_TECHNICAL),
	SECRET("payload-hash","Exact submission digest"),
	REF("connection-id","Lightsite account","integration_connection"), TECH_INT("connection-version","Account configuration revision")
};
/* Titled by whoever wrote in; a submission is somebody, not a number. */
static gchar *
submission_display_name(VentureEntity *self)
{
	static const gchar *const names[] = { "sender-name", "sender-email", NULL };
	gsize i;
	for (i = 0; names[i] != NULL; i++) {
		g_autofree gchar *text = NULL;
		g_object_get(self, names[i], &text, NULL);
		if (!venture_string_is_empty(text)) return g_steal_pointer(&text);
	}
	return g_strdup_printf("Form submission #%" G_GINT64_FORMAT, venture_entity_get_id(self));
}
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureAttributionSubmission, venture_attribution_submission, submission_fields,
	VENTURE_ENTITY_CLASS(klass)->get_display_name = submission_display_name;
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Form submission", NULL);)
static const VentureFieldDecl binding_fields[] = {
	SUBJECTS, RETAIN("deal-id","Converted deal","deal"), TOUCHES,
	INT("first-lookback-days","Original first-touch lookback days"), INT("last-lookback-days","Latest last-touch lookback days"),
	DATE("captured-at","First accepted acquisition form"), DATE("latest-submitted-at","Latest accepted acquisition form"),
	REF("submission-id","Acquisition submission","attribution_submission"), DATE("bound-at","Acquisition binding time"),
	UNIQUE("binding-key","Original conversion identity")
};
VENTURE_DEFINE_ENTITY(VentureAttributionBinding, venture_attribution_binding, binding_fields)
