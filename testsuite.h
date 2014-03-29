#pragma once
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <stdbool.h>

#include "cJSON.h"
#include "fdt.h"

GtkWidget * createTestsuiteTab();
GtkWidget * createTestsView();
void testdata_select_okbutton_handler(GtkWidget *, gpointer);
void testdata_select_cancelbutton_handler(GtkWidget *, gpointer);
gboolean testdata_select_close_handler(GtkWidget *, GdkEvent *, gpointer);
void testdata_select_handler(GtkWidget *, gpointer);
stopToolFunc startTestsuite();
void * doStartTestsuite(void *);
void stopTestsuite();
gboolean gui_idle_testsuite(void);
void showFailingTestParams(cJSON * node, GtkTreeIter parent);
bool isTSFunctionDisplayed(cJSON *);
void handleTestsuiteEvent(cJSON *);