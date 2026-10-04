/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_REFERRAL_RECORDS_H
#define VENTURE_REFERRAL_RECORDS_H
G_BEGIN_DECLS

/**
 * VentureReferralRewardKind:
 * @VENTURE_REFERRAL_REWARD_NONE: referrals are tracked and nothing is given
 * @VENTURE_REFERRAL_REWARD_CREDIT: a credit note of the program's amount
 * @VENTURE_REFERRAL_REWARD_FREE_PERIOD: the referrer's next subscription invoice is credited in full
 *
 * What a referrer is given when somebody they sent becomes a customer.
 * None is the zero value, so a program written without choosing gives
 * nothing rather than money.
 */
typedef enum {
	VENTURE_REFERRAL_REWARD_NONE,
	VENTURE_REFERRAL_REWARD_CREDIT,
	VENTURE_REFERRAL_REWARD_FREE_PERIOD
} VentureReferralRewardKind;
/**
 * venture_referral_reward_kind_get_type:
 * Returns: the reward kind enum type
 */
GType venture_referral_reward_kind_get_type(void) G_GNUC_CONST;

/**
 * VentureReferralStatus:
 * @VENTURE_REFERRAL_PENDING: referred, not yet qualified
 * @VENTURE_REFERRAL_QUALIFIED: the referred lead is qualified
 * @VENTURE_REFERRAL_WON: the referred lead became a customer
 * @VENTURE_REFERRAL_LOST: the referred lead was ruled out
 *
 * Where a referral stands. With a lead it follows the lead's status.
 */
typedef enum {
	VENTURE_REFERRAL_PENDING,
	VENTURE_REFERRAL_QUALIFIED,
	VENTURE_REFERRAL_WON,
	VENTURE_REFERRAL_LOST
} VentureReferralStatus;
/**
 * venture_referral_status_get_type:
 * Returns: the referral status enum type
 */
GType venture_referral_status_get_type(void) G_GNUC_CONST;

/**
 * VentureReferralRewardStatus:
 * @VENTURE_REFERRAL_REWARD_APPLIED: given to the referrer
 * @VENTURE_REFERRAL_REWARD_FAILED: could not be given; the record says why
 */
typedef enum {
	VENTURE_REFERRAL_REWARD_APPLIED,
	VENTURE_REFERRAL_REWARD_FAILED
} VentureReferralRewardStatus;
/**
 * venture_referral_reward_status_get_type:
 * Returns: the reward status enum type
 */
GType venture_referral_reward_status_get_type(void) G_GNUC_CONST;

#define VENTURE_TYPE_REFERRAL_PROGRAM (venture_referral_program_get_type())
VENTURE_DECLARE_ENTITY(VentureReferralProgram, venture_referral_program, REFERRAL_PROGRAM)
#define VENTURE_TYPE_REFERRAL_CODE (venture_referral_code_get_type())
VENTURE_DECLARE_ENTITY(VentureReferralCode, venture_referral_code, REFERRAL_CODE)
#define VENTURE_TYPE_REFERRAL (venture_referral_get_type())
VENTURE_DECLARE_ENTITY(VentureReferral, venture_referral, REFERRAL)
#define VENTURE_TYPE_REFERRAL_REWARD (venture_referral_reward_get_type())
VENTURE_DECLARE_ENTITY(VentureReferralReward, venture_referral_reward, REFERRAL_REWARD)
/**
 * venture_referral_program_new:
 * Returns: (transfer full): an unsaved referral program
 */
/**
 * venture_referral_code_new:
 * Returns: (transfer full): an unsaved referral code
 */
/**
 * venture_referral_new:
 * Returns: (transfer full): an unsaved referral
 */
/**
 * venture_referral_reward_new:
 * Returns: (transfer full): an unsaved referral reward
 */
G_END_DECLS
#endif
