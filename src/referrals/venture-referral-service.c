/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

/*
 * Referrals hang off writes that do not know about them. A lead saved with
 * a code -- by the API, a lead form, a form, the relay, an import -- is
 * seen on entity-saved and becomes a referral; the same handler carries
 * the lead's status onto it. Both writes join whatever transaction saved
 * the lead, so a capture that rolls back takes its referral with it.
 *
 * The reward is different. It writes a credit note or credits a
 * subscription, either of which the books or an approval rule may refuse,
 * and a refusal inside the conversion's transaction would roll the
 * conversion back with it: winning the customer must not depend on paying
 * the referrer. So a referral that becomes won is only remembered, and the
 * reward is given once the outermost transaction has committed, in a
 * transaction of its own. One that cannot be given is recorded with why.
 */

typedef struct _VentureReferralService VentureReferralService;
struct _VentureReferralService {
	VentureDatabase *database;
	gboolean installed;
	gboolean rewarding;
	GArray *won;
	VentureEntity *writing;
};

#define REFERRAL_CODE_ALPHABET "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
#define REFERRAL_CODE_LENGTH 8
#define REFERRAL_CODE_MAX 32

static void
service_free(gpointer data)
{
	VentureReferralService *self = data;
	g_array_unref(self->won);
	g_free(self);
}

static VentureReferralService *
service_get(VentureDatabase *database)
{
	VentureReferralService *self = g_object_get_data(G_OBJECT(database), "venture-referral-service");
	if (self == NULL)
	{
		self = g_new0(VentureReferralService, 1);
		/* Not a reference: the database owns the service through its data. */
		self->database = database;
		self->won = g_array_new(FALSE, FALSE, sizeof(gint64));
		g_object_set_data_full(G_OBJECT(database), "venture-referral-service", self, service_free);
	}
	return self;
}

static gboolean
refuse(GError **error, const gchar *field, const gchar *message)
{
	venture_set_error_validation(error, field, "%s", message);
	return FALSE;
}

static gint64
number(VentureEntity *e, const gchar *name)
{
	gint64 value = 0;
	g_object_get(e, name, &value, NULL);
	return value;
}

static gint
choice(VentureEntity *e, const gchar *name)
{
	gint value = 0;
	g_object_get(e, name, &value, NULL);
	return value;
}

static gboolean
flag(VentureEntity *e, const gchar *name)
{
	gboolean value = FALSE;
	g_object_get(e, name, &value, NULL);
	return value;
}

static gchar *
text(VentureEntity *e, const gchar *name)
{
	gchar *value = NULL;
	g_object_get(e, name, &value, NULL);
	return value;
}

static gboolean
enabled(const gchar *type)
{
	return venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), type);
}

/* A live row of @type in @org, or NULL with an error naming the field. */
static VentureEntity *
load(VentureDatabase *database, GType type, gint64 id, gint64 org, const gchar *field, GError **error)
{
	g_autoptr(GError) local = NULL;
	VentureEntity *row = venture_database_get(database, type, id, &local);
	if (local != NULL)
	{
		g_propagate_error(error, g_steal_pointer(&local));
		return NULL;
	}
	if (row == NULL || venture_entity_is_deleted(row) || venture_entity_get_organization_id(row) != org)
	{
		g_clear_object(&row);
		refuse(error, field, "names a record that is not in this organization");
		return NULL;
	}
	return row;
}

static GPtrArray *
find(VentureDatabase *database, GType type, gint64 org, const gchar *field, gint64 value,
	gboolean deleted, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, deleted);
	venture_query_set_limit(query, 0);
	if (field != NULL && !venture_query_add_filter_int(query, field, VENTURE_FILTER_OP_EQ, value, error))
		return NULL;
	if (!venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, error))
		return NULL;
	return venture_database_find(database, query, error);
}

/* Codes are compared as stored: trimmed and upper case. */
static gchar *
normalize_code(const gchar *code)
{
	g_autofree gchar *copy = g_strstrip(g_strdup(code != NULL ? code : ""));
	return g_ascii_strup(copy, -1);
}

static gboolean
code_well_formed(const gchar *code)
{
	gsize i, length = strlen(code);
	if (length < 3 || length > REFERRAL_CODE_MAX || !g_ascii_isalnum(code[0]))
		return FALSE;
	for (i = 0; i < length; i++)
		if (!g_ascii_isalnum(code[i]) && code[i] != '-' && code[i] != '_')
			return FALSE;
	return TRUE;
}

/* Soft-deleted codes count: the organization's unique index includes them. */
static gboolean
code_taken(VentureDatabase *database, gint64 org, const gchar *code, gint64 self_id, gboolean *taken, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_REFERRAL_CODE);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	venture_query_set_organization(query, org);
	venture_query_set_include_deleted(query, TRUE);
	if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, code, error))
		return FALSE;
	rows = venture_database_find(database, query, error);
	if (rows == NULL)
		return FALSE;
	*taken = FALSE;
	for (i = 0; i < rows->len; i++)
		if (venture_entity_get_id(g_ptr_array_index(rows, i)) != self_id)
			*taken = TRUE;
	return TRUE;
}

static gchar *
make_code(VentureDatabase *database, gint64 org, GError **error)
{
	guint attempt;
	for (attempt = 0; attempt < 16; attempt++)
	{
		gchar code[REFERRAL_CODE_LENGTH + 1];
		gboolean taken = TRUE;
		guint i;
		for (i = 0; i < REFERRAL_CODE_LENGTH; i++)
			code[i] = REFERRAL_CODE_ALPHABET[g_random_int_range(0, (gint32)strlen(REFERRAL_CODE_ALPHABET))];
		code[REFERRAL_CODE_LENGTH] = '\0';
		if (!code_taken(database, org, code, 0, &taken, error))
			return NULL;
		if (!taken)
			return g_strdup(code);
	}
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT, "Could not make an unused referral code; try again");
	return NULL;
}

/* The organization's only active program; 0 when it has none or several. */
static gboolean
only_program(VentureDatabase *database, gint64 org, gint64 *program_id, guint *active, GError **error)
{
	g_autoptr(GPtrArray) programs = find(database, VENTURE_TYPE_REFERRAL_PROGRAM, org, NULL, 0, FALSE, error);
	guint i;
	if (programs == NULL)
		return FALSE;
	*program_id = 0;
	*active = 0;
	for (i = 0; i < programs->len; i++)
	{
		VentureEntity *program = g_ptr_array_index(programs, i);
		if (!flag(program, "active"))
			continue;
		(*active)++;
		*program_id = venture_entity_get_id(program);
	}
	if (*active != 1)
		*program_id = 0;
	return TRUE;
}

/* --- Validators ----------------------------------------------------------- */

static gboolean
validate_program(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous,
	gpointer data, GError **error)
{
	g_autofree gchar *landing = text(entity, "landing-url");
	g_autoptr(VentureMoney) amount = NULL;
	(void)database;
	(void)previous;
	(void)data;
	g_object_get(entity, "reward-amount", &amount, NULL);
	if (choice(entity, "reward-kind") == VENTURE_REFERRAL_REWARD_CREDIT &&
	    (amount == NULL || venture_money_get_amount(amount) <= 0))
		return refuse(error, "reward-amount", "A credit reward needs a positive amount");
	if (!venture_string_is_empty(landing))
	{
		g_autoptr(GUri) uri = g_uri_parse(landing, G_URI_FLAGS_NONE, NULL);
		const gchar *scheme = uri != NULL ? g_uri_get_scheme(uri) : NULL;
		if (uri == NULL || venture_string_is_empty(g_uri_get_host(uri)) || g_uri_get_fragment(uri) != NULL ||
		    (g_strcmp0(scheme, "https") != 0 && g_strcmp0(scheme, "http") != 0))
			return refuse(error, "landing-url", "The landing page must be an http or https address without a fragment");
	}
	return TRUE;
}

static gchar *
code_link(VentureEntity *program, const gchar *code)
{
	g_autofree gchar *landing = text(program, "landing-url");
	g_autofree gchar *escaped = NULL;
	if (venture_string_is_empty(landing))
		return NULL;
	escaped = g_uri_escape_string(code, NULL, FALSE);
	return g_strdup_printf("%s%sref=%s", landing, strchr(landing, '?') != NULL ? "&" : "?", escaped);
}

static gboolean
validate_code(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous,
	gpointer data, GError **error)
{
	gint64 org = venture_entity_get_organization_id(entity);
	g_autofree gchar *raw = text(entity, "code");
	g_autofree gchar *code = normalize_code(raw);
	g_autofree gchar *link = NULL;
	g_autofree gchar *stored = previous != NULL ? text(previous, "code") : NULL;
	g_autoptr(VentureEntity) program = NULL;
	gint64 program_id = number(entity, "program-id");
	gboolean taken = FALSE;
	(void)data;
	if (number(entity, "company-id") == 0 && number(entity, "contact-id") == 0)
		return refuse(error, "company-id", "A code needs a referrer: the customer or contact who shares it");
	if (program_id == 0)
	{
		guint active = 0;
		if (!only_program(database, org, &program_id, &active, error))
			return FALSE;
		if (program_id == 0)
			return refuse(error, "program-id", active == 0
				? "Name the program: this organization has no active referral program"
				: "Name the program: this organization has more than one active referral program");
		g_object_set(entity, "program-id", program_id, NULL);
	}
	program = load(database, VENTURE_TYPE_REFERRAL_PROGRAM, program_id, org, "program-id", error);
	if (program == NULL)
		return FALSE;
	if (venture_string_is_empty(code))
	{
		g_free(code);
		code = make_code(database, org, error);
		if (code == NULL)
			return FALSE;
	}
	if (!code_well_formed(code))
		return refuse(error, "code", "A code is 3 to 32 letters, digits, hyphens or underscores, starting with a letter or digit");
	if (g_strcmp0(stored, code) != 0)
	{
		if (!code_taken(database, org, code, venture_entity_get_id(entity), &taken, error))
			return FALSE;
		if (taken)
			return refuse(error, "code", "That code is already used in this organization");
	}
	/* Derived from the program on every save, so a form posting the old
	 * link back after the program's page moved cannot keep it stale. */
	link = code_link(program, code);
	g_object_set(entity, "code", code, "link", link, NULL);
	return TRUE;
}

static VentureReferralStatus
status_for_lead(VentureLeadStatus state)
{
	switch (state)
	{
	case VENTURE_LEAD_QUALIFIED: return VENTURE_REFERRAL_QUALIFIED;
	case VENTURE_LEAD_CONVERTED: return VENTURE_REFERRAL_WON;
	case VENTURE_LEAD_UNQUALIFIED: return VENTURE_REFERRAL_LOST;
	case VENTURE_LEAD_NEW:
	case VENTURE_LEAD_WORKING:
	case VENTURE_LEAD_RECYCLED:
	default: return VENTURE_REFERRAL_PENDING;
	}
}

static gboolean
validate_referral(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous,
	gpointer data, GError **error)
{
	gint64 org = venture_entity_get_organization_id(entity);
	gint64 code_id = number(entity, "code-id"), lead_id = number(entity, "lead-id");
	gint64 referrer_company, referrer_contact, company, contact;
	gint status;
	g_autoptr(GDateTime) won_at = NULL;
	(void)data;
	if (code_id != 0 && (previous == NULL || number(previous, "code-id") != code_id))
	{
		g_autoptr(VentureEntity) code = load(database, VENTURE_TYPE_REFERRAL_CODE, code_id, org, "code-id", error);
		if (code == NULL)
			return FALSE;
		/* The code says who sent them and which program pays; a referrer
		 * typed beside it would be a second answer to the same question. */
		g_object_set(entity, "referrer-company-id", number(code, "company-id"),
			"referrer-contact-id", number(code, "contact-id"), "program-id", number(code, "program-id"), NULL);
	}
	if (number(entity, "program-id") == 0 && previous == NULL)
	{
		gint64 program_id = 0;
		guint active = 0;
		if (!only_program(database, org, &program_id, &active, error))
			return FALSE;
		g_object_set(entity, "program-id", program_id, NULL);
	}
	if (lead_id != 0)
	{
		g_autoptr(VentureEntity) lead = load(database, VENTURE_TYPE_LEAD, lead_id, org, "lead-id", error);
		VentureLeadStatus state;
		if (lead == NULL)
			return FALSE;
		if (previous == NULL || number(previous, "lead-id") != lead_id)
		{
			g_autoptr(GPtrArray) others = find(database, VENTURE_TYPE_REFERRAL, org, "lead-id", lead_id, FALSE, error);
			guint i;
			if (others == NULL)
				return FALSE;
			for (i = 0; i < others->len; i++)
				if (venture_entity_get_id(g_ptr_array_index(others, i)) != venture_entity_get_id(entity))
					return refuse(error, "lead-id", "That lead is already recorded as referred");
		}
		/* The lead decides. A status typed here that disagrees is put back
		 * rather than refused: a form posts every field back. */
		g_object_get(lead, "status", &state, NULL);
		g_object_set(entity, "status", status_for_lead(state), NULL);
		if (state == VENTURE_LEAD_CONVERTED)
		{
			if (number(entity, "company-id") == 0)
				g_object_set(entity, "company-id", number(lead, "converted-company-id"), NULL);
			if (number(entity, "contact-id") == 0)
				g_object_set(entity, "contact-id", number(lead, "converted-contact-id"), NULL);
		}
	}
	referrer_company = number(entity, "referrer-company-id");
	referrer_contact = number(entity, "referrer-contact-id");
	company = number(entity, "company-id");
	contact = number(entity, "contact-id");
	if (referrer_company == 0 && referrer_contact == 0)
		return refuse(error, "referrer-company-id", "A referral needs a referrer: the customer or contact who sent them");
	if (lead_id == 0 && company == 0 && contact == 0)
		return refuse(error, "company-id", "A referral needs who was referred: a lead, a customer or a contact");
	if ((referrer_company != 0 && referrer_company == company) || (referrer_contact != 0 && referrer_contact == contact))
		return refuse(error, "company-id", "A customer cannot be recorded as referring themselves");
	/* Won follows the status: stamped when empty, kept when given, cleared
	 * on leaving won. */
	status = choice(entity, "status");
	g_object_get(entity, "won-at", &won_at, NULL);
	if (status == VENTURE_REFERRAL_WON && won_at == NULL)
	{
		g_autoptr(GDateTime) now = venture_time_now();
		g_object_set(entity, "won-at", now, NULL);
	}
	else if (status != VENTURE_REFERRAL_WON && won_at != NULL)
		g_object_set(entity, "won-at", NULL, NULL);
	return TRUE;
}

/* Only pay() writes a reward: one typed in would say money moved that
 * never did, and the zero status is "applied". Once given, a reward only
 * records; what it gave is in the books. */
static gboolean
validate_reward(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous,
	gpointer data, GError **error)
{
	VentureReferralService *self = data;
	(void)database;
	if (self->writing != entity)
		return refuse(error, "status", "Rewards are given by the referral service; retry one with its apply action");
	if (number(entity, "referral-id") == 0)
		return refuse(error, "referral-id", "A reward is for a referral");
	if (previous != NULL && choice(previous, "status") == VENTURE_REFERRAL_REWARD_APPLIED &&
	    (choice(entity, "status") != VENTURE_REFERRAL_REWARD_APPLIED ||
	     number(entity, "credit-id") != number(previous, "credit-id") ||
	     number(entity, "subscription-id") != number(previous, "subscription-id")))
		return refuse(error, "status", "A reward that was given cannot be changed; record a correction in the books");
	return TRUE;
}

/* --- Giving the reward ------------------------------------------------------ */

/* Whom a reward is paid to: the referring customer, or the company of the
 * referring contact. */
static gint64
paid_company(VentureDatabase *database, VentureEntity *referral)
{
	gint64 company = number(referral, "referrer-company-id");
	if (company == 0 && number(referral, "referrer-contact-id") != 0)
	{
		g_autoptr(VentureEntity) contact = venture_database_get(database, VENTURE_TYPE_CONTACT,
			number(referral, "referrer-contact-id"), NULL);
		if (contact != NULL)
			company = number(contact, "company-id");
	}
	return company;
}

static gboolean
give_credit(VentureDatabase *database, VentureEntity *reward, VentureEntity *program,
	VentureEntity *referral, const VentureActor *actor, GError **error)
{
	gint64 org = venture_entity_get_organization_id(reward);
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureEntity) credit = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *reference = g_strdup_printf("Referral reward for referral #%" G_GINT64_FORMAT,
		venture_entity_get_id(referral));
	if (!enabled("customer_credit"))
		return refuse(error, "kind", "A credit reward needs the receivables module, which is disabled");
	if (number(reward, "company-id") == 0)
		return refuse(error, "company-id", "The referrer has no company to credit");
	g_object_get(program, "reward-amount", &amount, NULL);
	if (amount == NULL || venture_money_get_amount(amount) <= 0)
		return refuse(error, "amount", "The program has no positive credit amount");
	credit = VENTURE_ENTITY(venture_customer_credit_new());
	venture_entity_set_organization_id(credit, org);
	g_object_set(credit, "customer-id", number(reward, "company-id"), "kind", "credit_note", "date", now,
		"amount", amount, "reference", reference, NULL);
	if (!venture_database_save(database, credit, actor, error))
		return FALSE;
	g_object_set(reward, "amount", amount, "credit-id", venture_entity_get_id(credit), NULL);
	return TRUE;
}

static gboolean
give_free_period(VentureDatabase *database, VentureEntity *reward, const VentureActor *actor, GError **error)
{
	gint64 org = venture_entity_get_organization_id(reward);
	g_autoptr(GPtrArray) subscriptions = NULL;
	g_autoptr(VentureMoney) credited = NULL;
	VentureEntity *chosen = NULL;
	guint i;
	if (!enabled("customer_subscription"))
		return refuse(error, "kind", "A free-period reward needs the billing module, which is disabled");
	if (number(reward, "company-id") == 0)
		return refuse(error, "company-id", "The referrer has no company with a subscription");
	subscriptions = find(database, VENTURE_TYPE_CUSTOMER_SUBSCRIPTION, org, "company-id", number(reward, "company-id"), FALSE, error);
	if (subscriptions == NULL)
		return FALSE;
	/* The oldest subscription that will renew: trialing, active or past due. */
	for (i = 0; i < subscriptions->len && chosen == NULL; i++)
	{
		VentureEntity *sub = g_ptr_array_index(subscriptions, i);
		if (choice(sub, "status") <= 2 && !flag(sub, "cancel-at-period-end"))
			chosen = sub;
	}
	if (chosen == NULL)
		return refuse(error, "subscription-id", "The referrer has no subscription that will renew to give a free period on");
	credited = venture_billing_service_grant_free_period(venture_billing_service_get(database),
		VENTURE_CUSTOMER_SUBSCRIPTION(chosen), actor, error);
	if (credited == NULL)
		return FALSE;
	g_object_set(reward, "amount", credited, "subscription-id", venture_entity_get_id(chosen), NULL);
	return TRUE;
}

static gboolean
write_reward(VentureDatabase *database, VentureEntity *reward, const VentureActor *actor, GError **error)
{
	VentureReferralService *self = service_get(database);
	gboolean ok;
	self->writing = reward;
	ok = venture_database_save(database, reward, actor, error);
	self->writing = NULL;
	return ok;
}

/* A copy that saves as the same row: duplicate() would insert another. */
static VentureEntity *
same_row(VentureEntity *reward)
{
	VentureEntity *copy = g_object_new(G_OBJECT_TYPE(reward), NULL);
	venture_entity_copy_properties_from(copy, reward, FALSE);
	return copy;
}

/*
 * Pays @reward in a transaction of its own and saves it as given, or rolls
 * the payment back and saves it as not given, with the reason. Each save
 * gets its own copy: a rolled-back save has already stamped the object it
 * was handed.
 */
static gboolean
pay(VentureDatabase *database, VentureEntity *reward, VentureEntity *referral, VentureEntity *program,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) attempt = same_row(reward);
	g_autoptr(VentureEntity) failed = NULL;
	g_autoptr(GError) why = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	gboolean ok;
	if (!venture_database_begin(database, error))
		return FALSE;
	g_object_set(attempt, "status", VENTURE_REFERRAL_REWARD_APPLIED, "applied-at", now, "failure", NULL, NULL);
	ok = choice(program, "reward-kind") == VENTURE_REFERRAL_REWARD_CREDIT
		? give_credit(database, attempt, program, referral, actor, &why)
		: give_free_period(database, attempt, actor, &why);
	if (ok)
		ok = write_reward(database, attempt, actor, &why);
	if (!ok)
		venture_database_rollback(database);
	else if (venture_database_commit(database, &why))
		return TRUE;
	failed = same_row(reward);
	g_object_set(failed, "status", VENTURE_REFERRAL_REWARD_FAILED, "applied-at", NULL,
		"failure", why != NULL ? why->message : "The reward could not be given", NULL);
	if (!write_reward(database, failed, actor, error))
		return FALSE;
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Reward not given: %s",
		why != NULL ? why->message : "unknown reason");
	return FALSE;
}

static void
give_reward(VentureReferralService *self, gint64 referral_id)
{
	VentureDatabase *database = self->database;
	g_autoptr(VentureEntity) referral = NULL;
	g_autoptr(VentureEntity) program = NULL;
	g_autoptr(VentureEntity) reward = NULL;
	g_autoptr(GPtrArray) existing = NULL;
	g_autoptr(GError) error = NULL;
	gint64 org;
	if (!enabled("referral_reward"))
		return;
	referral = venture_database_get(database, VENTURE_TYPE_REFERRAL, referral_id, &error);
	if (referral == NULL || venture_entity_is_deleted(referral) || choice(referral, "status") != VENTURE_REFERRAL_WON ||
	    number(referral, "program-id") == 0)
		goto done;
	org = venture_entity_get_organization_id(referral);
	/* Once per referral, counting a deleted reward, as the unique index does. */
	existing = find(database, VENTURE_TYPE_REFERRAL_REWARD, org, "referral-id", referral_id, TRUE, &error);
	if (existing == NULL || existing->len > 0)
		goto done;
	program = venture_database_get(database, VENTURE_TYPE_REFERRAL_PROGRAM, number(referral, "program-id"), &error);
	if (program == NULL || choice(program, "reward-kind") == VENTURE_REFERRAL_REWARD_NONE)
		goto done;
	reward = VENTURE_ENTITY(venture_referral_reward_new());
	venture_entity_set_organization_id(reward, org);
	g_object_set(reward, "referral-id", referral_id, "program-id", venture_entity_get_id(program),
		"kind", choice(program, "reward-kind"), "company-id", paid_company(database, referral),
		"contact-id", number(referral, "referrer-contact-id"),
		"referred-company-id", number(referral, "company-id"), NULL);
	/* A refusal is recorded on the reward itself, and that is its error
	 * code; anything else means even the record could not be written. */
	if (!pay(database, reward, referral, program, NULL, &error) &&
	    g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION))
		g_clear_error(&error);
done:
	if (error != NULL)
		g_message("referrals: referral #%" G_GINT64_FORMAT " reward not recorded: %s", referral_id, error->message);
}

static void
flush(VentureReferralService *self)
{
	if (self->rewarding)
		return;
	self->rewarding = TRUE;
	while (self->won->len > 0)
	{
		gint64 id = g_array_index(self->won, gint64, 0);
		g_array_remove_index(self->won, 0);
		give_reward(self, id);
	}
	self->rewarding = FALSE;
}

static void
transaction_finished(VentureDatabase *database, gboolean committed, gpointer data)
{
	VentureReferralService *self = data;
	(void)database;
	/* A rollback undid whatever won the customer, and the reason to pay
	 * with it. A reward's own rollback, while paying, leaves the rest. */
	if (!committed)
	{
		if (!self->rewarding)
			g_array_set_size(self->won, 0);
		return;
	}
	flush(self);
}

/* --- Following the lead ----------------------------------------------------- */

VentureEntity *
venture_referral_find_code(VentureDatabase *database, gint64 organization_id, const gchar *code, GError **error)
{
	g_autofree gchar *wanted = normalize_code(code);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureEntity) program = NULL;
	VentureEntity *found;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	if (venture_string_is_empty(wanted) || !enabled("referral_code"))
		return NULL;
	query = venture_query_new(VENTURE_TYPE_REFERRAL_CODE);
	venture_query_set_organization(query, organization_id);
	if (!venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, wanted, error))
		return NULL;
	rows = venture_database_find(database, query, error);
	if (rows == NULL || rows->len == 0)
		return NULL;
	found = g_ptr_array_index(rows, 0);
	if (flag(found, "retired"))
		return NULL;
	program = venture_database_get(database, VENTURE_TYPE_REFERRAL_PROGRAM, number(found, "program-id"), NULL);
	if (program == NULL || venture_entity_is_deleted(program) || !flag(program, "active"))
		return NULL;
	return g_object_ref(found);
}

static void
lead_saved(VentureReferralService *self, VentureEntity *lead)
{
	VentureDatabase *database = self->database;
	gint64 org = venture_entity_get_organization_id(lead);
	g_autoptr(GPtrArray) referrals = NULL;
	g_autoptr(GError) error = NULL;
	referrals = find(database, VENTURE_TYPE_REFERRAL, org, "lead-id", venture_entity_get_id(lead), FALSE, &error);
	if (referrals == NULL)
		goto fail;
	if (referrals->len > 0)
	{
		VentureEntity *referral = g_ptr_array_index(referrals, 0);
		VentureLeadStatus state;
		g_object_get(lead, "status", &state, NULL);
		if (choice(referral, "status") == (gint)status_for_lead(state) &&
		    (state != VENTURE_LEAD_CONVERTED || number(referral, "company-id") != 0))
			return;
		/* Set here, not left to the validator: a save with nothing changed
		 * returns before any validator runs. */
		g_object_set(referral, "status", status_for_lead(state), NULL);
		if (state == VENTURE_LEAD_CONVERTED && number(referral, "company-id") == 0)
			g_object_set(referral, "company-id", number(lead, "converted-company-id"), NULL);
		if (!venture_database_save(database, referral, NULL, &error))
			goto fail;
		return;
	}
	{
		g_autofree gchar *given = text(lead, "referral-code");
		g_autoptr(VentureEntity) code = NULL;
		g_autoptr(VentureEntity) referral = NULL;
		if (venture_string_is_empty(given))
			return;
		/* A code nobody issued is kept on the lead as it arrived and
		 * attributes nothing: refusing it would lose the inquiry. */
		code = venture_referral_find_code(database, org, given, &error);
		if (code == NULL)
		{
			if (error != NULL)
				goto fail;
			return;
		}
		referral = VENTURE_ENTITY(venture_referral_new());
		venture_entity_set_organization_id(referral, org);
		g_object_set(referral, "lead-id", venture_entity_get_id(lead), "code-id", venture_entity_get_id(code), NULL);
		if (!venture_database_save(database, referral, NULL, &error))
			goto fail;
	}
	return;
fail:
	g_message("referrals: lead #%" G_GINT64_FORMAT " was saved but its referral was not: %s",
		venture_entity_get_id(lead), error != NULL ? error->message : "unknown error");
}

static void
entity_saved(VentureDatabase *database, VentureEntity *entity, gboolean created, gpointer data)
{
	VentureReferralService *self = data;
	gint64 id;
	(void)created;
	if (!enabled("referral"))
		return;
	if (VENTURE_IS_LEAD(entity))
	{
		lead_saved(self, entity);
		return;
	}
	if (!VENTURE_IS_REFERRAL(entity) || choice(entity, "status") != VENTURE_REFERRAL_WON)
		return;
	id = venture_entity_get_id(entity);
	g_array_append_val(self->won, id);
	/* A save outside any transaction has committed already. */
	if (!venture_database_has_transaction(database))
		flush(self);
}

void
venture_referrals_install(VentureDatabase *database)
{
	VentureReferralService *self;
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	self = service_get(database);
	if (self->installed)
		return;
	self->installed = TRUE;
	venture_database_add_save_validator(database, VENTURE_TYPE_REFERRAL_PROGRAM, validate_program, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_REFERRAL_CODE, validate_code, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_REFERRAL, validate_referral, NULL, NULL);
	venture_database_add_save_validator(database, VENTURE_TYPE_REFERRAL_REWARD, validate_reward, self, NULL);
	g_signal_connect(database, "entity-saved", G_CALLBACK(entity_saved), self);
	g_signal_connect(database, "transaction-finished", G_CALLBACK(transaction_finished), self);
}

gboolean
venture_referrals_check_removal(VentureEntity *entity, GError **error)
{
	if (VENTURE_IS_REFERRAL_REWARD(entity) && choice(entity, "status") == VENTURE_REFERRAL_REWARD_APPLIED)
		return refuse(error, "status", "A reward that was given cannot be deleted; record a correction in the books");
	return TRUE;
}

/* --- Codes and retries ------------------------------------------------------- */

VentureEntity *
venture_referral_code_for(VentureDatabase *database, VentureEntity *referrer, gint64 program_id,
	const VentureActor *actor, GError **error)
{
	gint64 org;
	const gchar *field;
	g_autoptr(GPtrArray) codes = NULL;
	g_autoptr(VentureEntity) code = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	g_return_val_if_fail(VENTURE_IS_ENTITY(referrer), NULL);
	if (!enabled("referral_code"))
	{
		refuse(error, "referral_code", "The referrals module is disabled");
		return NULL;
	}
	if (VENTURE_IS_COMPANY(referrer))
		field = "company-id";
	else if (VENTURE_IS_CONTACT(referrer))
		field = "contact-id";
	else
	{
		refuse(error, "referrer", "Only a customer or a contact can share a referral code");
		return NULL;
	}
	if (!venture_entity_is_persisted(referrer) || venture_entity_is_deleted(referrer))
	{
		refuse(error, "referrer", "Save the referrer before giving them a code");
		return NULL;
	}
	org = venture_entity_get_organization_id(referrer);
	if (program_id == 0)
	{
		guint active = 0;
		if (!only_program(database, org, &program_id, &active, error))
			return NULL;
		if (program_id == 0)
		{
			refuse(error, "program_id", active == 0
				? "This organization has no active referral program; create one first"
				: "This organization has more than one active referral program; name one");
			return NULL;
		}
	}
	codes = find(database, VENTURE_TYPE_REFERRAL_CODE, org, field, venture_entity_get_id(referrer), FALSE, error);
	if (codes == NULL)
		return NULL;
	for (i = 0; i < codes->len; i++)
	{
		VentureEntity *candidate = g_ptr_array_index(codes, i);
		/* A contact's code is theirs, not the one their company shares. */
		if (VENTURE_IS_COMPANY(referrer) && number(candidate, "contact-id") != 0)
			continue;
		if (number(candidate, "program-id") == program_id && !flag(candidate, "retired"))
			return g_object_ref(candidate);
	}
	code = VENTURE_ENTITY(venture_referral_code_new());
	venture_entity_set_organization_id(code, org);
	g_object_set(code, field, venture_entity_get_id(referrer), "program-id", program_id, NULL);
	if (VENTURE_IS_CONTACT(referrer))
		g_object_set(code, "company-id", number(referrer, "company-id"), NULL);
	if (!venture_database_save(database, code, actor, error))
		return NULL;
	return g_steal_pointer(&code);
}

gboolean
venture_referral_reward_apply(VentureDatabase *database, VentureEntity *reward,
	const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureEntity) referral = NULL;
	g_autoptr(VentureEntity) program = NULL;
	gint64 org;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), FALSE);
	g_return_val_if_fail(VENTURE_IS_REFERRAL_REWARD(reward), FALSE);
	org = venture_entity_get_organization_id(reward);
	/* Judge the row, not the caller's copy: a reward given since it was
	 * read must not be given again. */
	current = load(database, VENTURE_TYPE_REFERRAL_REWARD, venture_entity_get_id(reward), org, "id", error);
	if (current == NULL)
		return FALSE;
	if (choice(current, "status") == VENTURE_REFERRAL_REWARD_APPLIED)
		return refuse(error, "status", "This reward was given already");
	referral = load(database, VENTURE_TYPE_REFERRAL, number(current, "referral-id"), org, "referral-id", error);
	if (referral == NULL)
		return FALSE;
	if (choice(referral, "status") != VENTURE_REFERRAL_WON)
		return refuse(error, "referral-id", "The referral is no longer won");
	program = load(database, VENTURE_TYPE_REFERRAL_PROGRAM, number(current, "program-id"), org, "program-id", error);
	if (program == NULL)
		return FALSE;
	if (choice(program, "reward-kind") != choice(current, "kind"))
		return refuse(error, "kind", "The program's reward has changed since; this reward cannot be given as recorded");
	/* The referrer may have gained a company or a subscription since. */
	g_object_set(current, "company-id", paid_company(database, referral), NULL);
	return pay(database, current, referral, program, actor, error);
}

/* --- Actions ----------------------------------------------------------------- */

static gboolean
code_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)entity;
	(void)actor;
	if (!enabled("referral_code"))
		return refuse(error, "referral_code", "The referrals module is disabled");
	return TRUE;
}

static VentureEntity *
code_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureReferralService *self = venture_action_get_data(action);
	JsonNode *node = params != NULL ? g_hash_table_lookup(params, "program_id") : NULL;
	gint64 program_id = node != NULL && JSON_NODE_HOLDS_VALUE(node) ? json_node_get_int(node) : 0;
	if (!code_allowed(action, entity, actor, error))
		return NULL;
	return venture_referral_code_for(self->database, entity, program_id, actor, error);
}

static gboolean
apply_allowed(VentureAction *action, VentureEntity *entity, const VentureActor *actor, GError **error)
{
	(void)action;
	(void)actor;
	if (choice(entity, "status") == VENTURE_REFERRAL_REWARD_APPLIED)
		return refuse(error, "status", "This reward was given already");
	return TRUE;
}

static VentureEntity *
apply_invoke(VentureAction *action, VentureEntity *entity, GHashTable *params, const VentureActor *actor, GError **error)
{
	VentureReferralService *self = venture_action_get_data(action);
	(void)params;
	if (!venture_referral_reward_apply(self->database, entity, actor, error))
		return NULL;
	return venture_database_get(self->database, VENTURE_TYPE_REFERRAL_REWARD, venture_entity_get_id(entity), error);
}

static void
register_action(VentureDatabase *database, const gchar *type, const gchar *name, const gchar *label,
	const gchar *description, GPtrArray *parameters, VentureActionAllowed allowed, VentureActionInvoke invoke)
{
	g_autoptr(VentureAction) action = NULL;
	g_autoptr(GError) error = NULL;
	action = g_object_new(VENTURE_TYPE_ACTION, "data-class", VENTURE_DATA_CLASS_TENANT, "type-name", type, "name", name,
		"label", label, "description", description, "parameters", parameters, "stageable", FALSE,
		"service-transaction", TRUE, "roles", VENTURE_USER_ROLE_EDITOR, NULL);
	if (!venture_action_registry_register(venture_database_get_action_registry(database), action, allowed, invoke,
		service_get(database), NULL, &error))
		g_error("Referral action registration: %s", error->message);
}

void
venture_referrals_actions_register(VentureDatabase *database)
{
	static const gchar *const referrers[] = { "company", "contact" };
	g_autoptr(GPtrArray) none = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
	guint i;
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	for (i = 0; i < G_N_ELEMENTS(referrers); i++)
	{
		g_autoptr(GPtrArray) parameters = g_ptr_array_new_with_free_func((GDestroyNotify)venture_field_spec_free);
		VentureFieldSpec *program = venture_field_spec_new("program_id", "Program", VENTURE_FIELD_KIND_INTEGER);
		program->has_min = TRUE;
		program->min_value = 1;
		g_ptr_array_add(parameters, program);
		register_action(database, referrers[i], "referral_code", "Referral code",
			"This referrer's shareable code and link, made on first use", parameters, code_allowed, code_invoke);
	}
	register_action(database, "referral_reward", "apply", "Give the reward",
		"Try again to give a reward that could not be given", none, apply_allowed, apply_invoke);
}
