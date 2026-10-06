// ps5-homebrew-ui - Entry points the vendored archives call but the console
// libc does not export.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Statically linked FFmpeg and libiconv were built against a POSIX libc; the
// console's is a FreeBSD/MSVC hybrid, whose stubs are what the app links
// against (see .deps/native/ps5-payload-sdk/target/lib/libc_stub_weak.so,
// which answers gmtime, localtime, time, tzset and __mb_cur_max but not the
// three below). src/runtime/ is console-only: the PC snapshot host links the
// real libc and skips this file.

#include <errno.h>
#include <langinfo.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>

/* FFmpeg's timestamp parsing and its muxers ask for the POSIX localtime_r(),
 * which the console names localtime_s(). localtime() is exported, and the
 * only caller is the single decode worker, so copying its result out keeps
 * the contract without needing a second timezone implementation here. */
struct tm *localtime_r(const time_t *when, struct tm *result)
{
    if (when == NULL || result == NULL)
    {
        errno = EINVAL;
        return NULL;
    }
    const struct tm *local = localtime(when);
    if (local == NULL)
        return NULL;
    *result = *local;
    return result;
}

/* libiconv asks the locale which codeset it speaks. A payload never loads a
 * locale database, and every byte this app hands to iconv - the API's JSON,
 * its track titles and its subtitles - is UTF-8. */
char *nl_langinfo(nl_item item)
{
    if (item != CODESET)
    {
        errno = EINVAL;
        return NULL;
    }
    return "UTF-8";
}

/* iconv's wide-character loops read _MB_CUR_MAX, which the console spells as
 * the function ___mb_cur_max(). UTF-8 needs four bytes for its longest code
 * point; answering 1 would make those loops refuse every multi-byte sequence.
 * No standard C or wide-character call in this app depends on the value. */
int ___mb_cur_max(void)
{
    return 4;
}
