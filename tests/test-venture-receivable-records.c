/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

static void
test_event_book_amount(void)
{
	g_autoptr(VentureInvoiceEvent) event = venture_invoice_event_new();
	g_autoptr(VentureMoney) book = venture_money_new_for_currency(11000, "USD");
	g_autoptr(VentureMoney) stored = NULL;

	g_object_set(event, "book-amount", book, NULL);
	g_object_get(event, "book-amount", &stored, NULL);
	g_assert_cmpint(venture_money_get_amount(stored), ==, 11000);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/venture-receivable-records/book-amount", test_event_book_amount);
	return g_test_run();
}
