# The desk: tickets, service levels, comments, knowledge bases, the assistant

Read this for tickets (internal work and external support), SLA clocks,
macros, worklogs, sprints, routing, the AI's ticket judgements, discussion
comments on any record, knowledge bases, and the in-process assistant.
Inbox and watches are in [automation.md](automation.md). Sources:
`docs/workdesk.org`, `docs/comments.org`, `docs/knowledge-bases.org`,
`docs/ai.org`, `docs/data-model.org` (tickets).

## Tickets

One record for internal work and external support: `kind`
(internal/external -- whose problem, who may read replies) and
`issue_type` (task/subtask/story/epic/bug/research -- what shape the work
is; what forge rules key on) answer different questions; an external bug
is both. `status` triage -> todo -> in_progress -> blocked/review -> done
(or cancelled, an outcome rather than a column). Moving into done/cancelled
stamps `resolved_at`; moving back clears it. `/tickets` is the kanban
board; `?view=list`, filters `kind`, `assignee`, `venture_id`.

```bash
venturectl create ticket title="Checkout returns 500" kind=external issue_type=bug priority=high company_id=4
venturectl create ticket_comment ticket_id=7 body="Looking into it" internal=false
venturectl ticket 7 sla                 # clocks: state and seconds left for first reply and resolution
venturectl ticket 7 macro "Need logs"   # canned reply + field changes; not stageable
venturectl ticket 7 worklog 1.5 "Reproduced"     # logged_hours follows
venturectl sprints ; venturectl sprint 3
venturectl bulk ticket 4,5,9 status=done         # one transaction; --delete removes
venturectl incident 2 ticket                     # the bug for an incident, prioritised from severity
```

- A ticket's conversation is `ticket_comment` (with `internal` for notes the
  requester never sees); tickets take no generic discussion comment.
- **Service levels**: an `sla_policy` gives wall-clock hours to the first
  visible reply and to resolution by kind and priority (most specific
  wins). Due times are stamped once, on the first save, and never moved --
  a deadline that moves is not a deadline. The first reply the requester
  can read stamps `first_responded_at`. `sla_breached` is set by the
  sweep, which runs when the board or inbox opens or on `POST
  /api/v1/sla/sweep` -- call it from a rule for overnight marking.
- **Derived writes bump the version**: the first-reply stamp and the
  logged-hours roll-up are saves of the ticket, so an object read before
  them conflicts -- re-read after commenting or logging time.
- `routing_rule` assigns a new ticket (round_robin, least_busy, first) by
  kind/priority/tag, only when it has no assignee. An `assignee` is a
  username; one that is no user is nobody to tell.
- `bulk` and `macro` are several records at once and cannot be staged;
  propose the pieces with `create`/`update --stage` instead. A worklog is
  one record and stages.

## The assistant at the desk (AI module and a configured provider)

```bash
venturectl ticket 7 triage            # proposes priority, issue type, tags, summary, sentiment
venturectl ticket 7 triage --apply    # keeps them (only values the enums accept; tags merged)
venturectl ticket 7 summary           # what the whole thread amounts to
venturectl ticket 7 draft "apologise and ask for the order number"
```

A draft is **never posted** -- show it to the operator, then `create
ticket_comment` if they want it. These run on a toolless path: the
ticket's text is a stranger's writing and is treated as data, never
instructions.

## Comments: a discussion on every record

Every type takes comments unless it opts out (tickets, comments, links,
the audit log, KB internals). **Comments are not `create comment`**: the
generic routes refuse the type (and list it only to the owner).

```bash
venturectl comments list invoice 5            # threaded: top-level oldest first, each with replies;
                                              # -f json gives {subject, count, comments}
venturectl comments add invoice 5 "Customer asked for net 45 -- @bob can you check?"
printf 'Long text with an em dash \xe2\x80\x94 from stdin\n' | venturectl comments add deal 12 -
venturectl comments reply 31 "Done"           # lands in that comment's thread (one level)
venturectl comments edit 31 "Fixed typo" ; venturectl comments delete 31 ; venturectl comments get 31
venturectl activity invoice 5                 # timeline incl. comments, each with a url
```

- BODY is markdown (escaped; links only to http(s), `/path` or `#anchor`);
  `-` reads stdin -- use it for non-ASCII text, which argv refuses in a C
  locale. `@username` tells somebody **who may read the record** (anybody
  else stays plain text, silently); `#type/id` links a record. Commenting
  or being mentioned makes you a watcher.
- The author comes from your credential: a comment made with a token reads
  "API token #N" with its owner's name beside it. Only the author edits;
  the author or an organization/install owner or admin deletes. A record
  you cannot read is "not found" to every `comments` verb, never
  "forbidden". A private record takes no comments.
- Nothing about comments stages (`--stage` does not apply; a viewer
  comments directly). A token minted with the viewer role may read a
  discussion but not add to it. `/comments/N` is a comment's permalink.
- REST: `GET /api/v1/comments?subject_type=&subject_id=`, `POST
  /api/v1/comments`, `GET|PATCH|DELETE /api/v1/comments/:id`.

## Knowledge bases (module `kb`)

Knowledge bases are ordinary record types, so `list kb_article`, `get
knowledge_base 1` and `create kb_article ...` are the way to read or write
articles. The `kb` verbs are only the part that is not CRUD:

```bash
venturectl kb search "how do I add an API token?" [--kb venture_docs] [--limit 5]
venturectl kb sync 3                  # re-read the base's source directory on the server
venturectl kb reindex [3] [--force]   # re-embed what needs it
venturectl kb export 3 --format tar.gz > docs.tar.gz
venturectl kb crossref ticket 7       # link the knowledge bearing on one record
venturectl kb article ticket 7 --kb 3 # write an article from a record
```

- **`kb search` finds meaning, not words.** It is the right tool for
  something written down rather than recorded -- a policy, a spec, a
  handbook. `list kb_article search=...` matches characters and misses a
  passage that answers in other words.
- **`kb reindex` without `--force` is cheap and safe**; `--force`
  re-embeds everything, which a change of `kb.embedding_model` requires
  and is otherwise a waste. Articles indexed by a different model are
  always re-embedded: vectors from two models cannot be compared, so a base
  is claimed by the first model that indexes it.
- Indexing is synchronous on the request that saved an article; bulk work
  is an explicit command. Sync hashes file bytes; an article whose source
  file vanished is archived, not deleted. Inputs: `.org .md .txt .html`
  verbatim, `.pdf .docx` extracted, archives unpacked (nesting capped).
- Crossref eligibility comes from the field table: any type with a long
  text field participates.

## The in-process assistant (modules `ai`, `chat`)

The AI panel (`/ui/chat`, per-user threads) reads freely and writes
through the same staging queue: under `ai.policy=confirm_writes` (default)
creates, updates and links are staged, deletes always; `read_only`
registers no write tool; `autonomous` applies (deletes still staged). It
has no shell, filesystem or webhook/token access, and cannot read users,
tokens or chat history. A question carries the page it was asked from.
Slash skills (`/summarise` and others, plus `ai_skill` records) expand
prompts; `#slug` names a knowledge base; `@type/id` mentions a record.
Approve its cards in the panel or `POST /api/v1/confirmations/ID/approve`.
Organization AI providers are configured per purpose
([platform.md](platform.md)).
