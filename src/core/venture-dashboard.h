/*
 * venture-dashboard.h - Dashboards: pages of widgets, as many as you like
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A dashboard is a #VentureDashboard record and the #VentureDashboardWidget
 * records that point at it. Both are ordinary record types -- they have a
 * table, REST CRUD, a form and an audit trail like everything else -- and
 * what this file adds is the meaning of a widget: the catalogue of kinds
 * (a count, a list, a report, a note, ...), what each kind reads from the
 * widget's settings, and the one function that turns a widget into
 * something to show.
 *
 * A widget kind produces both JSON and HTML from one pass over the data,
 * so the browser, the API, venturectl and the assistant all see the same
 * answer; there is no second implementation to drift. A plugin registers
 * a kind the way it registers a report, into the process-wide registry.
 *
 * Nothing here decides who may see a dashboard beyond
 * venture_dashboard_is_visible_to(); the web layer and the API ask that
 * and enforce it.
 */

#ifndef VENTURE_DASHBOARD_H
#define VENTURE_DASHBOARD_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VentureWidgetScope:
 * @organization_ids: (nullable) (array length=n_organizations): the
 *   entities the viewer has selected, primary first; %NULL for every entity
 * @n_organizations: how many
 * @user_id: the viewer's user id, or 0
 * @username: (nullable): the viewer's name, what `{me}` in a filter means
 * @venture_id: a venture to narrow every widget that can be to, or 0
 *
 * Who is looking, and at what. Filled by the web layer from the session
 * and the entity picker; by the API and the assistant from the token. A
 * widget never scopes itself -- the same widget on a page and in an API
 * call must answer about the same rows.
 */
typedef struct
{
	const gint64	*organization_ids;
	gsize		 n_organizations;
	gint64		 user_id;
	const gchar	*username;
	gint64		 venture_id;
} VentureWidgetScope;

/**
 * VentureWidgetResult:
 * @title: the card's title, the widget's own or the kind's default
 * @html: the card body, an HTML fragment; %NULL when @error is set
 * @data: the same answer as JSON, in a shape the kind decides
 * @link: (nullable): where "all" leads, a site-relative path
 * @link_label: (nullable): what to call that link
 * @error: (nullable): why the widget could not be shown, in words
 *
 * What a widget kind produces. A failure is a result too: one widget
 * pointing at a report that no longer exists must not take the page down,
 * so the error is carried and rendered in place.
 */
struct _VentureWidgetResult
{
	gchar		*title;
	gchar		*html;
	JsonNode	*data;
	gchar		*link;
	gchar		*link_label;
	gchar		*error;
};

/**
 * venture_widget_result_new:
 *
 * Returns: (transfer full): an empty result
 */
VentureWidgetResult *
venture_widget_result_new(void);

/**
 * venture_widget_result_free:
 * @self: a #VentureWidgetResult
 */
void
venture_widget_result_free(VentureWidgetResult *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureWidgetResult, venture_widget_result_free)

/**
 * VentureWidgetKindFunc:
 * @context: the wiring
 * @widget: the widget, with its settings
 * @scope: who is looking
 * @user_data: the data given at registration
 * @error: (out) (optional): return location for a #GError
 *
 * The implementation of one widget kind. It reads what it needs from
 * @widget, answers within @scope, and fills a result with HTML and JSON.
 * Returning %NULL with @error set is a failure the page renders in place.
 *
 * Returns: (transfer full) (nullable): the result
 */
typedef VentureWidgetResult * (*VentureWidgetKindFunc) (
	VentureContext			 *context,
	VentureDashboardWidget		 *widget,
	const VentureWidgetScope	 *scope,
	gpointer			  user_data,
	GError				**error
);

/**
 * VentureWidgetKindInfo:
 * @name: the machine name, `[a-z0-9_]+`, what a widget's `kind` field holds
 * @label: the human name
 * @description: what the kind shows, in a sentence
 * @module: (nullable): the module the kind belongs to; a kind whose module
 *   is off says so instead of rendering
 * @uses: (nullable) (array zero-terminated=1): the widget fields the kind
 *   reads, in the wire spelling; the editor shows only these
 * @func: the implementation
 *
 * The static description of a widget kind.
 */
typedef struct
{
	const gchar		 *name;
	const gchar		 *label;
	const gchar		 *description;
	const gchar		 *module;
	const gchar *const	 *uses;
	VentureWidgetKindFunc	  func;
} VentureWidgetKindInfo;

#define VENTURE_TYPE_WIDGET_KIND_REGISTRY \
	(venture_widget_kind_registry_get_type())

G_DECLARE_FINAL_TYPE(VentureWidgetKindRegistry, venture_widget_kind_registry,
                     VENTURE, WIDGET_KIND_REGISTRY, GObject)

/**
 * venture_widget_kind_registry_get_default:
 *
 * The process-wide catalogue of widget kinds, with the built-in kinds
 * registered. Process-wide for the reason the entity registry is: a
 * plugin registers a kind once, and every context in the process offers
 * it.
 *
 * Returns: (transfer none): the registry
 */
VentureWidgetKindRegistry *
venture_widget_kind_registry_get_default(void);

/**
 * venture_widget_kind_registry_add:
 * @self: a #VentureWidgetKindRegistry
 * @info: the kind; the strings and the array it points at must outlive the
 *   registry, which for a static table they do
 * @user_data: (nullable): passed to the kind's function
 * @destroy: (nullable): how to free @user_data
 * @error: (out) (optional): return location for a #GError
 *
 * Registers a widget kind. Refused when the name is malformed or taken.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_widget_kind_registry_add(
	VentureWidgetKindRegistry	 *self,
	const VentureWidgetKindInfo	 *info,
	gpointer			  user_data,
	GDestroyNotify			  destroy,
	GError				**error
);

/**
 * venture_widget_kind_registry_lookup:
 * @self: a #VentureWidgetKindRegistry
 * @name: a kind name
 *
 * Returns: (transfer none) (nullable): the kind, or %NULL if unknown
 */
const VentureWidgetKindInfo *
venture_widget_kind_registry_lookup(
	VentureWidgetKindRegistry	*self,
	const gchar			*name
);

/**
 * venture_widget_kind_registry_list_names:
 * @self: a #VentureWidgetKindRegistry
 *
 * Returns: (transfer full) (array zero-terminated=1): every kind's name, in
 *   registration order, which puts the built-in kinds first
 */
gchar **
venture_widget_kind_registry_list_names(VentureWidgetKindRegistry *self);

/**
 * venture_widget_kind_registry_describe:
 * @self: a #VentureWidgetKindRegistry
 * @context: (nullable): the wiring, to say which kinds are usable now
 *
 * Describes every kind: name, label, description, module, the fields it
 * uses, and -- given a context -- whether its module is on. This is what
 * `GET /api/v1/widget-kinds` returns and what the editor's picker is
 * built from.
 *
 * Returns: (transfer full): a JSON array
 */
JsonNode *
venture_widget_kind_registry_describe(
	VentureWidgetKindRegistry	*self,
	VentureContext			*context
);

/**
 * venture_dashboard_install_validators:
 * @context: the wiring
 *
 * Registers the save-time checks on both record types: a widget's kind
 * must be registered, its record type and report must exist, its options
 * must be JSON; a dashboard's slug must be unique among the living, and
 * marking one as home unmarks the rest. The context does this at
 * construction, so every writer -- the form, the API, an import, the
 * assistant -- is held to the same rules.
 */
void
venture_dashboard_install_validators(VentureContext *context);

/**
 * venture_dashboard_find_by_slug:
 * @database: the database
 * @slug: the slug
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the dashboard, or %NULL with
 *   %VENTURE_ERROR_NOT_FOUND
 */
VentureDashboard *
venture_dashboard_find_by_slug(
	VentureDatabase	 *database,
	const gchar	 *slug,
	GError		**error
);

/**
 * venture_dashboard_find_home:
 * @database: the database
 * @user_id: who is asking
 *
 * Finds the dashboard marked as the home page, if there is one the viewer
 * may see.
 *
 * Returns: (transfer full) (nullable): the dashboard, or %NULL
 */
VentureDashboard *
venture_dashboard_find_home(
	VentureDatabase	*database,
	gint64		 user_id
);

/**
 * venture_dashboard_list_visible:
 * @database: the database
 * @user_id: who is asking
 * @error: (out) (optional): return location for a #GError
 *
 * Lists every dashboard the viewer may see, by purpose, then position,
 * then name. A personal dashboard is listed only for its owner.
 *
 * Returns: (transfer container) (element-type VentureDashboard): the
 *   dashboards
 */
GPtrArray *
venture_dashboard_list_visible(
	VentureDatabase	 *database,
	gint64		  user_id,
	GError		**error
);

/**
 * venture_dashboard_is_visible_to:
 * @dashboard: a dashboard
 * @user_id: who is asking
 *
 * Returns: %TRUE unless the dashboard is personal and @user_id is not its
 *   owner
 */
gboolean
venture_dashboard_is_visible_to(
	VentureDashboard	*dashboard,
	gint64			 user_id
);

/**
 * venture_dashboard_list_widgets:
 * @database: the database
 * @dashboard_id: the dashboard
 * @error: (out) (optional): return location for a #GError
 *
 * Lists a dashboard's widgets in page order: by position, then by id.
 *
 * Returns: (transfer container) (element-type VentureDashboardWidget): the
 *   widgets
 */
GPtrArray *
venture_dashboard_list_widgets(
	VentureDatabase	 *database,
	gint64		  dashboard_id,
	GError		**error
);

/**
 * VentureWidgetPlacement:
 * @widget: the widget
 * @col: the 1-based column it occupies
 * @row: the 1-based row
 * @width: how many columns it spans
 * @height: how many rows it spans
 * @placed: %TRUE if the widget asked for this spot, %FALSE if it flowed
 *   into the first free one
 *
 * Where a widget sits on a dashboard's grid, resolved.
 */
typedef struct
{
	VentureDashboardWidget	*widget;
	guint			 col;
	guint			 row;
	guint			 width;
	guint			 height;
	gboolean		 placed;
} VentureWidgetPlacement;

/**
 * venture_dashboard_layout:
 * @database: the database
 * @dashboard: the dashboard
 * @error: (out) (optional): return location for a #GError
 *
 * Resolves every widget's place on the grid. A widget with an explicit
 * column and row keeps it when it fits the layout and touches nothing
 * placed before it (page order); every other widget -- unplaced, off the
 * edge after the layout lost a column, or overlapping -- flows into the
 * first free cells in reading order. The result is always a valid
 * tiling, whatever the rows hold, and is sorted by row then column.
 *
 * Returns: (transfer container) (element-type VentureWidgetPlacement):
 *   the placements; each holds a reference to its widget
 */
GPtrArray *
venture_dashboard_layout(
	VentureDatabase		 *database,
	VentureDashboard	 *dashboard,
	GError			**error
);

/**
 * venture_widget_placement_free:
 * @placement: a placement
 */
void
venture_widget_placement_free(VentureWidgetPlacement *placement);

/**
 * venture_dashboard_place_widget:
 * @database: the database
 * @dashboard: the dashboard
 * @widget: the widget to place, saved on success
 * @col: the 1-based column, or 0 to let it flow
 * @row: the 1-based row, or 0 to let it flow
 * @width: columns to span, 1 to the layout's count
 * @height: rows to span, 1 to 8
 * @actor: (nullable): who is responsible
 * @error: (out) (optional): return location for a #GError
 *
 * Puts a widget somewhere on the grid, refusing a spot that falls off
 * the edge or is taken by another widget -- the refusal names it. This
 * is what the editor's drag, its nudge buttons and the API all call.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_dashboard_place_widget(
	VentureDatabase		 *database,
	VentureDashboard	 *dashboard,
	VentureDashboardWidget	 *widget,
	guint			  col,
	guint			  row,
	guint			  width,
	guint			  height,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_dashboard_nudge_widget:
 * @database: the database
 * @dashboard: the dashboard
 * @widget: the widget
 * @direction: `up`, `down`, `left`, `right`, `wider`, `narrower`, `taller`
 *   or `shorter`
 * @actor: (nullable): who is responsible
 * @error: (out) (optional): return location for a #GError
 *
 * Moves or resizes a widget by one cell, from where it currently sits --
 * placed or flowed -- through venture_dashboard_place_widget(), so the
 * same refusals apply. Growing into a taken cell is refused; shrinking
 * below one cell, or moving off the top or left edge, is not an error
 * and does nothing.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_dashboard_nudge_widget(
	VentureDatabase		 *database,
	VentureDashboard	 *dashboard,
	VentureDashboardWidget	 *widget,
	const gchar		 *direction,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_dashboard_arrange:
 * @database: the database
 * @dashboard: the dashboard
 * @actor: (nullable): who is responsible
 * @error: (out) (optional): return location for a #GError
 *
 * Tidies the grid: every widget is placed where the flow would put it,
 * in page order, and the placement is written down, so a page that
 * grew holes as widgets were removed closes them. Only rows whose
 * placement changes are saved.
 *
 * Returns: %TRUE on success
 */
gboolean
venture_dashboard_arrange(
	VentureDatabase		 *database,
	VentureDashboard	 *dashboard,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_dashboard_move_widget:
 * @database: the database
 * @widget: the widget to move
 * @direction: negative to move it earlier, positive to move it later
 * @actor: (nullable): who is responsible
 * @error: (out) (optional): return location for a #GError
 *
 * Swaps a widget's position with its neighbour. Positions that were never
 * set are first normalised to tens in page order, so the first move on a
 * freshly built page does what it looks like it should.
 *
 * Returns: %TRUE on success, including a move that had nowhere to go
 */
gboolean
venture_dashboard_move_widget(
	VentureDatabase		 *database,
	VentureDashboardWidget	 *widget,
	gint			  direction,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_dashboard_render_widget:
 * @context: the wiring
 * @widget: the widget
 * @scope: who is looking
 *
 * Turns a widget into something to show. Never fails: an unknown kind, a
 * module that is off, a report that errored -- each becomes a result with
 * its error set, which the page renders in place of the body.
 *
 * Returns: (transfer full): the result
 */
VentureWidgetResult *
venture_dashboard_render_widget(
	VentureContext			*context,
	VentureDashboardWidget		*widget,
	const VentureWidgetScope	*scope
);

/**
 * venture_dashboard_describe:
 * @context: the wiring
 * @dashboard: the dashboard
 * @scope: who is looking
 * @with_data: whether to evaluate every widget, or only list them
 * @error: (out) (optional): return location for a #GError
 *
 * Describes a dashboard as JSON: its own fields, and its widgets in page
 * order, each with its settings and -- when @with_data -- the answer it
 * gives right now. This is what `GET /api/v1/dashboards/:slug`, the
 * assistant's dashboard tool and venturectl all return.
 *
 * Returns: (transfer full) (nullable): a JSON object, or %NULL on error
 */
JsonNode *
venture_dashboard_describe(
	VentureContext			 *context,
	VentureDashboard		 *dashboard,
	const VentureWidgetScope	 *scope,
	gboolean			  with_data,
	GError				**error
);

/**
 * venture_dashboard_export:
 * @database: the database
 * @dashboard: the dashboard
 * @error: (out) (optional): return location for a #GError
 *
 * Renders a dashboard as a portable definition: its settings and its
 * widgets' settings, with no ids, owners or timestamps, so the file
 * imports cleanly on another install. The templates that ship with
 * VENTURE are written in this same shape.
 *
 * Returns: (transfer full) (nullable): a JSON object, or %NULL on error
 */
JsonNode *
venture_dashboard_export(
	VentureDatabase	 *database,
	VentureDashboard *dashboard,
	GError		**error
);

/**
 * venture_dashboard_import:
 * @context: the wiring
 * @definition: a definition as venture_dashboard_export() writes it
 * @owner_user_id: who will own it, or 0
 * @actor: (nullable): who is responsible
 * @error: (out) (optional): return location for a #GError
 *
 * Creates a dashboard and its widgets from a definition. The slug is
 * made unique by suffixing a number if it is taken, so importing the same
 * file twice gives two dashboards rather than a refusal. Every widget is
 * saved through the ordinary path, so one naming a kind or a type that
 * does not exist fails the import as a whole, and nothing half-made is
 * left behind.
 *
 * Returns: (transfer full) (nullable): the new dashboard, or %NULL
 */
VentureDashboard *
venture_dashboard_import(
	VentureContext		 *context,
	JsonNode		 *definition,
	gint64			  owner_user_id,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * VentureDashboardTemplate:
 * @name: the template's machine name
 * @label: the human name
 * @description: what the page is for
 * @module: (nullable): the module the template is mainly about; it is
 *   offered regardless, because a widget for a module that is off says so
 *   and the rest of the page still works
 * @definition: the dashboard, as venture_dashboard_export() writes it
 *
 * A dashboard VENTURE knows how to build for you.
 */
typedef struct
{
	const gchar	*name;
	const gchar	*label;
	const gchar	*description;
	const gchar	*module;
	const gchar	*definition;
} VentureDashboardTemplate;

/**
 * venture_dashboard_get_templates:
 * @n_templates: (out): return location for the count
 *
 * Returns: (transfer none) (array length=n_templates): the templates
 */
const VentureDashboardTemplate *
venture_dashboard_get_templates(gsize *n_templates);

/**
 * venture_dashboard_create_from_template:
 * @context: the wiring
 * @template_name: which template
 * @owner_user_id: who will own it, or 0
 * @actor: (nullable): who is responsible
 * @error: (out) (optional): return location for a #GError
 *
 * Builds a dashboard from a shipped template, through
 * venture_dashboard_import().
 *
 * Returns: (transfer full) (nullable): the new dashboard, or %NULL
 */
VentureDashboard *
venture_dashboard_create_from_template(
	VentureContext		 *context,
	const gchar		 *template_name,
	gint64			  owner_user_id,
	const VentureActor	 *actor,
	GError			**error
);

/**
 * venture_dashboard_widget_get_default_title:
 * @context: the wiring
 * @widget: the widget
 *
 * The title a widget shows when it was given none: the kind's label, made
 * specific by what it reads -- "Open tickets" for a count of tickets,
 * "Profit and loss" for that report.
 *
 * Returns: (transfer full): the title
 */
gchar *
venture_dashboard_widget_get_default_title(
	VentureContext		*context,
	VentureDashboardWidget	*widget
);

G_END_DECLS

#endif /* VENTURE_DASHBOARD_H */
