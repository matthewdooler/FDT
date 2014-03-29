#!/usr/bin/env python
from pprint import pprint
import re
import json

# Generates a file containing function signatures, by parsing the libfuse/osxfuse C code
# The diagnostic tool uses this to inform users of function definitions
# Author: Matthew Dooler <dooler.matthew@gmail.com> (2014)

libfuse_dname = "libfuse"
osxfuse_dname = "osxfuse/fuse"
include_dname = "/include"
header_fname = "/fuse.h"
out_fname = "/fsigs.json"

def get_header_functions(fname, using_osxfuse):
	with open(fpath) as f:
		content = f.readlines()

		# Create a big string containing only function definitions
		# i.e., strip comments and whitespace, and try to handle the ifdefs
		# Some functions span multiple lines
		parsed_str = ""
		reading_struct = False
		reading_ifdef = False
		reading_apple_only = False
		reading_linux_only = False
		for line in content:

			# Remove "//" and "/** */" type comments
			line = re.sub(re.compile("//.*" ), "", line)
			line = re.sub(re.compile("/\*.*?\*/", re.DOTALL), "", line)

			# Remove leading and trailing whitespace
			line = line.strip()

			if len(line) == 0:
				continue
			else:
				if reading_struct:
					if line == "};":
						reading_struct = False
					else:
						# Handle ifdef macros
						if line[0] == "#":
							if reading_ifdef:
								if line == "#else":
									reading_apple_only = False
									reading_linux_only = True
								elif line == "#endif":
									reading_ifdef = False
							else:
								if line == "#ifdef __APPLE__":
									reading_ifdef = True
									reading_apple_only = True
									reading_linux_only = False
						else:
							if reading_ifdef:
								if reading_apple_only:
									# This line should only be used for osxfuse
									if using_osxfuse:
										parsed_str += line
								elif reading_linux_only:
									# This line should only be used for standard linux libfuse
									if not using_osxfuse:
										parsed_str += line
							else:
								# Line not inside an ifdef, so ok for any OS
								parsed_str += line
				else:
					if line == "struct fuse_operations {":
						reading_struct = True
		
		# Strip multi-line block comments
		parsed_str = re.sub(re.compile("/\*.*?\*/", re.DOTALL), "", parsed_str)

		# Strip remaining whitespace and organise into a list of functions
		parsed_str = re.sub(re.compile(", *" ), ",", parsed_str)
		parsed_str = re.sub(re.compile("\) *\(" ), ")(", parsed_str)
		function_lines = parsed_str.split(';')
		function_lines.pop()

		# Parse each function signature into an object
		functions = []
		for line in function_lines:
			# Match the return type, function name, and the parameters
			# This also makes sure it is a function pointer, and not just a normal struct value
			# int (*setchgtime)(const char *,const struct timespec *tv)
			matcher = re.search('(.*?)\s*\(\*(.*?)\)\((.*?)\)', line)
			if matcher is not None:
				rtype = matcher.group(1)
				name = matcher.group(2)
				params = matcher.group(3).split(",")
				function = { "name": name,
					 		 "rtype": rtype,
					 		 "params": params}
				functions.append(function)

		return functions

def export(fpath, functions):
	out_str = json.dumps(functions, indent=4)
	f = open(fpath + out_fname, "w+")
	f.write(out_str)
	f.close()

if __name__ == "__main__":
	# Linux libfuse
	fpath = libfuse_dname + include_dname + header_fname
	print "[siggen] Reading " + fpath
	functions = get_header_functions(fpath, False)
	export(libfuse_dname, functions)
	print "[siggen] Output " + str(len(functions)) + " function signatures to " + libfuse_dname + out_fname

	# Apple osxfuse
	fpath = osxfuse_dname + include_dname + header_fname
	print "[siggen] Reading " + fpath
	functions = get_header_functions(fpath, True)
	export(osxfuse_dname, functions)
	print "[siggen] Output " + str(len(functions)) + " function signatures to " + osxfuse_dname + out_fname