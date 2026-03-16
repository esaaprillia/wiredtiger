/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wiredtiger_config.h"
#include "wiredtiger_ext.h"
#include "wt_system.h"
#include "wt_compiler.h"
#include "wt_fwd.h"
#include "misc.h"
#include "os.h"
#include "error.h"
#include "session.h"
#include "connection.h"
#include "extern_noninline.h"
#ifdef _WIN32
#include "extern_win.h"
#else
#include "extern_posix.h"
#ifdef __linux__
#include "extern_linux.h"
#elif __APPLE__
#include "extern_darwin.h"
#endif
#endif

/*
 * __stdio_close --
 *     ANSI C close/fclose.
 */
static int
__stdio_close(WT_SESSION_IMPL *session, WT_FSTREAM *fs)
{
    WT_RET_MSG(session, ENOTSUP, "%s: close", fs->name);
}

/*
 * __stdio_flush --
 *     POSIX fflush.
 */
static int
__stdio_flush(WT_SESSION_IMPL *session, WT_FSTREAM *fs)
{
    if (fflush(fs->fp) == 0)
        return (0);
    WT_RET_MSG(session, __wt_errno(), "%s: flush", fs->name);
}

/*
 * __stdio_getline --
 *     ANSI C getline.
 */
static int
__stdio_getline(WT_SESSION_IMPL *session, WT_FSTREAM *fs, WT_ITEM *buf)
{
    WT_UNUSED(buf);
    WT_RET_MSG(session, ENOTSUP, "%s: getline", fs->name);
}

/*
 * __stdio_printf --
 *     ANSI C vfprintf.
 */
static int
__stdio_printf(WT_SESSION_IMPL *session, WT_FSTREAM *fs, const char *fmt, va_list ap)
{
    WT_UNUSED(session);

    if (vfprintf(fs->fp, fmt, ap) >= 0)
        return (0);
    return (EIO);
}

/*
 * __stdio_init --
 *     Initialize stdio functions.
 */
static void
__stdio_init(WT_FSTREAM *fs, const char *name, FILE *fp)
{
    fs->name = name;
    fs->fp = fp;

    fs->close = __stdio_close;
    fs->fstr_flush = __stdio_flush;
    fs->fstr_getline = __stdio_getline;
    fs->fstr_printf = __stdio_printf;
}

/*
 * __wt_os_stdio --
 *     Initialize the stdio configuration.
 */
void
__wt_os_stdio(WT_SESSION_IMPL *session)
{
    __stdio_init(WT_STDERR(session), "stderr", stderr);
    __stdio_init(WT_STDOUT(session), "stdout", stdout);
}
