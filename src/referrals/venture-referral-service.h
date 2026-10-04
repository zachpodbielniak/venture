/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_REFERRAL_SERVICE_H
#define VENTURE_REFERRAL_SERVICE_H
G_BEGIN_DECLS
typedef struct _VentureReportRegistry VentureReportRegistry;

/**
 * venture_referrals_install:
 * @database: the repository
 *
 * Registers the referral save validators and the handlers that make a
 * lead arriving with a code into a referral, carry the lead's status onto
 * it, and give the referrer their reward once the outermost transaction
 * that won the customer has committed. Idempotent.
 */
void venture_referrals_install(VentureDatabase *database);

/**
 * venture_referrals_actions_register:
 * @database: the repository
 *
 * Registers "referral_code" on company and contact (the customer's code,
 * made on first use) and "apply" on a reward that could not be given.
 */
void venture_referrals_actions_register(VentureDatabase *database);

/**
 * venture_referrals_check_removal:
 * @entity: a record about to be deleted
 * @error: (out) (optional): the refusal
 *
 * A reward that was given is evidence of money that moved; it is refused
 * removal. Everything else is left to the generic rules.
 *
 * Returns: whether the removal may proceed
 */
gboolean venture_referrals_check_removal(VentureEntity *entity, GError **error);

/**
 * venture_referrals_register_reports:
 * @registry: the report registry
 *
 * Registers "referrals": referrals per referrer, conversion of the
 * referred leads and the rewards given.
 */
void venture_referrals_register_reports(VentureReportRegistry *registry);

/**
 * venture_referral_find_code:
 * @database: the repository
 * @organization_id: the organization the code must belong to
 * @code: (nullable): the code as somebody typed or passed it
 * @error: (out) (optional): a database error; an unknown code is not one
 *
 * Looks a code up the way an arriving lead does: without regard to case
 * or surrounding space, in one organization, live and active, in an
 * active program.
 *
 * Returns: (transfer full) (nullable): the code, or %NULL when none matches
 */
VentureEntity *venture_referral_find_code(VentureDatabase *database, gint64 organization_id,
	const gchar *code, GError **error);

/**
 * venture_referral_code_for:
 * @database: the repository
 * @referrer: a saved company or contact
 * @program_id: the program, or 0 for the organization's only active one
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): the refusal
 *
 * The referrer's code in a program, made on first use. With no program
 * named, an organization with no active program or with several is
 * refused with what to do: the code has to say which reward it earns.
 *
 * Returns: (transfer full) (nullable): the code
 */
VentureEntity *venture_referral_code_for(VentureDatabase *database, VentureEntity *referrer,
	gint64 program_id, const VentureActor *actor, GError **error);

/**
 * venture_referral_reward_apply:
 * @database: the repository
 * @reward: a saved reward that was not given
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): why it still cannot be given
 *
 * Gives a reward again after its cause was fixed: the subscription
 * started, the module switched on, the approval granted. A reward that was
 * given is refused, so a retry can never pay twice.
 *
 * Returns: whether the reward is now given
 */
gboolean venture_referral_reward_apply(VentureDatabase *database, VentureEntity *reward,
	const VentureActor *actor, GError **error);

G_END_DECLS
#endif
