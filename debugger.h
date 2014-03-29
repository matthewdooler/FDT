#pragma once
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <stdbool.h>

#include "cJSON.h"
#include "fdt.h"

GtkWidget * createTopButtons();
GtkWidget * createEventsView();
GtkWidget * createDebuggerTab();
stopToolFunc startDebugger();
void stopDebugger();
void * doStartDebugger(void * ptr);
void waitToAdvance();
bool canAdvance();
void advance();
void advancePending();
void scrollToTop(GtkWidget * scrolled_window);
void updateAdvanceButtonState();
gboolean gui_idle_debugger(void);
void showParams(cJSON * node, GtkTreeIter parent);
void handleDebuggerEvent(cJSON * event);
static void autoadvance_btn_handler(GtkWidget * widget, gpointer data);
static void advance_btn_handler(GtkWidget * widget, gpointer data);
void initGUIEventQueue();