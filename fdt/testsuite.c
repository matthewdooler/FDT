#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/queue.h>
#include <glib.h>

#include "cJSON.h"
#include "fdt.h"
#include "testsuite.h"

static const char * testsuite_fifo_name = "fuse-testsuite.fifo";
static FILE * testsuite_fifo = NULL;

static pthread_mutex_t tailq_events_lock;
static GSList * ts_events;
static GSList * ts_events_displayed;
static GHashTable * rows;
static GtkTreeStore * event_table_model;

static GtkWidget * testdata_selector;
static bool testdata_select_open = FALSE;
static GtkWidget * testdata_field;
static bool recv_end_event = FALSE;

// Used to keep track of the current test group/sequence as we're populating the results table
static GtkTreeIter * group_iter = NULL;
static GtkTreeIter * sequence_iter = NULL;

GtkWidget * createTestsuiteTab() {

    GtkWidget * tab = gtk_vbox_new(FALSE, 5);

    // Browse widget for JSON test data
	GtkWidget * testdata_box = gtk_hbox_new(FALSE, 5);
        GtkWidget * testdata_label = gtk_label_new("Test Data:");
        gtk_widget_show(testdata_label);
        gtk_box_pack_start((GtkBox *) testdata_box, testdata_label, FALSE, TRUE, 0);

        testdata_field = gtk_entry_new();
        gtk_widget_show(testdata_field);
        gtk_widget_set_sensitive(testdata_field, FALSE);
        gtk_box_pack_start((GtkBox *) testdata_box, testdata_field, TRUE, TRUE, 0);

        GtkWidget * testdata_select = gtk_button_new_with_label("Browse...");
        g_signal_connect(testdata_select, "clicked", G_CALLBACK(testdata_select_handler), NULL);
        gtk_widget_show(testdata_select);
        gtk_box_pack_start((GtkBox *) testdata_box, testdata_select, FALSE, TRUE, 0);
    gtk_widget_show(testdata_box);
    gtk_box_pack_start((GtkBox *) tab, testdata_box, FALSE, TRUE, 0);

    // Main results area containing a list of tests to the left and a sidebar to the right
    GtkWidget * results_box = gtk_hbox_new(FALSE, 5);
    	GtkWidget * tests_view = createTestsView();
    	gtk_box_pack_start((GtkBox *) results_box, tests_view, TRUE, TRUE, 0);
	gtk_widget_show(results_box);
	gtk_box_pack_start((GtkBox *) tab, results_box, TRUE, TRUE, 0);

    return tab;
}

GtkWidget * createTestsView() {
	GtkWidget * scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scrolled_window, 200, -1);
    
    event_table_model = gtk_tree_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget * tree_view = gtk_tree_view_new();
    gtk_container_add(GTK_CONTAINER(scrolled_window), tree_view);
    gtk_tree_view_set_model(GTK_TREE_VIEW(tree_view), GTK_TREE_MODEL(event_table_model));
    gtk_widget_show(tree_view);
    
    GtkCellRenderer * cell_func = gtk_cell_renderer_text_new();
    GtkTreeViewColumn * column_func = gtk_tree_view_column_new_with_attributes("Test", cell_func, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), GTK_TREE_VIEW_COLUMN(column_func));
    
    GtkCellRenderer * cell_status = gtk_cell_renderer_text_new();
    GtkTreeViewColumn * column_status = gtk_tree_view_column_new_with_attributes("Status", cell_status, "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), GTK_TREE_VIEW_COLUMN(column_status));

    gtk_widget_show(scrolled_window);
    return scrolled_window;
}

/* Handlers for the test data selection dialog */
void testdata_select_okbutton_handler(GtkWidget * widget, gpointer data) {
    const char * filename = gtk_file_selection_get_filename(GTK_FILE_SELECTION(testdata_selector));
    gtk_entry_set_text(GTK_ENTRY(testdata_field), filename);
    testdata_select_open = FALSE;
    setTestDataFile(filename);
}
void testdata_select_cancelbutton_handler(GtkWidget * widget, gpointer data) {
    testdata_select_open = FALSE;
}
gboolean testdata_select_close_handler(GtkWidget * widget, GdkEvent * event, gpointer data) {
    testdata_select_open = FALSE;
    return FALSE;
}
void testdata_select_handler(GtkWidget * widget, gpointer data) {
    if(!testdata_select_open) {
        testdata_select_open = TRUE;
        testdata_selector = gtk_file_selection_new("FUSE Binary");
        g_signal_connect(GTK_FILE_SELECTION(testdata_selector)->ok_button, "clicked", G_CALLBACK(testdata_select_okbutton_handler), testdata_selector);
        g_signal_connect(GTK_FILE_SELECTION(testdata_selector)->cancel_button, "clicked", G_CALLBACK(testdata_select_cancelbutton_handler), testdata_selector);
        g_signal_connect(testdata_selector, "delete-event", G_CALLBACK(testdata_select_close_handler), testdata_selector);
        g_signal_connect_swapped(GTK_FILE_SELECTION(testdata_selector)->ok_button, "clicked", G_CALLBACK(gtk_widget_destroy), testdata_selector);
        g_signal_connect_swapped(GTK_FILE_SELECTION (testdata_selector)->cancel_button, "clicked", G_CALLBACK(gtk_widget_destroy), testdata_selector);
        gtk_widget_show(testdata_selector);
    }
}

stopToolFunc startTestsuite() {

	// Create a FIFO so that we can receive events
    recv_end_event = FALSE;
    unlink(testsuite_fifo_name);
    mkfifo(testsuite_fifo_name, 0666);

    // Start receiving events in a separate thread
    pthread_t t;
    int t_retval;
    t_retval = pthread_create(&t, NULL, doStartTestsuite, NULL);
    if(t_retval != 0) {
        perror("pthread_create");
    }
    return stopTestsuite;
}

void * doStartTestsuite(void * ptr) {

    // Read JSON chunks from the shared pipe that the libfuse wrapper logs to
    testsuite_fifo = fopen(testsuite_fifo_name, "r");

    // Local lists of events sent by libfuse wrapper
    if(ts_events != NULL) {
        // Clear any old events from previous runs
        g_slist_free(ts_events);
        g_slist_free(ts_events_displayed);
        g_hash_table_destroy(rows);
    }

    ts_events = g_slist_alloc();
    ts_events_displayed = g_slist_alloc();
    rows = g_hash_table_new(g_str_hash, g_str_equal);
    
    bool fifo_open;
    if(testsuite_fifo != NULL) fifo_open = TRUE;
    else fifo_open = FALSE;
    
    size_t chunk_pos = 0;
    size_t json_chunk_capacity = 1024;
    char *json_chunk = malloc(json_chunk_capacity);

    while(fifo_open) {
        // Construct a chunk of JSON
        int unclosed_braces = 0;
        chunk_pos = 0;
        
        do {
            int c = fgetc(testsuite_fifo);
            if(c == EOF) {
                fifo_open = FALSE;
            } else {
                if(chunk_pos+1 >= json_chunk_capacity) {
                    // Double string capacity before we overflow it
                    json_chunk_capacity *= 2;
                    json_chunk = realloc(json_chunk, json_chunk_capacity);
                }
                json_chunk[chunk_pos++] = (char) c;
                if(c == '{') unclosed_braces++;
                if(c == '}') unclosed_braces--;
            }
        } while(unclosed_braces > 0);
        json_chunk[chunk_pos++] = '\0';
        
        // Parse the JSON and display the event
        if(strlen(json_chunk) > 0) {
            cJSON * event = cJSON_Parse(json_chunk);
            json_chunk[0] = '\0';
            handleTestsuiteEvent(event);
        }
    }
    free(json_chunk);

    if(testsuite_fifo != NULL) {
        fclose(testsuite_fifo);
    }
    
    unlink(testsuite_fifo_name);
}

void stopTestsuite() {
	pid_t fuse_pid = getFusePID();
    if(fuse_pid != 0) {
        setFusePID(0);
        kill(fuse_pid, SIGINT);
        printf("Sent SIGINT to FUSE binary\n");
    }
}

gboolean gui_idle_testsuite(void) {

    pthread_mutex_lock(&tailq_events_lock);

    // Clear table if there are no events currently displayed
    if(ts_events_displayed != NULL && ts_events_displayed->next == NULL) {
        gtk_tree_store_clear(GTK_TREE_STORE(event_table_model));
    }
    
    // Append any new rows
    GSList * node = ts_events;
    while(node != NULL) {
        cJSON * event = (cJSON *) node->data;
        if(event != NULL && !isTSFunctionDisplayed(event)) {
            const char * test_name = cJSON_GetObjectItem(event, "func_name")->valuestring;

            if(strcmp(test_name, "__GROUP_START") == 0) {
                // Create Group branch in tree
                GtkTreeIter * iter = malloc(sizeof(*iter));
                group_iter = iter;
                gdk_threads_enter();
                gtk_tree_store_append(GTK_TREE_STORE(event_table_model), iter, NULL);
                gtk_tree_store_set(GTK_TREE_STORE(event_table_model), iter, 0, "Test Group", 1, "Running", -1);
                gdk_threads_leave();
            } else if(strcmp(test_name, "__SEQUENCE_START") == 0) {
                // Create Sequence branch in current Group branch
                char * action = cJSON_GetObjectItem(event, "action")->valuestring;
                char * application = cJSON_GetObjectItem(event, "application")->valuestring;
                char * os = cJSON_GetObjectItem(event, "os")->valuestring;
                char sequence_label[strlen(action) + strlen(application) + strlen(os) + 128];
                sequence_label[0] = '\0';
                strcat(sequence_label, action);
                strcat(sequence_label, " using ");
                strcat(sequence_label, application);
                strcat(sequence_label, " on ");
                strcat(sequence_label, os);

                GtkTreeIter * iter = malloc(sizeof(*iter));
                sequence_iter = iter;
                gdk_threads_enter();
                gtk_tree_store_append(GTK_TREE_STORE(event_table_model), iter, group_iter);
                gtk_tree_store_set(GTK_TREE_STORE(event_table_model), iter, 0, sequence_label, 1, "Running", -1);
                gdk_threads_leave();
            } else if(strcmp(test_name, "__SEQUENCE_END") == 0) {
                // Close Sequence branch and display whether it passed or failed
                char * result = NULL;
                if(cJSON_GetObjectItem(event, "passed")->valueint == 0) result = "FAILED";
                else result = "OK";
                gdk_threads_enter();
                gtk_tree_store_set(GTK_TREE_STORE(event_table_model), sequence_iter, 1, result, -1);
                gdk_threads_leave();
                sequence_iter = NULL;
            } else if(strcmp(test_name, "__GROUP_END") == 0) {
                // Close Group branch and display whether it passed or failed
                char * result = NULL;
                if(cJSON_GetObjectItem(event, "passed")->valueint == 0) result = "FAIL";
                else result = "PASS";
                gdk_threads_enter();
                gtk_tree_store_set(GTK_TREE_STORE(event_table_model), group_iter, 1, result, -1);
                gdk_threads_leave();
                group_iter = NULL;
            } else if(strcmp(test_name, "__ERROR") == 0) {
                // Display error but let the tool continue reporting any other events as it might not be critical
                // __END will be sent when its time to terminate
                showErrorDialog(cJSON_GetObjectItem(event, "message")->valuestring);
            } else {
                // Add call to current Sequence branch
                GtkTreeIter iter;
                char * result = NULL;
                if(cJSON_GetObjectItem(event, "passed")->valueint == 0) result = "FAILED";
                else result = "OK";
                gdk_threads_enter();
                gtk_tree_store_append(GTK_TREE_STORE(event_table_model), &iter, sequence_iter);
                gtk_tree_store_set(GTK_TREE_STORE(event_table_model), &iter, 0, test_name, 1, result, -1);
                gdk_threads_leave();

                // If the test failed then display the parameter values that were passed to it
                if(cJSON_GetObjectItem(event, "passed")->valueint == 0) {
                    
                    // Display error message
                    cJSON * message = cJSON_GetObjectItem(event, "message");
                    if(message != NULL) {
                        char * wrapped = wrap_text(message->valuestring, 40); // wrap with linebreaks every 40 chars
                        GtkTreeIter msg_iter;
                        gdk_threads_enter();
                        gtk_tree_store_append(GTK_TREE_STORE(event_table_model), &msg_iter, &iter);
                        gtk_tree_store_set(GTK_TREE_STORE(event_table_model), &msg_iter, 0, wrapped, -1);
                        gdk_threads_leave();
                        free(wrapped);
                    } else {
                        fprintf(stderr, "Failing test did not include an error message\n");
                    }

                    // Display parameter data if present (it wont be present if the error is irrelevant to parameter values)
                    // Also enclose params within their own branch
                    cJSON * params = cJSON_GetObjectItem(event, "params");
                    if(params != NULL) {
                        GtkTreeIter params_iter;
                        gdk_threads_enter();
                        gtk_tree_store_append(GTK_TREE_STORE(event_table_model), &params_iter, &iter);
                        gtk_tree_store_set(GTK_TREE_STORE(event_table_model), &params_iter, 0, "Parameters passed to function", -1);
                        gdk_threads_leave();
                        showFailingTestParams(params, params_iter);
                    }
                }
            }

            ts_events_displayed = g_slist_prepend(ts_events_displayed, event);
        }

        node = node->next;
    }

    pthread_mutex_unlock(&tailq_events_lock);

    return TRUE; // only return false if we never want to be called again
}

void showFailingTestParams(cJSON * node, GtkTreeIter parent) {
    if(node->type == cJSON_Array || node->type == cJSON_Object) {
        for(int i = 0; i < cJSON_GetArraySize(node); i++) {
            cJSON * item = cJSON_GetArrayItem(node, i);
            
            GtkTreeIter itr;
            gdk_threads_enter();
            gtk_tree_store_prepend(GTK_TREE_STORE(event_table_model), &itr, &parent);
            gdk_threads_leave();
            
            gdk_threads_enter();
            if(item->type == cJSON_Array || item->type == cJSON_Object) {
                // inside node, so just show the label
                gtk_tree_store_set(GTK_TREE_STORE(event_table_model), &itr, 0, item->string, -1);
            } else {
                // leaf node, so show label with value (eg, "timestamp: 0189276")
                char * label = item->string;
                char * value = cJSON_Print(item);
                char label_and_value[strlen(label) + 2 + strlen(value) + 1];
                snprintf(label_and_value, sizeof label_and_value, "%s: %s", label, value);
                gtk_tree_store_set(GTK_TREE_STORE(event_table_model), &itr, 0, label_and_value, -1);
            }
            gdk_threads_leave();
            
            showFailingTestParams(item, itr);
        }
    } else {
        return;
    }
}

// True if this test result is already displayed in the results table
bool isTSFunctionDisplayed(cJSON * event) {
    GSList * node = ts_events_displayed;
    while(node != NULL) {
        cJSON * t_event = (cJSON *) node->data;
        if(event == t_event) {
            return true;
        }
        node = node->next;
    }
    return false;
}

void handleTestsuiteEvent(cJSON * event) {
    printf("%s\n", cJSON_Print(event));
    char * func_name = cJSON_GetObjectItem(event, "func_name")->valuestring;
    if(strcmp(func_name, "__END") == 0) {
        // Special event indicating that the test suite is finished
        // Set our internal state as unmounted before we get SIGCHLD
        recv_end_event = TRUE;
        setFusePID(0);
        printf("FUSE binary detached so test suite will terminate.\n");
    } else {
        if(isUsingGui()) {
            // Add event to GUI queue (as we can't make changes from this thread directly)
            pthread_mutex_lock(&tailq_events_lock);
            ts_events = g_slist_append(ts_events, event);
            pthread_mutex_unlock(&tailq_events_lock);
        } else {
            // Otherwise, we don't need the event anymore
            cJSON_Delete(event);
        }
    }
}