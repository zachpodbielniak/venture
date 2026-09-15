/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_TAX_RECORDS_H
#define VENTURE_TAX_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

#define VENTURE_TYPE_TAX_FILING (venture_tax_filing_get_type())
VENTURE_DECLARE_ENTITY(VentureTaxFiling, venture_tax_filing, TAX_FILING)
#define VENTURE_TYPE_CONTRACTOR_TAX_FORM (venture_contractor_tax_form_get_type())
VENTURE_DECLARE_ENTITY(VentureContractorTaxForm, venture_contractor_tax_form, CONTRACTOR_TAX_FORM)
#define VENTURE_TYPE_CONTRACTOR_TAX_PACK (venture_contractor_tax_pack_get_type())
VENTURE_DECLARE_ENTITY(VentureContractorTaxPack, venture_contractor_tax_pack, CONTRACTOR_TAX_PACK)

/**
 * venture_tax_filing_new:
 * Returns: (transfer full): a jurisdiction tax filing pack
 */
/**
 * venture_contractor_tax_form_new:
 * Returns: (transfer full): a contractor TIN record
 */
/**
 * venture_contractor_tax_pack_new:
 * Returns: (transfer full): a 1099-NEC pack for one vendor and year
 */

G_END_DECLS
#endif
