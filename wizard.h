#pragma once
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <stdbool.h>

#include "cJSON.h"
#include "fdt.h"

GtkWidget * createWizardTab();
GtkWidget * createMessageView();
void setMessageViewFunc(const char * str);
void setMessageViewMessage(const char * str);
void updateInfoTabs(const char * func_name);
void updateSignatureTab(const char * func_name);
void updateResourcesTab(const char * func_name);
gboolean hyperlink_handler_osx(GtkLabel * label, gchar * uri, gpointer  user_data);
void setLabelText(GtkLabel * label, const char * str, const int size);
void skip_btn_handler(GtkWidget * widget, gpointer data);
GtkWidget * createFunctionsView();
stopToolFunc startWizard();
void * doStartWizard(void * ptr);
void stopWizard();
gboolean gui_idle_wizard(void);
bool isFunctionDisplayed(cJSON * event);
void updateFunctionRow(const char * func_name);
funcstat getFunctionStatus(const char * test_name, cJSON ** failing_event);
void testToFunction(char * function, const char * test);
void handleWizardEvent(cJSON * event);