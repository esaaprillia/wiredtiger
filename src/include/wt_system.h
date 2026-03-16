/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 * 	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*******************************************
 * WiredTiger system include files.
 *******************************************/
#ifndef _WIN32
#include <sys/mman.h>
#endif
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/time.h>
#include <sys/uio.h>
#endif
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <inttypes.h>
#ifdef _WIN32
#include <io.h>
#endif
#include <limits.h>
#ifdef _WIN32
#include <process.h>
#else
#include <pthread.h>
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
