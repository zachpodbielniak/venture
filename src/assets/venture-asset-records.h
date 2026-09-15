/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ASSET_RECORDS_H
#define VENTURE_ASSET_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * VentureAssetMethod:
 * @VENTURE_ASSET_METHOD_STRAIGHT_LINE: straight line
 * @VENTURE_ASSET_METHOD_DECLINING_BALANCE: declining balance
 * @VENTURE_ASSET_METHOD_NONE: none
 *
 * Book accounting lifecycle values.
 */
typedef enum { VENTURE_ASSET_METHOD_STRAIGHT_LINE, VENTURE_ASSET_METHOD_DECLINING_BALANCE, VENTURE_ASSET_METHOD_NONE } VentureAssetMethod;
GType venture_asset_method_get_type(void) G_GNUC_CONST;
/**
 * VentureAssetConvention:
 * @VENTURE_ASSET_CONVENTION_FULL_MONTH: full month
 * @VENTURE_ASSET_CONVENTION_HALF_YEAR: half year
 */
typedef enum { VENTURE_ASSET_CONVENTION_FULL_MONTH, VENTURE_ASSET_CONVENTION_HALF_YEAR } VentureAssetConvention;
GType venture_asset_convention_get_type(void) G_GNUC_CONST;
/**
 * VentureAssetStatus:
 * @VENTURE_ASSET_STATUS_DRAFT: draft
 * @VENTURE_ASSET_STATUS_IN_SERVICE: in service
 * @VENTURE_ASSET_STATUS_DISPOSED: disposed
 * @VENTURE_ASSET_STATUS_WRITTEN_OFF: written off
 *
 * Book accounting lifecycle values.
 */
typedef enum { VENTURE_ASSET_STATUS_DRAFT, VENTURE_ASSET_STATUS_IN_SERVICE, VENTURE_ASSET_STATUS_DISPOSED, VENTURE_ASSET_STATUS_WRITTEN_OFF } VentureAssetStatus;
/** venture_asset_status_get_type: Returns: the asset status enumeration */
GType venture_asset_status_get_type(void) G_GNUC_CONST;
/**
 * VentureScheduleState:
 * @VENTURE_SCHEDULE_STATE_SCHEDULED: scheduled
 * @VENTURE_SCHEDULE_STATE_POSTED: posted
 * @VENTURE_SCHEDULE_STATE_SKIPPED: skipped
 *
 * Book accounting lifecycle values.
 */
typedef enum { VENTURE_SCHEDULE_STATE_SCHEDULED, VENTURE_SCHEDULE_STATE_POSTED, VENTURE_SCHEDULE_STATE_SKIPPED } VentureScheduleState;
/** venture_schedule_state_get_type: Returns: the schedule state enumeration */
GType venture_schedule_state_get_type(void) G_GNUC_CONST;
/**
 * VentureDeferralKind:
 * @VENTURE_DEFERRAL_KIND_PREPAYMENT: prepayment
 * @VENTURE_DEFERRAL_KIND_ACCRUAL: accrual
 *
 * Book accounting lifecycle values.
 */
typedef enum { VENTURE_DEFERRAL_KIND_PREPAYMENT, VENTURE_DEFERRAL_KIND_ACCRUAL } VentureDeferralKind;
/** venture_deferral_kind_get_type: Returns: the deferral kind enumeration */
GType venture_deferral_kind_get_type(void) G_GNUC_CONST;
/**
 * VentureDeferralStatus:
 * @VENTURE_DEFERRAL_STATUS_ACTIVE: active
 * @VENTURE_DEFERRAL_STATUS_COMPLETE: complete
 *
 * Book accounting lifecycle values.
 */
typedef enum { VENTURE_DEFERRAL_STATUS_ACTIVE, VENTURE_DEFERRAL_STATUS_COMPLETE } VentureDeferralStatus;
/** venture_deferral_status_get_type: Returns: the deferral status enumeration */
GType venture_deferral_status_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_FIXED_ASSET (venture_fixed_asset_get_type())
VENTURE_DECLARE_ENTITY(VentureFixedAsset, venture_fixed_asset, FIXED_ASSET)
#define VENTURE_TYPE_DEPRECIATION_ENTRY (venture_depreciation_entry_get_type())
VENTURE_DECLARE_ENTITY(VentureDepreciationEntry, venture_depreciation_entry, DEPRECIATION_ENTRY)
#define VENTURE_TYPE_DEFERRAL (venture_deferral_get_type())
VENTURE_DECLARE_ENTITY(VentureDeferral, venture_deferral, DEFERRAL)
#define VENTURE_TYPE_DEFERRAL_ENTRY (venture_deferral_entry_get_type())
VENTURE_DECLARE_ENTITY(VentureDeferralEntry, venture_deferral_entry, DEFERRAL_ENTRY)
#define VENTURE_TYPE_TAX_DEPRECIATION_ENTRY (venture_tax_depreciation_entry_get_type())
VENTURE_DECLARE_ENTITY(VentureTaxDepreciationEntry, venture_tax_depreciation_entry, TAX_DEPRECIATION_ENTRY)
G_END_DECLS
#endif
