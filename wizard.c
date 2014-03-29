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
#include "wizard.h"

static const char * wizard_fifo_name = "fuse-wizard.fifo";
static FILE * wizard_fifo = NULL;

static pthread_mutex_t tailq_events_lock;
static GSList * wizard_events;
static GSList * wizard_events_displayed;
static GHashTable * rows; // func_name:GtkTreeIter

static cJSON * notification_event = NULL;
static funcstat notification_event_status;
static bool recv_end_event = FALSE;

/* GUI Widgets */
static GtkWidget * message_view;
static GtkWidget * message_view_func;
static GtkWidget * message_view_message;
static GtkWidget * message_view_skip;
static GtkWidget * message_view_rerun;
static GtkWidget * message_view_tabs;
static GtkWidget * events_view;
static GtkTreeStore * event_table_model;
static GtkWidget * sig_page;
static GtkWidget * res_page;

GtkWidget * createWizardTab() {

    GtkWidget * tab = gtk_hbox_new(FALSE, 5);

    events_view = createFunctionsView();
    gtk_widget_show(events_view);
    gtk_box_pack_start((GtkBox *) tab, events_view, TRUE, TRUE, 0);

    message_view = createMessageView();
    gtk_widget_show(message_view);
    gtk_box_pack_start((GtkBox *) tab, message_view, TRUE, TRUE, 0);

    return tab;
}

GtkWidget * createMessageView() {
	GtkWidget * box = gtk_vbox_new(FALSE, 5);

	GtkWidget * info_box = gtk_vbox_new(FALSE, 5);
		message_view_func = gtk_label_new("");
		setMessageViewFunc("");
        gtk_widget_show(message_view_func);
		gtk_box_pack_start((GtkBox *) info_box, message_view_func, TRUE, FALSE, 0);

		message_view_message = gtk_label_new("");
        gtk_label_set_line_wrap(GTK_LABEL(message_view_message), TRUE);
        setMessageViewMessage("");
		gtk_widget_show(message_view_message);
		gtk_box_pack_start((GtkBox *) info_box, message_view_message, TRUE, FALSE, 0);

		message_view_skip = gtk_button_new_with_label("Skip");
	    g_signal_connect(message_view_skip, "clicked", G_CALLBACK(skip_btn_handler), NULL);
	    gtk_widget_set_sensitive(message_view_skip, FALSE);
	    gtk_widget_show(message_view_skip);
	    gtk_box_pack_start((GtkBox *) info_box, message_view_skip, TRUE, FALSE, 0);

	    message_view_rerun = gtk_button_new_with_label("Run wizard again");
	    g_signal_connect(message_view_rerun, "clicked", G_CALLBACK(start_btn_handler), NULL);
	    gtk_widget_show(message_view_rerun);
	    gtk_box_pack_start((GtkBox *) info_box, message_view_rerun, TRUE, FALSE, 0);

	gtk_widget_show(info_box);
	gtk_box_pack_start((GtkBox *) box, info_box, TRUE, FALSE, 0);

    message_view_tabs = gtk_notebook_new();
        GtkWidget * t1_label = gtk_label_new("Function Signature");
        sig_page = gtk_vbox_new(FALSE, 5);
        updateSignatureTab("");
        gtk_widget_show(sig_page);
        gtk_notebook_append_page(GTK_NOTEBOOK(message_view_tabs), sig_page, t1_label);

        GtkWidget * t3_label = gtk_label_new("Links & Resources");
        res_page = gtk_vbox_new(FALSE, 5);
        gtk_widget_show(res_page);
        gtk_notebook_append_page(GTK_NOTEBOOK(message_view_tabs), res_page, t3_label);

    gtk_widget_show(message_view_tabs);
    gtk_box_pack_start((GtkBox *) box, message_view_tabs, TRUE, FALSE, 0);

	return box;
}

void setMessageViewFunc(const char * str) {
    setLabelText(GTK_LABEL(message_view_func), str, 25000);
}

void setMessageViewMessage(const char * str) {
    setLabelText(GTK_LABEL(message_view_message), str, 15000);
}

void updateInfoTabs(const char * func_name) {

    static bool first_update = TRUE;
    static char last_func_name[1024];

    // Only update the widgets if the function is different from the one already being displayed
    if(!first_update && strcmp(last_func_name, func_name) == 0) {
        return;
    }
    strcpy(last_func_name, func_name);
    first_update = FALSE;
    printf("Updating info tabs\n");
    if(strlen(func_name) == 0) {
        gtk_widget_hide(message_view_tabs);
        gtk_widget_hide(message_view_skip);
        gtk_widget_hide(message_view_rerun);
        gtk_widget_set_sensitive(message_view_skip, FALSE);
    } else {
        gtk_widget_show(message_view_tabs);
        gtk_widget_show(message_view_skip);
        gtk_widget_show(message_view_rerun);
        gtk_widget_set_sensitive(message_view_skip, TRUE);
    }
    updateSignatureTab(func_name);
    updateResourcesTab(func_name);
}

void updateSignatureTab(const char * func_name) {

    static GtkWidget * l1 = NULL;
    static GtkWidget * params[32];
    static int num_params = 0;
    static GtkWidget * l2 = NULL;

    if(l1 != NULL) gtk_container_remove(GTK_CONTAINER(sig_page), l1);
    for(int i = 0; i < num_params; i++) {
        gtk_container_remove(GTK_CONTAINER(sig_page), params[i]);
    }
    if(l2 != NULL) gtk_container_remove(GTK_CONTAINER(sig_page), l2);

    cJSON * func = getFunctionSignature(func_name);
    if(func != NULL) {

        // Display first line
        char params_label[strlen(func_name) + 33];
        snprintf(params_label, sizeof(params_label), "%s takes the following parameters:", func_name);
        l1 = gtk_label_new(params_label);
        gtk_label_set_line_wrap(GTK_LABEL(l1), TRUE);
        gtk_widget_show(l1);
        gtk_box_pack_start(GTK_BOX(sig_page), l1, FALSE, FALSE, 0);

        // Display list of parameters
        cJSON * params_array = cJSON_GetObjectItem(func, "params");
        num_params = cJSON_GetArraySize(params_array);
        for(int i = 0; i < num_params; i++) {
            char * param = cJSON_GetArrayItem(params_array, i)->valuestring;
            GtkWidget * param_widget = gtk_label_new(param);
            gtk_widget_show(param_widget);
            gtk_box_pack_start(GTK_BOX(sig_page), param_widget, FALSE, FALSE, 0);
            params[i] = param_widget;
        }

        // Display return type
        char * rtype = cJSON_GetObjectItem(func, "rtype")->valuestring;
        char rtype_label[34 + strlen(rtype)];
        snprintf(rtype_label, sizeof(rtype_label), "It should return a value of type %s", rtype);
        l2 = gtk_label_new(rtype_label);
        gtk_label_set_line_wrap(GTK_LABEL(l2), TRUE);
        gtk_widget_show(l2);
        gtk_box_pack_start(GTK_BOX(sig_page), l2, FALSE, FALSE, 0);
    }

}

void updateResourcesTab(const char * func_name) {

    static GtkWidget * links[32];
    static int num_links = 0;

    for(int i = 0; i < num_links; i++) {
        gtk_container_remove(GTK_CONTAINER(res_page), links[i]);
    }

    cJSON * func = getFunctionResources(func_name);
    if(func != NULL) {

        // Display list of hyperlinks
        cJSON * links_array = cJSON_GetObjectItem(func, "links");
        if(links_array != NULL) {
            num_links = cJSON_GetArraySize(links_array);
            for(int i = 0; i < num_links; i++) {
                char * link = cJSON_GetArrayItem(links_array, i)->valuestring;
                char link_html[(strlen(link) * 2) + 16];
                snprintf(link_html, sizeof(link_html), "<a href=\"%s\">%s</a>", link, link);
                GtkWidget * link_widget = gtk_label_new("");
                gtk_label_set_markup(GTK_LABEL(link_widget), link_html);
                #if __APPLE__
                    g_signal_connect(link_widget, "activate-link", G_CALLBACK(hyperlink_handler_osx), NULL);
                #endif
                gtk_widget_show(link_widget);
                gtk_box_pack_start(GTK_BOX(res_page), link_widget, FALSE, FALSE, 0);
                links[i] = link_widget;
            }
        }
    }

}

/* The default handler for hyperlinks is broken on gtk for osx, so needs a custom handler */
gboolean hyperlink_handler_osx(GtkLabel * label, gchar * uri, gpointer  user_data) {
    char * argv[3];
    argv[0] = "open";
    argv[1] = uri;
    argv[2] = 0;
    int status;
    pid_t pid;
    switch((pid = fork())) {
        case -1:
            perror("fork");
            break;
        case 0:
            execv("/usr/bin/open", argv);
            perror("Failed to open link in default browser");
            exit(EXIT_FAILURE);
            break;
    }
    return TRUE;
}

void setLabelText(GtkLabel * label, const char * str, const int size) {
    size_t len = 64 + strlen(str) + 1;
    char * formatted = malloc(len);
    snprintf(formatted, len, "<span size=\"%d\"><b>%s</b></span>", size, str);
    gtk_label_set_markup(label, formatted);
    free(formatted);
}

// Skip an event (for this session)
void skip_btn_handler(GtkWidget * widget, gpointer data) {
    if(notification_event != NULL) {
        
        // Append event to the list of skipped events
        const char * func_name = cJSON_GetObjectItem(notification_event, "func_name")->valuestring;
        const int test_num = cJSON_GetObjectItem(notification_event, "test_num")->valueint;
        addSkippedTest(func_name, test_num);

        // Run the wizard again
        start_btn_handler(NULL, NULL);
    } else {
        fprintf(stderr, "Can't skip if there's no event\n");
    }

}

GtkWidget * createFunctionsView() {

    GtkWidget * scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scrolled_window, 200, -1);

    event_table_model = gtk_tree_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget * tree_view = gtk_tree_view_new();
    gtk_container_add(GTK_CONTAINER(scrolled_window), tree_view);
    gtk_tree_view_set_model(GTK_TREE_VIEW(tree_view), GTK_TREE_MODEL(event_table_model));
    gtk_widget_show(tree_view);
    
    GtkCellRenderer * cell_func = gtk_cell_renderer_text_new();
    GtkTreeViewColumn * column_func = gtk_tree_view_column_new_with_attributes("Function", cell_func, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), GTK_TREE_VIEW_COLUMN(column_func));
    
    //GtkCellRenderer * cell_optional = gtk_cell_renderer_text_new();
    //GtkTreeViewColumn * column_optional = gtk_tree_view_column_new_with_attributes("Optional?", cell_optional, "text", 1, NULL);
    //gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), GTK_TREE_VIEW_COLUMN(column_optional));
    
    GtkCellRenderer * cell_status = gtk_cell_renderer_text_new();
    GtkTreeViewColumn * column_status = gtk_tree_view_column_new_with_attributes("Status", cell_status, "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), GTK_TREE_VIEW_COLUMN(column_status));

    return scrolled_window;
}

stopToolFunc startWizard() {

    // Create a FIFO so that we can receive events
    recv_end_event = FALSE;
    unlink(wizard_fifo_name);
    mkfifo(wizard_fifo_name, 0666);

    // Start receiving events in a separate thread
    pthread_t t;
    int t_retval;
    t_retval = pthread_create(&t, NULL, doStartWizard, NULL);
    if(t_retval != 0) {
        perror("pthread_create");
    }
    return stopWizard;
}

void * doStartWizard(void * ptr) {

    // Read JSON chunks from the shared pipe that the libfuse wrapper logs to
    wizard_fifo = fopen(wizard_fifo_name, "r");

    // Local lists of events sent by libfuse wrapper
    if(wizard_events != NULL) {
        // Clear any old events from previous runs
        g_slist_free(wizard_events);
        g_slist_free(wizard_events_displayed);
        g_hash_table_destroy(rows);
        notification_event = NULL;
    }
    wizard_events = g_slist_alloc();
    wizard_events_displayed = g_slist_alloc();
    rows = g_hash_table_new(g_str_hash, g_str_equal);
    
    bool fifo_open;
    if(wizard_fifo != NULL) fifo_open = TRUE;
    else fifo_open = FALSE;
    
    size_t chunk_pos = 0;
    size_t json_chunk_capacity = 1024;
    char *json_chunk = malloc(json_chunk_capacity);

    while(fifo_open) {
        
        // Construct a chunk of JSON
        int unclosed_braces = 0;
        chunk_pos = 0;
        
        do {
            int c = fgetc(wizard_fifo);
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
            cJSON *event = cJSON_Parse(json_chunk);
            json_chunk[0] = '\0';
            handleWizardEvent(event);
        }
    }
    free(json_chunk);

    if(wizard_fifo != NULL) {
        fclose(wizard_fifo);
    }
    
    unlink(wizard_fifo_name);
}

void stopWizard() {
    pid_t fuse_pid = getFusePID();
    if(fuse_pid != 0) {
        setFusePID(0);
        kill(fuse_pid, SIGINT);
        printf("Sent SIGINT to FUSE binary\n");
    }
}

gboolean gui_idle_wizard(void) {

    pthread_mutex_lock(&tailq_events_lock);

    // Clear table if there are no events currently displayed
    if(wizard_events_displayed != NULL && wizard_events_displayed->next == NULL) {
        gtk_tree_store_clear(GTK_TREE_STORE(event_table_model));
    }
    
    // Append any new rows
    GSList * node = wizard_events;
    while(node != NULL) {
        cJSON * event = (cJSON *) node->data;
        if(event != NULL && !isFunctionDisplayed(event)) {
            const char * test_name = cJSON_GetObjectItem(event, "func_name")->valuestring;
            char func_name[strlen(test_name) + 1];
            testToFunction(func_name, test_name);
            GtkTreeIter * iter = malloc(sizeof(GtkTreeIter));
            gdk_threads_enter();
            gtk_tree_store_append(GTK_TREE_STORE(event_table_model), iter, NULL);
            gtk_tree_store_set(GTK_TREE_STORE(event_table_model), iter, 0, func_name, 1, "", -1);
            gdk_threads_leave();
            g_hash_table_insert(rows, g_strdup(func_name), iter);
            wizard_events_displayed = g_slist_prepend(wizard_events_displayed, event);
        }

        node = node->next;
    }

    // For each of the displayed nodes, find out of all of the events for that function are ok
    // and change the status of the row accordingly
    node = wizard_events_displayed;
    while(node != NULL) {
        cJSON * event = (cJSON *) node->data;
        if(event != NULL) {
            const char * func_name = cJSON_GetObjectItem(event, "func_name")->valuestring;
            updateFunctionRow(func_name);
        }
        node = node->next;
    }
    pthread_mutex_unlock(&tailq_events_lock);

    // If there is information about a function then display it in the box to the right
    if(notification_event != NULL) {
        const char * test_name = cJSON_GetObjectItem(notification_event, "func_name")->valuestring;
        char func_name[strlen(test_name) + 1];
        testToFunction(func_name, test_name);
        const int test_num = cJSON_GetObjectItem(notification_event, "test_num")->valueint;
        const int passed = cJSON_GetObjectItem(notification_event, "passed")->valueint;
        const int optional = cJSON_GetObjectItem(notification_event, "optional")->valueint;
        const char * message = cJSON_GetObjectItem(notification_event, "message")->valuestring;
        setMessageViewFunc(func_name);
        setMessageViewMessage(message);
        updateInfoTabs(func_name);
    } else {
        if(wizard_events_displayed != NULL && wizard_events_displayed->next != NULL) {
            // There's no notification event, but we're displaying events - this means there are no errors
            if(isFSMounted()) {
                // Filesystem is still mounted, so tests are still being run
                setMessageViewFunc("Running tests...");
                setMessageViewMessage("");
            } else if(recv_end_event) {
                // Filesystem unmounted and terminated cleanly.. so all tests must have passed
                setMessageViewFunc("Success!");
                setMessageViewMessage("Well done, you have defined and implemented all of the essential FUSE functions and your filesystem should now be usable. Try running it against the test suite to find any defects and make your filesystem more robust.");
            } else {
                // If the filesystem is unmounted but we didn't get the __END event this means the filesystem crashed before running every test
                setMessageViewFunc("Filesystem crashed");

                // Get the last event received
                cJSON * last_event = NULL;
                GSList * node = wizard_events;
                while(node != NULL) {
                    cJSON * event = (cJSON *) node->data;
                    if(event != NULL) {
                        last_event = event;
                    }
                    node = node->next;
                }

                // Construct an error message and display it
                char error_str[1024];
                error_str[0] = '\0';
                char * error_prefix = "Your filesystem terminated unexpectedly while running tests against it. Check the console for any error messages. ";
                strncat(error_str, error_prefix, sizeof(error_str));
                if(last_event != NULL) {
                    const char * func_name = cJSON_GetObjectItem(last_event, "func_name")->valuestring;
                    const int test_num = cJSON_GetObjectItem(last_event, "test_num")->valueint;
                    char error_suffix[512];
                    if(test_num == 0) {
                        snprintf(error_suffix, sizeof(error_suffix), "The last succesful test was making sure that %s is defined.", func_name);
                    } else {
                        snprintf(error_suffix, sizeof(error_suffix), "The last succesful test was a call to %s.", func_name);
                    }
                    strncat(error_str, error_suffix, sizeof(error_suffix));
                } else {
                    char * error_suffix = "No tests could be run successfully. Make sure you are calling fuse_main correctly, and that there are no errors before this point.";
                    strncat(error_str, error_suffix, strlen(error_suffix));
                }
                setMessageViewMessage(error_str);
            }
        } else {
            setMessageViewFunc("Wizard");
            setMessageViewMessage("Press Start to run the wizard");
        }
        updateInfoTabs("");
    }


    return TRUE; // only return false if we never want to be called again
}

// True if there is an entry in the function table for the function this event is testing
bool isFunctionDisplayed(cJSON * event) {
    const char * func_name = cJSON_GetObjectItem(event, "func_name")->valuestring;
    GSList * node = wizard_events_displayed;
    while(node != NULL) {
        cJSON * t_event = (cJSON *) node->data;
        if(t_event != NULL) {
            const char * t_func_name = cJSON_GetObjectItem(t_event, "func_name")->valuestring;
            if(strcmp(func_name, t_func_name) == 0) {
                return true;
            }
        }
        node = node->next;
    }
    return false;
}

void updateFunctionRow(const char * test_name) {
    // Strip the test_ prefix and find the row for the function (in the table)
    char func_name[strlen(test_name) + 1];
    testToFunction(func_name, test_name);
    GtkTreeIter * iter = (GtkTreeIter *) g_hash_table_lookup(rows, func_name);
    if(iter != NULL) {
        cJSON * failing_event = NULL;
        funcstat status = getFunctionStatus(test_name, &failing_event);
        char * status_str;
        switch(status) {
            case IMPLEMENTED:
                status_str = "Done";
            break;
            case SKIPPED:
                status_str = "Skipped";
            break;
            case DEFINED:
                status_str = "Needs fixing";
                notification_event = failing_event;
                notification_event_status = status;
            break;
            case UNDEFINED:
                status_str = "Needs defining";
                notification_event = failing_event;
                notification_event_status = status;
            break;
            default:
                status_str = "?";
        }
        gdk_threads_enter();
        gtk_tree_store_set(GTK_TREE_STORE(event_table_model), iter, 1, status_str, -1);
        gdk_threads_leave();
    } else {
        fprintf(stderr, "Failed to lookup row with key %s\n", func_name);
    }
}

// Work out the status of a function using all of its reported events
funcstat getFunctionStatus(const char * test_name, cJSON ** failing_event) {
    /* States:
     *   Implemented - Every event with this func_name must have passed==true
     *   Skipped - Same as Implemented, but some events were skipped
     *   Defined - At least one event with this func_name must have passed==true
     *   Undefined - No events with this func_name have passed==true
     */
    int num_passed = 0;
    int num_failed = 0;
    int num_skipped = 0;
    GSList * node = wizard_events;
    while(node != NULL) {
        cJSON * event = (cJSON *) node->data;
        if(event != NULL) {
            const char * t_test_name = cJSON_GetObjectItem(event, "func_name")->valuestring;
            if(strcmp(test_name, t_test_name) == 0) {
                const int passed = cJSON_GetObjectItem(event, "passed")->valueint;
                const char * msg = cJSON_GetObjectItem(event, "message")->valuestring;
                if(passed) {
                    num_passed++;
                    if(strcmp("Skipped", msg) == 0) {
                        num_skipped++;
                    }
                } else {
                    if(*failing_event == NULL) {
                        *failing_event = event;
                    }
                    num_failed++;
                }
            }
        }
        node = node->next;
    }
    if(num_failed == 0) {
        if(num_skipped > 0) {
            return SKIPPED;
        } else {
            return IMPLEMENTED;
        }
    } else if(num_passed > 0) {
        return DEFINED;
    } else {
        return UNDEFINED;
    }
}

void handleWizardEvent(cJSON * event) {
    printf("%s\n", cJSON_Print(event));
    char * func_name = cJSON_GetObjectItem(event, "func_name")->valuestring;
    if(strcmp(func_name, "__END") == 0) {
        // Special event indicating that the wizard is finished
        // Set our internal state as unmounted before we get SIGCHLD
        recv_end_event = TRUE;
        setFusePID(0);
        printf("FUSE binary detached so wizard will terminate.\n");
    } else {
        if(isUsingGui()) {
            // Add event to GUI queue (as we can't make changes from this thread directly)
            pthread_mutex_lock(&tailq_events_lock);
            wizard_events = g_slist_append(wizard_events, event);
            pthread_mutex_unlock(&tailq_events_lock);
        } else {
            // Otherwise, we don't need the event anymore
            cJSON_Delete(event);
        }
    }
}