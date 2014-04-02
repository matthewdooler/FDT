#pragma once
#define _GNU_SOURCE
#include <gtk/gtk.h>
#include <stdbool.h>

typedef void (*stopToolFunc)(void);
typedef stopToolFunc (*startToolFunc)(void);
typedef enum {IMPLEMENTED, DEFINED, UNDEFINED, SKIPPED} funcstat;

char * bufferToAscii(const char * str);
cJSON * getFunctionSignature(const char * name);
cJSON * getFunctionResources(const char * name);
void setStopToolFunction(stopToolFunc f);
void testToFunction(char * function, const char * test);
char * wrap_text(const char * original, int line_length);
bool isUsingGui();
char * getLibpath();
pid_t getFusePID();
void setFusePID(pid_t);
void setFSMounted(bool mounted);
bool isFSMounted();
void addSkippedTest(const char * func_name, const int test_num);
void setTestDataFile(const char * fname);
char ** getEnvVarsForFork(const char * tool_ident);
GtkWidget * createBinarySelectionWidgets();
GtkWidget * createControlButtons();
void terminate();
void sigintHandler();
void binary_select_okbutton_handler(GtkWidget * widget, gpointer data);
void binary_select_cancelbutton_handler(GtkWidget * widget, gpointer data);
void binary_select_handler(GtkWidget * widget, gpointer data);
gboolean binary_select_close_handler(GtkWidget * widget, GdkEvent * event, gpointer data);
void start_btn_handler(GtkWidget * widget, gpointer data);
void stop_btn_handler(GtkWidget * widget, gpointer data);
gboolean delete_event(GtkWidget * widget, GdkEvent * event, gpointer data);
void destroy(GtkWidget *widget, gpointer data);
void showErrorDialog(char * message);
void showGUI(int * argc, char *** argv);
gboolean gui_idle(void);
void setSharedError(char * err_str);
void startTool(const char * bin_str, const char * args_str, const char * tool_ident, startToolFunc toolFunc);
static void childKilled(int sig);
cJSON * readJSONFile(const char * fpath);
void printLoggerUsage();
void resolveLibraryPaths(int argc, char **argv);
int main(int argc, char **argv);