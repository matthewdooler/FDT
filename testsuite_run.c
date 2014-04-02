/*
  FUSE Development Tool: Test suite framework
  Matthew Dooler <dooler.matthew@gmail.com>
*/

#include "config.h"
//#include "fuse_i.h"
#include "fuse_misc.h"
#include "fuse_opt.h"
#include "fuse_lowlevel.h"
//#include "fuse_common_compat.h"
#ifdef __APPLE__
#include "fuse_darwin_private.h"
#include "../../lfs.c"
#else
#include "../bbfs.c"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <glib.h>

static const char * testsuite_fifo_name = "fuse-testsuite.fifo";
static FILE * testsuite_fifo = NULL;
static cJSON * groups;
static const struct fuse_operations * passthru_ops;
static const struct fuse_operations * real_ops;
static struct fuse_file_info * fi_passthru;
static struct fuse_file_info * fi_real;
static GHashTable * fhs_passthru;
static GHashTable * fhs_real;
static char * mountpoint;

struct fh_entry {
    uint64_t fh;
    int open;
};

void empty_fi(struct fuse_file_info * fi);
void empty_fi(struct fuse_file_info * fi)
{
    fi->flags = 0;
    fi->fh_old = 0;
    fi->writepage = 0;
    fi->direct_io = 0;
    fi->keep_cache = 0;
    fi->flush = 0;
    fi->fh = 0;
    fi->lock_owner = 0;
}

void store_fh(GHashTable * fhs, const char * path, uint64_t fh);
void store_fh(GHashTable * fhs, const char * path, uint64_t fh) {
    size_t path_len = strlen(path);
    char * path_m = malloc(path_len + 1);
    strcpy(path_m, path);
    struct fh_entry * entry = malloc(sizeof(*entry));
    entry->fh = fh;
    entry->open = 0;
    g_hash_table_insert(fhs, path_m, (void *) entry);
}

void increment_open(GHashTable * fhs, const char * path);
void increment_open(GHashTable * fhs, const char * path) {
    struct fh_entry * entry = ((struct fh_entry *) g_hash_table_lookup(fhs, path));
    entry->open++;
}

uint64_t path_to_fh(GHashTable * fhs, const char * path);
uint64_t path_to_fh(GHashTable * fhs, const char * path) {
    return ((struct fh_entry *) g_hash_table_lookup(fhs, path))->fh;
}

void update_fi(GHashTable * fhs, const char * path, struct fuse_file_info * fi, cJSON * obj);
void update_fi(GHashTable * fhs, const char * path, struct fuse_file_info * fi, cJSON * obj)
{
    fi->flags = cJSON_GetObjectItem(obj, "flags")->valueint;
    fi->writepage = cJSON_GetObjectItem(obj, "writepage")->valueint;
    fi->direct_io = cJSON_GetObjectItem(obj, "direct_io")->valueint;
    fi->keep_cache = cJSON_GetObjectItem(obj, "keep_cache")->valueint;
    fi->flush = cJSON_GetObjectItem(obj, "flush")->valueint;
    fi->lock_owner = cJSON_GetObjectItem(obj, "lock_owner")->valueint;

    // Resolve the path into a filehandle, as the logged filehandle will be wrong
    fi->fh_old = 0;
    if(path != NULL) {
        fi->fh = path_to_fh(fhs, path);
    } else {
        fi->fh = 0;
    }
}

// Remove a path->fh entry from a filehandle map
void destroy_fh(GHashTable * fhs, const char * path);
void destroy_fh(GHashTable * fhs, const char * path)
{
    // Find the file entry and decrement its open counter
    struct fh_entry * entry = ((struct fh_entry *) g_hash_table_lookup(fhs, path));
    entry->open--;

    // If we've had a close for every open then we can remove the entry
    if(entry->open == 0) {
        g_hash_table_remove(fhs, (void *) path);
        free(entry);
    }
}

struct stat * json_to_stat(cJSON * obj);
struct stat * json_to_stat(cJSON * obj)
{
    struct stat * s = malloc(sizeof(*s));
    s->st_dev = cJSON_GetObjectItem(obj, "st_dev")->valueint;
    s->st_ino = cJSON_GetObjectItem(obj, "st_ino")->valueint;
    s->st_mode = cJSON_GetObjectItem(obj, "st_mode")->valueint;
    s->st_nlink = cJSON_GetObjectItem(obj, "st_nlink")->valueint;
    s->st_uid = cJSON_GetObjectItem(obj, "st_uid")->valueint;
    s->st_gid = cJSON_GetObjectItem(obj, "st_gid")->valueint;
    s->st_rdev = cJSON_GetObjectItem(obj, "st_rdev")->valueint;
    s->st_size = cJSON_GetObjectItem(obj, "st_size")->valueint;
    s->st_atime = cJSON_GetObjectItem(obj, "st_atime")->valueint;
    s->st_mtime = cJSON_GetObjectItem(obj, "st_mtime")->valueint;
    s->st_ctime = cJSON_GetObjectItem(obj, "st_ctime")->valueint;
    s->st_blksize = cJSON_GetObjectItem(obj, "st_blksize")->valueint;
    s->st_blocks = cJSON_GetObjectItem(obj, "st_blocks")->valueint;
    return s;
}

cJSON * stat_to_json(struct stat * s);
cJSON * stat_to_json(struct stat * s)
{
    cJSON * obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "st_dev", s->st_dev);
    cJSON_AddNumberToObject(obj, "st_ino", s->st_ino);
    cJSON_AddNumberToObject(obj, "st_mode", s->st_mode);
    cJSON_AddNumberToObject(obj, "st_nlink", s->st_nlink);
    cJSON_AddNumberToObject(obj, "st_uid", s->st_uid);
    cJSON_AddNumberToObject(obj, "st_gid", s->st_gid);
    cJSON_AddNumberToObject(obj, "st_rdev", s->st_rdev);
    cJSON_AddNumberToObject(obj, "st_size", s->st_size);
    cJSON_AddNumberToObject(obj, "st_atime", s->st_atime);
    cJSON_AddNumberToObject(obj, "st_mtime", s->st_mtime);
    cJSON_AddNumberToObject(obj, "st_ctime", s->st_ctime);
    cJSON_AddNumberToObject(obj, "st_blksize", s->st_blksize);
    cJSON_AddNumberToObject(obj, "st_blocks", s->st_blocks);
    return obj;
}

struct timespec json_to_timespec(cJSON * obj);
struct timespec json_to_timespec(cJSON * obj)
{
    struct timespec * tv = malloc(sizeof(*tv));
    tv->tv_sec = cJSON_GetObjectItem(obj, "tv_sec")->valueint;
    tv->tv_nsec = cJSON_GetObjectItem(obj, "tv_nsec")->valueint;
    return *tv;
}

struct timespec * json_to_timespec_array(cJSON * obj);
struct timespec * json_to_timespec_array(cJSON * obj)
{

    // Construct an array of timespec structs
    size_t size = cJSON_GetArraySize(obj);
    struct timespec * tv = malloc(size * sizeof(*tv));
    size_t i;
    for(i = 0; i < size; i++) {
        cJSON * tv_obj = cJSON_GetArrayItem(obj, i);
        tv[i].tv_sec = cJSON_GetObjectItem(tv_obj, "tv_sec")->valueint;
        tv[i].tv_nsec = cJSON_GetObjectItem(tv_obj, "tv_nsec")->valueint;
    }
    return tv;
}

struct utimbuf * json_to_utimbuf(cJSON * obj);
struct utimbuf * json_to_utimbuf(cJSON * obj)
{
    struct utimbuf * ubuf = malloc(sizeof(*ubuf));
    ubuf->actime = cJSON_GetObjectItem(obj, "actime")->valueint;
    ubuf->modtime = cJSON_GetObjectItem(obj, "modtime")->valueint;
    return ubuf;
}

// Linux libfuse-only
#ifdef __APPLE__
#else
struct fuse_bufvec * json_to_fuse_bufvec(const char * path, GHashTable * fhs, cJSON * obj);
struct fuse_bufvec * json_to_fuse_bufvec(const char * path, GHashTable * fhs, cJSON * obj)
{
    struct fuse_bufvec * bufvec = malloc(sizeof(*bufvec));

    bufvec->count = cJSON_GetObjectItem(obj, "count")->valueint;
    bufvec->idx = cJSON_GetObjectItem(obj, "idx")->valueint;
    bufvec->off = cJSON_GetObjectItem(obj, "off")->valueint;

    cJSON * buf_array = cJSON_GetObjectItem(obj, "buf");
    size_t buf_array_len = cJSON_GetArraySize(buf_array);

    // Read buffers into array
    size_t i;
    for(i = 0; i < buf_array_len; i++) {
        cJSON * buf_obj = cJSON_GetArrayItem(buf_array, i);
        struct fuse_buf * buf = malloc(sizeof(*buf));
        buf->size = cJSON_GetObjectItem(buf_obj, "size")->valueint;
        const char * flags = cJSON_GetObjectItem(buf_obj, "flags")->valuestring;
        if(strcmp(flags, "FUSE_BUF_IS_FD") == 0) {
            buf->flags = FUSE_BUF_IS_FD;
        } else if(strcmp(flags, "FUSE_BUF_FD_SEEK") == 0) {
            buf->flags = FUSE_BUF_FD_SEEK;
        } else if(strcmp(flags, "FUSE_BUF_FD_RETRY") == 0) {
            buf->flags = FUSE_BUF_FD_RETRY;
        }
        buf->mem = malloc(buf->size);
        buf->fd = path_to_fh(fhs, path);
        buf->pos = cJSON_GetObjectItem(buf_obj, "pos")->valueint;
        bufvec->buf[i] = *buf;
    }

    return bufvec;
}
#endif

struct statvfs * json_to_statvfs(cJSON * obj);
struct statvfs * json_to_statvfs(cJSON * obj)
{
    struct statvfs * s = malloc(sizeof(*s));
    s->f_bsize = cJSON_GetObjectItem(obj, "f_bsize")->valueint;
    s->f_frsize = cJSON_GetObjectItem(obj, "f_frsize")->valueint;
    s->f_blocks = cJSON_GetObjectItem(obj, "f_blocks")->valueint;
    s->f_bfree = cJSON_GetObjectItem(obj, "f_bfree")->valueint;
    s->f_bavail = cJSON_GetObjectItem(obj, "f_bavail")->valueint;
    s->f_files = cJSON_GetObjectItem(obj, "f_files")->valueint;
    s->f_ffree = cJSON_GetObjectItem(obj, "f_ffree")->valueint;
    s->f_favail = cJSON_GetObjectItem(obj, "f_favail")->valueint;
    s->f_fsid = cJSON_GetObjectItem(obj, "f_fsid")->valueint;
    s->f_flag = cJSON_GetObjectItem(obj, "f_flag")->valueint;
    s->f_namemax = cJSON_GetObjectItem(obj, "f_namemax")->valueint;
    return s;
}

struct fuse_conn_info * json_to_fuse_conn_info(cJSON * obj);
struct fuse_conn_info * json_to_fuse_conn_info(cJSON * obj)
{
    struct fuse_conn_info * conn = malloc(sizeof(*conn));
    conn->proto_major = cJSON_GetObjectItem(obj, "proto_major")->valueint;
    conn->proto_minor = cJSON_GetObjectItem(obj, "proto_minor")->valueint;
    conn->async_read = cJSON_GetObjectItem(obj, "async_read")->valueint;
    conn->max_write = cJSON_GetObjectItem(obj, "max_write")->valueint;
    conn->max_readahead = cJSON_GetObjectItem(obj, "max_readahead")->valueint;

#ifdef __APPLE__
    cJSON * enable = cJSON_GetObjectItem(obj, "enable");
    conn->enable.case_insensitive = cJSON_GetObjectItem(enable, "case_insensitive")->valueint;
    conn->enable.setvolname = cJSON_GetObjectItem(enable, "setvolname")->valueint;
    conn->enable.xtimes = cJSON_GetObjectItem(enable, "xtimes")->valueint;
#endif /* __APPLE__ */

    return conn;
}

struct flock * json_to_flock(cJSON * obj);
struct flock * json_to_flock(cJSON * obj)
{
    struct flock * f = malloc(sizeof(*f));
    f->l_type = cJSON_GetObjectItem(obj, "l_type")->valueint;
    f->l_whence = cJSON_GetObjectItem(obj, "l_whence")->valueint;
    f->l_start = cJSON_GetObjectItem(obj, "l_start")->valueint;
    f->l_len = cJSON_GetObjectItem(obj, "l_len")->valueint;
    f->l_pid = cJSON_GetObjectItem(obj, "l_pid")->valueint;
    return f;
}

#ifdef __APPLE__
struct setattr_x * json_to_setattr_x(cJSON * obj);
struct setattr_x * json_to_setattr_x(cJSON * obj)
{
    struct setattr_x * attr = malloc(sizeof(*attr));
    attr->valid = cJSON_GetObjectItem(obj, "valid")->valueint;
    attr->mode = cJSON_GetObjectItem(obj, "mode")->valueint;
    attr->uid = cJSON_GetObjectItem(obj, "uid")->valueint;
    attr->gid = cJSON_GetObjectItem(obj, "gid")->valueint;
    attr->size = cJSON_GetObjectItem(obj, "size")->valueint;
    attr->acctime = json_to_timespec(cJSON_GetObjectItem(obj, "acctime"));
    attr->modtime = json_to_timespec(cJSON_GetObjectItem(obj, "modtime"));
    attr->crtime = json_to_timespec(cJSON_GetObjectItem(obj, "crtime"));
    attr->chgtime = json_to_timespec(cJSON_GetObjectItem(obj, "chgtime"));
    attr->bkuptime = json_to_timespec(cJSON_GetObjectItem(obj, "bkuptime"));
    attr->flags = cJSON_GetObjectItem(obj, "flags")->valueint;
    return attr;
}
#endif /* __APPLE__ */

// Open a file, read the string, and parse it into a JSON object
cJSON * readJSONFile(const char * fpath);
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
        if(obj != NULL) {
            printf("Loaded %zu bytes\n", fsize);
            return obj;
        } else {
            errno = 0;
            return NULL;
        }
    } else {
        perror("Unable to read file");
        return NULL;
    }
}


















/* Open the FIFO for communicating events */
void testsuite_fifo_init(void);
void testsuite_fifo_init(void) {
    if(testsuite_fifo == NULL) {
        testsuite_fifo = fopen(testsuite_fifo_name, "w");
    }
}

/* Package event as a chunk of JSON and write it to the FIFO */
void report_testsuite_event_obj(cJSON * event);
void report_testsuite_event_obj(cJSON * event) {
    char * event_json = cJSON_Print(event);
    cJSON_Delete(event);
    fwrite(event_json, sizeof(char), strlen(event_json), testsuite_fifo);
    fflush(testsuite_fifo);
    free(event_json);
}

/* Report an event by writing JSON to the FIFO (called via ts_test_fail or ts_test_pass) */
void report_testsuite_event(const char * func_name, cJSON * params, bool passed, const char * message, va_list args_in);
void report_testsuite_event(const char * func_name, cJSON * params, bool passed, const char * message, va_list args_in)
{
    cJSON * event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "func_name", func_name);
    cJSON_AddBoolToObject(event, "passed", passed);

    if(params != NULL) {
        cJSON_AddItemToObject(event, "params", params);
    }

    if(args_in != NULL) {
        // Produce a formatted string from the variadic arguments struct
        va_list args;
        va_copy(args, args_in);
        size_t formatted_len = strlen(func_name) + strlen(message) + 1024;
        char formatted[formatted_len];
        vsnprintf(formatted, formatted_len, message, args);
        va_end(args);
        cJSON_AddStringToObject(event, "message", formatted);
    } else {
        // No arguments, so just pass the raw message
        cJSON_AddStringToObject(event, "message", message);
    }

    report_testsuite_event_obj(event);
}

void report_testsuite_error(const char * message, ...);
void report_testsuite_error(const char * message, ...)
{
    va_list args;
    va_start(args, message);
    report_testsuite_event("__ERROR", NULL, false, message, args);
    va_end(args);
}

void ts_test_pass(const char * func_name);
void ts_test_pass(const char * func_name)
{
    report_testsuite_event(func_name, NULL, true, "", NULL);
}

void ts_test_fail(const char * func_name, cJSON * params, const char * message, ...);
void ts_test_fail(const char * func_name, cJSON * params, const char * message, ...)
{
    va_list args;
    va_start(args, message);
    report_testsuite_event(func_name, params, false, message, args);
    va_end(args);
}

/* Starting a group of sequences */
void testsuite_grp_start(cJSON * group);
void testsuite_grp_start(cJSON * group)
{
    cJSON * event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "func_name", "__GROUP_START");
    cJSON_AddNumberToObject(event, "num_sequences", cJSON_GetArraySize(group));
    report_testsuite_event_obj(event);
}

/* Finished a group of sequences */
void testsuite_grp_end(bool passed);
void testsuite_grp_end(bool passed)
{
    cJSON * event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "func_name", "__GROUP_END");
    cJSON_AddBoolToObject(event, "passed", passed);
    report_testsuite_event_obj(event);
}

/* Starting a sequence of calls */
void testsuite_seq_start(cJSON * sequence);
void testsuite_seq_start(cJSON * sequence)
{
    cJSON * event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "func_name", "__SEQUENCE_START");
    cJSON_AddStringToObject(event, "action", cJSON_GetObjectItem(sequence, "action")->valuestring);
    cJSON_AddStringToObject(event, "application", cJSON_GetObjectItem(sequence, "application")->valuestring);
    cJSON_AddStringToObject(event, "os", cJSON_GetObjectItem(sequence, "os")->valuestring);
    cJSON_AddNumberToObject(event, "num_calls", cJSON_GetArraySize(cJSON_GetObjectItem(sequence, "calls")));
    report_testsuite_event_obj(event);
}

/* Finished a sequence of calls */
void testsuite_seq_end(bool passed);
void testsuite_seq_end(bool passed)
{
    cJSON * event = cJSON_CreateObject();
    cJSON_AddStringToObject(event, "func_name", "__SEQUENCE_END");
    cJSON_AddBoolToObject(event, "passed", passed);
    report_testsuite_event_obj(event);
}

/* Special event indicating that the entire testsuite has finished */
void testsuite_end(void);
void testsuite_end(void)
{
    report_testsuite_event("__END", NULL, true, "", NULL);
}

/* Close the FIFO */
void testsuite_fifo_destroy(void);
void testsuite_fifo_destroy(void)
{
    if(testsuite_fifo != NULL) {
        fclose(testsuite_fifo);
        testsuite_fifo = NULL;
        unlink(testsuite_fifo_name);
    }
}
















struct filler_data {
    char ** files;
    off_t num_files;
    size_t capacity;
};

// Callback invoked during the readdir call in reset_directory_fuse
int testsuite_filler(void * buf, const char * name, const struct stat * stbuf, off_t off);
int testsuite_filler(void * buf, const char * name, const struct stat * stbuf, off_t off) {
    (void) stbuf;
    (void) off;

    struct filler_data * fdata = (struct filler_data *) buf;

    // Double array capacity if we're about to overflow it
    if(fdata->num_files + 1 >= fdata->capacity) {
        fdata->capacity *= 2;
        fdata->files = realloc(fdata->files, fdata->capacity * sizeof(fdata->files));
    }

    // Append the filename to the array
    fdata->files[fdata->num_files] = malloc(strlen(name) + 1);
    strcpy(fdata->files[fdata->num_files], name);
    fdata->num_files++;
    
    return 0;
}


// Reset an entire FUSE filesystem by recursively deleting every file and directory
// This requires opendir, readdir, releasedir, unlink and rmdir to be defined and implemented correctly
void reset_filesystem_path(const struct fuse_operations * op, const char * path);
void reset_filesystem_path(const struct fuse_operations * op, const char * path) {
    //printf("Entering directory '%s'\n", path);

    // Open directory
    struct fuse_file_info * fi = malloc(sizeof(*fi));
    empty_fi(fi);
    if(op->opendir(path, fi) == 0) {

        // Read in a list of filenames of files/dirs in this dir
        struct filler_data * fdata = malloc(sizeof(*fdata));
        fdata->num_files = 0;
        fdata->capacity = 16; // initial capacity - it will realloc
        fdata->files = malloc(fdata->capacity * sizeof(fdata->files));
        op->readdir(path, (void *) fdata, testsuite_filler, 0, fi);
        op->releasedir(path, fi);

        if(fdata->num_files > 0) {
            int i;
            char ** files = fdata->files;
            for(i = 0; i < fdata->num_files; i++) {
                char * fname = files[i];

                // Skip pointers to the current and parent directories
                if(strcmp(fname, ".") == 0 || strcmp(fname, "..") == 0) continue;

                // Construct absolute path (because we just have a filename)
                size_t path_len = strlen(path) + strlen(fname) + 1;
                char fpath[path_len + 1];
                strcpy(fpath, path);
                if(fpath[strlen(path)-1] != '/') {
                    // Add a directory separator if we need one
                    strcat(fpath, "/");
                }
                strcat(fpath, fname);
                free(fname);

                // Delete the file (if it's a file) or recurse into the subdirectory (if it's a directory)
                struct stat * s = malloc(sizeof(*s));
                s->st_mode = 0;
                if(op->getattr(fpath, s) == 0) {
                    if(S_ISDIR(s->st_mode) != 0) {
                        reset_filesystem_path(op, fpath);
                    } else {
                        printf("Unlinking file '%s'\n", fpath);
                        if(op->unlink(fpath) < 0) {
                            ts_test_fail("unlink", NULL, "Call failed on path %s. Error was: %s", fpath, strerror(errno));
                        }
                    }
                } else {
                    ts_test_fail("getattr", NULL, "Call failed on path %s. Error was: %s", fpath, strerror(errno));
                }
                free(s);
            }
        } else {
            ts_test_fail("readdir", NULL, "Directory %s is empty", path);
        }
        free(fdata->files);
        free(fdata);
    } else {
        ts_test_fail("opendir", NULL, "Call failed on %s. Error was: %s", path, strerror(errno));
    }
    free(fi);

    // Delete empty directory, but not the mountpoint!
    if(strcmp(path, "/") != 0) {
        printf("Removing directory '%s'\n", path);
        int retval = op->rmdir(path);
        if(retval < 0) {
            ts_test_fail("rmdir", NULL, "Call failed on %s. Error was: %s", path, strerror(errno));
        }
    }
}

// Convenience method for the above
void reset_filesystem(const struct fuse_operations * op);
void reset_filesystem(const struct fuse_operations * op) {
    reset_filesystem_path(op, "/");
}

bool timesRoughlyEqual(time_t t1, time_t t2);
bool timesRoughlyEqual(time_t t1, time_t t2) {
    time_t window = 5;
    if(t2 >= t1 - window && t2 <= t1 + window) return true;
    else return false;
}

// Check whether the states both filesystems are equivalent
bool assert_equivalent_path(const struct fuse_operations * passthru, const struct fuse_operations * real, const char * path);
bool assert_equivalent_path(const struct fuse_operations * passthru, const struct fuse_operations * real, const char * path) {
    
    bool equivalent = true;

    // Open directory
    struct fuse_file_info * fi = malloc(sizeof(*fi));
    empty_fi(fi);
    if(passthru->opendir(path, fi) == 0) {

        // Read in a list of filenames of files/dirs in this dir
        struct filler_data * fdata = malloc(sizeof(*fdata));
        fdata->num_files = 0;
        fdata->capacity = 16; // initial capacity - it will realloc
        fdata->files = malloc(fdata->capacity * sizeof(fdata->files));
        passthru->readdir(path, (void *) fdata, testsuite_filler, 0, fi);
        passthru->releasedir(path, fi);

        if(fdata->num_files > 0) {
            int i;
            char ** files = fdata->files;
            for(i = 0; i < fdata->num_files; i++) {
                char * fname = files[i];

                // Skip pointers to the current and parent directories
                if(strcmp(fname, ".") == 0 || strcmp(fname, "..") == 0) continue;

                // Construct absolute path (because we just have a filename)
                size_t path_len = strlen(path) + strlen(fname) + 1;
                char fpath[path_len + 1];
                strcpy(fpath, path);
                if(fpath[strlen(path)-1] != '/') {
                    // Add a directory separator if we need one
                    strcat(fpath, "/");
                }
                strcat(fpath, fname);
                free(fname);

                // Read the file from the passthru fs
                struct stat * s_passthru = malloc(sizeof(*s_passthru));
                s_passthru->st_mode = 0;
                if(passthru->getattr(fpath, s_passthru) == 0) {

                    // Try reading the file at the same path from the real fs
                    struct stat * s_real = malloc(sizeof(*s_real));
                    s_real->st_mode = 0;
                    
                    // Create a JSON object containing the params, so we can pass it back to the test suite
                    cJSON * params = cJSON_CreateObject();
                    cJSON_AddStringToObject(params, "path", fpath);
                    cJSON_AddItemToObject(params, "stat", stat_to_json(s_real));

                    if(real->getattr(fpath, s_real) == 0) {

                        // Make sure file properties match up
                        if(S_ISBLK(s_passthru->st_mode) && !S_ISBLK(s_real->st_mode)) {
                            ts_test_fail("getattr", params, "'%s' should be a block special file", fpath);
                            equivalent = false;
                        } else if(S_ISCHR(s_passthru->st_mode) && !S_ISCHR(s_real->st_mode)) {
                            ts_test_fail("getattr", params, "'%s' should be a character special file", fpath);
                            equivalent = false;
                        } else if(S_ISDIR(s_passthru->st_mode) && !S_ISDIR(s_real->st_mode)) {
                            ts_test_fail("getattr", params, "'%s' should be a directory", fpath);
                            equivalent = false;
                        } else if(S_ISFIFO(s_passthru->st_mode) && !S_ISFIFO(s_real->st_mode)) {
                            ts_test_fail("getattr", params, "'%s' should be a pipe or FIFO special file", fpath);
                            equivalent = false;
                        } else if(S_ISREG(s_passthru->st_mode) && !S_ISREG(s_real->st_mode)) {
                            ts_test_fail("getattr", params, "'%s' should be a regular file", fpath);
                            equivalent = false;
                        } else if(S_ISLNK(s_passthru->st_mode) && !S_ISLNK(s_real->st_mode)) {
                            ts_test_fail("getattr", params, "'%s' should be a symbolic link", fpath);
                            equivalent = false;
                        } else if(S_ISSOCK(s_passthru->st_mode) && !S_ISSOCK(s_real->st_mode)) {
                            ts_test_fail("getattr", params, "'%s' should be a socket", fpath);
                            equivalent = false;
                        /*} else if(s_passthru->st_nlink != s_real->st_nlink) {
                            ts_test_fail("getattr", params, "'%s' has %d hard links but should have %d", fpath, s_real->st_nlink, s_passthru->st_nlink);
                            equivalent = false;*/
                        /*} else if(s_passthru->st_size != s_real->st_size) {
                            ts_test_fail("getattr", params, "'%s' has size %d but should be %d", fpath, s_real->st_size, s_passthru->st_size);
                            equivalent = false;*/
                        } else if(!timesRoughlyEqual(s_passthru->st_atime, s_real->st_atime)) {
                            ts_test_fail("getattr", params, "'%s' has last access time of %lld but should be %lld", fpath, (long long) s_real->st_atime, (long long) s_passthru->st_atime);
                            equivalent = false;
                        } else if(!timesRoughlyEqual(s_passthru->st_mtime, s_real->st_mtime)) {
                            ts_test_fail("getattr", params, "'%s' has last modification time of %lld but should be %lld", fpath, (long long) s_real->st_mtime, (long long) s_passthru->st_mtime);
                            equivalent = false;
                        } else if(!timesRoughlyEqual(s_passthru->st_ctime, s_real->st_ctime)) {
                            ts_test_fail("getattr", params, "'%s' has last status change time of %lld but should be %lld", fpath, (long long) s_real->st_ctime, (long long) s_passthru->st_ctime);
                            equivalent = false;
                        }

                        // If there are no errors so far and the file is actually a directory then recurse into it
                        if(equivalent && S_ISDIR(s_passthru->st_mode) != 0) {
                            equivalent = assert_equivalent_path(passthru, real, fpath) == false ? false : equivalent;
                        }
                    } else {
                        ts_test_fail("getattr", params, "'%s' should exist but getattr failed", fpath);
                        equivalent = false;
                    }
                    free(s_real);
                    cJSON_Delete(params);
                } else {
                    fprintf(stderr, "Getattr failed on '%s'\n", fpath);
                    equivalent = false;
                }
                free(s_passthru);
            }
        } else {
            fprintf(stderr, "  Directory '%s' is empty\n", path);
            equivalent = false;
        }
        free(fdata->files);
        free(fdata);
    } else {
        fprintf(stderr, "  Error calling opendir on '%s'\n", path);
        equivalent = false;
    }
    free(fi);
    return equivalent;
}

// Convenience method for the above
bool assert_equivalent(const struct fuse_operations * passthru, const struct fuse_operations * real);
bool assert_equivalent(const struct fuse_operations * passthru, const struct fuse_operations * real) {
    return assert_equivalent_path(passthru, real, "/");
}

// FUSE normally provides this method in a call to readdir, so must be implemented here instead 
int readdir_filler(void * buf, const char * name, const struct stat * stbuf, off_t off);
int readdir_filler(void * buf, const char * name, const struct stat * stbuf, off_t off) {
    (void) buf;
    (void) name;
    (void) stbuf;
    (void) off;
    return 0;
}

// Call a function in the FUSE operations struct, identified by its name as a string
// Pass the parameters as a JSON object and pass a reference to the map of filenames to filehandles
int make_call(const struct fuse_operations * op, const char * func_name, cJSON * params, struct fuse_file_info * fi, GHashTable * fhs, bool * func_not_defined);
int make_call(const struct fuse_operations * op, const char * func_name, cJSON * params, struct fuse_file_info * fi, GHashTable * fhs, bool * func_not_defined) {
    if(op != NULL) {
        //printf("   %s call\n", func_name);

        int retval = -1;
        if(strcmp(func_name, "getattr") == 0)
        {
            if(op->getattr != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                struct stat * s = json_to_stat(cJSON_GetObjectItem(params, "stat"));
                retval = op->getattr(path, s);
                free(s);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "readlink") == 0)
        {
            if(op->readlink != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                size_t size = cJSON_GetObjectItem(params, "size")->valueint;
                char link[size];
                retval = op->readlink(path, link, size);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "getdir") == 0)
        {
            if(op->getdir != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                retval = op->getdir(path, NULL, NULL);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "mknod") == 0)
        {
            if(op->mknod != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                mode_t mode = cJSON_GetObjectItem(params, "mode")->valueint;
                dev_t dev = cJSON_GetObjectItem(params, "dev")->valueint;
                retval = op->mknod(path, mode, dev);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "mkdir") == 0)
        {
            if(op->mkdir != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                mode_t mode = cJSON_GetObjectItem(params, "mode")->valueint;
                retval = op->mkdir(path, mode);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "unlink") == 0)
        {
            if(op->unlink != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                retval = op->unlink(path);
                destroy_fh(fhs, path);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "rmdir") == 0)
        {
            if(op->rmdir != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                retval = op->rmdir(path);
                destroy_fh(fhs, path);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "symlink") == 0)
        {
            if(op->symlink != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                const char * link = cJSON_GetObjectItem(params, "link")->valuestring;
                retval = op->symlink(path, link);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "rename") == 0)
        {
            if(op->rename != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                const char * newpath = cJSON_GetObjectItem(params, "newpath")->valuestring;
                retval = op->rename(path, newpath);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "link") == 0)
        {
            if(op->link != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                const char * newpath = cJSON_GetObjectItem(params, "newpath")->valuestring;
                retval = op->link(path, newpath);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "chmod") == 0)
        {
            if(op->chmod != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                mode_t mode = cJSON_GetObjectItem(params, "mode")->valueint;
                retval = op->chmod(path, mode);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "chown") == 0)
        {
            if(op->chown != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                uid_t uid = cJSON_GetObjectItem(params, "uid")->valueint;
                gid_t gid = cJSON_GetObjectItem(params, "gid")->valueint;
                retval = op->chown(path, uid, gid);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "truncate") == 0)
        {
            if(op->truncate != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                off_t newsize = cJSON_GetObjectItem(params, "newsize")->valueint;
                retval = op->truncate(path, newsize);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "utime") == 0)
        {
            if(op->utime != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                struct utimbuf * ubuf = json_to_utimbuf(cJSON_GetObjectItem(params, "ubuf"));
                retval = op->utime(path, ubuf);
                free(ubuf);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "open") == 0)
        {
            if(op->open != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                update_fi(NULL, NULL, fi, cJSON_GetObjectItem(params, "fi"));
                retval = op->open(path, fi);
                store_fh(fhs, path, fi->fh);
                increment_open(fhs, path);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "read") == 0)
        {
            if(op->read != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                size_t size = cJSON_GetObjectItem(params, "size")->valueint;
                void * buf = malloc(size);
                off_t offset = cJSON_GetObjectItem(params, "offset")->valueint;
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));
                retval = op->read(path, buf, size, offset, fi);
                free(buf);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "write") == 0)
        {
            if(op->write != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                const char * buf = cJSON_GetObjectItem(params, "buf")->valuestring;
                size_t size = cJSON_GetObjectItem(params, "size")->valueint;
                off_t offset = cJSON_GetObjectItem(params, "offset")->valueint;
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));
                retval = op->write(path, buf, size, offset, fi);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "statfs") == 0)
        {
            if(op->statfs != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                struct statvfs * s = json_to_statvfs(cJSON_GetObjectItem(params, "statvfs"));
                retval = op->statfs(path, s);
                free(s);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "flush") == 0)
        {
            if(op->flush != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));
                retval = op->flush(path, fi);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "release") == 0)
        {
            if(op->release != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));
                retval = op->release(path, fi);
                //destroy_fh(fhs, path);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "fsync") == 0)
        {
            if(op->fsync != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                int datasync = cJSON_GetObjectItem(params, "datasync")->valueint;
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));
                retval = op->fsync(path, datasync, fi);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "setxattr") == 0)
        {
            if(op->setxattr != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                const char * name = cJSON_GetObjectItem(params, "name")->valuestring;
                const char * value = cJSON_GetObjectItem(params, "value")->valuestring;
                size_t size = cJSON_GetObjectItem(params, "size")->valueint;
                int flags = cJSON_GetObjectItem(params, "flags")->valueint;

                #ifdef __APPLE__
                    // This function takes a position parameter on Mac but not Linux
                    // We also wont have position in the test data if it was generated on Linux
                    cJSON * position_obj = cJSON_GetObjectItem(params, "position");
                    uint32_t position = 0;
                    if(position_obj != NULL) {
                        position = position_obj->valueint;
                    }
                    retval = op->setxattr(path, name, value, size, flags, position);
                #else
                    retval = op->setxattr(path, name, value, size, flags);
                #endif
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "getxattr") == 0)
        {
            if(op->getxattr != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                const char * name = cJSON_GetObjectItem(params, "name")->valuestring;
                size_t size = cJSON_GetObjectItem(params, "size")->valueint;
                char value[size];

                #ifdef __APPLE__
                    // This function takes a position parameter on Mac but not Linux
                    // We also wont have position in the test data if it was generated on Linux
                    cJSON * position_obj = cJSON_GetObjectItem(params, "position");
                    uint32_t position = 0;
                    if(position_obj != NULL) {
                        position = position_obj->valueint;
                    }
                    retval = op->getxattr(path, name, value, size, position);
                #else
                    retval = op->getxattr(path, name, value, size);
                #endif
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "listxattr") == 0)
        {
            if(op->listxattr != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                size_t size = cJSON_GetObjectItem(params, "size")->valueint;
                char list[size];
                retval = op->listxattr(path, list, size);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "removexattr") == 0)
        {
            if(op->removexattr != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                const char * name = cJSON_GetObjectItem(params, "name")->valuestring;
                retval = op->removexattr(path, name);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "opendir") == 0)
        {
            if(op->opendir != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                update_fi(NULL, NULL, fi, cJSON_GetObjectItem(params, "fi"));
                retval = op->opendir(path, fi);
                store_fh(fhs, path, fi->fh);
                increment_open(fhs, path);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "readdir") == 0)
        {
            if(op->readdir != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                off_t offset = cJSON_GetObjectItem(params, "offset")->valueint;
                void * buf = NULL; // this buffer is for our filler function, which is empty
                fuse_fill_dir_t filler = readdir_filler;
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));
                retval = op->readdir(path, buf, filler, offset, fi);
                //free(buf);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "releasedir") == 0)
        {
            if(op->releasedir != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));
                //retval = op->releasedir(path, fi);
                //destroy_fh(fhs, path);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "fsyncdir") == 0)
        {
            if(op->fsyncdir != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                int datasync = cJSON_GetObjectItem(params, "datasync")->valueint;
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));
                retval = op->fsyncdir(path, datasync, fi);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "init") == 0)
        {
            if(op->init != NULL) {
                struct fuse_conn_info * conn = json_to_fuse_conn_info(cJSON_GetObjectItem(params, "conn"));
                op->init(conn);
                retval = 0;
                free(conn);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "destroy") == 0)
        {
            if(op->destroy != NULL) {
                void * userdata = NULL;
                op->destroy(userdata);
                retval = 0;
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "access") == 0)
        {
            if(op->access != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                int mask = cJSON_GetObjectItem(params, "mask")->valueint;
                retval = op->access(path, mask);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "create") == 0)
        {
            if(op->create != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                mode_t mode = cJSON_GetObjectItem(params, "mode")->valueint;
                update_fi(NULL, NULL, fi, cJSON_GetObjectItem(params, "fi"));
                retval = op->create(path, mode, fi);
                store_fh(fhs, path, fi->fh);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "ftruncate") == 0)
        {
            if(op->ftruncate != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                off_t offset = cJSON_GetObjectItem(params, "offset")->valueint;
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));
                retval = op->ftruncate(path, offset, fi);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "fgetattr") == 0)
        {
            if(op->fgetattr != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                struct stat * s = json_to_stat(cJSON_GetObjectItem(params, "stat"));
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));
                retval = op->fgetattr(path, s, fi);
                free(s);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "lock") == 0)
        {
            if(op->lock != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));
                int cmd = cJSON_GetObjectItem(params, "cmd")->valueint;
                struct flock * flock = json_to_flock(cJSON_GetObjectItem(params, "flock"));
                retval = op->lock(path, fi, cmd, flock);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "utimens") == 0)
        {
            if(op->utimens != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                struct timespec * tv = json_to_timespec_array(cJSON_GetObjectItem(params, "tv"));
                retval = op->utimens(path, tv);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "bmap") == 0)
        {
            if(op->bmap != NULL) {
                // This makes sense only for block device backed filesystems mounted with the 'blkdev' option
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                size_t blocksize = cJSON_GetObjectItem(params, "blocksize")->valueint;
                retval = op->bmap(path, blocksize, NULL);
            } else {
                *func_not_defined = true;
            }
        }

        #ifdef __APPLE__ // Apple osxfuse-only functions

        else if(strcmp(func_name, "setvolname") == 0)
        {
            if(op->setvolname != NULL) {
                const char * volname = cJSON_GetObjectItem(params, "volname")->valuestring;    
                retval = op->setvolname(volname);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "exchange") == 0)
        {
            if(op->exchange != NULL) {
                const char * path1 = cJSON_GetObjectItem(params, "path1")->valuestring;
                const char * path2 = cJSON_GetObjectItem(params, "path2")->valuestring;
                int options = cJSON_GetObjectItem(params, "options")->valueint; 
                retval = op->exchange(path1, path2, options);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "getxtimes") == 0)
        {
            if(op->getxtimes != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                struct timespec bkuptime = json_to_timespec(cJSON_GetObjectItem(params, "bkuptime"));
                struct timespec crtime = json_to_timespec(cJSON_GetObjectItem(params, "crtime"));    
                retval = op->getxtimes(path, &bkuptime, &crtime);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "setbkuptime") == 0)
        {
            if(op->setbkuptime != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                struct timespec tv = json_to_timespec(cJSON_GetObjectItem(params, "tv"));    
                retval = op->setbkuptime(path, &tv);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "setchgtime") == 0)
        {
            if(op->setchgtime != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                struct timespec tv = json_to_timespec(cJSON_GetObjectItem(params, "tv"));    
                retval = op->setchgtime(path, &tv);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "setcrtime") == 0)
        {
            if(op->setcrtime != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                struct timespec tv = json_to_timespec(cJSON_GetObjectItem(params, "tv"));    
                retval = op->setcrtime(path, &tv);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "chflags") == 0)
        {
            if(op->chflags != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                int flags = cJSON_GetObjectItem(params, "flags")->valueint;
                retval = op->chflags(path, flags);
            } else {
                *func_not_defined = true;
            }
        }

        else if(strcmp(func_name, "setattr_x") == 0)
        {
            if(op->setattr_x != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                struct setattr_x * attr = json_to_setattr_x(cJSON_GetObjectItem(params, "attr"));
                retval = op->setattr_x(path, attr);
            } else {
                *func_not_defined = true;
            }
        }

        else if(strcmp(func_name, "fsetattr_x") == 0)
        {
            if(op->fsetattr_x != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                struct setattr_x * attr = json_to_setattr_x(cJSON_GetObjectItem(params, "attr"));
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));    
                retval = op->fsetattr_x(path, attr, fi);
            } else {
                *func_not_defined = true;
            }
        }

        #else // Linux libfuse-only functions

        else if(strcmp(func_name, "ioctl") == 0)
        {
            if(op->ioctl != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                int cmd = cJSON_GetObjectItem(params, "cmd")->valueint;
                void * arg = NULL;
                int flags = cJSON_GetObjectItem(params, "flags")->valueint;
                void * data = NULL;
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));
                retval = op->ioctl(path, cmd, arg, fi, flags, data);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "poll") == 0)
        {
            if(op->poll != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                void * ph = NULL;
                unsigned reventsp = cJSON_GetObjectItem(params, "reventsp")->valueint;
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));
                retval = op->poll(path, fi, ph, &reventsp);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "write_buf") == 0)
        {
            if(op->write_buf != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                struct fuse_bufvec * buf = json_to_fuse_bufvec(path, fhs, cJSON_GetObjectItem(params, "buf"));
                int off = cJSON_GetObjectItem(params, "off")->valueint;
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));    
                retval = op->write_buf(path, buf, off, fi);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "read_buf") == 0)
        {
            if(op->read_buf != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                int size = cJSON_GetObjectItem(params, "size")->valueint;
                void * bufp = malloc(size);
                int off = cJSON_GetObjectItem(params, "off")->valueint;
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));
                retval = op->read_buf(path, bufp, size, off, fi);
                free(bufp);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "flock") == 0)
        {
            if(op->flock != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                int op_int = cJSON_GetObjectItem(params, "op")->valueint;
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));
                retval = op->flock(path, fi, op_int);
            } else {
                *func_not_defined = true;
            }
        }
        else if(strcmp(func_name, "fallocate") == 0)
        {
            if(op->fallocate != NULL) {
                const char * path = cJSON_GetObjectItem(params, "path")->valuestring;
                int mode = cJSON_GetObjectItem(params, "mode")->valueint;
                int offset = cJSON_GetObjectItem(params, "offset")->valueint;
                int len = cJSON_GetObjectItem(params, "len")->valueint;
                update_fi(fhs, path, fi, cJSON_GetObjectItem(params, "fi"));    
                retval = op->fallocate(path, mode, offset, len, fi);
            } else {
                *func_not_defined = true;
            }
        }

        #endif

        else
        {
            report_testsuite_error("Cannot make call '%s' as it is not handled by the test suite", func_name);
        }
        return retval;
    } else {
        report_testsuite_error("Cannot make call '%s' as fuse_operations is null\n", func_name);
        return -1;
    }
}

// Returns true if all tests passed, or false if a single test failed
bool run_test_sequence(cJSON * calls);
bool run_test_sequence(cJSON * calls) {
    int num_calls = cJSON_GetArraySize(calls);
    int i;
    for(i = 0; i < num_calls; i++) {
        cJSON * call = cJSON_GetArrayItem(calls, i);
        const char * func_name = cJSON_GetObjectItem(call, "name")->valuestring;
        cJSON * params = cJSON_GetObjectItem(call, "params");

        // Call passthru filesystem and record any error string
        bool passthru_func_not_defined = false;
        int passthru_retval = make_call(passthru_ops, func_name, params, fi_passthru, fhs_passthru, &passthru_func_not_defined);
        
        // If the passthru function is not defined then there's either an error in our passthru fs or the test data is bad (or more up-to-date)
        if(passthru_func_not_defined) {
            ts_test_fail(func_name, NULL, "Cannot make call '%s' as it is not handled by the internal passthru filesystem", func_name);
            return false;
        }

        // Get the error string if there was an error
        char * passthru_error = NULL;
        if(passthru_retval < 0) {
            passthru_error = strerror(errno);
        }

        // Call the filesystem under test
        bool real_func_not_defined = false;
        int real_retval = make_call(real_ops, func_name, params, fi_real, fhs_real, &real_func_not_defined);

        // If the function is not defined they just need to implement it
        if(real_func_not_defined) {
            ts_test_fail(func_name, NULL, "Function not defined");
            return false;
        }

        // Get the error string if there was an error (note: this relies on the users filesystem setting errno)
        char * real_error = NULL;
        if(real_retval < 0) {
            real_error = strerror(errno);
        }
        
        // Check that both functions either passed or failed
        // For example, getattr may fail by returning -1 on one filesystem but another will return -6
        // They both indicate failure and this is all that the FUSE/POSIX spec requires.
        if((passthru_retval < 0 && real_retval >= 0) || (passthru_retval >= 0 && real_retval < 0)) {
            // Report the error and skip checking for filesystem equivalence
            if(passthru_retval < 0) {
                ts_test_fail(func_name, params, "Expected call to fail with return value -1 but it returned %d which indicates success. It should have failed with this error: %s", real_retval, passthru_error);
            } else {
                ts_test_fail(func_name, params, "Expected call to pass with return value 0 but it returned %d which indicates failure. The filesystem reported this error: %s", real_retval, real_error);
            }
            return false;
        } else {
            // Function returned the correct value so check whether the states both filesystems are equivalent
            bool equivalent = assert_equivalent(passthru_ops, real_ops);
            if(equivalent) {
                // If we got to this point and the filesystems are equivalent then the test passes
                ts_test_pass(func_name);
            } else {
                // don't need to report this one, as the assert method reports the specific errors
                return false;
            }
        }
    }
    return true;
}

// Test that basic operations work. This is more of a sanity check before ploughing through with the main tests.
// Returns true if everything passed, otherwise false.
bool test_basic_ops(const struct fuse_operations * op);
bool test_basic_ops(const struct fuse_operations * op) {
    if(op->readdir == NULL) {
        ts_test_fail("readdir", NULL, "Function not defined");
        return false;
    } else if(op->opendir == NULL) {
        ts_test_fail("opendir", NULL, "Function not defined");
        return false;
    } else if(op->releasedir == NULL) {
        ts_test_fail("releasedir", NULL, "Function not defined");
        return false;
    } else if(op->rmdir == NULL) {
        ts_test_fail("rmdir", NULL, "Function not defined");
        return false;
    } else if(op->unlink == NULL) {
        ts_test_fail("unlink", NULL, "Function not defined");
        return false;
    } else {
        return true;
    }
}

void testsuite_init(const struct fuse_operations * op, size_t op_size, char * mountpoint);
void testsuite_init(const struct fuse_operations * op, size_t op_size, char * mpoint) {
    (void) op_size;
    printf("[libfuse] Called testsuite_init\n");
    real_ops = op;
    passthru_ops = &passthru_ops_s;
    mountpoint = mpoint;
    testsuite_fifo_init();

    // Load tests using the filename passed in the environment variable
    char * tests_file = getenv("TEST_DATA_FILE");
    if(tests_file != NULL && strlen(tests_file) > 0) {
        groups = readJSONFile(tests_file);
    } else {
        groups = NULL;
    }

    if(groups != NULL) {

        // Create the root directory for the passthru filesystem, if it does not already exist
        root_path = "/tmp/fdt-ts-root";
        struct stat s;
        int retval = stat(root_path, &s);
        if(retval != 0) {
            retval = mkdir(root_path, 0777);
            if(retval != 0) {
                report_testsuite_error("Unable to create root directory for passthru filesystem at '%s': %s", root_path, strerror(errno));
                goto end;
            }
        }

        // Initialise empty file info structs
        fi_passthru = malloc(sizeof(*fi_passthru));
        empty_fi(fi_passthru);
        fi_real = malloc(sizeof(*fi_real));
        empty_fi(fi_real);

        // Make sure basic operations work before running the main tests
        // This includes deletion, for example, as chaos would ensue without this
        bool basics_ok = test_basic_ops(real_ops);
        if(basics_ok) {

            // Run each group of sequences
            int i;
            for(i = 0; i < cJSON_GetArraySize(groups); i++) {
                cJSON * group = cJSON_GetArrayItem(groups, i);
                testsuite_grp_start(group);

                // Setup new filehandle tables for this group
                // These map from filename to filehandle, as the real FUSE library would be responsible for keeping track of them and passing them to functions
                fhs_passthru = g_hash_table_new(g_str_hash, g_str_equal);
                fhs_real = g_hash_table_new(g_str_hash, g_str_equal);

                // Run sequences within this group, stopping if any test fails
                bool group_passed = true;
                int j;
                for(j = 0; j < cJSON_GetArraySize(group); j++) {
                    cJSON * sequence = cJSON_GetArrayItem(group, j);
                    testsuite_seq_start(sequence);
                    cJSON * calls = cJSON_GetObjectItem(sequence, "calls");
                    bool all_passed = run_test_sequence(calls);
                    if(all_passed) {
                        // Sequence passed
                        testsuite_seq_end(true);
                    } else {
                        // Sequence failed at some point, and group therefore fails too
                        testsuite_seq_end(false);
                        group_passed = false;
                        break;
                    }
                }

                // Reset the passthru and real filesystems
                reset_filesystem(passthru_ops);
                reset_filesystem(real_ops);

                testsuite_grp_end(group_passed);

                // Destroy filehandle tables
                g_hash_table_destroy(fhs_passthru);
                g_hash_table_destroy(fhs_real);

            }
        } else {
            report_testsuite_error("Not running main test suite as a number of basic operations are not defined");
        }
        free(fi_passthru);
        free(fi_real);
    } else {
        if(errno == 0) {
            report_testsuite_error("Error parsing JSON test data");
        } else {
            report_testsuite_error("Error loading JSON test data: %s", strerror(errno));
        }
    }
    end:
    testsuite_end();
    testsuite_fifo_destroy();
}
