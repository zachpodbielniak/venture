/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SEQUENCE_RECORDS_H
#define VENTURE_SEQUENCE_RECORDS_H
G_BEGIN_DECLS
/**
 * venture_sequence_goal_get_type:
 * Returns: the goal enumeration
 */
GType venture_sequence_goal_get_type(void) G_GNUC_CONST;
/**
 * venture_sequence_channel_get_type:
 * Returns: the channel enumeration
 */
GType venture_sequence_channel_get_type(void) G_GNUC_CONST;
/**
 * venture_sequence_status_get_type:
 * Returns: the status enumeration
 */
GType venture_sequence_status_get_type(void) G_GNUC_CONST;
/**
 * venture_sequence_delivery_state_get_type:
 * Returns: the delivery state enumeration
 */
GType venture_sequence_delivery_state_get_type(void) G_GNUC_CONST;
/**
 * venture_sequence_suppression_reason_get_type:
 * Returns: the suppression reason enumeration
 */
GType venture_sequence_suppression_reason_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_SEQUENCE (venture_sequence_get_type())
VENTURE_DECLARE_ENTITY(VentureSequence, venture_sequence, SEQUENCE)
#define VENTURE_TYPE_SEQUENCE_STEP (venture_sequence_step_get_type())
VENTURE_DECLARE_ENTITY(VentureSequenceStep, venture_sequence_step, SEQUENCE_STEP)
#define VENTURE_TYPE_SEQUENCE_ENROLLMENT (venture_sequence_enrollment_get_type())
VENTURE_DECLARE_ENTITY(VentureSequenceEnrollment, venture_sequence_enrollment, SEQUENCE_ENROLLMENT)
#define VENTURE_TYPE_SEQUENCE_DELIVERY (venture_sequence_delivery_get_type())
VENTURE_DECLARE_ENTITY(VentureSequenceDelivery, venture_sequence_delivery, SEQUENCE_DELIVERY)
#define VENTURE_TYPE_SUPPRESSION (venture_suppression_get_type())
VENTURE_DECLARE_ENTITY(VentureSuppression, venture_suppression, SUPPRESSION)
G_END_DECLS
#endif
