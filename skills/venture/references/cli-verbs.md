# Every venturectl verb, by subsystem

Read this to find the verb for a task; the linked guide holds its options
and traps. Generic verbs, flags, output and exit codes are in
[cli.md](cli.md). Arguments are `key=value` unless shown positional; most
take `organization_id=N` to work outside the default organization.

## Records, links, reports, the server

| Verb | What it does | Guide |
|---|---|---|
| `types`, `describe TYPE`, `list`, `get`, `create`, `update`, `delete`, `restore`, `bulk`, `act` | generic records and actions | [records.md](records.md) |
| `links TYPE ID`; `link TYPE ID TYPE ID [kind=K] [note=T]` | links; kinds related, blocks, blocked_by, depends_on, required_by, parent_of, child_of, duplicates, causes, caused_by, produces, produced_by, references, referenced_by, supersedes, superseded_by; unlink with `delete record_link ID` | [records.md](records.md) |
| `report [NAME] [PERIOD] [k=v ...]` | list reports, or run one; the period may be left out before options (`report holdings organization_id=2` is `this_month`); `report aggregate ...` totals any type | [reports.md](reports.md) |
| `report packs deliver ID` | re-send a report pack's last retained output by email | [ledger-operations.md](ledger-operations.md) |
| `modules`, `health`, `plugins [list]` | modules and why one is off; liveness; loaded plugins (owner) | [system.md](system.md) |
| `fields define\|layout\|value k=v ...` | custom fields, layouts, one attribute value | [taxonomy.md](taxonomy.md) |
| `docs build [source=DIR] [output=DIR] [renderer=auto\|emacs\|builtin]` | render docs/*.org to a static site; needs no server | [platform.md](platform.md) |
| `mcp [--apply-writes]` | stdio MCP server for an outside agent | [cli.md](cli.md) |
| `federation JSON` | identity, remote, pull, edit, sync, resolve | [operations.md](operations.md) |
| `user mfa reset USER` | break glass: turn off a user's second factor (owner, audited) | [platform.md](platform.md) |
| `printers [list\|test NAME\|status NAME]`, `print payment\|invoice ID [PRINTER]` | receipt printers | [platform.md](platform.md) |
| `backup run SCHEDULE_ID [as_of=]`, `backup verify RUN_ID`, `backup restore-drill [run_id=] [organization_id=] [name=]` | scheduled backups | [operations.md](operations.md) |

## The workdesk, knowledge, the assistant at the desk

| Verb | What it does | Guide |
|---|---|---|
| `inbox [--all]`, `inbox read ID\|all` | what you have been told; mark read | [desk.md](desk.md) |
| `watch TYPE ID`, `unwatch TYPE ID` | follow a record | [desk.md](desk.md) |
| `activity TYPE ID` | a record's timeline: changes, comments, worklogs | [desk.md](desk.md) |
| `comments list\|add\|reply\|edit\|delete\|get ...` | discussion on any record; BODY `-` reads stdin | [desk.md](desk.md) |
| `ticket ID sla\|macro NAME\|worklog H [NOTE]` | clocks; canned reply + changes (not stageable); log time | [desk.md](desk.md) |
| `ticket ID triage [--apply]\|summary\|draft [AIM]` | AI judgements; a draft is never posted | [desk.md](desk.md) |
| `sprints`, `sprint ID` | sprints with their burn | [desk.md](desk.md) |
| `support rollup --from D --to D [--sort [-]COL] [min_tickets=] [company=] [product=] [group_by=product]` | support cost and volume per customer | [reports.md](reports.md) |
| `kb search QUERY [--kb SLUG] [--limit N]`, `kb sync KB_ID`, `kb reindex [KB_ID] [--force]`, `kb export KB_ID [--format zip\|tar.gz]`, `kb crossref TYPE ID`, `kb article TYPE ID --kb N` | knowledge bases | [desk.md](desk.md) |

## The software factory and forges

| Verb | What it does | Guide |
|---|---|---|
| `factory`, `factory actions`, `factory briefing` | at a glance; what needs somebody (start here for "what next"); prose (AI) | [factory.md](factory.md) |
| `release readiness\|changelog [--replace]\|publish [--prerelease]\|deploy ENV [NOTES]\|notes [AUDIENCE] ID` | release operations (publish cannot be undone or staged) | [factory.md](factory.md) |
| `environment ID rollback [REASON]`, `milestone ID forecast`, `build ID ticket\|triage`, `incident ID ticket\|postmortem` | factory records | [factory.md](factory.md) |
| `runs [--state S]`, `budgets` | coding runs with cost; agent budgets | [factory.md](factory.md) |
| `forge settings ID` (JSON stdin), `forge verify ID`; `forge set-token\|set-secret` retired | forge credentials | [factory.md](factory.md) |
| `webhooks`, `webhook test ID`, `webhook secret ID`, `webhook token ID` (stdin) | outbound webhooks (owner); token: a gotify/ntfy push's | [automation.md](automation.md) |
| `dashboards`, `dashboard SLUG\|export SLUG\|import FILE\|create TEMPLATE\|templates\|kinds` | dashboards | [dashboards.md](dashboards.md) |

## Money in

| Verb | What it does | Guide |
|---|---|---|
| `compose invoice\|quote JSON` | lines, tax and optional send in one call | [receivables.md](receivables.md) |
| `invoice checkout ID` | hosted Stripe Checkout URL for a sent invoice | [billing.md](billing.md) |
| `quote send\|accept\|decline\|revise\|start-subscription ID [by=] [reason=]` | quote service | [receivables.md](receivables.md) |
| `deal move ID STAGE [NOTE]`, `deal quote ID` | pipeline transition; quote from deal lines | [crm.md](crm.md) |
| `billing start\|change\|change-seats\|cancel\|pause\|resume\|mark-payment-failed\|recover\|collect\|usage ...`, `billing renew\|dunning [--as-of] [--dry-run]` | SaaS subscriptions | [billing.md](billing.md) |
| `recurring run`, `collections run`, `batch invoice\|expense format=csv\|json payload=...` | schedules, reminders, batch entry | [billing.md](billing.md) |
| `dunning sweep [as_of=] [organization_id=] [limit=] [dry_run=true]` | overdue reminders | [billing.md](billing.md) |
| `customers health-sweep [as_of=] [organization_id=] [limit=]` | one check-in per red customer | [crm.md](crm.md) |
| `commerce import [JSON]` | Shopify orders as invoices | [receivables.md](receivables.md) |
| `sales-tax export period=P [jurisdiction=] [organization_id=] [as_of=]` | sales tax return CSV | [receivables.md](receivables.md) |
| `money calendar --from D --to D [--kind K] [--as-of D]` | dated money events agenda | [reports.md](reports.md) |

## Money out and the books

| Verb | What it does | Guide |
|---|---|---|
| `bill approve\|pay\|void ID`, `bill pay-bulk 1,2,3 [adapter=transfer] [date=]` | supplier bills | [payables.md](payables.md) |
| `purchase approve\|send\|receive\|match\|cancel\|return ID`, `sales-order allocate\|ship\|invoice\|cancel ID` | purchasing and fulfilment | [payables.md](payables.md) |
| `supplier invite company_id= email=`, `supplier revoke ID` | supplier portal | [payables.md](payables.md) |
| `capture ingest\|convert\|reject`, `claim submit\|approve\|pay ID`, `payroll import\|disburse\|reverse` | inbox, claims, payroll | [payables.md](payables.md) |
| `bank ACTION ID [JSON\|@FILE]`, `bank match AUTO STATEMENT_ID`, `bankfeed sync ID [JSON]`, `reconcile suggest TYPE ID [--matcher] [--threshold]` | banking | [payables.md](payables.md) |
| `tax-filing prepare\|review\|submit\|acknowledge\|amend`, `contractor-tax prepare\|review\|approve\|export` | returns and 1099-NEC | [payables.md](payables.md) |
| `journal post ID`, `post backfill [--dry-run]` | posting | [ledger.md](ledger.md) |
| `close open\|run\|sign\|complete\|reopen\|pack`, `accounting` | close workspace; next actions | [ledger.md](ledger.md) |
| `asset place\|dispose\|write-off ID`, `assets run-period YYYY-MM [dry_run=true]` | fixed assets | [ledger-operations.md](ledger-operations.md) |
| `cutover template SECTION`, `cutover csv source= cutoff= currency= SECTION=@FILE` | opening balances | [ledger-operations.md](ledger-operations.md) |
| `budget vs-actual\|forecast YYYY-MM`, `equity KIND AMOUNT [memo=]`, `group income\|balance\|trial [PERIOD]` | budgets, owner equity, consolidation | [ledger-operations.md](ledger-operations.md) |

## Customers, outreach, mail

| Verb | What it does | Guide |
|---|---|---|
| `lead convert ID [deal=yes\|no] [company_id=] [contact_id=]`, `lead reassign ID [owner=]`, `leads reroute\|rescore ID` | leads | [crm.md](crm.md) |
| `activity complete ID outcome=...`, `activity list mine\|overdue\|today` | planned work | [crm.md](crm.md) |
| `dedupe scan [kind=] \| merge ID survivor=N \| dismiss ID` | duplicates | [crm.md](crm.md) |
| `crm preview MANIFEST \| import MANIFEST\|ID \| activate ID \| rollback ID` | CRM migration | [crm.md](crm.md) |
| `sequence enroll ID contact_id=`, `sequence run [--as-of]`, `sequence status ID` | follow-ups | [outreach.md](outreach.md) |
| `mail list\|send\|test\|deliver\|retry\|sync\|contact\|dismiss` | transactional and inbound mail | [mail.md](mail.md) |
| `calendar sync [organization_id=] [limit=]` | CalDAV sweep | [mail.md](mail.md) |

## Market data, accounts, arbitrage

| Verb | What it does | Guide |
|---|---|---|
| `feeds sync ID [--wait]`, `feeds push ID FILE\|- [--wait]`, `feeds runs ID`, `feeds due`, `feeds upkeep ID [rebuild=1] [--wait]`, `feeds upkeep-status ID` | data sources, store upkeep | [feeds.md](feeds.md) |
| `market quote\|promote\|browse\|deals\|venues\|instrument\|watchlist\|alerts [evaluate RULE_ID [--dry-run]]`, `market help` | the price oracle and the Trading pages | [market-data.md](market-data.md) |
| `accounts [list\|attention]\|show\|inventory\|pnl\|post\|record-flips`, `accounts help` | the operator's accounts and their books | [accounts.md](accounts.md) |
| `arbitrage scan\|record\|crafting\|plan\|export\|calc\|registries\|close\|reopen\|abandon\|execute`, `arbitrage help` | opportunities and trades | [arbitrage-scan.md](arbitrage-scan.md), [arbitrage.md](arbitrage.md) |
