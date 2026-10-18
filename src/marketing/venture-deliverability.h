/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_DELIVERABILITY_H
#define VENTURE_DELIVERABILITY_H
G_BEGIN_DECLS

/* Deliverability: what happened to campaign mail after the relay took it,
 * whether the sending domain is set up to be believed, and how fast a new
 * domain may send. Everything here feeds the evidence rules that
 * VentureMarketingService already owns (venture_marketing_service_feedback());
 * nothing here keeps a second suppression list, reply matcher or report.
 * docs/deliverability.org says what each number means. */

/**
 * VENTURE_DELIVERABILITY_DEFAULT_RAMP:
 *
 * The default warm-up: "day:limit" pairs, each limit holding from that day
 * of the domain's sending life until the next pair. Zero is no limit.
 */
#define VENTURE_DELIVERABILITY_DEFAULT_RAMP "1:50,3:100,5:250,7:500,10:1000,14:2500,21:5000,30:0"

/**
 * VENTURE_DELIVERABILITY_TOLERANCE_SECONDS:
 *
 * How far a signed feed delivery's timestamp may be from this server's
 * clock. Older is a replay; newer is a clock that cannot be trusted.
 */
#define VENTURE_DELIVERABILITY_TOLERANCE_SECONDS 300

/**
 * VENTURE_DELIVERABILITY_MAX_BODY:
 *
 * The largest signed feed delivery accepted, in bytes.
 */
#define VENTURE_DELIVERABILITY_MAX_BODY (256 * 1024)

/**
 * VENTURE_DELIVERABILITY_MAX_EVENTS:
 *
 * The most events one feed delivery may carry.
 */
#define VENTURE_DELIVERABILITY_MAX_EVENTS 100

/**
 * VentureDeliverabilityKind:
 * @VENTURE_DELIVERABILITY_DELIVERED: the receiving server accepted it
 * @VENTURE_DELIVERABILITY_HARD_BOUNCE: permanently refused (5.x.x)
 * @VENTURE_DELIVERABILITY_SOFT_BOUNCE: refused for now (4.x.x)
 * @VENTURE_DELIVERABILITY_COMPLAINT: the recipient reported it as spam
 *
 * What a provider or a delivery status notification says happened.
 */
typedef enum {
	VENTURE_DELIVERABILITY_DELIVERED = 0,
	VENTURE_DELIVERABILITY_HARD_BOUNCE,
	VENTURE_DELIVERABILITY_SOFT_BOUNCE,
	VENTURE_DELIVERABILITY_COMPLAINT
} VentureDeliverabilityKind;

/**
 * VentureDeliverabilityEvent:
 * @kind: what happened
 * @event_id: (nullable): the provider's own identity for this event
 * @message_id: (nullable): the Message-ID it is about, without angle brackets
 * @recipient: (nullable): the address it is about, trimmed and lower case
 * @occurred_at: (nullable): when it happened
 * @status: (nullable): the enhanced status code, for example 5.1.1
 * @detail: (nullable): the diagnostic text, bounded
 *
 * One event, from whichever source. Adapters and the DSN parser make these.
 */
typedef struct {
	VentureDeliverabilityKind kind;
	gchar *event_id;
	gchar *message_id;
	gchar *recipient;
	GDateTime *occurred_at;
	gchar *status;
	gchar *detail;
} VentureDeliverabilityEvent;

/**
 * venture_deliverability_event_new:
 * @kind: what happened
 * Returns: (transfer full): an empty event of @kind
 */
VentureDeliverabilityEvent *venture_deliverability_event_new(VentureDeliverabilityKind kind);
/**
 * venture_deliverability_event_free:
 * @event: (nullable): an event
 */
void venture_deliverability_event_free(VentureDeliverabilityEvent *event);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureDeliverabilityEvent, venture_deliverability_event_free)

/**
 * venture_deliverability_kind_to_string:
 * @kind: a kind
 * Returns: delivered, hard_bounce, temporary_bounce or complaint -- the
 *   names venture_marketing_service_feedback() takes
 */
const gchar *venture_deliverability_kind_to_string(VentureDeliverabilityKind kind);

/**
 * VentureDeliverabilityParse:
 * @body: the verified request body
 * @error: (out) (optional): why the body is not this provider's shape
 *
 * Returns: (transfer full) (element-type VentureDeliverabilityEvent): events
 */
typedef GPtrArray *(*VentureDeliverabilityParse)(GBytes *body, GError **error);

/**
 * VentureDeliverabilityAdapter:
 * @id: the provider name in the feed's address, [a-z0-9-]
 * @label: what the settings page calls it
 * @parse: turns a verified body into events
 *
 * A provider's event shape. The signature is checked before @parse runs and
 * is the same for every adapter, so an adapter only reads its JSON.
 */
typedef struct {
	const gchar *id;
	const gchar *label;
	VentureDeliverabilityParse parse;
} VentureDeliverabilityAdapter;

/**
 * venture_deliverability_adapter_register:
 * @adapter: (transfer none): a static adapter; a second with the same id is ignored
 */
void venture_deliverability_adapter_register(const VentureDeliverabilityAdapter *adapter);
/**
 * venture_deliverability_adapter_lookup:
 * @id: (nullable): a provider name
 * Returns: (transfer none) (nullable): the adapter, or %NULL
 */
const VentureDeliverabilityAdapter *venture_deliverability_adapter_lookup(const gchar *id);
/**
 * venture_deliverability_parse_generic:
 * @body: a body in the documented generic shape
 * @error: (out) (optional): which event is malformed
 *
 * The built-in "generic" adapter: {"events":[{"id","type","bounce",
 * "message_id","recipient","occurred_at","status","detail"}]}.
 *
 * Returns: (transfer full) (element-type VentureDeliverabilityEvent) (nullable): events
 */
GPtrArray *venture_deliverability_parse_generic(GBytes *body, GError **error);

/**
 * venture_deliverability_verify_signature:
 * @secret: the organization's signing secret
 * @timestamp: (nullable): X-Venture-Timestamp, Unix seconds
 * @signature: (nullable): X-Venture-Signature, hex HMAC-SHA256, optionally prefixed sha256=
 * @body: the exact request bytes
 * @now: this server's clock
 * @error: (out) (optional): one refusal for every reason, so a caller learns nothing
 *
 * Verifies HMAC-SHA256(secret, timestamp "\n" body) in constant time and
 * refuses a timestamp more than VENTURE_DELIVERABILITY_TOLERANCE_SECONDS away.
 *
 * Returns: whether the delivery is authentic and fresh
 */
gboolean venture_deliverability_verify_signature(const gchar *secret, const gchar *timestamp,
	const gchar *signature, GBytes *body, GDateTime *now, GError **error);

/**
 * venture_deliverability_set_feed_secret:
 * @database: repository
 * @organization_id: the organization
 * @secret: (nullable): 32 to 128 printable characters, or %NULL to make one
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 *
 * Seals the feed's signing secret as an integration credential (provider
 * "marketing_feedback"), replacing any earlier one. Owners and
 * administrators only.
 *
 * Returns: (transfer full) (nullable): the secret, to be shown once
 */
gchar *venture_deliverability_set_feed_secret(VentureDatabase *database, gint64 organization_id,
	const gchar *secret, const VentureActor *actor, GError **error);

/**
 * venture_deliverability_feed_configured:
 * @database: repository
 * @organization_id: the organization
 * Returns: whether the organization has a feed secret
 */
gboolean venture_deliverability_feed_configured(VentureDatabase *database, gint64 organization_id);

/**
 * venture_deliverability_receive:
 * @database: repository
 * @organization_id: the organization in the feed's address
 * @provider: the adapter in the feed's address
 * @timestamp: (nullable): X-Venture-Timestamp
 * @signature: (nullable): X-Venture-Signature
 * @body: the exact request bytes
 * @now: this server's clock
 * @error: (out) (optional): NOT_FOUND for an unknown provider,
 *   PERMISSION_DENIED for anything unauthenticated
 *
 * Verifies, parses and records each event through the marketing evidence
 * rules. An event about mail this organization did not send is ignored,
 * not refused; a replayed event id is recorded once.
 *
 * Returns: (transfer full) (nullable): {"accepted":n,"ignored":n,"results":[...]}
 */
JsonNode *venture_deliverability_receive(VentureDatabase *database, gint64 organization_id,
	const gchar *provider, const gchar *timestamp, const gchar *signature, GBytes *body,
	GDateTime *now, GError **error);

/**
 * venture_deliverability_apply:
 * @database: repository
 * @organization_id: the organization
 * @event: an event
 * @source: where it came from, retained as evidence
 * @now: this server's clock; a later event time is taken as now
 * @error: (out) (optional): persistence failure
 *
 * Finds the campaign recipient by Message-ID (and address, when the event
 * names one) and records the event as marketing feedback.
 *
 * Returns: "recorded", or why it was ignored; %NULL on failure
 */
const gchar *venture_deliverability_apply(VentureDatabase *database, gint64 organization_id,
	const VentureDeliverabilityEvent *event, const gchar *source, GDateTime *now, GError **error);

/**
 * venture_deliverability_parse_dsn:
 * @raw: an RFC 5322 message
 * @error: (out) (optional): unparseable message
 *
 * Reads an RFC 3464 delivery status notification (multipart/report;
 * report-type=delivery-status) or an RFC 5965 abuse report
 * (report-type=feedback-report): one event per recipient. Action failed
 * with a 5.x.x status is a hard bounce; delayed, or any 4.x.x, is soft;
 * delivered and relayed are delivered. The Message-ID is the per-recipient
 * Original-Message-ID, else the returned message's own, else In-Reply-To.
 *
 * Returns: (transfer full) (element-type VentureDeliverabilityEvent): events;
 *   empty for anything that is not a report
 */
GPtrArray *venture_deliverability_parse_dsn(GBytes *raw, GError **error);

/**
 * venture_deliverability_apply_dsn:
 * @database: repository
 * @organization_id: the mailbox's organization
 * @raw: the message as synced
 * @now: this server's clock
 * @error: (out) (optional): persistence failure
 *
 * What the mailbox sync calls for every new message: parses it as a report
 * and applies each event. Mail that is not a report changes nothing.
 *
 * Returns: the number of events recorded, or -1
 */
gint venture_deliverability_apply_dsn(VentureDatabase *database, gint64 organization_id,
	GBytes *raw, GDateTime *now, GError **error);

#define VENTURE_TYPE_DNS_RESOLVER (venture_dns_resolver_get_type())
G_DECLARE_INTERFACE(VentureDnsResolver, venture_dns_resolver, VENTURE, DNS_RESOLVER, GObject)

/**
 * VentureDnsResolverInterface:
 * @lookup_txt: the TXT records at a name, each record's strings joined
 *
 * How the domain check asks DNS. Tests install a fake; nothing else in
 * the check touches the network.
 */
struct _VentureDnsResolverInterface {
	GTypeInterface parent_iface;
	gchar **(*lookup_txt)(VentureDnsResolver *self, const gchar *name, GCancellable *cancellable, GError **error);
	/*< private >*/
	gpointer padding[4];
};

/**
 * venture_dns_resolver_lookup_txt:
 * @self: a resolver
 * @name: a DNS name
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): G_RESOLVER_ERROR_NOT_FOUND when there are none
 * Returns: (transfer full) (array zero-terminated=1) (nullable): the records
 */
gchar **venture_dns_resolver_lookup_txt(VentureDnsResolver *self, const gchar *name,
	GCancellable *cancellable, GError **error);

#define VENTURE_TYPE_GIO_DNS_RESOLVER (venture_gio_dns_resolver_get_type())
G_DECLARE_FINAL_TYPE(VentureGioDnsResolver, venture_gio_dns_resolver, VENTURE, GIO_DNS_RESOLVER, GObject)

/**
 * venture_gio_dns_resolver_new:
 * @timeout_ms: the most one lookup may take
 *
 * The default resolver: GResolver's TXT lookup, run on a private main
 * context so the server's own loop is never driven, and abandoned at the
 * deadline.
 *
 * Returns: (transfer full): a resolver
 */
VentureGioDnsResolver *venture_gio_dns_resolver_new(guint timeout_ms);

/**
 * venture_deliverability_set_resolver:
 * @database: repository
 * @resolver: (nullable): the resolver its checks use; %NULL restores GResolver
 */
void venture_deliverability_set_resolver(VentureDatabase *database, VentureDnsResolver *resolver);
/**
 * venture_deliverability_get_resolver:
 * @database: repository
 * Returns: (transfer full): the resolver its checks use
 */
VentureDnsResolver *venture_deliverability_get_resolver(VentureDatabase *database);

/**
 * venture_deliverability_check_domain:
 * @resolver: how to ask DNS
 * @domain: the sending domain
 * @selector: (nullable): the DKIM selector; empty skips the key lookup
 * @error: (out) (optional): an invalid domain or selector
 *
 * SPF, DKIM and DMARC for @domain, each with a status (pass, weak, missing
 * or error), the record found, what it means in plain words and what to
 * change. Lookup failures are results, not errors.
 *
 * Returns: (transfer full) (nullable): {"domain","selector","overall","checks":[...]}
 */
JsonNode *venture_deliverability_check_domain(VentureDnsResolver *resolver, const gchar *domain,
	const gchar *selector, GError **error);

/**
 * venture_deliverability_ramp_limit:
 * @schedule: (nullable): "day:limit" pairs, or "off"; %NULL or empty is the default
 * @day: the day of the domain's sending life, starting at one
 * @error: (out) (optional): a malformed schedule
 * Returns: the most campaign messages that day, 0 for no limit, -1 on error
 */
gint64 venture_deliverability_ramp_limit(const gchar *schedule, gint64 day, GError **error);

/**
 * venture_deliverability_ramp_status:
 * @database: repository
 * @organization_id: the organization
 * @now: the delivery run's clock
 * @day: (out) (optional): the day of sending life
 * @limit: (out) (optional): today's limit, 0 for none
 * @used: (out) (optional): campaign messages queued today (UTC)
 * @error: (out) (optional): persistence failure or a malformed schedule
 *
 * The organization's warm-up today, from its first sending domain record
 * (its start date and schedule) or, with none, the default schedule from
 * the day its first campaign message was accepted.
 *
 * Returns: whether the status was computed
 */
gboolean venture_deliverability_ramp_status(VentureDatabase *database, gint64 organization_id,
	GDateTime *now, gint64 *day, gint64 *limit, gint64 *used, GError **error);

/**
 * venture_marketing_machine_reason:
 * @sent_at: (nullable): when the relay accepted the message
 * @observed_at: when the open or click arrived
 * @user_agent: (nullable): the request's User-Agent, read and not kept
 * @window_seconds: an observation this soon after acceptance is a machine's; 0 turns that rule off
 *
 * The machine-open heuristic, in one place. See docs/deliverability.org.
 *
 * Returns: (nullable): a static reason, or %NULL for "no sign of a machine"
 */
const gchar *venture_marketing_machine_reason(GDateTime *sent_at, GDateTime *observed_at,
	const gchar *user_agent, gint64 window_seconds);

/**
 * VentureMarketingLinkCount:
 * @position: the tracked link's number, from one
 * @destination: where it goes, without its query or fragment
 * @clicks: recipients who clicked it, machines aside
 * @machine_clicks: recipients whose clicks on it were judged a machine's
 *
 * One row of a send's click map.
 */
typedef struct {
	gint64 position;
	gchar *destination;
	gint64 clicks;
	gint64 machine_clicks;
} VentureMarketingLinkCount;

/**
 * VentureMarketingSendReport:
 * @send_id: the send
 * @name: its name
 * @eligible: approved recipients
 * @excluded: preview exclusions
 * @accepted: SMTP accepted
 * @pending: not yet resolved
 * @uncertain: uncertain SMTP outcome
 * @failed: known failures
 * @cancelled: cancelled
 * @delivered: a provider said the receiving server accepted it
 * @opened: recipients with an open not judged a machine's
 * @machine_opened: recipients whose only opens were judged a machine's
 * @clicked: recipients with a click not judged a machine's
 * @replied: recipients who replied
 * @unsubscribed: recipients who unsubscribed
 * @bounced: hard bounce evidence
 * @soft_bounced: temporary bounce evidence
 * @complained: complaints
 * @links: (element-type VentureMarketingLinkCount): the click map
 *
 * What happened to one send. The marketing_performance report and the
 * send's report page both read this.
 */
typedef struct {
	gint64 send_id;
	gchar *name;
	gint64 eligible, excluded, accepted, pending, uncertain, failed, cancelled;
	gint64 delivered, opened, machine_opened, clicked, replied, unsubscribed;
	gint64 bounced, soft_bounced, complained;
	GPtrArray *links;
} VentureMarketingSendReport;

/**
 * venture_marketing_send_report:
 * @database: repository
 * @organization_id: the send's organization
 * @send_id: a send
 * @error: (out) (optional): not found or persistence failure
 * Returns: (transfer full) (nullable): the counts and click map
 */
VentureMarketingSendReport *venture_marketing_send_report(VentureDatabase *database,
	gint64 organization_id, gint64 send_id, GError **error);
/**
 * venture_marketing_send_report_free:
 * @report: (nullable): a report
 */
void venture_marketing_send_report_free(VentureMarketingSendReport *report);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureMarketingSendReport, venture_marketing_send_report_free)
/**
 * venture_marketing_send_report_to_json:
 * @report: a report
 * Returns: (transfer full): the report as JSON, wire spelling
 */
JsonNode *venture_marketing_send_report_to_json(const VentureMarketingSendReport *report);

/**
 * venture_deliverability_install:
 * @database: owning database
 *
 * Registers the generic adapter and the mailbox sync's report hook, and the
 * campaign reply and sending-domain validators. Called by
 * venture_marketing_install().
 */
void venture_deliverability_install(VentureDatabase *database);

G_END_DECLS
#endif
