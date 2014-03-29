FDT: FUSE Diagnostic Tool
===

Development toolkit for writing FUSE filesystems, providing fully automated testing and advanced debugging facilities.

Features
---
- Interactive assistance during filesystem development
- Easily extensible test suite
- Displays API calls in real-time
- Graphical user interface
- Compatible with Mac OS X and Linux

How it works
---
The tool intercepts FUSE API calls at the userspace level by wrapping the existing FUSE library.

Mounting a FUSE filesystem with the tool will use our modified library, allowing artificial calls to be made against the filesystem for testing and providing visualisation and control over execution of real functions.

It runs on Linux using the standard libfuse library, and for Mac OS X compatibility it uses osxfuse.

Getting started
---
Running ./fdt will open the application GUI. Refer to the User Guide for details on how to use it.

The tool can be compiled from source by running make in the main directory.

On Linux, the libfuse wrapper can be compiled by running make in the libfuse directory.

On Mac, the osxfuse wrapper can be compiled by running sudo ./build.sh -t dist in the osxfuse directory.
