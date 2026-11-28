/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-web-picker-private.h - How many rows a reference picker draws
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * Shared by venture-web-server.c and the tests that hold it to these
 * numbers (docs/interface.org, "Pickers"). Not installed, not in the GIR.
 */

#ifndef VENTURE_WEB_PICKER_PRIVATE_H
#define VENTURE_WEB_PICKER_PRIVATE_H

/*
 * The most candidates a reference picker lists as a <select>. A target
 * with more is drawn as a lookup instead: a text box, a hidden id, and
 * GET /ui/records/search behind it. Decided by one read of at most one
 * more row than this, never by loading or counting the whole table.
 */
#define VENTURE_WEB_REFERENCE_PICKER_MAX (200)

/* The most matches GET /ui/records/search answers with. */
#define VENTURE_WEB_RECORD_SEARCH_MAX (12)

/* What a lookup's text box is called: the field's name and this. */
#define VENTURE_WEB_REFERENCE_LOOKUP_SUFFIX "__lookup"

#endif /* VENTURE_WEB_PICKER_PRIVATE_H */
