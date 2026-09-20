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
static const VentureFieldDecl submission_fields[] = {
	REF("site-id","Site","attribution_site"), PRIVATE_REF("visitor-id","Consenting visitor","attribution_visitor"),
	SUBJECTS, TOUCHES, INT("lookback-days","Frozen lookback days"), DATE("submitted-at","Accepted form time"), UNIQUE("submission-key","Submission replay identity"),
	SECRET("payload-hash","Exact submission digest"), REF("marketing-consent-id","Independent email permission","marketing_consent"),
	STR("marketing-status","Email permission outcome"), STR("marketing-policy","Requested email policy"),
	VENTURE_FIELD_TEXT("marketing-statement","Requested email permission statement",NULL), DATE("marketing-requested-at","Email permission occurrence"),
	REF("connection-id","Verified Lightsite account","integration_connection"), INT("connection-version","Verified account configuration")
};
VENTURE_DEFINE_ENTITY(VentureAttributionSubmission, venture_attribution_submission, submission_fields)
static const VentureFieldDecl binding_fields[] = {
	SUBJECTS, RETAIN("deal-id","Converted deal","deal"), TOUCHES,
	INT("first-lookback-days","Original first-touch lookback days"), INT("last-lookback-days","Latest last-touch lookback days"),
	DATE("captured-at","First accepted acquisition form"), DATE("latest-submitted-at","Latest accepted acquisition form"),
	REF("submission-id","Acquisition submission","attribution_submission"), DATE("bound-at","Acquisition binding time"),
	UNIQUE("binding-key","Original conversion identity")
};
VENTURE_DEFINE_ENTITY(VentureAttributionBinding, venture_attribution_binding, binding_fields)
