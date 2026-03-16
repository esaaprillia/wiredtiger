/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 * 	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * Clang and gcc use different mechanisms to detect TSan, clang using __has_feature. Consolidate
 * them into a single TSAN_BUILD pre-processor flag.
 */
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TSAN_BUILD 1
#endif
#endif

#if defined(__SANITIZE_THREAD__)
#define TSAN_BUILD 1
#endif

/*******************************************
 * Compiler-specific include files.
 *******************************************/
#if defined(__GNUC__)
#include "gcc.h"
#elif defined(_MSC_VER)
#include "msvc.h"
#endif

/*
 * GLIBC 2.26 and later use the openat syscall to implement open. Set this flag so that our strace
 * tests know to expect this.
 */
#ifdef __GLIBC_PREREQ
#if __GLIBC_PREREQ(2, 26)
#define WT_USE_OPENAT 1
#endif
#endif
