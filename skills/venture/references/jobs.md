# Home-services jobs and visits

The `jobs` module requires `crm` and suggests `quotes` and `invoicing`.
Read `describe job`, `describe property`, `describe equipment`,
`describe maintenance_plan` and the generated action schemas before writing.
These are ordinary organization-scoped records, available through generic
CRUD, the web forms and `venturectl`; disabling jobs hides them without
removing stored work.

## From customer to completed work

1. Select the customer (`company_id`), then create a `property` with its
   service `address`, `access_notes` and optional contact. A customer can
   have several properties. Equipment names its `property_id`, make,
   model, serial, installed year and warranty date.
2. Create a `job` with title, customer, problem and optional property,
   quote and invoice. A blank job address takes the property's address,
   then the estimate's explicit service address. A typed address is kept.
   Customer references must agree; an estimate or invoice for another
   customer is refused. Blank numbers receive the next unused `J-` number.
3. `act job ID assign user_id=USER lead=true` sends an active user.
   Replacing the lead is atomic: a refused assignment preserves the old
   lead. A user appears once per job.
4. `act job ID on_the_way`, `arrive`, `complete` and `cancel` apply only
   in their permitted states. `invoiced invoice_id=INVOICE` applies to a
   done job; omit the ID to use the job's invoice or its accepted quote's.
   A linked draft is issued with today's date and, when absent, a due date
   thirty days later. Issue, posting and job update commit together.
   Configured accounting approval applies to draft issuance and the complete
   job handoff. Linking an already-issued invoice posts nothing and needs no
   new posting approval.

Actions need editor access and are not stageable. Read the current record
and available actions before invoking one; never treat a refusal as success.

## Scheduling and maintenance

`/jobs/day?date=YYYY-MM-DD&tech=ID` shows that technician's jobs in arrival
order. Omit `tech` for the signed-in person or use `tech=all`. It includes
customer phone, directions, access notes and assignments. Jobs also appear
on the calendar, My day and the home Today card. Customer pages link to
work, properties, quotes and invoices; job pages show equipment and access.

Datetime form inputs use the business time zone, including instants at
midnight UTC. DATE fields retain the calendar day. For API/CLI datetime
writes, provide an ISO instant with an explicit offset; do not infer that
midnight means a date-only value. Historical datetime values have no
provenance flag, so their stored instants are retained and converted.

A `maintenance_plan` names customer, property and optional equipment,
`visit`, `every_months` (default 12), `next_visit` and optional `ended_at`.
Equipment alone derives its property and still must belong to that customer.
`/jobs/visits` lists overdue visits and those due in thirty days.
`act maintenance_plan ID book` creates one job and advances the due date
by its interval from the old due date, in one transaction. A failure leaves
both unchanged. Set the job's arrival time after agreeing it with the customer.
Plans do not automatically charge a subscription or schedule a technician.

The optional `data/examples/home-services.yaml` preset trims unrelated
modules. It changes navigation, not the meaning or ownership of records.
