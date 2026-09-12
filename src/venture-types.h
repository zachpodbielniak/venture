/*
 * venture-types.h - Forward declarations and common typedefs
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every VENTURE type is forward declared here so that headers can refer to
 * each other without circular includes. Nothing in this header pulls in a
 * dependency beyond GLib/GObject, which is what keeps libventure-core.a
 * buildable without the database, web or AI stacks.
 */

#ifndef VENTURE_TYPES_H
#define VENTURE_TYPES_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* --- Boxed value types --------------------------------------------------- */

typedef struct _VentureMoney		VentureMoney;
typedef struct _VentureDateRange	VentureDateRange;
typedef struct _VentureMetric		VentureMetric;
typedef struct _VentureFieldSpec	VentureFieldSpec;
typedef struct _VentureQueryFilter	VentureQueryFilter;
typedef struct _VentureColumnSpec	VentureColumnSpec;

/* --- Interfaces ---------------------------------------------------------- */

typedef struct _VentureSerializable		VentureSerializable;
typedef struct _VentureReportable		VentureReportable;
typedef struct _VentureToolProvider		VentureToolProvider;
typedef struct _VenturePlugin			VenturePlugin;

/* --- Model --------------------------------------------------------------- */

typedef struct _VentureEntity			VentureEntity;
typedef struct _VentureEntityClass		VentureEntityClass;
typedef struct _VentureOrganization		VentureOrganization;
typedef struct _VentureVenture			VentureVenture;
typedef struct _VentureVentureType		VentureVentureType;
typedef struct _VentureProduct			VentureProduct;
typedef struct _VentureInventoryItem		VentureInventoryItem;
typedef struct _VentureInventoryTxn		VentureInventoryTxn;
typedef struct _VentureSale			VentureSale;
typedef struct _VentureExpense			VentureExpense;
typedef struct _VentureAccount			VentureAccount;
typedef struct _VentureLedgerEntry		VentureLedgerEntry;
typedef struct _VentureContact			VentureContact;
typedef struct _VentureInteraction		VentureInteraction;
typedef struct _VentureDeal			VentureDeal;
typedef struct _VentureCampaign			VentureCampaign;
typedef struct _VentureIdea			VentureIdea;
typedef struct _VentureResearchNote		VentureResearchNote;
typedef struct _VentureTask			VentureTask;
typedef struct _VentureDocument			VentureDocument;
typedef struct _VentureNewsletter		VentureNewsletter;
typedef struct _VentureSubscriber		VentureSubscriber;
typedef struct _VenturePost			VenturePost;
typedef struct _VentureTaxCategory		VentureTaxCategory;
typedef struct _VentureUser			VentureUser;
typedef struct _VentureApiToken			VentureApiToken;
typedef struct _VentureAuditEntry		VentureAuditEntry;

/* --- Forge integration ---------------------------------------------------- */

/* Named ForgeRepo rather than Repository: VentureRepository is already taken
 * below by the persistence layer, and one identifier meaning both "a git
 * repository" and "the thing that talks to the database" would be a needless
 * ambiguity in a codebase that has both. */
typedef struct _VentureForge			VentureForge;
typedef struct _VentureForgeRepo		VentureForgeRepo;
typedef struct _VentureForgeRule		VentureForgeRule;
typedef struct _VentureTicketRelation		VentureTicketRelation;
typedef struct _VentureRecordLink		VentureRecordLink;
typedef struct _VentureTicketLink		VentureTicketLink;
typedef struct _VentureForgeRun			VentureForgeRun;

/* --- The software factory ------------------------------------------------- */

typedef struct _VentureMilestone		VentureMilestone;
typedef struct _VentureRelease			VentureRelease;
typedef struct _VentureBuild			VentureBuild;
typedef struct _VentureEnvironment		VentureEnvironment;
typedef struct _VentureDeployment		VentureDeployment;
typedef struct _VentureIncident			VentureIncident;
typedef struct _VentureWorkService		VentureWorkService;

/* --- Dashboards ----------------------------------------------------------- */

typedef struct _VentureDashboard		VentureDashboard;
typedef struct _VentureDashboardWidget		VentureDashboardWidget;
typedef struct _VentureWidgetKindRegistry	VentureWidgetKindRegistry;
typedef struct _VentureWidgetResult		VentureWidgetResult;

/* --- Registries and infrastructure --------------------------------------- */

typedef struct _VentureEntityRegistry		VentureEntityRegistry;
typedef struct _VentureModule			VentureModule;
typedef struct _VentureModuleRegistry		VentureModuleRegistry;
typedef struct _VentureVentureTypeRegistry	VentureVentureTypeRegistry;
typedef struct _VentureConfig			VentureConfig;
typedef struct _VentureContext			VentureContext;
typedef struct _VentureConfirmation		VentureConfirmation;
typedef struct _VentureConfirmationStore	VentureConfirmationStore;
typedef struct _VentureApplication		VentureApplication;

/* --- Database ------------------------------------------------------------ */

typedef struct _VentureDatabase			VentureDatabase;
typedef struct _VentureRepository		VentureRepository;
typedef struct _VentureQuery			VentureQuery;
typedef struct _VentureMigration		VentureMigration;
typedef struct _VentureSchema			VentureSchema;

/* --- Reporting ----------------------------------------------------------- */

typedef struct _VentureReport			VentureReport;
typedef struct _VentureReportClass		VentureReportClass;
typedef struct _VentureReportResult		VentureReportResult;
typedef struct _VentureReportRegistry		VentureReportRegistry;

/* --- AI ------------------------------------------------------------------ */

typedef struct _VentureAiService		VentureAiService;
typedef struct _VentureKbService		VentureKbService;
typedef struct _VentureEmbedder		VentureEmbedder;
typedef struct _VentureAiSession		VentureAiSession;
typedef struct _VentureAiToolRegistry		VentureAiToolRegistry;

/* A staged change is #VentureConfirmation, above: it stopped being about AI
 * when a token-authenticated REST write could stage one too. */

/* --- Automation ---------------------------------------------------------- */

typedef struct _VentureAutomation		VentureAutomation;

/* --- Plugins ------------------------------------------------------------- */

typedef struct _VenturePluginManager		VenturePluginManager;
typedef struct _VenturePluginInfo		VenturePluginInfo;

/* --- Web ----------------------------------------------------------------- */

typedef struct _VentureWebServer			VentureWebServer;
typedef struct _VentureAuth			VentureAuth;
typedef struct _VentureSession			VentureSession;

/* --- Common function types ----------------------------------------------- */

/**
 * VentureEntityForeachFunc:
 * @entity: the entity being visited
 * @user_data: caller data
 *
 * Callback invoked for each entity in a result set.
 *
 * Returns: %TRUE to continue iterating, %FALSE to stop early
 */
typedef gboolean (*VentureEntityForeachFunc) (
	VentureEntity	*entity,
	gpointer	 user_data
);

/**
 * VentureProgressFunc:
 * @fraction: completion between 0.0 and 1.0
 * @message: (nullable): a human-readable status line
 * @user_data: caller data
 *
 * Progress callback used by long-running operations such as migrations,
 * bulk imports and report generation.
 */
typedef void (*VentureProgressFunc) (
	gdouble		 fraction,
	const gchar	*message,
	gpointer	 user_data
);

G_END_DECLS

#endif /* VENTURE_TYPES_H */
