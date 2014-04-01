#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#include <dirent.h>
#include <errno.h>

#include "wizard.h"
#include "testsuite.h"
#include "debugger.h"
#include "logger.h"
#include "fdt.h"

static const int window_width = 700;
static const int window_height = 650;

static char * abs_libpath;
static char * bin_path;
static bool usingGui = TRUE;
static int terminate_calls = 0;
static pid_t fuse_pid = 0;
static stopToolFunc stopToolFunction = NULL;
static bool * fs_mounted;
static char * shared_error;
static size_t shared_error_maxlen = 1024;
static cJSON * fsigs;
static cJSON * resources;
static time_t last_fuse_activity;
static char * skipped_tests = NULL;
static char * test_data_file = NULL;

/* GUI widgets */
static GtkWidget * window;
static GtkWidget * binary_selector;
static bool binary_select_open = FALSE;
static GtkWidget * binary_field;
static GtkWidget * arguments_field;
static GtkWidget * start_button;
static GtkWidget * stop_button;
static GtkWidget * tabs;

bool isUsingGui() {
    return usingGui;
}

cJSON * getFunctionSignature(const char * name) {
    for(int i = 0; i < cJSON_GetArraySize(fsigs); i++) {
        cJSON * item = cJSON_GetArrayItem(fsigs, i);
        char * name_str = cJSON_GetObjectItem(item, "name")->valuestring;
        if(strcmp(name_str, name) == 0) {
            return item;
        }
    }
    return NULL;
}

cJSON * getFunctionResources(const char * name) {
    for(int i = 0; i < cJSON_GetArraySize(resources); i++) {
        cJSON * item = cJSON_GetArrayItem(resources, i);
        char * name_str = cJSON_GetObjectItem(item, "name")->valuestring;
        if(strcmp(name_str, name) == 0) {
            return item;
        }
    }
    return NULL;
}

void setStopToolFunction(stopToolFunc f) {
    stopToolFunction = f;
}

void testToFunction(char * function, const char * test) {
    strcpy(function, test + 5);
}

char * wrap_text(const char * orig, int line_len) {

    size_t orig_len = strlen(orig);
    char * wrapped = malloc(orig_len + 1);

    // Replace spaces with linebreaks every line_len chars, skipping non-space chars until we find a space
    // This allows longer words to overflow the line length
    bool needs_break = false;
    for(int i = 0 ; i <= orig_len; i++) {
        char c = orig[i];
        if(i > 0 && i % line_len == 0) needs_break = true;
        if(needs_break && c == ' ') {
            wrapped[i] = '\n';
            needs_break = false;
        } else {
            wrapped[i] = c;
        }
    }

    return wrapped;
}

char * getLibpath() {
    return abs_libpath;
}

pid_t getFusePID() {
    return fuse_pid;
}

void setFusePID(pid_t id) {
    fuse_pid = id;
}

void setFSMounted(bool mounted) {
    *fs_mounted = mounted;
}

bool isFSMounted() {
    return *fs_mounted;
}

void addSkippedTest(const char * func_name, const int test_num) {
    size_t test_str_len = strlen(func_name) + 64;
    char test_str[test_str_len];
    snprintf(test_str, test_str_len, "%s:%d", func_name, test_num);

    if(skipped_tests == NULL) {
        // First test
        skipped_tests = malloc(test_str_len);
        strcpy(skipped_tests, test_str);
    } else {
        // Append test to existing list
        skipped_tests = realloc(skipped_tests, strlen(skipped_tests) + test_str_len);
        strncat(skipped_tests, ",", 1);
        strncat(skipped_tests, test_str, test_str_len);
    }
}

void setTestDataFile(const char * fname) {
    if(test_data_file != NULL) {
        free(test_data_file);
    }
    test_data_file = malloc(strlen(fname) + 1);
    strcpy(test_data_file, fname);
    printf("Set test data file to '%s'\n", test_data_file);
}

/**
 * Setup environment variables before executing a filesystem, mainly for communication with libfuse
 */
char ** getEnvVarsForFork(const char * tool_ident) {
    char ** envp = malloc(10 * sizeof(char*));
    size_t envp_idx = 0;

    // Use libfuse wrapper is used instead of the real libfuse
    size_t ld_len = 16 + strlen(abs_libpath) + 1;
    char * ld_declaration = malloc(ld_len);
    snprintf(ld_declaration, ld_len, "LD_LIBRARY_PATH=%s", abs_libpath);
    envp[envp_idx++] = ld_declaration;

    // Same as the above but for Mac
    size_t dyld_len = 18 + strlen(abs_libpath) + 1;
    char * dyld_declaration = malloc(dyld_len);
    snprintf(dyld_declaration, dyld_len, "DYLD_LIBRARY_PATH=%s", abs_libpath);
    envp[envp_idx++] = dyld_declaration;

    // Specifies which tool we're using (i.e., wizard, testsuite or debugger)
    size_t tool_len = 9 + strlen(tool_ident) + 1;
    char * tool_declaration = malloc(tool_len);
    snprintf(tool_declaration, tool_len, "FDT_TOOL=%s", tool_ident);
    envp[envp_idx++] = tool_declaration;
    
    // Add the name and IDs of any skipped tests (for the wizard)
    if(skipped_tests != NULL) {
        size_t skipped_len = 14 + strlen(skipped_tests) + 1;
        char * skipped_declaration = malloc(skipped_len);
        snprintf(skipped_declaration, skipped_len, "SKIPPED_TESTS=%s", skipped_tests);
        printf("%s\n", skipped_declaration);
        envp[envp_idx++] = skipped_declaration;
    }

    // Set the path of the test data (for the test suite)
    if(test_data_file != NULL) {
        size_t testdatafile_len = 15 + strlen(test_data_file) + 1;
        char * testdatafile_declaration = malloc(testdatafile_len);
        snprintf(testdatafile_declaration, testdatafile_len, "TEST_DATA_FILE=%s", test_data_file);
        envp[envp_idx++] = testdatafile_declaration;
    }

    envp[envp_idx++] = 0;
    return envp;
}

/* Widgets for selecting the FUSE binary and passing arguments */
GtkWidget * createBinarySelectionWidgets() {
    GtkWidget * box = gtk_vbox_new(FALSE, 5);

    GtkWidget * bin_box = gtk_hbox_new(FALSE, 5);
        GtkWidget * binary_label = gtk_label_new("FUSE Binary:");
        gtk_widget_show(binary_label);
        gtk_box_pack_start((GtkBox *) bin_box, binary_label, FALSE, TRUE, 0);

        binary_field = gtk_entry_new();
        gtk_widget_show(binary_field);
        #if __APPLE__
            gtk_entry_set_text(GTK_ENTRY(binary_field), "/Users/md49/hg/CS4099-MajorSP/loopback-mac/loopback"); // TODO: empty by default
        #else
            gtk_entry_set_text(GTK_ENTRY(binary_field), "/home/md49/hg/CS4099-MajorSP/demofs-2/bbfs"); // TODO: empty by default
        #endif
        gtk_box_pack_start((GtkBox *) bin_box, binary_field, FALSE, TRUE, 0);

        GtkWidget* binary_select = gtk_button_new_with_label("Browse...");
        g_signal_connect(binary_select, "clicked", G_CALLBACK(binary_select_handler), NULL);
        gtk_widget_show(binary_select);
        gtk_box_pack_start((GtkBox *) bin_box, binary_select, FALSE, TRUE, 0);
    gtk_widget_show(bin_box);
    gtk_box_pack_start((GtkBox *) box, bin_box, TRUE, TRUE, 0);

    GtkWidget * args_box = gtk_hbox_new(FALSE, 5);
        GtkWidget * arguments_label = gtk_label_new("Arguments:");
        gtk_widget_show(arguments_label);
        gtk_box_pack_start((GtkBox *) args_box, arguments_label, FALSE, TRUE, 0);

        arguments_field = gtk_entry_new();
        gtk_widget_show(arguments_field);
        #if __APPLE__
            gtk_entry_set_text(GTK_ENTRY(arguments_field), "/tmp/lfsroot -f /tmp/lfsmnt -oallow_other,native_xattr,volname=LoopbackFS"); // TODO -f /tmp/mountpoint
        #else
            gtk_entry_set_text(GTK_ENTRY(arguments_field), "-f rootdir /tmp/fsmnt"); // TODO -f /tmp/mountpoint
        #endif
        gtk_box_pack_start((GtkBox *) args_box, arguments_field, TRUE, TRUE, 0);
    gtk_widget_show(args_box);
    gtk_box_pack_start((GtkBox *) box, args_box, TRUE, TRUE, 0);

    return box;
}

/* Control buttons for starting/stopping the utilities */
GtkWidget * createControlButtons() {
    GtkWidget * box = gtk_hbox_new(FALSE, 5);

    start_button = gtk_button_new_with_label("Start");
    g_signal_connect(start_button, "clicked", G_CALLBACK(start_btn_handler), NULL);
    gtk_widget_show(start_button);
    gtk_box_pack_start((GtkBox *) box, start_button, TRUE, TRUE, 0);

    stop_button = gtk_button_new_with_label("Stop");
    gtk_widget_set_sensitive(stop_button, FALSE);
    g_signal_connect(stop_button, "clicked", G_CALLBACK(stop_btn_handler), NULL);
    gtk_widget_show(stop_button);
    gtk_box_pack_start((GtkBox *) box, stop_button, TRUE, TRUE, 0);

    return box;
}

// Kill the FUSE binary and close the debugger
void terminate() {
    terminate_calls++;

    if(stopToolFunction == NULL) {
        // No stop function available, so it's safe to just quit
        if(fuse_pid != 0) {
            // Might as well send a SIGKILL to make sure the FUSE binary isn't left running
            kill(fuse_pid, SIGKILL);
            printf("Sent SIGKILL to FUSE binary\n");
        }
        exit(0);
    } else {
        // Killing the FUSE binary -should- cause the debugger to close gracefully
        // If the FUSE binary has a problem then this might not work, so force-quit after two terminate calls
        if(terminate_calls == 1) {
            printf("Stopping tool gracefully (try again for force-quit)\n");
            stopToolFunction();
            stopToolFunction = NULL;
            //exit(0);
            //kill(fuse_pid, SIGINT);
        } else {
            printf("Terminating forcefully\n");
            if(fuse_pid != 0) {
                kill(fuse_pid, SIGKILL);
                printf("Sent SIGKILL to FUSE binary\n");
            }
            exit(0);
        }
    }
}

void sigintHandler() {
    terminate();
}

/* Handlers for the FUSE binary selection dialog */
void binary_select_okbutton_handler(GtkWidget * widget, gpointer data) {
    const char * filename = gtk_file_selection_get_filename(GTK_FILE_SELECTION(binary_selector));
    gtk_entry_set_text(GTK_ENTRY(binary_field), filename);
    binary_select_open = FALSE;
}
void binary_select_cancelbutton_handler(GtkWidget * widget, gpointer data) {
    binary_select_open = FALSE;
}
gboolean binary_select_close_handler(GtkWidget * widget, GdkEvent * event, gpointer data) {
    binary_select_open = FALSE;
    return FALSE;
}
void binary_select_handler(GtkWidget * widget, gpointer data) {
    if(!binary_select_open) {
        binary_select_open = TRUE;
        binary_selector = gtk_file_selection_new("FUSE Binary");
        g_signal_connect(GTK_FILE_SELECTION(binary_selector)->ok_button, "clicked", G_CALLBACK(binary_select_okbutton_handler), binary_selector);
        g_signal_connect(GTK_FILE_SELECTION(binary_selector)->cancel_button, "clicked", G_CALLBACK(binary_select_cancelbutton_handler), binary_selector);
        g_signal_connect(binary_selector, "delete-event", G_CALLBACK(binary_select_close_handler), binary_selector);
        g_signal_connect_swapped(GTK_FILE_SELECTION(binary_selector)->ok_button, "clicked", G_CALLBACK(gtk_widget_destroy), binary_selector);
        g_signal_connect_swapped(GTK_FILE_SELECTION (binary_selector)->cancel_button, "clicked", G_CALLBACK(gtk_widget_destroy), binary_selector);
        gtk_widget_show(binary_selector);
    }
}

/* Main start button (so start debugging, or run the wizard, etc - depending on which tab is selected) */
void start_btn_handler(GtkWidget * widget, gpointer data) {
    gdk_threads_enter();
    const char * bin_str = gtk_entry_get_text(GTK_ENTRY(binary_field));
    const char * args_str = gtk_entry_get_text(GTK_ENTRY(arguments_field));
    gtk_widget_set_sensitive(start_button, FALSE);
    gdk_threads_leave();

    // Execute the correct function
    int selected_tab = gtk_notebook_get_current_page(GTK_NOTEBOOK(tabs));
    switch(selected_tab) {
        case 0:
            startTool(bin_str, args_str, "wizard", &startWizard);
        break;
        case 1:
            startTool(bin_str, args_str, "testsuite", &startTestsuite);
        break;
        case 2:
            startTool(bin_str, args_str, "debugger", &startDebugger);
        break;
    }
}

/* Stop the running filesystem and unmount. So terminate the debugger, for example */
void stop_btn_handler(GtkWidget * widget, gpointer data) {
    gdk_threads_enter();
    gtk_widget_set_sensitive(stop_button, FALSE);
    gdk_threads_leave();
    if(stopToolFunction != NULL) {
        stopToolFunction();
        stopToolFunction = NULL;
    } else {
        fprintf(stderr, "No stop function defined for tool\n");
    }
}

/* Called on an attempt to close the main window */
gboolean delete_event(GtkWidget * widget, GdkEvent * event, gpointer data) {
    terminate();
    return TRUE;
}

/* Called when the main window is actually closing */
void destroy(GtkWidget *widget, gpointer data) {
    terminate();
}

void showErrorDialog(char * message) {
    GtkWidget * dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                                  GTK_DIALOG_DESTROY_WITH_PARENT,
                                  GTK_MESSAGE_ERROR,
                                  GTK_BUTTONS_CLOSE,
                                  "%s", message);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void showGUI(int * argc, char *** argv) {

    gtk_init(argc, argv);
    
    // Create the main window, and attach close handlers
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "FUSE Diagnostic Tool (FDT)");
    gtk_window_set_default_size(GTK_WINDOW(window), window_width, window_height);
    g_signal_connect(window, "delete-event", G_CALLBACK(delete_event), NULL);
    g_signal_connect(window, "destroy", G_CALLBACK(destroy), NULL);
    gtk_container_set_border_width(GTK_CONTAINER(window), 10);
    
    GtkWidget * window_vbox = gtk_vbox_new(FALSE, 20);
        
        char title_img_path[strlen(bin_path) + 14 + 1];
        strcpy(title_img_path, bin_path);
        strcat(title_img_path, "/fdt-title.png");
        GtkWidget * title = gtk_image_new_from_file(title_img_path);
        gtk_widget_show(title);
        gtk_box_pack_start((GtkBox *) window_vbox, title, FALSE, TRUE, 10);

        /* Widgets for selecting the FUSE binary and passing arguments */
        GtkWidget * binary_selection_widgets = createBinarySelectionWidgets();
        gtk_widget_show(binary_selection_widgets);
        gtk_box_pack_start((GtkBox *) window_vbox, binary_selection_widgets, FALSE, TRUE, 0);

        /* Control buttons for starting/stopping the utilities */
        GtkWidget * control_buttons = createControlButtons();
        gtk_widget_show(control_buttons);
        gtk_box_pack_start((GtkBox *) window_vbox, control_buttons, FALSE, TRUE, 0);

        /* Tabs to choose between wizard, test suite and debugger */
        tabs = gtk_notebook_new();
            GtkWidget * wizard_label = gtk_label_new("Wizard");
            GtkWidget * wizard_page = createWizardTab();
            gtk_widget_show(wizard_page);
            gtk_notebook_append_page(GTK_NOTEBOOK(tabs), wizard_page, wizard_label);

            GtkWidget * test_label = gtk_label_new("Test Suite");
            GtkWidget * test_page = createTestsuiteTab();
            gtk_widget_show(test_page);
            gtk_notebook_append_page(GTK_NOTEBOOK(tabs), test_page, test_label);

            GtkWidget * debugger_label = gtk_label_new("Debugger");
            GtkWidget * debugger_page = createDebuggerTab();
            gtk_widget_show(debugger_page);
            gtk_notebook_append_page(GTK_NOTEBOOK(tabs), debugger_page, debugger_label);
        gtk_widget_show(tabs);
        gtk_box_pack_start((GtkBox *) window_vbox, tabs, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(window), window_vbox);
    gtk_widget_show(window_vbox);
    gtk_window_present(GTK_WINDOW(window));
    
    // Call this function whenever the GUI is idle so that it can perform queued updates
    g_idle_add((GSourceFunc) gui_idle, NULL);

    // Hand over control of the thread to gtk
    gtk_main();
}

gboolean gui_idle(void) {
    if(shared_error[0] != '\0') {
        showErrorDialog(shared_error);
        shared_error[0] = '\0';
    }

    // Make sure start/stop buttons are activated correctly
    gdk_threads_enter();
    if(*fs_mounted) {
        if(gtk_widget_is_sensitive(start_button)) gtk_widget_set_sensitive(start_button, FALSE);
        if(!gtk_widget_is_sensitive(stop_button)) gtk_widget_set_sensitive(stop_button, TRUE);
    } else {
        if(!gtk_widget_is_sensitive(start_button)) gtk_widget_set_sensitive(start_button, TRUE);
        if(gtk_widget_is_sensitive(stop_button)) gtk_widget_set_sensitive(stop_button, FALSE);
    }
    gdk_threads_leave();

    gui_idle_wizard();
    gui_idle_testsuite();
    gui_idle_debugger();
    
    return TRUE;
}

void setSharedError(char * err_str) {
    if(strlen(err_str) >= shared_error_maxlen) {
        err_str[shared_error_maxlen-1] = '\0';
    }
    strcpy(shared_error, err_str);
}

void setSharedErrorPrefixed(char * prefix, char * err_str_raw) {
    char err_str[strlen(prefix) + strlen(err_str_raw) + 1];
    snprintf(err_str, sizeof err_str, "%s%s", prefix, err_str_raw);
    setSharedError(err_str);
}

/* Start a tool by passing info about the FUSE binary and a
 * function pointer to either the wizard, test suite or debugger
 */
void startTool(const char * bin_str, const char * args_str, const char * tool_ident, startToolFunc toolFunc) {

    terminate_calls = 0;

    char * args_str_cpy = malloc(strlen(args_str) + 1);
    strcpy(args_str_cpy, args_str);

    // First count the number of arguments in the string, and init an empty array
    int num_args = 0;
    char ** stringp = &args_str_cpy;
    char * token = NULL;
    while(token = strsep(stringp, " ")) {
        num_args++;
    }
    free(args_str_cpy);
    char * fuse_argv[num_args + 1];

    // Parse the arg string again, putting the args into the array
    args_str_cpy = malloc(strlen(args_str) + 1);
    strcpy(args_str_cpy, args_str);
    stringp = &args_str_cpy;
    token = NULL;
    int i = 1;
    while(token = strsep(stringp, " ")) {
        fuse_argv[i] = token;
        i++;
    }
    free(args_str_cpy);

    // Add binary name and NULL terminator
    char * bin_str_cpy = malloc(strlen(bin_str) + 1);
    strcpy(bin_str_cpy, bin_str);
    fuse_argv[0] = bin_str_cpy;
    fuse_argv[num_args + 1] = NULL;

    // Get environment variables needed to run filesystem with our modified libfuse
    char ** envp = getEnvVarsForFork(tool_ident);

    // Execute both the tool and FUSE binary in parallel
    // The tool executes in a new thread while the FUSE binary executes in a new forked process
    int status;
    switch((fuse_pid = fork())) {
        case -1:
            perror("fork");
            break;
        case 0:
            *fs_mounted = TRUE;
            execve(fuse_argv[0], fuse_argv, envp);
            setSharedErrorPrefixed("Could not execute fuse binary: ", strerror(errno));
            fuse_pid = 0;
            //perror("Could not execute fuse binary");
            //fprintf(stderr, "Try using the absolute path to the FUSE implementation if it was not found\n");
            exit(EXIT_FAILURE);
            break;
        default:
            stopToolFunction = toolFunc();
            break;
    }
    free(bin_str_cpy);
}

static void childKilled(int sig) {
    // Iterate over killed child processes
    pid_t pid;
    int status;
    while((pid = waitpid(-1, &status, WNOHANG)) != -1) {
        if(pid == fuse_pid) {
            // FUSE binary terminated but we expect it to be running
            // This detects when the FUSE binary crashes, but we wait a bit to make sure we don't show an error just because the event reader is slow
            // Wait up to 1s, checking every 100ms
            printf("Unexpected SIGCHLD! Waiting 1000ms to synchronise...\n");
            bool clean_exit = FALSE;
            int slot_id = 0;
            for(slot_id = 0; slot_id < 10; slot_id++) {
                usleep(100000);
                if(pid != fuse_pid) {
                    // Fuse unmounted cleanly, so don't need to wait any longer
                    printf("Filesystem unmounted cleanly while waiting\n");
                    *fs_mounted = FALSE;
                    clean_exit = TRUE;
                    break;
                }
            }

            if(!clean_exit) {
                printf("Filesystem unmounted unexpectedly\n");
                if(strlen(shared_error) == 0) {
                    // Child exited but didn't pass an error message
                    setSharedError("FUSE binary terminated. Check the console for a possible cause.");
                }
                fuse_pid = 0;
                *fs_mounted = FALSE;
            }
            break;
        } else if(fuse_pid == 0) {
            // If we don't have a FUSE PID but something exited, then we have to assume the FS is unmounted
            *fs_mounted = FALSE;
        }
    }
}

// Open a file, read the string, and parse it into a JSON object
cJSON * readJSONFile(const char * fpath) {
    printf("Loading JSON file: %s\n", fpath);
    FILE * file = fopen(fpath, "r");
    if(file != NULL) {
        fseek(file, 0, SEEK_END);
        size_t fsize = ftell(file);
        rewind(file);
        char * str = malloc((fsize+1) * sizeof(char));
        fread(str, sizeof(char), fsize, file);
        str[fsize] = '\0';
        fclose(file);
        cJSON * obj = cJSON_Parse(str);
        free(str);
        printf("Loaded %zu bytes\n", fsize);
        return obj;
    } else {
        perror("Unable to read file");
        return NULL;
    }
}

void printLoggerUsage() {
    printf("Usage: fdt --logger [-a] [FUSE Binary] [FUSE Arguments]\n\n");
    printf("Options:\n");
    printf("\t-a\t Start capturing sequence automatically on mount\n\n");
    printf("Example: ./fdt --logger /Users/md49/hg/CS4099-MajorSP/loopback-mac/loopback /tmp/lfsroot -f /tmp/lfsmnt -oallow_other,native_xattr,volname=LoopbackFS\n");
}

void resolveLibraryPaths(int argc, char **argv) {
    // Get the path of the binary (only works when executed from special shell script)
    int last_slash_pos = 0;
    for(int i = 0; i < strlen(argv[0]); i++) {
        if(argv[0][i] == '/') last_slash_pos = i;
    }
    bin_path = malloc(last_slash_pos + 1);
    for(int i = 0; i <= last_slash_pos; i++) {
        bin_path[i] = argv[0][i];
    }
    bin_path[last_slash_pos] = '\0';
    
    // Now append our libfuse directory to this path
    #if __APPLE__
        // osxfuse outputs a folder labelled with the version numbers, so find it
        // it starts with osxfuse-core-
        char * outdir = "/osxfuse/out/";
        char outpath[strlen(bin_path) + strlen(outdir) + 1];
        snprintf(outpath, sizeof outpath, "%s%s", bin_path, outdir);

        char * coredir = NULL;
        DIR * dp;
        struct dirent * ep;
        dp = opendir(outpath);
        if(dp != NULL) {
            while(ep = readdir(dp)) {
                char * prefix = "osxfuse-core-";
                char * d_name = ep->d_name;
                if(strncmp(prefix, d_name, strlen(prefix)) == 0) {
                    coredir = d_name;
                }
            }
            closedir(dp);
        } else {
            perror("Unable to find osxfuse wrapper directory\n");
            fprintf(stderr, "Make sure there is a compiled version of osxfuse in %s\n", outpath);
            exit(EXIT_FAILURE);
        }
        char * libdir;
        if(coredir != NULL) {
            char * libdir_prefix = "/osxfuse/out/";
            char * libdir_suffix = "/osxfuse/usr/local/lib";
            size_t libdir_len = strlen(libdir_prefix) + strlen(coredir) + strlen(libdir_suffix) + 1;
            libdir = malloc(libdir_len);
            snprintf(libdir, libdir_len, "%s%s%s", libdir_prefix, coredir, libdir_suffix);
        } else {
            fprintf(stderr, "Unable to find osxfuse wrapper directory\n");
            fprintf(stderr, "Make sure there is a compiled version of osxfuse in %s\n", outpath);
            exit(EXIT_FAILURE);
        }
    #else
        char * libdir = "/libfuse/lib/.libs";
    #endif
    char libpath[strlen(bin_path) + strlen(libdir) + 1];
    snprintf(libpath, sizeof libpath, "%s%s", bin_path, libdir);
    
    // Resolve the relative path into an absolute path
    abs_libpath = malloc(PATH_MAX + 1);
    char * res = realpath(libpath, abs_libpath);
    if(!res) {
        perror("Unable to open libfuse wrapper directory\n");
        fprintf(stderr, "Make sure there is a compiled version of libfuse in %s\n", libpath);
        exit(EXIT_FAILURE);
    }
}

int main(int argc, char **argv) {

    // Handle sigint
    signal(SIGINT, sigintHandler);

    // Handle SIGCHLD, which occurs when child processes exit
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = childKilled;
    sigaction(SIGCHLD, &sa, NULL);

    usingGui = TRUE;

    // Allocate some shared memory for forked processes to communicate errors
    // Linux uses MAP_ANONYMOUS while Mac uses MAP_ANON
    #if __APPLE__
        shared_error = mmap(NULL, shared_error_maxlen, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
        fs_mounted = mmap(NULL, sizeof(fs_mounted), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    #else
        shared_error = mmap(NULL, shared_error_maxlen, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        fs_mounted = mmap(NULL, sizeof(fs_mounted), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    #endif
    strcpy(shared_error, "");
    *fs_mounted = FALSE;

    resolveLibraryPaths(argc, argv);
    printf("Using libfuse wrapper directory: %s\n", abs_libpath);

    /* Console-based logger */
    if(argc >= 2 && strcmp(argv[1], "--logger") == 0) {
        if(argc >= 4) {
            
            // Check for the -a flag, and also get the FUSE binary name
            int bin_idx;
            if(strcmp(argv[2], "-a") == 0) {
                setLoggerState(CAPTURING_SEQUENCE);
                bin_idx = 3;
            } else {
                bin_idx = 2;
            }
            char * bin_str = argv[bin_idx];

            // Concatenate remaining arguments into a single string
            size_t args_len = 1;
            for(int i = bin_idx + 1; i < argc; i++) {
                args_len = strlen(argv[i]) + 1;
            }
            char args_str[args_len];
            args_str[0] = '\0';
            strcat(args_str, argv[bin_idx+1]);
            for(int i = bin_idx + 2; i < argc; i++) {
                strcat(args_str, " ");
                strcat(args_str, argv[i]);
            }

            // The tool identifier is debugger as we need to debug to log calls
            startTool(bin_str, args_str, "debugger", &startLogger);
        } else {
            printLoggerUsage();
        }
        return 0;
    }

    // Load the JSON file containing function signatures
    #if __APPLE__
        char * suf_fsigs_path = "/osxfuse/fuse/fsigs.json";
    #else
        char * suf_fsigs_path = "/libfuse/fsigs.json";
    #endif
    char rel_fsigs_path[strlen(bin_path) + strlen(suf_fsigs_path) + 1];
    snprintf(rel_fsigs_path, sizeof rel_fsigs_path, "%s%s", bin_path, suf_fsigs_path);
    char abs_fsigs_path[PATH_MAX + 1];
    char * res = realpath(rel_fsigs_path, abs_fsigs_path);
    if(!res) {
        perror("Unable to open JSON file containing function signatures\n");
        exit(EXIT_FAILURE);
    }
    fsigs = readJSONFile(abs_fsigs_path);

    // Load the JSON file containing resources for each function
    char rel_res_path[strlen(bin_path) + 16];
    snprintf(rel_res_path, sizeof rel_res_path, "%s/resources.json", bin_path);
    char abs_res_path[PATH_MAX + 1];
    res = realpath(rel_res_path, abs_res_path);
    if(!res) {
        perror("Unable to open JSON file containing function resources\n");
        exit(EXIT_FAILURE);
    }
    resources = readJSONFile(abs_res_path);

    // Open the GUI
    if(usingGui) {
        initGUIEventQueue();
        showGUI(&argc, &argv);
    }
    
    return 0;
}
