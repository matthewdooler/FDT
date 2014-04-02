#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <string.h>
#include <gtk/gtk.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/queue.h>

#include "cJSON.h"
#include "fdt.h"
#include "debugger.h"

static int pendingInvocations = 0;
static bool autoAdvance = FALSE;
static bool closing = FALSE;

static char *debugFifoName = "fuse-debug.fifo";
static FILE *debugFifo = NULL;

static char *stepSemName = "fuse-step.sem";
static sem_t *stepSem = NULL;

struct tailq_event {
    cJSON * value;
    TAILQ_ENTRY(tailq_event) entries;
};

pthread_mutex_t tailq_events_lock;
TAILQ_HEAD(, tailq_event) tailq_events;

/* GUI widgets */
static GtkWidget * advance_btn;
static GtkWidget * autoadvance_btn;
static GtkWidget * events_view;
static GtkTreeStore * event_table_model;
static GtkTreeIter event_table_iter;

GtkWidget * createTopButtons() {
    GtkWidget * top_buttons = gtk_hbox_new(TRUE, 5);
    
    autoadvance_btn = gtk_check_button_new_with_label("Auto-advance");
    g_signal_connect(autoadvance_btn, "clicked", G_CALLBACK(autoadvance_btn_handler), NULL);
    gtk_widget_show(autoadvance_btn);
    gtk_box_pack_start((GtkBox *) top_buttons, autoadvance_btn, TRUE, TRUE, 0);
    
    advance_btn = gtk_button_new_with_label("Advance");
    g_signal_connect(advance_btn, "clicked", G_CALLBACK(advance_btn_handler), NULL);
    gtk_widget_show(advance_btn);
    gtk_box_pack_start((GtkBox *) top_buttons, advance_btn, TRUE, TRUE, 0);
    
    return top_buttons;
}

GtkWidget * createEventsView() {

    GtkWidget * scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
   
    event_table_model = gtk_tree_store_new(3, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget * tree_view = gtk_tree_view_new();
    gtk_container_add(GTK_CONTAINER(scrolled_window), tree_view);
    gtk_tree_view_set_model(GTK_TREE_VIEW(tree_view), GTK_TREE_MODEL(event_table_model));
    gtk_widget_show(tree_view);
    
    GtkCellRenderer * cell_seqnum = gtk_cell_renderer_text_new();
    GtkTreeViewColumn * column_seqnum = gtk_tree_view_column_new_with_attributes("Call #", cell_seqnum, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), GTK_TREE_VIEW_COLUMN(column_seqnum));
    
    GtkCellRenderer * cell_event = gtk_cell_renderer_text_new();
    GtkTreeViewColumn * column_event = gtk_tree_view_column_new_with_attributes("Event", cell_event, "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), GTK_TREE_VIEW_COLUMN(column_event));
    
    GtkCellRenderer * cell_func = gtk_cell_renderer_text_new();
    GtkTreeViewColumn * column_func = gtk_tree_view_column_new_with_attributes("Function", cell_func, "text", 2, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), GTK_TREE_VIEW_COLUMN(column_func));

    return scrolled_window;
}

GtkWidget * createDebuggerTab() {

    GtkWidget * tab = gtk_vbox_new(FALSE, 5);

    // Create the control buttons at the top of the tab
    GtkWidget * top_buttons = createTopButtons();
    gtk_widget_show(top_buttons);
    gtk_box_pack_start((GtkBox *) tab, top_buttons, FALSE, TRUE, 0);
    
    events_view = createEventsView();
    gtk_widget_show(events_view);
    gtk_box_pack_start((GtkBox *) tab, events_view, TRUE, TRUE, 0);

    return tab;
}

stopToolFunc startDebugger() {

    closing = FALSE;

    // Create a FIFO so that libfuse can tell us what's going on
    unlink(debugFifoName);
    mkfifo(debugFifoName, 0666);

    // Create a semaphore so we can pause libfuse and step-thru method calls
    sem_unlink(stepSemName);
    stepSem = sem_open(stepSemName, O_CREAT, 0644, 0);

    // Start debugging in a separate thread
    pthread_t debugger_thread;
    int debugger_thread_retval;
    debugger_thread_retval = pthread_create(&debugger_thread, NULL, doStartDebugger, NULL);
    if(debugger_thread_retval != 0) {
        perror("pthread_create");
    }

    return stopDebugger;
}

void stopDebugger() {
    printf("Stopping debugger\n");
    closing = TRUE;
    advancePending();
    pid_t fuse_pid = getFusePID();
    if(fuse_pid != 0) {
        setFusePID(0);
        kill(fuse_pid, SIGINT);
        printf("Sent SIGINT to FUSE binary\n");
    }
}

void * doStartDebugger(void * ptr) {

    // Read JSON chunks from the shared pipe that the libfuse wrapper logs to
    debugFifo = fopen(debugFifoName, "r");
    
    bool fifoOpen;
    if(debugFifo != NULL) fifoOpen = TRUE;
    else fifoOpen = FALSE;

    size_t chunk_pos = 0;
    size_t json_chunk_capacity = 1024;
    char *json_chunk = malloc(json_chunk_capacity);
    
    while(fifoOpen) {
        
        // Construct a chunk of JSON
        int unclosed_braces = 0;
        bool unclosed_quotes = false;
        bool escaping = false;
        chunk_pos = 0;
        
        do {
            int c = fgetc(debugFifo);
            if(c == EOF) {
                fifoOpen = FALSE;
            } else {
                if(chunk_pos+1 >= json_chunk_capacity) {
                    // Double string capacity before we overflow it
                    json_chunk_capacity *= 2;
                    json_chunk = realloc(json_chunk, json_chunk_capacity);
                }
                json_chunk[chunk_pos++] = (char) c;
                if(!unclosed_quotes && c == '{') unclosed_braces++;
                else if(!unclosed_quotes && c == '}') unclosed_braces--;
                else if(!escaping && c == '"') unclosed_quotes = !unclosed_quotes;

                if(c == '\\') escaping = true;
                else escaping = false;
            }
        } while(unclosed_braces > 0);
        json_chunk[chunk_pos++] = '\0';
        
        // Parse the JSON and display the event
        if(strlen(json_chunk) > 0) {
            cJSON *event = cJSON_Parse(json_chunk);
            json_chunk[0] = '\0';
            handleDebuggerEvent(event);
        }
    }
    free(json_chunk);
    
    printf("FUSE binary detached so debugger will terminate.\n");
    
    if(debugFifo != NULL) {
        fclose(debugFifo);
    }
    
    unlink(debugFifoName);
    sem_unlink(stepSemName);
}

void waitToAdvance() {
    printf("[ paused ]");
    char *line = NULL;
    size_t line_allocsize = 0;
    getline(&line, &line_allocsize, stdin);
}

bool canAdvance() {
    return pendingInvocations > 0 ? TRUE : FALSE;
}

void advance() {
    if(canAdvance()) {
        sem_post(stepSem);
        pendingInvocations--;
    } else {
        printf("Cannot advance until next FUSE operation executes\n");
    }
}

void advancePending() {
    while(canAdvance()) {
        advance();
        printf("Advanced pending invocation\n");
    }
}

void scrollToTop(GtkWidget * scrolled_window) {
    gdk_threads_enter();
    GtkObject * top = gtk_adjustment_new(-1, -1, 0, 0, 0, 0);
    gdk_threads_leave();
    
    gdk_threads_enter();
    gtk_scrolled_window_set_vadjustment((GtkScrolledWindow *) scrolled_window, (GtkAdjustment *) top);
    gdk_threads_leave();
}

void updateAdvanceButtonState() {
    if(canAdvance()) {
        gdk_threads_enter();
        gtk_widget_set_sensitive(advance_btn, TRUE);
        gdk_threads_leave();
    } else {
        gdk_threads_enter();
        gtk_widget_set_sensitive(advance_btn, FALSE);
        gdk_threads_leave();
    }
}

gboolean gui_idle_debugger(void) {
    pthread_mutex_lock(&tailq_events_lock);
    while(!TAILQ_EMPTY(&tailq_events)) {

        struct tailq_event * tailq_event_node;
        tailq_event_node = TAILQ_FIRST(&tailq_events);
        
        if(tailq_event_node != NULL) {
        
            cJSON * event = tailq_event_node->value;
            char * type = cJSON_GetObjectItem(event, "type")->valuestring;
            char * type_labelled = NULL;
            char * name = cJSON_GetObjectItem(event, "name")->valuestring;
            int seqnum = cJSON_GetObjectItem(event, "seqnum")->valueint;
            
            cJSON * params;
            if(strcmp(type, "invoke") == 0) {
                params = cJSON_GetObjectItem(event, "params");
            } else if(strcmp(type, "return") == 0) {
                params = cJSON_GetObjectItem(event, "modified_params");
                
                // If there is a return value then add this next to the event
                cJSON *returnval_obj = cJSON_GetObjectItem(event, "returnval");
                if(returnval_obj->type == cJSON_Number) {
                    size_t new_len = strlen(type) + 1 + 50;
                    type_labelled = malloc(new_len);
                    snprintf(type_labelled, new_len, "%s %d", type, returnval_obj->valueint);
                    type = type_labelled;
                }
            }
            
            gdk_threads_enter();
            gtk_tree_store_prepend(GTK_TREE_STORE(event_table_model), &event_table_iter, NULL);
            gdk_threads_leave();
            
            gdk_threads_enter();
            gtk_tree_store_set(GTK_TREE_STORE(event_table_model), &event_table_iter, 0, seqnum, 1, type, 2, name, -1);
            gdk_threads_leave();
            
            showParams(params, event_table_iter);
            
            if(type_labelled != NULL) {
                free(type_labelled);
            }
            
            // Enable or disable the advance button
            updateAdvanceButtonState();
            
            scrollToTop(events_view);
            
            // Remove the event from the queue and destroy it
            TAILQ_REMOVE(&tailq_events, tailq_event_node, entries);
            cJSON_Delete(tailq_event_node->value);
            free(tailq_event_node);
        }
    }
    pthread_mutex_unlock(&tailq_events_lock);
    return TRUE; // only return false if we never want to be called again
}

void showParams(cJSON * node, GtkTreeIter parent) {
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
                gtk_tree_store_set(GTK_TREE_STORE(event_table_model), &itr, 2, item->string, -1);
            } else {
                // leaf node, so show label with value (eg, "timestamp: 0189276")
                char * label = item->string;
                char * value = cJSON_Print(item);
                char label_and_value[strlen(label) + 2 + strlen(value) + 1];
                snprintf(label_and_value, sizeof label_and_value, "%s: %s", label, value);
                gtk_tree_store_set(GTK_TREE_STORE(event_table_model), &itr, 2, label_and_value, -1);
            }
            gdk_threads_leave();
            
            showParams(item, itr);
        }
    } else {
        return;
    }
}

void handleDebuggerEvent(cJSON * event) {

    char * type = cJSON_GetObjectItem(event, "type")->valuestring;
    char * name = cJSON_GetObjectItem(event, "name")->valuestring;
    int seqnum = cJSON_GetObjectItem(event, "seqnum")->valueint;
    
    if(strcmp(type, "invoke") == 0) {
        printf("[->] (%d)\t%s\n", seqnum, name);
        //cJSON * params = cJSON_GetObjectItem(event, "params");
        //printf("%s\n", cJSON_Print(params));

        pendingInvocations++;
        if(closing || autoAdvance) {
            // automatically advance to the next call if we're closing or in auto-advance mode
            advance();
        } else if(isUsingGui()) {
            // rely on the GUI to call advance()
        } else {
            // wait for enter to be pressed at the console if we're not using a GUI (and not closing or in auto-advance mode)
            waitToAdvance();
            advance();
        }
    } else if(strcmp(type, "return") == 0) {
        cJSON *returnval_obj = cJSON_GetObjectItem(event, "returnval");

        if(returnval_obj->type == cJSON_NULL) {
            printf("[<-] (%d)\t%s\n", seqnum, name);
        } else if(returnval_obj->type == cJSON_Number) {
            printf("[<-] (%d)\t%s returned %d\n", seqnum, name, returnval_obj->valueint);
        } else {
            printf("[<-] (%d)\t%s (returned an unexpected type)\n", seqnum, name);
        }
        //cJSON *modified_params = cJSON_GetObjectItem(event, "modified_params");
        //printf("%s\n", cJSON_Print(modified_params));
    } else {
        fprintf(stderr, "Unexpected event type '%s'\n", type);
    }
    
    if(isUsingGui()) {
        // Add event to GUI queue (as we can't make changes from this thread directly)
        pthread_mutex_lock(&tailq_events_lock);
        struct tailq_event * tailq_event_node;
        tailq_event_node = malloc(sizeof(*tailq_event_node));
        tailq_event_node->value = event;
        TAILQ_INSERT_TAIL(&tailq_events, tailq_event_node, entries);
        pthread_mutex_unlock(&tailq_events_lock);
    } else {
        // Otherwise, we don't need the event anymore
        cJSON_Delete(event);
    }
}

static void autoadvance_btn_handler(GtkWidget * widget, gpointer data) {
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) {
        autoAdvance = TRUE;
    } else {
        autoAdvance = FALSE;
    }
    updateAdvanceButtonState();
    advancePending();
}
static void advance_btn_handler(GtkWidget * widget, gpointer data) {
    advance();
}

void initGUIEventQueue() {
    pthread_mutex_init(&tailq_events_lock, NULL);
    pthread_mutex_lock(&tailq_events_lock);
    TAILQ_INIT(&tailq_events);
    pthread_mutex_unlock(&tailq_events_lock);
}