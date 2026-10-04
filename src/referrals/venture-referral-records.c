/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

GType
venture_referral_reward_kind_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_REFERRAL_REWARD_NONE, "VENTURE_REFERRAL_REWARD_NONE", "none" },
			{ VENTURE_REFERRAL_REWARD_CREDIT, "VENTURE_REFERRAL_REWARD_CREDIT", "credit" },
			{ VENTURE_REFERRAL_REWARD_FREE_PERIOD, "VENTURE_REFERRAL_REWARD_FREE_PERIOD", "free_period" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureReferralRewardKind", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

GType
venture_referral_status_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_REFERRAL_PENDING, "VENTURE_REFERRAL_PENDING", "pending" },
			{ VENTURE_REFERRAL_QUALIFIED, "VENTURE_REFERRAL_QUALIFIED", "qualified" },
			{ VENTURE_REFERRAL_WON, "VENTURE_REFERRAL_WON", "won" },
			{ VENTURE_REFERRAL_LOST, "VENTURE_REFERRAL_LOST", "lost" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureReferralStatus", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

GType
venture_referral_reward_status_get_type(void)
{
	static gsize type_id = 0;
	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ VENTURE_REFERRAL_REWARD_APPLIED, "VENTURE_REFERRAL_REWARD_APPLIED", "applied" },
			{ VENTURE_REFERRAL_REWARD_FAILED, "VENTURE_REFERRAL_REWARD_FAILED", "failed" },
			{ 0, NULL, NULL }
		};
		GType id = g_enum_register_static("VentureReferralRewardStatus", values);
		g_once_init_leave(&type_id, id);
	}
	return type_id;
}

static const VentureFieldDecl program_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", NULL),
	VENTURE_FIELD("active", "Accepting referrals", "Codes of an inactive program attribute nothing", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("reward-kind", "Reward", "What the referrer is given for each customer won", venture_referral_reward_kind_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("reward-amount", "Credit per customer won", "Credit rewards: the credit note given to the referrer"),
	VENTURE_FIELD("landing-url", "Landing page", "Where a shared link points; the code is added as ?ref=", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureReferralProgram, venture_referral_program, program_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Referral program", NULL);)

/* A code is titled by itself: it is what a customer reads out. */
static gchar *
code_display_name(VentureEntity *self)
{
	g_autofree gchar *code = NULL;
	g_object_get(self, "code", &code, NULL);
	if (!venture_string_is_empty(code)) return g_steal_pointer(&code);
	return g_strdup_printf("Referral code #%" G_GINT64_FORMAT, venture_entity_get_id(self));
}

static const VentureFieldDecl code_fields[] = {
	VENTURE_FIELD("code", "Code", "Left blank, one is made", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION | VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("program-id", "Program", NULL, "referral_program", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("company-id", "Customer", "The customer who shares it", "company", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("contact-id", "Contact", "The person who shares it", "contact", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("link", "Link to share", "The program's landing page carrying this code", VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("retired", "Retired", "A retired code attributes nothing; unticked, the zero value, a new code works", VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureReferralCode, venture_referral_code, code_fields,
	VENTURE_ENTITY_CLASS(klass)->get_display_name = code_display_name;
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Referral code", NULL);)

static const VentureFieldDecl referral_fields[] = {
	VENTURE_FIELD_ENUM("status", "Status", "Follows the referred lead when there is one", venture_referral_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("referrer-company-id", "Referred by", "The customer who sent them", "company", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("referrer-contact-id", "Referred by contact", "The person who sent them", "contact", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("lead-id", "Lead", "The inquiry that arrived", "lead", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("company-id", "Customer", "Who was referred, once a customer", "company", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("contact-id", "Contact", "Who was referred", "contact", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("code-id", "Code", "The code they arrived with", "referral_code", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("program-id", "Program", "Decides the reward", "referral_program", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("won-at", "Won", "When the referred lead became a customer", VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureReferral, venture_referral, referral_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Referral", NULL);)

static const VentureFieldDecl reward_fields[] = {
	VENTURE_FIELD_ENUM("status", "Status", NULL, venture_referral_reward_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("kind", "Reward", NULL, venture_referral_reward_kind_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_MONEY("amount", "Value", "The credit given, or what the free period was worth"),
	VENTURE_FIELD_REF("company-id", "Given to", "The referrer", "company", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("contact-id", "Given to contact", "The referrer", "contact", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("referred-company-id", "For referring", "The customer won", "company", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("referral-id", "Referral", NULL, "referral", VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION),
	VENTURE_FIELD_REF("program-id", "Program", NULL, "referral_program", VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_REF("credit-id", "Credit note", NULL, "customer_credit", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("subscription-id", "Subscription", "Whose next invoice was credited", "customer_subscription", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("applied-at", "Given", NULL, VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_TEXT("failure", "Why it was not given", NULL)
};
VENTURE_DEFINE_ENTITY_WITH_CODE(VentureReferralReward, venture_referral_reward, reward_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Referral reward", NULL);)
