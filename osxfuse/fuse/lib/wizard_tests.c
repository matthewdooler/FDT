/*
  FUSE Development Tool: Wizard tests
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

const char * wizard_fifo_name = "fuse-wizard.fifo";
FILE * wizard_fifo = NULL;
const char * function_not_defined_err = "Define this function and set it in the fuse_operations struct passed to fuse_main.";
const char * non_existent_file = "/doesnotexistfile.txt";
const char * non_existent_dir = "/doesnotexistdir";
bool test_failed = false;
static GSList * skipped_tests;

/* Open the FIFO for communicating events */
void wizard_fifo_init(void);
void wizard_fifo_init(void)
{
    if(wizard_fifo == NULL) {
    	wizard_fifo = fopen(wizard_fifo_name, "w");
    }
}

/* Report an event by writing JSON to the FIFO (called via test_fail or test_pass) */
void report_wizard_event(const char * func_name, int test_num, bool passed, bool optional, const char * message, va_list args_in);
void report_wizard_event(const char * func_name, int test_num, bool passed, bool optional, const char * message, va_list args_in)
{
	cJSON * event = cJSON_CreateObject();
	cJSON_AddStringToObject(event, "func_name", func_name);
	cJSON_AddNumberToObject(event, "test_num", test_num);
	cJSON_AddBoolToObject(event, "passed", passed);
	cJSON_AddBoolToObject(event, "optional", optional);

	if(args_in != NULL) {
		// Produce a formatted string from the arguments
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
	
	// Package event as a chunk of JSON and write it to the FIFO
	char * event_json = cJSON_Print(event);
	cJSON_Delete(event);
    fwrite(event_json, sizeof(char), strlen(event_json), wizard_fifo);
    fflush(wizard_fifo);
	free(event_json);
}

/* Special event indicating that the wizard is finished */
void tests_end(void);
void tests_end(void)
{
	report_wizard_event("__END", 0, true, true, "", NULL);
}

/* Close the FIFO */
void wizard_fifo_destroy(void);
void wizard_fifo_destroy(void)
{
	if(wizard_fifo != NULL) {
		fclose(wizard_fifo);
		wizard_fifo = NULL;
		unlink(wizard_fifo_name);
	}
}

void test_pass(const char * func_name, int test_num, bool optional);
void test_pass(const char * func_name, int test_num, bool optional)
{
	report_wizard_event(func_name, test_num, true, optional, "", NULL);
}

void test_skip(const char * func_name, int test_num, bool optional);
void test_skip(const char * func_name, int test_num, bool optional)
{
	report_wizard_event(func_name, test_num, true, optional, "Skipped", NULL);
}

void test_fail(const char * func_name, int test_num, bool optional, const char * message, ...);
void test_fail(const char * func_name, int test_num, bool optional, const char * message, ...)
{
    test_failed = true;
    va_list args;
    va_start(args, message);
	report_wizard_event(func_name, test_num, false, optional, message, args);
	va_end(args);
}

/* True if a test should be skipped */
bool is_skipped(const char * func_name, int test_num);
bool is_skipped(const char * func_name, int test_num) {

	size_t test_str_len = strlen(func_name) + 64;
    char test_str[test_str_len];
    snprintf(test_str, test_str_len, "%s:%d", func_name, test_num);

    // Search the list of tests that should be skipped
	GSList * node = skipped_tests;
    while(node != NULL) {
        char * this_test_str = (char *) node->data;
        if(this_test_str != NULL && strcmp(test_str, this_test_str) == 0) {
        	printf("[libfuse] Called is_skipped on %s\n", test_str);
        	return true;
        }
        node = node->next;
    }
    return false;
}


/**
 * TESTS
 */
void test_operations(const struct fuse_operations * op);
void test_operations(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op != NULL) {
		test_pass(__func__, 0, optional);
	} else {
		test_fail(__func__, 0, optional, "Initialise the fuse_operations struct with pointers to your functions and pass it to fuse_main. This lets FUSE know which of your functions to use when it gets a filesystem call.");
	}
}

void test_init(const struct fuse_operations * op);
void test_init(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = true;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->init != NULL) {
		test_pass(__func__, 0, optional);
		//fprintf(stderr, "wizard init invoke\n");
		//op->init(NULL);
		//fprintf(stderr, "wizard init return\n");
		//test_pass(__func__, 1, optional);
	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_getattr(const struct fuse_operations * op);
void test_getattr(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->getattr != NULL) {
		test_pass(__func__, 0, optional);

		if(!is_skipped(__func__, 1)) {
			// Get non-existent file, which should not return 0
			struct stat * stbuf = malloc(sizeof(*stbuf));
			int retval = op->getattr(non_existent_file, stbuf);
			if(retval < 0) {
				test_pass(__func__, 1, optional);
			} else {
				test_fail(__func__, 1, optional, "Return a negative number when a file does not exist. It currently returns %d when called with the path of a non-existent file (\"%s\").", retval, non_existent_file);
			}
			free(stbuf);
		} else {
			test_skip(__func__, 1, optional);
		}

		if(!is_skipped(__func__, 2)) {
			// Get "/", which should return 0
			struct stat * stbuf = malloc(sizeof(*stbuf));
			int retval = op->getattr("/", stbuf);
			if(retval == 0) {
				test_pass(__func__, 2, optional);
			} else {
				test_fail(__func__, 2, optional, "Return 0 when a file exists. It currently returns %d when called with the root path (\"/\"), but this always exists.", retval);
			}
			free(stbuf);
		} else {
			test_skip(__func__, 2, optional);
		}
	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_access(const struct fuse_operations * op);
void test_access(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->access != NULL) {
		test_pass(__func__, 0, optional);

		// F_OK tests whether or not the file exists
		// Bitwise OR of R_OK, W_OK and X_OK for read/write/execute
		// Method returns 0 when all requested permissions are granted, but -1 if any are not allowed
		int retval;

		if(!is_skipped(__func__, 1)) {
			// Access non-existent file, which should return -1
			retval = op->access(non_existent_file, F_OK);
			if(retval < 0) {
				test_pass(__func__, 1, optional);
			} else {
				test_fail(__func__, 1, optional, "Return a negative number if called with F_OK and the file does not exist. F_OK is used to test for existence of the file. It currently returns %d when called with the path of a non-existent file (\"%s\").", retval, non_existent_file);
			}
		} else {
			test_skip(__func__, 1, optional);
		}

		if(!is_skipped(__func__, 2)) {
			// Access root, which should return 0
			retval = op->access("/", F_OK);
			if(retval == 0) {
				test_pass(__func__, 2, optional);
			} else {
				test_fail(__func__, 2, optional, "Return 0 if called with F_OK and the file exists. It currently returns %d when called with the root path (\"/\"), but this always exists.", retval);
			}
		} else {
			test_skip(__func__, 2, optional);
		}

		if(!is_skipped(__func__, 3)) {
			// Should have read permission on the filesystem
			retval = op->access("/", R_OK);
			if(retval == 0) {
				test_pass(__func__, 3, optional);
			} else {
				test_fail(__func__, 3, optional, "Return 0 if called with R_OK, the file exists, and can be read by the process. It currently returns %d when called with the root path (\"/\"), but the filesystem should at least have read permissions.", retval);
			}
		} else {
			test_skip(__func__, 3, optional);
		}

	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_rmdir(const struct fuse_operations * op);
void test_rmdir(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->rmdir != NULL) {
		test_pass(__func__, 0, optional);

		if(!is_skipped(__func__, 1)) {
			int retval = op->rmdir(non_existent_dir);
			if(retval < 0) {
				test_pass(__func__, 1, optional);

				if(!is_skipped(__func__, 2)) {
					// Check that the correct errno is set
					if(errno == ENOENT) {
						test_pass(__func__, 2, optional);
					} else {
						test_fail(__func__, 2, optional, "Set errno to ENOENT when trying to delete a directory that does not exist (\"%s\").", non_existent_dir);
					}
				} else {
					test_skip(__func__, 2, optional);
				}
			} else {
				test_fail(__func__, 1, optional, "Return -1 if the directory could not be removed. It currently returns %d when trying to remove the non-existent directory \"%s\".", retval, non_existent_dir);
			}
		} else {
			test_skip(__func__, 1, optional);
		}

		// Try deleting new_dir incase we need to clean up from a previous run
		op->rmdir("/new_dir");
	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_mkdir(const struct fuse_operations * op);
void test_mkdir(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->mkdir != NULL) {
		test_pass(__func__, 0, optional);
		
		if(!is_skipped(__func__, 1)) {
			// Create a new directory
			const char * new_dir = "/new_dir";
			int retval = op->mkdir(new_dir, 0777);
			if(retval == 0) {
				test_pass(__func__, 1, optional);

				if(!is_skipped(__func__, 2)) {
					// Try creating the same directory again (should fail)
					retval = op->mkdir(new_dir, 0);
					if(retval < 0) {
						test_pass(__func__, 2, optional);

						// Check that the correct errno is set
						if(errno == EEXIST) {
							test_pass(__func__, 3, optional);
						} else {
							test_fail(__func__, 3, optional, "Set errno to EEXIST when trying to create a directory that already exists (\"%s\").", new_dir);
						}
					} else {
						test_fail(__func__, 2, optional, "Return a negative number if there was an error creating the directory. It currently returns %d when trying to create \"%s\", which had already been created. The filesystem is either returning the wrong value, or new directories are not being persisted.", retval, new_dir);
					}

					if(!is_skipped(__func__, 3)) {
						// Remove the directory
						retval = op->rmdir(new_dir);
						if(retval == 0) {
							test_pass("test_rmdir", 3, optional);
						} else {
							test_fail("test_rmdir", 3, optional, "Return 0 if the directory was successfully removed. It currently returns %d when trying to remove a directory just created: \"%s\".", retval, new_dir);
						}
					} else {
						test_skip(__func__, 3, optional);
					}
				} else {
					test_skip(__func__, 2, optional);
				}
			} else {
				test_fail(__func__, 1, optional, "Return 0 if the directory was successfully created. It currently returns %d when trying to create \"%s\".", retval, new_dir);
			}
		} else {
			test_skip(__func__, 1, optional);
		}
		
	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_unlink(const struct fuse_operations * op);
void test_unlink(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->unlink != NULL) {
		test_pass(__func__, 0, optional);

		if(!is_skipped(__func__, 1)) {
			int retval = op->unlink(non_existent_file);
			if(retval < 0) {
				test_pass(__func__, 1, optional);

				if(!is_skipped(__func__, 2)) {
					// Check that the correct errno is set
					if(errno == ENOENT) {
						test_pass(__func__, 2, optional);
					} else {
						test_fail(__func__, 2, optional, "Set errno to ENOENT when trying to delete a file that does not exist (\"%s\").", non_existent_file);
					}
				} else {
					test_skip(__func__, 2, optional);
				}
			} else {
				test_fail(__func__, 1, optional, "Return -1 if the file could not be removed. It currently returns %d when trying to remove the non-existent file \"%s\".", retval, non_existent_file);
			}
		} else {
			test_skip(__func__, 1, optional);
		}

		// Try deleting new_file incase we need to clean up from a previous run
		op->unlink("/new_file.txt");
	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_create(const struct fuse_operations * op);
void test_create(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->create != NULL) {
		test_pass(__func__, 0, optional);

		if(!is_skipped(__func__, 1)) {
			mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
			const char * new_file = "/new_file.txt";
			struct fuse_file_info * fi = malloc(sizeof(struct fuse_file_info));
			fi->flags = O_RDWR | O_CREAT;
			int retval = op->create(new_file, mode, fi);

			if(retval >= 0) {
				test_pass(__func__, 1, optional);

				if(!is_skipped("test_unlink", 3)) {
					// File created, so try to delete it
					retval = op->unlink(new_file);
					if(retval == 0) {
						test_pass("test_unlink", 3, optional);
					} else {
						test_fail("test_unlink", 3, optional, "Return 0 if the file was successfully removed. It currently returns %d when trying to remove a file just created: \"%s\".", retval, new_file);
					}
				} else {
					test_skip("test_unlink", 3, optional);
				}
			} else {
				test_fail(__func__, 1, optional, "Return a non-negative number if the file was created successfully. It currently returns %d when trying to create the file: \"%s\".", retval, new_file);
			}
			free(fi);
		} else {
			test_skip(__func__, 1, optional);
		}
	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_fgetattr(const struct fuse_operations * op);
void test_fgetattr(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->fgetattr != NULL) {
		test_pass(__func__, 0, optional);

		// Call create and use the file handle to call fgetattr
		if(!is_skipped(__func__, 1)) {
			mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
			const char * new_file = "/new_file.txt";
			struct fuse_file_info * fi = malloc(sizeof(struct fuse_file_info));
			fi->flags = O_RDWR | O_CREAT;
			op->create(new_file, mode, fi);

			// Call fgetattr on created file, which gets the attributes of an open file using fi->fh
			struct stat * stbuf = malloc(sizeof(*stbuf));
			int retval = op->fgetattr(new_file, stbuf, fi);
			if(retval == 0) {
				test_pass(__func__, 1, optional);
			} else {
				test_fail(__func__, 1, optional, "Return 0 when a file exists. It currently returns %d when called with the path and filehandle of a file just created and opened (\"%s\").", retval, new_file);
			}
			free(stbuf);

			// Cleanup
			op->unlink(new_file);
			free(fi);
		} else {
			test_skip(__func__, 1, optional);
		}
	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_flush(const struct fuse_operations * op);
void test_flush(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->flush != NULL) {
		test_pass(__func__, 0, optional);
		// Nothing else to test as this modifies complex internal state
	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_release(const struct fuse_operations * op);
void test_release(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = true;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->release != NULL) {
		test_pass(__func__, 0, optional);
		// Nothing else to test as return value is ignored
	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_opendir(const struct fuse_operations * op);
void test_opendir(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = true;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->opendir != NULL) {
		test_pass(__func__, 0, optional);

		if(!is_skipped(__func__, 1)) {
			struct fuse_file_info * fi = malloc(sizeof(struct fuse_file_info));
			int retval = op->opendir("/", fi);
			if(retval == 0) {
				test_pass(__func__, 1, optional);
			} else {
				test_fail(__func__, 1, optional, "Return 0 if the directory exists and the process has permission to open it. It currently returns %d when called with the root path (\"/\"), but the filesystem should at least have read permissions.", retval);
			}
			free(fi);
		} else {
			test_skip(__func__, 1, optional);
		}

		if(!is_skipped(__func__, 2)) {
			struct fuse_file_info * fi = malloc(sizeof(struct fuse_file_info));
			int retval = op->opendir(non_existent_dir, fi);
			if(retval == 0) {
				test_fail(__func__, 2, optional, "Return a negative number if a directory cannot be opened. It currently returns %d when called with a non-existent path (\"%s\").", retval, non_existent_dir);
			} else {
				test_pass(__func__, 2, optional);
			}
			free(fi);
		} else {
			test_skip(__func__, 2, optional);
		}

		// don't test that fi->fh is set as this is optional

	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

// Expecting 4 calls to this method - '.', '..', 'new_file_1.txt' and 'new_file_2.txt'
bool filled_file_cur = FALSE;
bool filled_file_parent = FALSE;
const char * new_file_1 = "/new_file_1.txt";
bool filled_new_file_1 = FALSE;
const char * new_file_2 = "/new_file_2.txt";
bool filled_new_file_2 = FALSE;
bool executing_readdir = FALSE;
bool bad_increment = FALSE;
int files_filled = 0;
int filler(void * buf, const char * name, const struct stat * stbuf, off_t off);
int filler(void * buf, const char * name, const struct stat * stbuf, off_t off) {
	/*
	 * buf: the buffer passed to the readdir() operation
	 * name: the file name of the directory entry
	 * stat: file attributes, can be NULL
	 * off: offset of the next entry or zero
	 */
	(void) buf;
	(void) stbuf;
	
	if(executing_readdir) {
		printf("Directory contains file '%s' and offset is %td\n", name, off);

		// Make sure offsets count upwards with each filler call, starting from 1
		/*if(!is_skipped("test_readdir", 2)) {
			if(off-1 != files_filled) {
				test_fail("test_readdir", 2, FALSE, "Call filler with incrementing offsets, starting from 1. It currently returned %d instead of %d, on iteration %d.", off, files_filled+1, files_filled);
				bad_increment = TRUE;
			}
		}*/

		// Check for each of the 4 files we expect
		if(strcmp(name, ".") == 0) {
			filled_file_cur = TRUE;
		} else if(strcmp(name, "..") == 0) {
			filled_file_parent = TRUE;
		} else if(strcmp(name, "new_file_1.txt") == 0) {
			filled_new_file_1 = TRUE;
		} else if(strcmp(name, "new_file_2.txt") == 0) {
			filled_new_file_2 = TRUE;
		}

		files_filled++;
	} else {
		fprintf(stderr, "filler function was called outside of readdir\n");
	}

	// 0 tells readdir to call filler with next entry
	return 0;
}

void test_readdir(const struct fuse_operations * op);
void test_readdir(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->readdir != NULL) {
		test_pass(__func__, 0, optional);

		// Open directory (which we know works) and try reading it
		if(!is_skipped(__func__, 1)) {

			// Create some files which we expect to be listed
			mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
			struct fuse_file_info * fi = malloc(sizeof(struct fuse_file_info));
			fi->flags = O_RDWR | O_CREAT;
			int retval = op->create(new_file_1, mode, fi);
			free(fi);
			fi = malloc(sizeof(struct fuse_file_info));
			fi->flags = O_RDWR | O_CREAT;
			retval = op->create(new_file_2, mode, fi);
			free(fi);

			// Open directory, which may optionally set fi->fh
			fi = malloc(sizeof(struct fuse_file_info));
			retval = op->opendir("/", fi);

			// Try reading the directory, which should callback our function called filler
			executing_readdir = TRUE;
			retval = op->readdir("/", NULL, filler, 0, fi);
			executing_readdir = FALSE;
			if(retval == 0) {
				test_pass(__func__, 1, optional);
			} else {
				test_fail(__func__, 1, optional, "Return 0 if the directory exists and the process has permission to open it. It currently returns %d when called with the root path (\"/\"), but the filesystem should at least have read permissions.", retval);
			}

			// Test 2 is failed inside filler callback. The callback never knows when it's passed, so we do the check here.
			if(is_skipped(__func__, 2)) {
				test_skip(__func__, 2, optional);
			} else if(!bad_increment) {
				test_pass(__func__, 2, optional);
			}

			// Check that each of the 4 expected files exist
			if(!is_skipped(__func__, 3)) {
				if(filled_file_cur) {
					test_pass(__func__, 3, optional);
				} else {
					test_fail(__func__, 3, optional, "Invoke filler callback with file for the current directory (\".\").");
				}
			} else {
				test_skip(__func__, 3, optional);
			}

			if(!is_skipped(__func__, 4)) {
				if(filled_file_parent) {
					test_pass(__func__, 4, optional);
				} else {
					test_fail(__func__, 4, optional, "Invoke filler callback with file for the parent directory (\"..\").");
				}
			} else {
				test_skip(__func__, 4, optional);
			}

			if(!is_skipped(__func__, 5)) {
				if(filled_new_file_1) {
					test_pass(__func__, 5, optional);
				} else {
					test_fail(__func__, 5, optional, "Invoke filler callback with one of the files just created (\"%s\").", new_file_1);
				}
			} else {
				test_skip(__func__, 5, optional);
			}

			if(!is_skipped(__func__, 6)) {
				if(filled_new_file_2) {
					test_pass(__func__, 6, optional);
				} else {
					test_fail(__func__, 6, optional, "Invoke filler callback with one of the files just created (\"%s\").", new_file_2);
				}
			} else {
				test_skip(__func__, 6, optional);
			}

			if(!is_skipped(__func__, 7)) {
				if(files_filled == 4) {
					test_pass(__func__, 7, optional);
				} else {
					test_fail(__func__, 7, optional, "Filler callback needs to be invoked exactly 4 times, for each of the 4 files that should exist in the root directory (\".\", \"..\", \"%s\", \"%s\"). It was invoked %d times.", new_file_1, new_file_2, files_filled);
				}
			} else {
				test_skip(__func__, 7, optional);
			}

			op->unlink(new_file_1);
			op->unlink(new_file_2);
			free(fi);
		}

	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_releasedir(const struct fuse_operations * op);
void test_releasedir(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->releasedir != NULL) {
		test_pass(__func__, 0, optional);
		// nothing else to test, as this modifies internal state of the developers filesystem
	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_open(const struct fuse_operations * op);
void test_open(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = true;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->open != NULL) {
		test_pass(__func__, 0, optional);

		// We know create works so create a file and test that we can open it
		if(!is_skipped(__func__, 1)) {
			mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
			const char * new_file = "/new_file.txt";
			struct fuse_file_info * fi = malloc(sizeof(struct fuse_file_info));
			fi->flags = O_RDWR | O_CREAT;
			int retval = op->create(new_file, mode, fi);

			if(retval >= 0) {
				test_pass("test_create", 2, optional);

				// Open the file
				fi->flags = O_RDWR;
				retval = op->open(new_file, fi);
				if(retval == 0) {
					test_pass(__func__, 1, optional);
				} else {
					test_fail(__func__, 1, optional, "Return 0 if the file exists and the user has permission to open it. It currently returns %d when trying to open a file just created: \"%s\".", retval, new_file);
				}

				// Delete the file
				retval = op->unlink(new_file);
				if(retval == 0) {
					test_pass("test_unlink", 4, optional);
				} else {
					test_fail("test_unlink", 4, optional, "Return 0 if the file was successfully removed. It currently returns %d when trying to remove a file just created: \"%s\".", retval, new_file);
				}
			} else {
				test_fail("test_create", 2, optional, "Return a non-negative number if the file was created successfully. It currently returns %d when trying to create the file: \"%s\".", retval, new_file);
			}
		} else {
			test_skip(__func__, 1, optional);
		}

		// Shouldn't be able to open a nonexistent file
		if(!is_skipped(__func__, 2)) {
			struct fuse_file_info * fi = malloc(sizeof(struct fuse_file_info));
			fi->flags = O_RDWR;
			int retval = op->open(non_existent_file, fi);
			if(retval == 0) {
				test_fail(__func__, 2, optional, "Return a negative number if a file cannot be opened. It currently returns %d when called with a non-existent path (\"%s\").", retval, non_existent_file);
			} else {
				test_pass(__func__, 2, optional);
			}
			free(fi);
		} else {
			test_skip(__func__, 2, optional);
		}

	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_read_and_write(const struct fuse_operations * op);
void test_read_and_write(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped("test_write", 0)) return test_skip("test_write", 0, optional);

	if(op->write != NULL) {
		test_pass("test_write", 0, optional);

		// Create a test file
		if(!is_skipped("test_create", 3)) {
			mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
			const char * new_file = "/new_file.txt";
			struct fuse_file_info * fi = malloc(sizeof(struct fuse_file_info));
			fi->flags = O_RDWR | O_CREAT;
			int retval = op->create(new_file, mode, fi);

			if(retval >= 0) {
				test_pass("test_create", 3, optional);

				// Open the file
				//struct fuse_file_info * fi = malloc(sizeof(struct fuse_file_info));
				//fi->flags = O_RDWR;
				retval = op->open(new_file, fi);
				if(retval == 0) {
					test_pass("test_open", 2, optional);

					// Write to the file
					if(!is_skipped("test_write", 2)) {
						const char * data = "this is a test string";
						size_t data_len = strlen(data);
						retval = op->write(new_file, data, data_len, 0, fi);
						if(retval == data_len) {
							test_pass("test_write", 2, optional);
						} else {
							test_fail("test_write", 2, optional, "Write should return the number of bytes written except on error, so %d (not %d) when writing \"%s\" to \"%s\").", data_len, retval, data, new_file);
						}

						// Try reading the written data
						if(!is_skipped("test_read", 0)) {
							if(op->read != NULL) {
								test_pass("test_read", 0, optional);

								if(!is_skipped("test_read", 1)) {
									char data_read[1024];
									retval = op->read(new_file, data_read, sizeof(data_read), 0, fi);
									if(retval == data_len) {
										test_pass("test_read", 1, optional);

										if(!is_skipped("test_read", 2)) {
											if(strcmp(data, data_read) == 0) {
												test_pass("test_read", 2, optional);
											} else {
												test_fail("test_read", 2, optional, "Read should have filled the buffer with \"%s\" as this was the data written to \"%s\").", data, new_file);
											}
										} else {
											test_skip("test_read", 2, optional);
										}
									} else {
										test_fail("test_read", 1, optional, "Read should return the number of bytes read except on error, so %d (not %d) when reading \"%s\" from \"%s\").", data_len, retval, data, new_file);
									}
								} else {
									test_skip("test_read", 1, optional);
								}
							} else {
								test_fail("test_read", 0, optional, function_not_defined_err);
							}
						} else {
							test_skip("test_read", 0, optional);
						}
					} else {
						test_skip("test_write", 2, optional);
					}
				} else {
					test_fail("test_open", 2, optional, "Return 0 if the file exists and the user has permission to open it. It currently returns %d when trying to open a file just created: \"%s\".", retval, new_file);
				}

				// Delete the file
				retval = op->unlink(new_file);
				if(retval == 0) {
					test_pass("test_unlink", 5, optional);
				} else {
					test_fail("test_unlink", 5, optional, "Return 0 if the file was successfully removed. It currently returns %d when trying to remove a file just created: \"%s\".", retval, new_file);
				}
			} else {
				test_fail("test_create", 3, optional, "Return a non-negative number if the file was created successfully. It currently returns %d when trying to create the file: \"%s\".", retval, new_file);
			}
		} else {
			test_skip("test_write", 1, optional);
		}

	} else {
		test_fail("test_write", 0, optional, function_not_defined_err);
	}
}

void test_symlink(const struct fuse_operations * op);
void test_symlink(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->symlink != NULL) {
		test_pass(__func__, 0, optional);
		
		mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
		const char * new_file = "/new_file.txt";
		struct fuse_file_info * fi = malloc(sizeof(struct fuse_file_info));
		fi->flags = O_RDWR | O_CREAT;
		int retval = op->create(new_file, mode, fi);

		if(!is_skipped(__func__, 1)) {
			const char * new_link = "/new_link.txt";
			retval = op->symlink(new_file, new_link);
			if(retval == 0) {
				test_pass(__func__, 1, optional);
			} else {
				test_fail(__func__, 1, optional, "Symlink should return 0 when a symbolic link is created successfully (actual return value was %d)", retval);
			}
			op->unlink(new_link);
		} else {
			test_skip(__func__, 1, optional);
		}

		op->unlink(new_file);

		free(fi);
	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_link(const struct fuse_operations * op);
void test_link(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->link != NULL) {
		test_pass(__func__, 0, optional);
		
		mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
		const char * new_file = "/new_file.txt";
		struct fuse_file_info * fi = malloc(sizeof(struct fuse_file_info));
		fi->flags = O_RDWR | O_CREAT;
		int retval = op->create(new_file, mode, fi);

		if(!is_skipped(__func__, 1)) {
			const char * new_link = "/new_link.txt";
			retval = op->link(new_file, new_link);
			if(retval == 0) {
				test_pass(__func__, 1, optional);
			} else {
				test_fail(__func__, 1, optional, "Link should return 0 when a hard link is created successfully (actual return value was %d)", retval);
			}
			op->unlink(new_link);
		} else {
			test_skip(__func__, 1, optional);
		}

		op->unlink(new_file);

		free(fi);
	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_readlink(const struct fuse_operations * op);
void test_readlink(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->readlink != NULL) {
		test_pass(__func__, 0, optional);

		if(!is_skipped(__func__, 1)) {
			char buf[1024];
			int retval = readlink(non_existent_file, buf, sizeof(buf));
			if(retval == -1) {
				test_pass(__func__, 1, optional);
			} else {
				test_fail(__func__, 1, optional, "Return a negative number if a link cannot be opened. It currently returns %d when called with a non-existent path (\"%s\").", retval, non_existent_file);
			}
		} else {
			test_skip(__func__, 1, optional);
		}
	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_rename(const struct fuse_operations * op);
void test_rename(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->rename != NULL) {
		test_pass(__func__, 0, optional);
		
		mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
		const char * new_file = "/new_file.txt";
		const char * moved_file = "/moved_file.txt";
		struct fuse_file_info * fi = malloc(sizeof(struct fuse_file_info));
		fi->flags = O_RDWR | O_CREAT;
		int retval = op->create(new_file, mode, fi);

		if(!is_skipped(__func__, 1)) {
			const char * new_link = "/new_link.txt";
			retval = op->rename(new_file, moved_file);
			if(retval == 0) {
				test_pass(__func__, 1, optional);
			} else {
				test_fail(__func__, 1, optional, "Rename should return 0 when the file is renamed successfully (actual return value was %d)", retval);
			}
			op->unlink(new_link);
		} else {
			test_skip(__func__, 1, optional);
		}

		op->unlink(new_file);
		op->unlink(moved_file);

		free(fi);
	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_truncate(const struct fuse_operations * op);
void test_truncate(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->truncate != NULL) {
		test_pass(__func__, 0, optional);

		if(!is_skipped(__func__, 1)) {
			// Create and open the file
			mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
			const char * new_file = "/new_file.txt";
			struct fuse_file_info * fi = malloc(sizeof(struct fuse_file_info));
			fi->flags = O_RDWR | O_CREAT;
			op->create(new_file, mode, fi);
			op->open(new_file, fi);

			// Write some test data
			const char * data = "this is a test string";
			size_t data_len = strlen(data);
			op->write(new_file, data, data_len, 0, fi);

			// Truncate it
			int retval = op->truncate(new_file, 5);
			if(retval == 0) {
				test_pass(__func__, 1, optional);
			} else {
				test_fail(__func__, 1, optional, "Truncate should return 0 when the file is truncated successfully (actual return value was %d)", retval);
			}

			// Delete the file
			op->unlink(new_file);

		} else {
			test_skip(__func__, 1, optional);
		}

	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_statfs(const struct fuse_operations * op);
void test_statfs(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->statfs != NULL) {
		test_pass(__func__, 0, optional);
		if(!is_skipped(__func__, 1)) {
			struct statvfs * s = malloc(sizeof(*s));
			int retval = op->statfs("/", s);
			free(s);
			if(retval == 0) {
				test_pass(__func__, 1, optional);
			} else {
				test_fail(__func__, 1, optional, "Statfs should always return 0 when called with root path as it should never fail. It returned %d.", retval);
			}
		} else {
			test_skip(__func__, 1, optional);
		}
	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void test_utimens(const struct fuse_operations * op);
void test_utimens(const struct fuse_operations * op) {
	if(test_failed) return;
	bool optional = false;
	if(is_skipped(__func__, 0)) return test_skip(__func__, 0, optional);

	if(op->utimens != NULL) {
		test_pass(__func__, 0, optional);

		if(!is_skipped(__func__, 1)) {
			// Create and open a file
			mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
			const char * new_file = "/new_file.txt";
			struct fuse_file_info * fi = malloc(sizeof(struct fuse_file_info));
			fi->flags = O_RDWR | O_CREAT;
			op->create(new_file, mode, fi);
			op->open(new_file, fi);

			// Change the file access (0) and modification (1) times
			struct timespec tv[2];
			tv[0].tv_sec = 50;
			tv[0].tv_nsec = 50000000000L;
			tv[1].tv_sec = 20;
			tv[1].tv_nsec = 20000000000L;
			int retval = op->utimens(new_file, tv);
			if(retval == 0) {
				test_pass(__func__, 1, optional);
			} else {
				test_fail(__func__, 1, optional, "Function should return 0 when setting access and modification times for a file that exists. It returned %d.", retval);
			}
			
			// Delete the file
			op->unlink(new_file);

		} else {
			test_skip(__func__, 1, optional);
		}

	} else {
		test_fail(__func__, 0, optional, function_not_defined_err);
	}
}

void fwizard_init(const struct fuse_operations * op, size_t op_size);
void fwizard_init(const struct fuse_operations * op, size_t op_size) {
	printf("[libfuse] Called fwizard_init\n");
	(void) op_size;

	// Get string containing a list of skipped tests, and parse into an actual list
	skipped_tests = g_slist_alloc();
	char * skipped_tests_str = getenv("SKIPPED_TESTS");
	if(skipped_tests_str != NULL) {
		char * test = strtok(skipped_tests_str, ",");
		while(test != NULL) {
			char * test_cpy = malloc(strlen(test) + 1);
			strcpy(test_cpy, test);
			printf("[libfuse] Will skip %s\n", test_cpy);
			skipped_tests = g_slist_append(skipped_tests, test_cpy);
			test = strtok(NULL, ",");
		}

	}

	wizard_fifo_init();
	test_operations(op);

	// mount
	test_init(op);
	test_getattr(op);

	// cd to mount-point
	test_access(op);

	// rmdir - needed before mkdir
	test_rmdir(op);

	// mkdir
	test_mkdir(op);

	// unlink - needed before create
	test_unlink(op);
	
	// touch newfile
	test_create(op);
	test_fgetattr(op);
	test_flush(op);
	test_release(op);

	// ls
	test_opendir(op);
	test_readdir(op);
	test_releasedir(op);

	// reading and writing
	test_open(op);
	test_read_and_write(op);

	// links
	test_symlink(op);
	test_readlink(op);
	test_link(op);

	// misc
	test_rename(op);
	test_truncate(op);
	//test_statfs(op);
	test_utimens(op);

	tests_end();
	wizard_fifo_destroy();
}

