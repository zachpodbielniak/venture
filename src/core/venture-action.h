/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ACTION_H
#define VENTURE_ACTION_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_ACTION (venture_action_get_type())
G_DECLARE_FINAL_TYPE(VentureAction, venture_action, VENTURE, ACTION, GObject)
#define VENTURE_TYPE_ACTION_REGISTRY (venture_action_registry_get_type())
G_DECLARE_FINAL_TYPE(VentureActionRegistry, venture_action_registry, VENTURE, ACTION_REGISTRY, GObject)
/**
 * VentureActionAllowed:
 * @action: declaration
 * @entity: current record
 * @actor: (nullable): attribution
 * @error: (out) (optional): refusal reason
 * Returns: whether the action is currently allowed; must not write
 */
typedef gboolean (*VentureActionAllowed)(VentureAction *action, VentureEntity *entity,
	const VentureActor *actor, GError **error);
/**
 * VentureActionInvoke:
 * @action: declaration
 * @entity: current record
 * @params: (element-type utf8 JsonNode): validated parameter values
 * @actor: (nullable): attribution
 * @error: (out) (optional): refusal reason
 * Returns: (transfer full) (nullable): resulting record
 */
typedef VentureEntity *(*VentureActionInvoke)(VentureAction *action, VentureEntity *entity,
	GHashTable *params, const VentureActor *actor, GError **error);
/**
 * venture_action_get_data:
 * @self: declaration
 * Returns: (transfer none) (nullable): registration's service data
 */
gpointer venture_action_get_data(VentureAction *self);
/**
 * venture_action_registry_register: (skip)
 * @self: registry
 * @action: declaration, retained by the registry
 * @allowed: eligibility callback
 * @invoke: service callback
 * @data: (nullable): callback data
 * @destroy: (nullable): callback data destructor, on successful registration
 * @error: (out) (optional): registration failure
 *
 * C registration ABI: callbacks recover their shared service data through
 * venture_action_get_data(), rather than a callback closure argument. Language
 * binding trampolines cannot attach independent closures to these callbacks.
 * The registry owns the registration data until its destroy notification.
 * Returns: TRUE on success; duplicate keys are refused
 */
gboolean venture_action_registry_register(VentureActionRegistry *self, VentureAction *action,
	VentureActionAllowed allowed, VentureActionInvoke invoke, gpointer data,
	GDestroyNotify destroy, GError **error);
/**
 * venture_action_registry_lookup:
 * @self: registry
 * @type_name: registered singular or plural type
 * @name: action name
 * Returns: (transfer none) (nullable): declaration, hidden when its type is off
 */
VentureAction *venture_action_registry_lookup(VentureActionRegistry *self,
	const gchar *type_name, const gchar *name);
/**
 * venture_action_registry_list_for_type:
 * @self: registry
 * @type_name: registered type
 * Returns: (transfer container) (element-type VentureAction): declarations in registration order
 */
GPtrArray *venture_action_registry_list_for_type(VentureActionRegistry *self, const gchar *type_name);
/**
 * venture_action_registry_allowed:
 * @self: registry
 * @action: registered declaration
 * @entity: current record
 * @actor: (nullable): attribution
 * @role: authenticated caller's role
 * @error: (out) (optional): refusal reason
 * Returns: TRUE if type, live record, role and service allow the action
 */
gboolean venture_action_registry_allowed(VentureActionRegistry *self, VentureAction *action,
	VentureEntity *entity, const VentureActor *actor, VentureUserRole role, GError **error);
/**
 * venture_action_registry_perform:
 * @self: registry
 * @type_name: registered type
 * @id: persisted record identifier
 * @name: action name
 * @params: (element-type utf8 JsonNode): parameter values
 * @actor: (nullable): attribution
 * @role: authenticated caller's role
 * @error: (out) (optional): refusal reason
 * Returns: (transfer full) (nullable): service result
 */
VentureEntity *venture_action_registry_perform(VentureActionRegistry *self,
	const gchar *type_name, gint64 id, const gchar *name, GHashTable *params,
	const VentureActor *actor, VentureUserRole role, GError **error);
/**
 * venture_action_validate_parameters:
 * @self: declaration
 * @params: (element-type utf8 JsonNode): values
 * @error: (out) (optional): refusal
 * Returns: TRUE if the parameter names, kinds and required values match
 */
gboolean venture_action_validate_parameters(VentureAction *self, GHashTable *params, GError **error);
/**
 * venture_action_registry_describe:
 * @self: registry
 * @type_name: registered type
 * Returns: (transfer full): JSON action descriptions, including parameter schemas
 */
JsonNode *venture_action_registry_describe(VentureActionRegistry *self, const gchar *type_name);
/**
 * venture_action_parameters_from_json:
 * @node: JSON object
 * @error: (out) (optional): parse error
 * Returns: (transfer full) (element-type utf8 JsonNode) (nullable): independent values
 */
GHashTable *venture_action_parameters_from_json(JsonNode *node, GError **error);
/**
 * venture_database_get_action_registry:
 * @self: database
 * Returns: (transfer none): the database's action registry
 */
VentureActionRegistry *venture_database_get_action_registry(VentureDatabase *self);
/**
 * venture_confirmation_store_stage_action:
 * @self: shared confirmation queue
 * @action: registered action
 * @entity: target record
 * @params: (element-type utf8 JsonNode): values, copied for approval
 * @origin: (nullable): proposer
 * @role: authenticated proposer's role
 * @via: originating surface
 * @error: (out) (optional): refusal
 * Returns: (transfer none) (nullable): pending action confirmation
 */
VentureConfirmation *venture_confirmation_store_stage_action(VentureConfirmationStore *self,
	VentureAction *action, VentureEntity *entity, GHashTable *params,
	const VentureActor *origin, VentureUserRole role, const gchar *via, GError **error);
/**
 * venture_confirmation_store_approve_as:
 * @self: queue
 * @id: confirmation identifier
 * @approver: authenticated approver name
 * @role: authenticated approver role, supplied by the transport
 * @error: (out) (optional): refusal
 * Returns: TRUE when approved; action eligibility is checked again
 */
gboolean venture_confirmation_store_approve_as(VentureConfirmationStore *self,
	const gchar *id, const gchar *approver, VentureUserRole role, GError **error);
/**
 * venture_action_prepare_target:
 * @self: action declaration
 * @entity: new action target
 * @params: (element-type utf8 JsonNode): invocation values
 * @error: (out) (optional): invalid subject
 *
 * Hydrates a type-level action's declared subject before access checks. A
 * type-level action that declares no subject parameter is placed in the
 * organization its organization_id parameter names (a JSON integer, or a
 * string of digits), so the access policy judges the organization the
 * action will run in rather than none.
 * Returns: TRUE when the target represents the declared input record
 */
gboolean venture_action_prepare_target(VentureAction *self, VentureEntity *entity,
	GHashTable *params, GError **error);
/**
 * venture_action_require_organization:
 * @self: action declaration
 * @entity: the prepared target
 * @database: the repository whose access scope is current
 * @error: (out) (optional): refusal naming organization_id
 *
 * Refuses a type-level action with no subject parameter and no named
 * organization when the current access scope is an authenticated member
 * who is not a global owner or administrator. Internal work and global
 * administrators keep running such actions in the default organization.
 * Call it after venture_action_prepare_target() on every path that checks
 * an action, so a member is told to name an organization rather than told
 * there is no such record.
 *
 * Returns: TRUE when the action may go on to the access checks
 */
gboolean venture_action_require_organization(VentureAction *self, VentureEntity *entity,
	VentureDatabase *database, GError **error);
G_END_DECLS
#endif
