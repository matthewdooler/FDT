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
#include <termios.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>

#include "cJSON.h"
#include "fdt.h"
#include "logger.h"

static bool fifoOpen;
static int prompting = 0;
static int pendingInvocations = 0;
static cJSON * current_sequence = NULL;
static cJSON * all_sequences = NULL;

static char *debugFifoName = "fuse-debug.fifo";
static FILE *debugFifo = NULL;

static char *stepSemName = "fuse-step.sem";
static sem_t *stepSem = NULL;

static state current_state = IDLE;

void setLoggerState(state s) {
    current_state = s;
}

void quitLogger() {
    pid_t fuse_pid = getFusePID();
    if(fuse_pid != 0) {
        setFusePID(0);
        kill(fuse_pid, SIGINT);
        printf("Sent SIGINT to FUSE binary\n");
    }

    // Make sure we don't leave filesystem blocked
    for(int i = 0; i < 50; i++) {
        sem_post(stepSem);
    }
    fifoOpen = FALSE;
}

// Get the full name of the operating system
char * getOSName() {
    #if __APPLE__
        char os_release[256];
        size_t size = sizeof(os_release);
        int ret = sysctlbyname("kern.osrelease", os_release, &size, NULL, 0);

        // Parse Darwin version, which is in the format of major.minor.sub
        char * first_dot = strchr(os_release, '.');
        size_t darwin_major_len = first_dot - os_release;
        char * second_dot = strchr(first_dot + 1, '.');
        size_t darwin_minor_len = second_dot - first_dot - 1;

        char darwin_major[darwin_major_len+1];
        strncpy(darwin_major, os_release, darwin_major_len);
        darwin_major[darwin_major_len] = '\0';

        char darwin_minor[darwin_minor_len+1];
        strncpy(darwin_minor, first_dot + 1, darwin_minor_len);
        darwin_minor[darwin_minor_len] = '\0';

        // Translate Darwin version to Mac OS X version
        // x.y.z -> 10.(x-4).y (z discarded)
        int mac_major = 10;
        int mac_minor = atoi(darwin_major) - 4;
        int mac_sub = atoi(darwin_minor);

        char * os_name = malloc(64);
        snprintf(os_name, 64, "Mac OS X %d.%d.%d", mac_major, mac_minor, mac_sub);
        return os_name;
    #else
        // Otherwise use the standard Unix method for getting the OS name and version
        char * os_name = malloc(256);
        struct utsname * buf = malloc(sizeof(*buf));
        int retval = uname(buf);
        if(retval == 0) {
            strcpy(os_name, buf->sysname);
            strcat(os_name, " ");
            strcat(os_name, buf->release);
        } else {
            // uname call failed
            strcpy(os_name, "Unix");
        }
        free(buf);
        return os_name;
    #endif
}

// Check if STDIN has a key available (note that the terminal must have canonical mode disabled)
int isKeyAvailable() {
    int fd = STDIN_FILENO;
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(fd, &fdset);

    // Check if a char is available with an instant timeout (i.e., non-blocking)
    struct timeval t;
    t.tv_sec = 0;
    t.tv_usec = 0;
    select(fd + 1, &fdset, NULL, NULL, &t);

    return FD_ISSET(fd, &fdset);
}

void exportJSON(const char * fname, cJSON * obj) {
    char * data = cJSON_Print(obj);
    FILE * f = fopen(fname, "w+");
    if(f != NULL) {
        fputs(data, f);
        fclose(f);
    }
}

void choicePrompt() {

    // If we were not already prompting, print the choice
    if(!prompting) {
        switch(current_state) {
            case IDLE:
                printf("\nNot capturing data. Choose an action:\n");
                printf("  s : start capturing sequence\n");
                printf("  q : quit and (optionally) export\n");
            break;
            case CAPTURING_SEQUENCE:
                printf("\nPress 'e' key to end this sequence capture.\n");
            break;
        }
        prompting = 1;
    }

    // Handle the input key (if any)
    if(isKeyAvailable()) {
        int c = getchar();
        switch(current_state) {
            case IDLE:
                if(c == 's') {
                    /* Handle start-capture */
                    current_state = CAPTURING_SEQUENCE;
                } else if(c == 'q') {
                    /* Handle quit */
                    if(cJSON_GetArraySize(all_sequences) > 0) {
                        
                        // Prompt for export filename
                        printf("Enter filename to export sequences to. Leave blank to discard data.\n > ");
                        char * fname_str = NULL;
                        size_t line_len = 0;
                        getline(&fname_str, &line_len, stdin);
                        fname_str[strlen(fname_str)-1] = '\0';

                        if(strlen(fname_str) > 0) {
                            if(access(fname_str, F_OK) != -1) {
                                // File already exists, so read it in and append the new group of sequences
                                printf("Appending to existing file '%s'\n", fname_str);
                                cJSON * groups = readJSONFile(fname_str);
                                if(groups != NULL) {
                                    cJSON_AddItemToArray(groups, all_sequences);
                                    exportJSON(fname_str, groups);
                                } else {
                                    printf("Did not export data as there was an error reading the existing file\n");
                                }
                            } else {
                                // File does not exist, so create it and export the data
                                printf("Exporting to new file '%s'\n", fname_str);
                                cJSON * groups = cJSON_CreateArray();
                                cJSON_AddItemToArray(groups, all_sequences);
                                exportJSON(fname_str, groups);
                            }
                        } else {
                            printf("Discarding data\n");
                        }
                    } else {
                        printf("Nothing to export as no sequences were captured\n");
                    }
                    quitLogger();
                }
            break;
            case CAPTURING_SEQUENCE:
                if(c == 'e') {
                    /* Handle stop-capture */
                    int num_calls = cJSON_GetArraySize(current_sequence);
                    if(cJSON_GetArraySize(current_sequence) > 0) {

                        printf("\nCaptured %d calls\n", num_calls);

                        printf("Describe the action performed:\n > ");
                        char * action_str = NULL;
                        size_t line_len = 0;
                        getline(&action_str, &line_len, stdin);
                        action_str[strlen(action_str)-1] = '\0'; // strip trailing \n

                        printf("What application was used?:\n > ");
                        char * application_str = NULL;
                        line_len = 0;
                        getline(&application_str, &line_len, stdin);
                        application_str[strlen(application_str)-1] = '\0'; // strip trailing \n

                        char * os_str = getOSName();
                        printf("Operating system logged as '%s'\n", os_str);

                        // Create wrapper object that contains metadata about the call sequence
                        cJSON * sequence_obj = cJSON_CreateObject();
                        cJSON_AddStringToObject(sequence_obj, "action", action_str);
                        cJSON_AddStringToObject(sequence_obj, "application", application_str);
                        cJSON_AddStringToObject(sequence_obj, "os", os_str);
                        cJSON_AddItemToObject(sequence_obj, "calls", current_sequence);
                        free(action_str);
                        free(application_str);
                        free(os_str);

                        // Append the sequence to the set of logged sequences
                        cJSON_AddItemToArray(all_sequences, sequence_obj);
                        printf("Sequence saved (but not written to disk!)\n");
                        printf("Number of sequences logged so far: %d\n", cJSON_GetArraySize(all_sequences));
                    } else {
                        printf("\nNo calls captured so did not log empty sequence\n");
                        cJSON_Delete(current_sequence);
                    }

                    // Prepare a new empty sequence
                    current_sequence = cJSON_CreateArray();

                    current_state = IDLE;
                }
            break;
        }
        prompting = 0;
    }
}

stopToolFunc startLogger() {

    // Create a FIFO so that libfuse can tell us what's going on
    unlink(debugFifoName);
    mkfifo(debugFifoName, 0666);

    // Create a semaphore so we can pause libfuse and step-thru method calls
    sem_unlink(stepSemName);
    stepSem = sem_open(stepSemName, O_CREAT, 0644, 0);

    setStopToolFunction(stopLogger);
    doStartLogger();

    return stopLogger;
}

void stopLogger() {
    printf("Stopping logger\n");
    l_advancePending();
    pid_t fuse_pid = getFusePID();
    if(fuse_pid != 0) {
        setFusePID(0);
        kill(fuse_pid, SIGINT);
        printf("Sent SIGINT to FUSE binary\n");
    }
}

void doStartLogger() {

    // Read JSON chunks from the shared pipe that the libfuse wrapper logs to
    debugFifo = fopen(debugFifoName, "r");
    int fd = fileno(debugFifo);
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    if(debugFifo != NULL) fifoOpen = TRUE;
    else fifoOpen = FALSE;

    size_t chunk_pos = 0;
    size_t json_chunk_capacity = 1024;
    char *json_chunk = malloc(json_chunk_capacity);

    current_sequence = cJSON_CreateArray();
    all_sequences = cJSON_CreateArray();

    long min_sleep_time = 10;
    long max_sleep_time = 1000000;
    long sleep_time = min_sleep_time;

    // Disable canonical mode in terminal, as this lets us get key presses directly without blocking
    struct termios current_term;
    tcgetattr(0, &current_term);
    struct termios new_term;
    memcpy(&new_term, &current_term, sizeof(struct termios));
    new_term.c_lflag &= ~ICANON;
    tcsetattr(0, TCSANOW, &new_term);

    while(fifoOpen) {
        
        // Construct a chunk of JSON
        int unclosed_braces = 0;
        chunk_pos = 0;
        
        do {
            int c = fgetc(debugFifo);
            if(c == -1) {
                usleep(10000);
                choicePrompt();
            } else if(c == EOF) {
                printf("fgetc returned EOF\n");
                fifoOpen = FALSE;
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
            handleLoggerEvent(event);
        }
    }
    free(json_chunk);
    
    printf("FUSE binary detached so logger will terminate.\n");

    // Restore original terminal settings
    tcsetattr(0, TCSANOW, &current_term);
    
    if(debugFifo != NULL) {
        fclose(debugFifo);
    }
    
    unlink(debugFifoName);
    sem_unlink(stepSemName);
}

void l_waitToAdvance() {
    printf("[ paused ]");
    char *line = NULL;
    size_t line_allocsize = 0;
    getline(&line, &line_allocsize, stdin);
}

bool l_canAdvance() {
    return pendingInvocations > 0 ? TRUE : FALSE;
}

void l_advance() {
    if(l_canAdvance()) {
        sem_post(stepSem);
        pendingInvocations--;
    } else {
        printf("Cannot advance until next FUSE operation executes\n");
    }
}

void l_advancePending() {
    while(l_canAdvance()) {
        l_advance();
        printf("Advanced pending invocation\n");
    }
}



void handleLoggerEvent(cJSON * event) {
    char * type = cJSON_GetObjectItem(event, "type")->valuestring;
    char * name = cJSON_GetObjectItem(event, "name")->valuestring;
    
    // Only care about invocations, as we're logging for a test suite
    if(strcmp(type, "invoke") == 0) {
        //printf("[->] (%d)\t%s\n", seqnum, name);
        //cJSON * params = cJSON_GetObjectItem(event, "params");

        // Remove seqnum and type (we only need name and params)
        cJSON_DeleteItemFromObject(event, "type");
        cJSON_DeleteItemFromObject(event, "seqnum");

        switch(current_state) {
            case CAPTURING_SEQUENCE:
                cJSON_AddItemToArray(current_sequence, event);
                printf("%s captured\n", name);
            break;
            default:
                printf("%s ignored\n", name);
                cJSON_Delete(event);
            break;
        }
        //printf("%s\n", cJSON_Print(event));

        pendingInvocations++;
        prompting = 0;
        l_advance();
    } else {
        cJSON_Delete(event);
    }
}