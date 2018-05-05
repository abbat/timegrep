/**
 * Copyright (c) 2017-2018 Anton Batenev
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define _GNU_SOURCE

#ifdef __linux__
    #include <features.h>
#endif

#include <pcre.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <libintl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>

/**
 * Program version for --version, -v
 */
static const char* TG_VERSION = "0.4alpha";

/**
 * Chunk size for IO in bytes
 */
static const size_t TG_CHUNK_SIZE = 512 * 1024;

/**
 * Use TG_TIMEZONE instead glibc timezone external variable
 * to compile on FreeBSD and other "non linux"
 */
static time_t TG_TIMEZONE = 0;

/**
 * Error codes
 */
static const int TG_FOUND     = 2;    // something found
static const int TG_NOT_FOUND = 1;    // nothing found
static const int TG_NULL      = 0;    // undetermined result
static const int TG_ERROR     = -1;   // unrecoverable error

/**
 * Predefined datetime formats
 */
static const struct {
    const char* name;     // format name
    const char* alias;    // format alias
    const char* format;   // datetime format (see strptime)
} TG_FORMATS[] = {
    {
        "default",
        NULL,
        "%Y-%m-%d %H:%M:%S"
    },
    {
        "iso",
        NULL,
        "%Y-%m-%dT%H:%M:%S%z"
    },
    {
        "common",
        NULL,
        "%d/%b/%Y:%H:%M:%S %z"
    },
    {
        "syslog",
        NULL,
        "%b %d %H:%M:%S"
    },
    {
        "tskv",
        NULL,
        "unixtime=%s"
    },
    { "apache", "common", NULL },
    { "nginx",  "common", NULL },
    { NULL,     NULL,     NULL }
};

/**
 * strptime_regex named subexpressions
 * has_* is a parsable named expressions
 * fallback is a flag to force use tg_strptime
 */
typedef struct {
    int has_year;
    int has_month;
    int has_month_t;
    int has_day;
    int has_hour;
    int has_minute;
    int has_second;
    int has_timezone;
    int has_timestamp;
    int fallback;
} pcre_extra_opt;

/**
 * pcre context to search and parse datetime
 */
typedef struct {
    pcre*          re;         // compiled regular expression for datetime
    pcre_extra*    re_extra;   // optimized regular expression for datetime
    pcre_extra_opt re_opt;     // named regular expressions indexes or fallback flag
    const char*    format;     // datetime format
} pcre_context;

/**
 * working context
 */
typedef struct {
    const char*    filename;   // current filename
    int            fd;         // file descriptor
    size_t         size;       // size of file / mapped memory
    char*          data;       // mapped memory
    time_t         start;      // timestamp from search
    time_t         stop;       // timestamp to search
    pcre_context   re_ctx;     // pcre context to search and parse datetime
} tg_context;

/**
 * Print program name and version
 */
static void print_version()
{
    printf("timegrep %s\n", TG_VERSION);
}

/**
 * Print usage
 */
static void print_help()
{
    size_t i;
    size_t l;
    int    m;

    printf(gettext(
        "\n"
        "Usage:\n"
        "   timegrep [options] [files]\n"
        "\n"
        "Options:\n"
        "   --format,  -e -- datetime format (default: 'default')\n"
        "   --start,   -f -- datetime to start search (default: now)\n"
        "   --stop,    -t -- datetime to stop search (default: now)\n"
        "   --seconds, -s -- seconds to substract from --start (default: 0)\n"
        "   --minutes, -m -- minutes to substract from --start (default: 0)\n"
        "   --hours,   -h -- hours to substract from --start (default: 0)\n"
        "   --version, -v -- print program version and exit\n"
        "   --help,    -? -- print this help message\n"
        "\n"
        "Formats:\n"
    ));

    m = 0;
    i = 0;
    while (TG_FORMATS[i].name != NULL) {
        l = strlen(TG_FORMATS[i].name);
        if (l > m)
            m = l;
        i++;
    }

    i = 0;
    while (TG_FORMATS[i].name != NULL) {
        if (TG_FORMATS[i].alias != NULL)
            printf("   %-*s -- %s '%s'\n", m, TG_FORMATS[i].name, gettext("alias for"), TG_FORMATS[i].alias);
        else
            printf("   %-*s -- %s\n", m, TG_FORMATS[i].name, TG_FORMATS[i].format);
        i++;
    }

    printf(gettext(
        "\n"
        "See strptime(3) for format details\n"
        "\n"
    ));
}

/**
 * Set global TG_TIMEZONE variable,
 * so local time = GMT time + TG_TIMEZONE
 */
static void set_timezone()
{
    time_t    t;
    struct tm tm;

    tzset();

    t = time(NULL);

    localtime_r(&t, &tm);

    TG_TIMEZONE = tm.tm_gmtoff;
}

/**
 * Parse time unit interval from string and combine with multipler
 * Return parsed value in seconds on success
 * Return LONG_MIN on error or invalid interval
 */
static long int parse_interval(const char* string, long int multipler)
{
    long int value;

    value = strtol(string, NULL, 10);
    if (value < 0 || value == LONG_MAX) {
        errno = ERANGE;
        return LONG_MIN;
    }

    return value * multipler;
}

/**
 * Parse datetime format to pcre regular expression
 * regex may be NULL to found result regex length
 * Return length of result regex on success
 * Retrun TG_ERROR on error
 */
static size_t strptime_regex(const char* format, char* regex, pcre_extra_opt* re_opt)
{
    char        c;
    size_t      format_index;
    size_t      format_length;
    size_t      regex_index;
    const char* escape;
    size_t      escape_length;
    const char* part;
    size_t      part_length;

    escape        = "^$|()[]{}.*+?\\";
    escape_length = strlen(escape);
    format_length = strlen(format);
    regex_index   = 0;

    for (format_index = 0; format_index < format_length; format_index++) {
        c = format[format_index];
        if (c == '%') {
            if (format_index + 1 == format_length) {
                errno = 0;
                fprintf(stderr, gettext("%s Unexpected format char '%%' at end of format string\n"), gettext("ERROR:"));
                return TG_ERROR;
            }

            c = format[format_index + 1];
            format_index++;

            // http://www.pcre.org/original/doc/html/pcresyntax.html#SEC1
            switch (c) {
                // The % character
                case '%':
                    part = "%";
                    break;

                // The weekday name according to the current locale, in abbreviated form or the full name.
                // timegrep use only English forms as all world do
                case 'a':
                case 'A':
                    part = "(Mon|Monday|Tue|Tuesday|Wed|Wednesday|Thu|Thursday|Fri|Friday|Sat|Saturday|Sun|Sunday)";
                    re_opt->fallback = 1;
                    break;

                // The month name according to the current locale, in abbreviated form or the full name
                // timegrep use only English forms as all world do
                case 'b':
                case 'B':
                case 'h':
                    part = "(?P<month_t>Jan|January|Feb|February|Mar|March|Apr|April|May|Jun|June|Jul|July|Aug|August|Sep|September|Oct|October|Nov|November|Dec|December)";
                    re_opt->has_month_t++;
                    break;

                // The date and time representation for the current locale
                // timegrep use heuristic here
                case 'c':
                    part         = NULL;
                    regex_index += strptime_regex("%x %X", (regex == NULL ? NULL : &regex[regex_index]), re_opt);
                    break;

                // The century number (0-99)
                case 'C':
                    part = "\\d{1,2}";
                    re_opt->fallback = 1;
                    break;

                // The day of month (1-31)
                case 'd':
                case 'e':
                    part = "(?P<day>[1-2][0-9]|3[0-1]|0?[1-9])";
                    re_opt->has_day++;
                    break;

                // Equivalent to %m/%d/%y (American style date)
                case 'D':
                    part         = NULL;
                    regex_index += strptime_regex("%m/%d/%y", (regex == NULL ? NULL : &regex[regex_index]), re_opt);
                    break;

                // The hour (0-23)
                case 'H':
                    part = "(?P<hour>1[0-9]|2[0-3]|0?[0-9])";
                    re_opt->has_hour++;
                    break;

                // The hour on a 12-hour clock (1-12)
                case 'I':
                    part = "(1[0-2]|0?[1-9])";
                    re_opt->fallback = 1;
                    break;

                // The day number in the year (1-366)
                case 'j':
                    part = "([1-2][0-9][0-9]|3[0-5][0-9]|36[0-6]|0?[1-9][0-9]|0{0,2}[1-9])";
                    re_opt->fallback = 1;
                    break;

                // The month number (1-12)
                case 'm':
                    part = "(?P<month>1[0-2]|0?[1-9])";
                    re_opt->has_month++;
                    break;

                // The minute (0-59)
                case 'M':
                    part = "(?P<minute>[1-5][0-9]|0?[0-9])";
                    re_opt->has_minute++;
                    break;

                // Arbitrary whitespace
                case 'n':
                case 't':
                    part = "\\s";
                    break;

                // The locale's equivalent of AM or PM
                // timegrep use only English forms as all world do
                case 'p':
                    part = "(AM|PM)";
                    re_opt->fallback = 1;
                    break;

                // The 12-hour clock time (using the locale's AM or PM), Equivalent to %I:%M:%S %p
                case 'r':
                    part         = NULL;
                    regex_index += strptime_regex("%I:%M:%S %p", (regex == NULL ? NULL : &regex[regex_index]), re_opt);
                    break;

                // Equivalent to %H:%M
                case 'R':
                    part         = NULL;
                    regex_index += strptime_regex("%H:%M", (regex == NULL ? NULL : &regex[regex_index]), re_opt);
                    break;

                // The second (0-60)
                case 'S':
                    part = "(?P<second>[1-5][0-9]|60|0?[0-9])";
                    re_opt->has_second++;
                    break;

                // Equivalent to %H:%M:%S
                case 'T':
                    part         = NULL;
                    regex_index += strptime_regex("%H:%M:%S", (regex == NULL ? NULL : &regex[regex_index]), re_opt);
                    break;

                // The week number with Sunday the first day of the week (0-53)
                case 'U':
                // The week number with Monday the first day of the week (0-53)
                case 'W':
                    part = "([1-4][0-9]|5[0-3]|0?[0-9])";
                    re_opt->fallback = 1;
                    break;

                // The weekday number (0-6) with Sunday = 0
                case 'w':
                    part = "[0-6]";
                    re_opt->fallback = 1;
                    break;

                // The date, using the locale's date format
                // timegrep use %Y-%m-%d (also known as %F)
                case 'x':
                    part         = NULL;
                    regex_index += strptime_regex("%Y-%m-%d", (regex == NULL ? NULL : &regex[regex_index]), re_opt);
                    break;

                // The time, using the locale's time format
                // timegrep use %H:%M:%S (also known as %T)
                case 'X':
                    part         = NULL;
                    regex_index += strptime_regex("%H:%M:%S", (regex == NULL ? NULL : &regex[regex_index]), re_opt);
                    break;

                // The year within century (0-99)
                case 'y':
                    part = "\\d{1,2}";
                    re_opt->fallback = 1;
                    break;

                // The year, including century (for example, 1991)
                case 'Y':
                    part = "(?P<year>\\d{4})";
                    re_opt->has_year++;
                    break;

                // E or O modifier characters to indicate that an alternative format or specification
                // not supported by timegrep
                case 'O':
                case 'E':
                    errno = 0;
                    fprintf(stderr, gettext("%s 'O' and 'E' modifiers not supported by timegrep\n"), gettext("ERROR:"));
                    return TG_ERROR;

                //
                // Glibc
                //

                // Equivalent to %Y-%m-%d
                case 'F':
                    part         = NULL;
                    regex_index += strptime_regex("%Y-%m-%d", (regex == NULL ? NULL : &regex[regex_index]), re_opt);
                    break;

                // The year corresponding to the ISO week number, but without the century (0-99)
                case 'g':
                    part = "\\d{1,2}";
                    re_opt->fallback = 1;
                    break;

                // The year corresponding to the ISO week number
                case 'G':
                    part = "\\d{4}";
                    re_opt->fallback = 1;
                    break;

                // The day of the week as a decimal number (1-7, where Monday = 1)
                case 'u':
                    part = "[1-7]";
                    re_opt->fallback = 1;
                    break;

                // The ISO 8601:1988 week number as a decimal number (1-53)
                case 'V':
                    part = "([1-4][0-9]|5[0-3]|0?[1-9])";
                    re_opt->fallback = 1;
                    break;

                // An RFC-822/ISO 8601 standard timezone specification
                // https://tools.ietf.org/html/rfc822#section-5
                case 'z':
                    part = "(?P<timezone>((\\+|\\-)\\d{2}:?\\d{2})|UT|UTC|GMT|EST|EDT|CST|CDT|MST|MDT|PST|PDT|[A-Z])";
                    re_opt->has_timezone++;
                    break;

                // The timezone name
                // PST8PDT, America
                // Etc/UTC, Etc/GMT+2, Etc/GMT-3, Asia/Kuala_Lumpur, America/Port-au-Prince
                // America/Argentina/Rio_Gallegos, America/Argentina/ComodRivadavia
                case 'Z':
                    // FIXME: if you feel a power
                    part = "[A-Za-z0-9_\\+\\-/]{3,33}";
                    re_opt->fallback = 1;
                    re_opt->has_timezone++;
                    break;

                // The number of seconds since the Epoch
                case 's':
                    part = "(?P<timestamp>\\d{1,20})";
                    re_opt->has_timestamp++;
                    break;

                default:
                    errno = 0;
                    fprintf(stderr, gettext("%s Unexpected format char '%c'\n"), gettext("ERROR:"), c);
                    return TG_ERROR;
            }

            if (part != NULL) {
                part_length = strlen(part);
                if (regex != NULL)
                    memcpy(&regex[regex_index], part, part_length);

                regex_index += part_length;

            } else if (regex_index == TG_ERROR)
                return TG_ERROR;

        } else if (memchr(escape, c, escape_length) != NULL) {
            if (regex != NULL) {
                regex[regex_index]     = '\\';
                regex[regex_index + 1] = c;
            }

            regex_index += 2;

        } else {
            if (regex != NULL)
                regex[regex_index] = c;

            regex_index++;
        }
    }

    if (
        re_opt->has_year      > 1 ||
        re_opt->has_month     > 1 ||
        re_opt->has_month_t   > 1 ||
        re_opt->has_day       > 1 ||
        re_opt->has_hour      > 1 ||
        re_opt->has_minute    > 1 ||
        re_opt->has_second    > 1 ||
        re_opt->has_timezone  > 1 ||
        re_opt->has_timestamp > 1 ||
        (re_opt->has_month
            + re_opt->has_month_t) > 1 ||
        (re_opt->has_timestamp > 0 && (
            re_opt->has_year    +
            re_opt->has_month   +
            re_opt->has_month_t +
            re_opt->has_day     +
            re_opt->has_hour    +
            re_opt->has_minute  +
            re_opt->has_second
        ) > 1)
    )
        re_opt->fallback = 1;

    return regex_index;
}

/**
 * Convert a string representation of datetime to a time_t like strptime
 * Return TG_FOUND on success
 * Return TG_NOT_FOUND if nothing found or convert error
 */
static int tg_strptime(
    const char* string,      // source string
    const char* format,      // datetime format (see strptime)
    int         format_tz,   // datetime format use timezone information
    time_t*     timestamp    // result timestamp
)
{
    struct tm tm;
    long int  tm_gmtoff;

    memset(&tm, 0, sizeof(struct tm));
    if (strptime(string, format, &tm) == NULL)
        return TG_NOT_FOUND;

    if (format_tz == 0)
        tm_gmtoff = TG_TIMEZONE;
    else
        tm_gmtoff = tm.tm_gmtoff;

    *timestamp = timegm(&tm) - tm_gmtoff;
    if ((*timestamp) == -1)
        return TG_NOT_FOUND;

    return TG_FOUND;
}

/**
 * Convert a string representation of integer to a int like atoi
 * Return TG_ERROR on error
 */
static int tg_atoi(const char* buffer)
{
    long int result;

    result = strtol(buffer, NULL, 10);
    if (result < 0 || result >= INT_MAX)
        return TG_ERROR;

    return (int)result;
}

/**
 * Convert a string representation of month name to a int
 * Month name buffer MUST be valid month name (strptime_regex ensure it)
 * Return TG_ERROR on error
 */
static int tg_atom(const char* buffer, int length)
{
    if (length < 3)
        return TG_ERROR;

    switch (buffer[0]) {
        // Apr
        // Aug
        case 'A':
            return (buffer[1] == 'p' ? 3 : 7);
        // Dec
        case 'D':
            return 11;
        // Feb
        case 'F':
            return 1;
        // Jan
        // Jul
        // Jun
        case 'J':
            return (buffer[1] == 'a' ? 0 : (buffer[2] == 'n' ? 5 : 6));
        // Mar
        // May
        case 'M':
            return (buffer[2] == 'r' ? 2 : 4);
        // Nov
        case 'N':
            return 10;
        // Oct
        case 'O':
            return 9;
        // Sep
        case 'S':
            return 8;
    }

    return TG_ERROR;
}

/**
 * Convert a string representation of timezone name to tm_gmtoff
 * Timezone buffer MUST be valid timezone (strptime_regex ensure it)
 * Return LONG_MIN on error
 */
static long int atogmtoff(const char* buffer, int length)
{
    long int result;

    if (length == 5) {
        // +0000
        result = (((buffer[1] - '0') * 10) + (buffer[2] - '0')) * 60 * 60 + (((buffer[3] - '0') * 10) + (buffer[4] - '0')) * 60;
        if (buffer[0] == '-')
            result = -result;
    } else if (length == 6) {
        // +00:00
        result = (((buffer[1] - '0') * 10) + (buffer[2] - '0')) * 60 * 60 + (((buffer[4] - '0') * 10) + (buffer[5] - '0')) * 60;
        if (buffer[0] == '-')
            result = -result;
    } else if (length == 1) {
        // military
        switch (buffer[0]) {
            case 'A': result = -1;  break;
            case 'B': result = -2;  break;
            case 'C': result = -3;  break;
            case 'D': result = -4;  break;
            case 'E': result = -5;  break;
            case 'F': result = -6;  break;
            case 'G': result = -7;  break;
            case 'H': result = -8;  break;
            case 'I': result = -9;  break;
            case 'K': result = -10; break;
            case 'L': result = -11; break;
            case 'M': result = -12; break;
            case 'N': result = 1;   break;
            case 'O': result = 2;   break;
            case 'P': result = 3;   break;
            case 'Q': result = 4;   break;
            case 'R': result = 5;   break;
            case 'S': result = 6;   break;
            case 'T': result = 7;   break;
            case 'U': result = 8;   break;
            case 'V': result = 9;   break;
            case 'W': result = 10;  break;
            case 'X': result = 11;  break;
            case 'Y': result = 12;  break;
            case 'Z': result = 0;   break;
            default:
                return LONG_MIN;
        }

        result = result * 60 * 60;
    } else if (length >= 2) {
        switch (buffer[0]) {
            // UT, UTC, GMT
            case 'U':
            case 'G':
                result = 0;
                break;
            // EST, EDT
            case 'E':
                result = (buffer[1] == 'S' ? -5 : -4);
                break;
            // CST, CDT
            case 'C':
                result = (buffer[1] == 'S' ? -6 : -5);
                break;
            // MST, MDT
            case 'M':
                result = (buffer[1] == 'S' ? -7 : -6);
                break;
            // PST, PDT
            case 'P':
                result = (buffer[1] == 'S' ? -8 : -7);
                break;
            default:
                return LONG_MIN;
        }

        result = result * 60 * 60;
    } else
        return LONG_MIN;

    return result;
}

/**
 * Convert a string representation of datetime to a time_t like strptime using precompiled named regex
 * Return TG_FOUND on success
 * Return TG_NOT_FOUND if nothing found or convert error
 * Return TG_ERROR on error - so we need fallback
 */
static int tg_strptime_re(
    const char*           string,     // source string (pcre_exec subject)
    int*                  matches,    // offset vector that pcre_exec used
    int                   count,      // value returned by pcre_exec
    const pcre_extra_opt* re_opt,     // named regular expressions indexes or fallback flag
    time_t*               timestamp   // result timestamp
)
{
    int       result;
    struct tm tm;
    long int  tm_gmtoff;
    char      buffer[35];

    memset(&tm, 0, sizeof(tm));

    if (re_opt->has_year >= 0) {
        result = pcre_copy_substring(string, matches, count, re_opt->has_year, buffer, sizeof(buffer));
        if (result < 0)
            return TG_ERROR;

        tm.tm_year = tg_atoi(buffer) - 1900;
    }

    if (re_opt->has_month >= 0) {
        result = pcre_copy_substring(string, matches, count, re_opt->has_month, buffer, sizeof(buffer));
        if (result < 0)
            return TG_ERROR;

        tm.tm_mon = tg_atoi(buffer) - 1;
    }

    if (re_opt->has_month_t >= 0) {
        result = pcre_copy_substring(string, matches, count, re_opt->has_month_t, buffer, sizeof(buffer));
        if (result < 0)
            return TG_ERROR;

        tm.tm_mon = tg_atom(buffer, result);
    }

    if (re_opt->has_day >= 0) {
        result = pcre_copy_substring(string, matches, count, re_opt->has_day, buffer, sizeof(buffer));
        if (result < 0)
            return TG_ERROR;

        tm.tm_mday = tg_atoi(buffer);
    }

    if (re_opt->has_hour >= 0) {
        result = pcre_copy_substring(string, matches, count, re_opt->has_hour, buffer, sizeof(buffer));
        if (result < 0)
            return TG_ERROR;

        tm.tm_hour = tg_atoi(buffer);
    }

    if (re_opt->has_minute >= 0) {
        result = pcre_copy_substring(string, matches, count, re_opt->has_minute, buffer, sizeof(buffer));
        if (result < 0)
            return TG_ERROR;

        tm.tm_min = tg_atoi(buffer);
    }

    if (re_opt->has_second >= 0) {
        result = pcre_copy_substring(string, matches, count, re_opt->has_second, buffer, sizeof(buffer));
        if (result < 0)
            return TG_ERROR;

        tm.tm_sec = tg_atoi(buffer);
    }

    if (re_opt->has_timezone >= 0) {
        result = pcre_copy_substring(string, matches, count, re_opt->has_timezone, buffer, sizeof(buffer));
        if (result < 0)
            return TG_ERROR;

       tm.tm_gmtoff = atogmtoff(buffer, result);
    }

    if (re_opt->has_timestamp >= 0) {
        result = pcre_copy_substring(string, matches, count, re_opt->has_timestamp, buffer, sizeof(buffer));
        if (result < 0)
            return TG_ERROR;

        *timestamp = (time_t)strtoll(buffer, NULL, 10);

        return TG_FOUND;
    }

    if (re_opt->has_timezone == 0)
        tm_gmtoff = TG_TIMEZONE;
    else
        tm_gmtoff = tm.tm_gmtoff;

    *timestamp = timegm(&tm) - tm_gmtoff;
    if ((*timestamp) == -1)
        return TG_NOT_FOUND;

    return TG_FOUND;
}

/**
 * Convert a string representation of datetime to a time_t
 * based on heuristic of short human input
 * Return TG_FOUND on success
 * Return TG_NOT_FOUND if nothing found or convert error
 */
int tg_strptime_heuristic(const char* string, time_t* timestamp)
{
    if (tg_strptime(string, TG_FORMATS[0].format, 0, timestamp) == TG_FOUND)
        return TG_FOUND;

    if (tg_strptime(string, "%Y-%m-%d", 0, timestamp) == TG_FOUND)
        return TG_FOUND;
    if (tg_strptime(string, "%Y/%m/%d", 0, timestamp) == TG_FOUND)
        return TG_FOUND;
    if (tg_strptime(string, "%Y.%m.%d", 0, timestamp) == TG_FOUND)
        return TG_FOUND;

    if (tg_strptime(string, "%d-%m-%Y", 0, timestamp) == TG_FOUND)
        return TG_FOUND;
    if (tg_strptime(string, "%d/%m/%Y", 0, timestamp) == TG_FOUND)
        return TG_FOUND;
    if (tg_strptime(string, "%d.%m.%Y", 0, timestamp) == TG_FOUND)
        return TG_FOUND;

    return TG_NOT_FOUND;
}

/**
 * Search, parse and convert datetime to timestamp from single string
 * Return TG_FOUND on success
 * Return TG_NOT_FOUND if nothing found, parse or convert error
 * Retrun TG_ERROR on error, errno is set on system error and 0 on pcre error
 */
static int get_timestamp(
    const char*           string,     // source string
    size_t                length,     // source string length
    const pcre*           re,         // compiled regular expression for datetime
    const pcre_extra*     re_extra,   // optimized regular expression for datetime
    const pcre_extra_opt* re_opt,     // named regular expressions indexes or fallback flag
    const char*           format,     // datetime format (see strptime)
    time_t*               timestamp   // result timestamp
)
{
    int         result;
    const char* match;
    int         matches[30];

    result = pcre_exec(re, re_extra, string, length, 0, 0, matches, sizeof(matches) / sizeof(int));
    if (result < 0) {
        switch (result) {
            case PCRE_ERROR_NOMATCH:
            case PCRE_ERROR_BADUTF8:
            case PCRE_ERROR_BADUTF8_OFFSET:
#ifdef PCRE_ERROR_SHORTUTF8
            case PCRE_ERROR_SHORTUTF8:
#endif
                return TG_NOT_FOUND;
            case PCRE_ERROR_NOMEMORY:
                errno = ENOMEM;
            default:
                errno = 0;
                fprintf(stderr, gettext("%s pcre_exec error %i\n"), gettext("ERROR:"), result);
        }

        return TG_ERROR;
    }

    if (re_opt->fallback != 0) {
        result = pcre_get_substring(string, matches, result, 0, &match);
        if (result < 0) {
            if (result == PCRE_ERROR_NOMEMORY)
                errno = ENOMEM;
            else {
                errno = 0;
                fprintf(stderr, gettext("%s pcre_get_substring error\n"), gettext("ERROR:"));
            }

            return TG_ERROR;
        }

        result = tg_strptime(match, format, re_opt->has_timezone, timestamp);

        pcre_free_substring(match);
    } else
        result = tg_strptime_re(string, matches, result, re_opt, timestamp);

    return result;
}

/**
 * Search string boundaries in multiline data starting from position
 * Return TG_FOUND on success
 * Return TG_NOT_FOUND if newline delimeter exactly in position
 * Return TG_NULL if nothing found (whole data is single string without delimeter)
 */
static int get_string(
    const char* data,       // multiline data
    size_t      size,       // size of multiline data
    size_t      position,   // position to start search
    size_t*     start,      // result string start
    size_t*     length      // result string length (not including delimeter)
)
{
    char* nl;

    if (data[position] == '\n')
        return TG_NOT_FOUND;

    nl = memrchr(data, '\n', position);
    if (nl == NULL)
        *start = 0;
    else
        *start = nl - data + 1;

    nl = memchr(&data[position], '\n', size - position);
    if (nl == NULL)
        *length = size - (*start);
    else
        *length = nl - data - (*start);

    if ((*length) == size)
        return TG_NULL;

    return TG_FOUND;
}

/**
 * Forward search any timestamp in multiline data starting from position to ubound
 * Return TG_FOUND on success
 * Return TG_NOT_FOUND if nothing found from position to size
 * Return TG_NULL if whole data is single string
 * Retrun TG_ERROR on error, errno is set on system error and 0 on pcre error
 */
static int forward_search(
    const char*       data,        // multiline data
    size_t            size,        // size of multiline data
    size_t            position,    // position to start search
    size_t            ubound,      // upper bound position to search
    const pcre*       re,          // compiled regular expression for datetime
    const pcre_extra* re_extra,    // optimized regular expression for datetime
    pcre_extra_opt*   re_opt,      // named regular expressions indexes or fallback flag
    const char*       format,      // datetime format (see strptime)
    size_t*           start,       // result string start
    size_t*           length,      // result string length (not including delimeter)
    time_t*           timestamp    // result timestamp
)
{
    int    result;
    size_t _start;
    size_t _length;
    time_t _timestamp;

    result = TG_NOT_FOUND;
    while (result == TG_NOT_FOUND && position < ubound) {
        result = get_string(data, size, position, &_start, &_length);
        if (result == TG_FOUND) {
            result = get_timestamp(
                &data[_start],
                _length,
                re,
                re_extra,
                re_opt,
                format,
                &_timestamp
            );

            if (result == TG_NOT_FOUND)
                position = _start + _length + 1;
        } else if (result == TG_NULL)
            break;

        position++;
    }

    if (result == TG_FOUND) {
        *start     = _start;
        *length    = _length;
        *timestamp = _timestamp;
    }

    return result;
}

/**
 * Binary search timestamp in multiline data
 * Return TG_FOUND on success
 * Return TG_NOT_FOUND if nothing found
 * Return TG_NULL if whole data is single string without delimeter
 * Retrun TG_ERROR on error, errno is set on system error and 0 on pcre error
 */
static int binary_search(
    const char*       data,        // multiline data
    size_t            size,        // size of multiline data
    size_t            lbound,      // recommended lower bound postion to search
    time_t            search,      // timestamp to search
    const pcre*       re,          // compiled regular expression for datetime
    const pcre_extra* re_extra,    // optimized regular expression for datetime
    pcre_extra_opt*   re_opt,      // named regular expressions indexes or fallback flag
    const char*       format,      // datetime format (see strptime)
    size_t*           position     // result string start
)
{
    int    result;
    size_t middle;
    size_t ubound;
    time_t timestamp;
    size_t start;
    size_t length;

    result = TG_NULL;
    ubound = size;
    middle = lbound + (ubound - lbound) / 2;

    while (lbound != middle) {
        result = forward_search(
            data,
            size,
            middle,
            ubound,
            re,
            re_extra,
            re_opt,
            format,
            &start,
            &length,
            &timestamp
        );

        if (result == TG_FOUND) {
            if (timestamp < search) {
                lbound = start + length;
                middle = ubound;
                if (lbound != ubound)
                    lbound++;
            } else if (timestamp >= search) {
                ubound = start;
                middle = ubound;
            }
        } else if (result == TG_NOT_FOUND)
            ubound = middle;
        else   // TG_ERROR || TG_NULL
            break;

        middle = lbound + (middle - lbound) / 2;
    }

    if (result == TG_FOUND)
        *position = lbound;

    return result;
}

/**
 * File timegrep with binary search
 * Return TG_FOUND on success
 * Return TG_NOT_FOUND if nothing found
 * Return TG_NULL if whole data is single string without delimeter
 * Retrun TG_ERROR on error, errno is set on system error and 0 on pcre error
 */
static int file_timegrep(
    const char*       data,       // multiline data
    size_t            size,       // size of multiline data
    time_t            start,      // start timestamp to search
    time_t            stop,       // stop timestamp to search
    const pcre*       re,         // compiled regular expression for datetime
    const pcre_extra* re_extra,   // optimized regular expression for datetime
    pcre_extra_opt*   re_opt,     // named regular expressions indexes or fallback flag
    const char*       format      // datetime format (see strptime)
)
{
    int     result;
    size_t  lbound;
    size_t  ubound;
    ssize_t actual;
    size_t  lbound_aligned;
    size_t  ubound_aligned;
    size_t  page_size = getpagesize();
    size_t  page_mask = ~(page_size - 1);

    result = binary_search(
        data,
        size,
        0,
        start,
        re,
        re_extra,
        re_opt,
        format,
        &lbound
    );

    if (result != TG_FOUND)
        return result;

    result = binary_search(
        data,
        size,
        lbound,
        stop,
        re,
        re_extra,
        re_opt,
        format,
        &ubound
    );

    if (result != TG_FOUND)
        return result;

    lbound_aligned = lbound & page_mask;
    while (lbound < ubound) {
        actual = TG_CHUNK_SIZE;
        if (lbound + actual >= ubound)
            actual = ubound - lbound;

        actual = write(STDOUT_FILENO, &data[lbound], actual);
        if (actual == -1)
            return TG_ERROR;

        lbound += actual;

        if (lbound_aligned + TG_CHUNK_SIZE < lbound) {
            ubound_aligned = lbound & page_mask;
            if (lbound_aligned < ubound_aligned)
                madvise((void*)(data + lbound_aligned), ubound_aligned - lbound_aligned, MADV_DONTNEED);

            lbound_aligned = ubound_aligned;
        }
    }

    if (ubound == size && write(STDOUT_FILENO, "\n", 1) == -1)
        return TG_ERROR;

    return TG_FOUND;
}

/**
 * Read string from file and dynamically (re)allocate frame data if needed
 * Return TG_FOUND on success
 * Return TG_NOT_FOUND on EOF
 * Retrun TG_ERROR on error, errno is set on system error
 */
static int read_stream_string(
    int     fd,       // file descriptor
    char**  data,     // frame data (may be reallocated)
    size_t* size,     // frame size (may be resized)
    size_t  lbound,   // lower bound frame position
    size_t* ubound,   // upper bound frame position
    size_t* length    // found string length
)
{
    char*   nl;
    char*   buffer;
    ssize_t actual;

    nl = memchr((*data) + lbound, '\n', (*ubound) - lbound);
    if (nl != NULL) {
        *length = (nl - (*data)) - lbound;
        return TG_FOUND;
    }

    while (1) {
        if ((*size) - (*ubound) < TG_CHUNK_SIZE) {
            buffer = realloc(*data, (*size) + TG_CHUNK_SIZE * 2);
            if (buffer == NULL)
                return TG_ERROR;

            *data  = buffer;
            *size += TG_CHUNK_SIZE * 2;
        }

        actual = read(fd, (*data) + (*ubound), TG_CHUNK_SIZE);
        if (actual == -1)
            return TG_ERROR;
        else if (actual == 0)
            return TG_NOT_FOUND;

        nl = memchr((*data) + (*ubound), '\n', actual);

        *ubound += actual;

        if (nl != NULL) {
            *length = (nl - (*data)) - lbound;
            break;
        }
    }

    return TG_FOUND;
}

/**
 * Stream timegrep with sequential search
 * Return TG_FOUND on success
 * Return TG_NOT_FOUND if nothing found
 * Retrun TG_ERROR on error, errno is set on system error and 0 on pcre error
 */
static int stream_timegrep(const tg_context* ctx)
{
    int     result;
    ssize_t actual;
    size_t  length;
    time_t  timestamp;
    char*   data   = NULL;
    size_t  size   = 0;
    size_t  lbound = 0;
    size_t  ubound = 0;
    int     stream = 0;

    while (1) {
        result = read_stream_string(ctx->fd, &data, &size, lbound, &ubound, &length);
        if (result == TG_ERROR)
            goto ERROR;
        else if (result == TG_NOT_FOUND)
            break;

        result = get_timestamp(data + lbound, length, ctx->re_ctx.re, ctx->re_ctx.re_extra, &ctx->re_ctx.re_opt, ctx->re_ctx.format, &timestamp);
        if (result == TG_ERROR)
            goto ERROR;

        if (result == TG_FOUND) {
            if (timestamp >= ctx->stop)
                break;
            else if (stream == 0 && timestamp >= ctx->start)
                stream = 1;
        }

        if (stream == 1) {
            length++;

            while (length > 0) {
                actual = write(STDOUT_FILENO, data + lbound, length);
                if (actual == -1)
                    goto ERROR;

                length -= actual;
                lbound += actual;
            }
        } else
            lbound += length + 1;

        if (ubound - lbound < lbound) {
            memmove(data, data + lbound, ubound - lbound);

            ubound = ubound - lbound;
            lbound = 0;
        }
    }

    free(data);

    return (stream == 1 ? TG_FOUND : TG_NOT_FOUND);

ERROR:

    result = errno;

    free(data);

    errno = result;

    return TG_ERROR;
}

/**
 * Parse command line options
 * Return TG_FOUND on success
 * Return TG_NOT_FOUND if nothing to do (help or version printed)
 * Retrun TG_ERROR on error
 */
int parse_options(int argc, char* argv[], tg_context* ctx)
{
    // options parsing
    int      index;    // option index
    int      option;   // option name
    long int value;    // option numeric value

    // options values
    const char* from   = NULL;   // from datetime
    const char* to     = NULL;   // to datetime
    long int    offset = 0;      // offset in seconds from now

    // pcre
    char*       regex       = NULL;   // format regular expression
    size_t      regex_len   = 0;      // length of regex string
    const char* pcre_error  = NULL;   // current pcre error message
    int         pcre_offset = 0;      // current pcre error offset

    int result = TG_NOT_FOUND;

    while (1) {
        static struct option long_options[] = {
            { "format",  required_argument, 0, 'e' },
            { "start",   required_argument, 0, 'f' },
            { "stop",    required_argument, 0, 't' },
            { "seconds", required_argument, 0, 's' },
            { "minutes", required_argument, 0, 'm' },
            { "hours",   required_argument, 0, 'h' },
            { "version", no_argument,       0, 'v' },
            { "help",    no_argument,       0, '?' },
            { NULL, 0, NULL, 0 }
        };

        option = getopt_long(argc, argv, "r:e:f:t:s:m:h:v?", long_options, &index);

        if (option == -1)
            break;

        switch (option) {
            case 'e':
                ctx->re_ctx.format = optarg;
                break;
            case 'f':
                from = optarg;
                break;
            case 't':
                to = optarg;
                break;
            case 's':
                value = parse_interval(optarg, 1);
                if (value == LONG_MIN)
                    goto ERROR;
                offset += value;
                break;
            case 'm':
                value = parse_interval(optarg, 60);
                if (value == LONG_MIN)
                    goto ERROR;
                offset += value;
                break;
            case 'h':
                value = parse_interval(optarg, 60 * 60);
                if (value == LONG_MIN)
                    goto ERROR;
                offset += value;
                break;
            case 'v':
                print_version();
                goto SUCCESS;
            case '?':
                print_help();
                goto SUCCESS;
            default:
                print_help();
                goto SUCCESS;
        }
    }

    if (ctx->re_ctx.format != NULL) {
        index = 0;
        while (TG_FORMATS[index].name != NULL) {
            if (strcmp(ctx->re_ctx.format, TG_FORMATS[index].name) == 0) {
                if (TG_FORMATS[index].alias != NULL) {
                    ctx->re_ctx.format = TG_FORMATS[index].alias;
                    index       = 0;
                    continue;
                }

                ctx->re_ctx.format = TG_FORMATS[index].format;

                break;
            }

            index++;
        }
    } else
        ctx->re_ctx.format = TG_FORMATS[0].format;

    memset(&ctx->re_ctx.re_opt, 0, sizeof(ctx->re_ctx.re_opt));

    regex_len = strptime_regex(ctx->re_ctx.format, NULL, &ctx->re_ctx.re_opt);
    if (regex_len == TG_ERROR)
        goto ERROR;

    regex = malloc(regex_len + 1);
    if (regex == NULL)
        goto ERROR;

    regex[regex_len] = 0;

    memset(&ctx->re_ctx.re_opt, 0, sizeof(ctx->re_ctx.re_opt));

    if (strptime_regex(ctx->re_ctx.format, regex, &ctx->re_ctx.re_opt) == TG_ERROR)
        goto ERROR;

    ctx->re_ctx.re = pcre_compile(regex, PCRE_UTF8 | PCRE_DUPNAMES, &pcre_error, &pcre_offset, NULL);
    if (ctx->re_ctx.re == NULL) {
        errno = 0;
        fprintf(stderr, gettext("%s Could not compile '%s' at %d: %s\n"), gettext("ERROR:"), regex, pcre_offset, pcre_error);
        goto ERROR;
    }

    ctx->re_ctx.re_extra = pcre_study(
        ctx->re_ctx.re,
#ifdef PCRE_CONFIG_JIT
        PCRE_STUDY_JIT_COMPILE,
#else
        0,
#endif
        &pcre_error
    );
    if (pcre_error != NULL) {
        errno = 0;
        fprintf(stderr, gettext("%s Could not study '%s': %s\n"), gettext("ERROR:"), regex, pcre_error);
        goto ERROR;
    }

    if (ctx->re_ctx.re_opt.fallback == 0) {
        ctx->re_ctx.re_opt.has_year      = pcre_get_stringnumber(ctx->re_ctx.re, "year");
        ctx->re_ctx.re_opt.has_month     = pcre_get_stringnumber(ctx->re_ctx.re, "month");
        ctx->re_ctx.re_opt.has_month_t   = pcre_get_stringnumber(ctx->re_ctx.re, "month_t");
        ctx->re_ctx.re_opt.has_day       = pcre_get_stringnumber(ctx->re_ctx.re, "day");
        ctx->re_ctx.re_opt.has_hour      = pcre_get_stringnumber(ctx->re_ctx.re, "hour");
        ctx->re_ctx.re_opt.has_minute    = pcre_get_stringnumber(ctx->re_ctx.re, "minute");
        ctx->re_ctx.re_opt.has_second    = pcre_get_stringnumber(ctx->re_ctx.re, "second");
        ctx->re_ctx.re_opt.has_timezone  = pcre_get_stringnumber(ctx->re_ctx.re, "timezone");
        ctx->re_ctx.re_opt.has_timestamp = pcre_get_stringnumber(ctx->re_ctx.re, "timestamp");
    }

    if (to == NULL)
        ctx->stop = time(NULL);
    else if (tg_strptime(to, ctx->re_ctx.format, ctx->re_ctx.re_opt.has_timezone, &ctx->stop) == TG_NOT_FOUND && tg_strptime_heuristic(to, &ctx->stop) == TG_NOT_FOUND) {
        errno = 0;
        fprintf(stderr, gettext("%s Can not convert argument '%s' to timestamp\n"), gettext("ERROR:"), to);
        goto ERROR;
    }

    if (from == NULL)
        ctx->start = ctx->stop - offset;
    else if (tg_strptime(from, ctx->re_ctx.format, ctx->re_ctx.re_opt.has_timezone, &ctx->start) == TG_NOT_FOUND && tg_strptime_heuristic(from, &ctx->start) == TG_NOT_FOUND) {
        errno = 0;
        fprintf(stderr, gettext("%s Can not convert argument '%s' to timestamp\n"), gettext("ERROR:"), from);
        goto ERROR;
    }

    result = TG_FOUND;

    goto SUCCESS;

ERROR:

    result = TG_ERROR;

    if (errno != 0) {
        fprintf(stderr, "%s %s\n", gettext("ERROR:"), strerror(errno));
        errno = 0;
    }

SUCCESS:

    if (regex != NULL)
        free(regex);

    return result;
}

/**
 * Main magic
 */
int main(int argc, char* argv[])
{
    int         result;
    struct stat file_stat;
    tg_context  ctx;

    set_timezone();

    memset(&ctx, 0, sizeof(ctx));

    ctx.fd   = -1;
    ctx.data = MAP_FAILED;

    result = parse_options(argc, argv, &ctx);
    if (result == TG_NOT_FOUND) {
        result = 0;
        goto SUCCESS;
    } else if (result == TG_ERROR)
        goto ERROR;

    if (optind < argc) {
        while (optind < argc) {
            ctx.filename = argv[optind++];

            ctx.fd = open(ctx.filename, O_RDONLY);
            if (ctx.fd == -1)
                goto ERROR;

            if (fstat(ctx.fd, &file_stat) == -1)
                goto ERROR;

            ctx.size = file_stat.st_size;

            ctx.data = mmap(NULL, ctx.size, PROT_READ, MAP_PRIVATE, ctx.fd, 0);
            if (ctx.data == MAP_FAILED)
                goto ERROR;

            close(ctx.fd);
            ctx.fd = -1;

            result = file_timegrep(
                ctx.data,
                ctx.size,
                ctx.start,
                ctx.stop,
                ctx.re_ctx.re,
                ctx.re_ctx.re_extra,
                &ctx.re_ctx.re_opt,
                ctx.re_ctx.format
            );

            if (result == TG_ERROR)
                goto ERROR;

            munmap(ctx.data, ctx.size);
            ctx.data = MAP_FAILED;
        }
    } else {
        ctx.fd = STDIN_FILENO;
        result = stream_timegrep(&ctx);
    }

    result = (result == TG_FOUND ? 0 : 1);

    goto SUCCESS;

ERROR:

    result = 2;

    if (errno != 0)
        fprintf(stderr, "%s %s\n", gettext("ERROR:"), strerror(errno));

SUCCESS:

    if (ctx.re_ctx.re != NULL)
        pcre_free(ctx.re_ctx.re);

    if (ctx.re_ctx.re_extra != NULL)
#ifdef PCRE_CONFIG_JIT
        pcre_free_study(ctx.re_ctx.re_extra);
#else
        pcre_free(ctx.re_ctx.re_extra);
#endif

    if (ctx.data != MAP_FAILED)
        munmap(ctx.data, ctx.size);

    if (ctx.fd != -1 && ctx.fd != STDIN_FILENO)
        close(ctx.fd);

    return result;
}
