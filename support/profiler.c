/* profiler.c — thecoolestcompiler profiling runtime implementation (REQ-PR3)
 *
 * Plain C11, no C++/ROSE/Clang dependency, no external libraries (JSON is
 * hand-written with fprintf — a full JSON library is not needed for a
 * single flat array of flat objects).
 *
 * Design:
 *   - A dynamic array of ProfileRecord, grown with realloc() as
 *     profile_loop_end() is called.
 *   - An atexit() handler, registered exactly once (guarded by a static
 *     flag), that serializes the array to JSON and writes it to the file
 *     named by $PROFILE_OUTPUT, or "profile.json" in the current working
 *     directory if that variable is unset/empty.
 */

#include "profiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char* loop_id;
    long  trip_count;
    long  flops;
    long  bytes_read;
    long  bytes_written;
} ProfileRecord;

static ProfileRecord* g_records = NULL;
static size_t         g_count = 0;
static size_t         g_capacity = 0;
static int            g_atexit_registered = 0;

/* Small local strdup replacement — avoids relying on POSIX strdup() being
 * visible under strict -std=c11. */
static char* profiler_dup_string(const char* s) {
    size_t len;
    char* copy;
    if (s == NULL) {
        s = "";
    }
    len = strlen(s);
    copy = (char*)malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, len + 1);
    return copy;
}

/* Writes s to f as a JSON string, escaping backslashes, double quotes, and
 * control characters. loop_id values are expected to be simple
 * "filename:line" strings, but escaping is done defensively. */
static void profiler_fprint_json_string(FILE* f, const char* s) {
    const unsigned char* p = (const unsigned char*)s;
    fputc('"', f);
    if (p != NULL) {
        for (; *p != '\0'; ++p) {
            switch (*p) {
                case '\\': fputs("\\\\", f); break;
                case '"':  fputs("\\\"", f); break;
                case '\n': fputs("\\n", f);  break;
                case '\r': fputs("\\r", f);  break;
                case '\t': fputs("\\t", f);  break;
                default:
                    if (*p < 0x20) {
                        fprintf(f, "\\u%04x", (unsigned int)*p);
                    } else {
                        fputc((int)*p, f);
                    }
                    break;
            }
        }
    }
    fputc('"', f);
}

static void profiler_write_json(void) {
    const char* out_path;
    FILE* f;
    size_t i;

    out_path = getenv("PROFILE_OUTPUT");
    if (out_path == NULL || out_path[0] == '\0') {
        out_path = "profile.json";
    }

    f = fopen(out_path, "w");
    if (f == NULL) {
        /* Nothing sensible to do here (we're inside atexit); just skip. */
        return;
    }

    fprintf(f, "[\n");
    for (i = 0; i < g_count; ++i) {
        fprintf(f, "  {\"loop_id\": ");
        profiler_fprint_json_string(f, g_records[i].loop_id);
        fprintf(f,
                ", \"trip_count\": %ld, \"flops\": %ld, "
                "\"bytes_read\": %ld, \"bytes_written\": %ld}%s\n",
                g_records[i].trip_count,
                g_records[i].flops,
                g_records[i].bytes_read,
                g_records[i].bytes_written,
                (i + 1 < g_count) ? "," : "");
    }
    fprintf(f, "]\n");

    fclose(f);
}

static void profiler_atexit_handler(void) {
    profiler_write_json();
}

static void profiler_ensure_registered(void) {
    if (!g_atexit_registered) {
        atexit(profiler_atexit_handler);
        g_atexit_registered = 1;
    }
}

void profile_loop_start(const char* loop_id) {
    (void)loop_id; /* start timestamps are not required by REQ-PR3 */
    profiler_ensure_registered();
}

void profile_loop_end(const char* loop_id,
                       long trip_count,
                       long flops,
                       long bytes_read,
                       long bytes_written) {
    ProfileRecord* rec;

    profiler_ensure_registered();

    if (g_count == g_capacity) {
        size_t new_capacity = (g_capacity == 0) ? 16 : (g_capacity * 2);
        ProfileRecord* new_records =
            (ProfileRecord*)realloc(g_records, new_capacity * sizeof(ProfileRecord));
        if (new_records == NULL) {
            /* Allocation failure: drop this record rather than crash the
             * instrumented program (REQ-PR1: must not change observable
             * behavior of the program being profiled). */
            return;
        }
        g_records = new_records;
        g_capacity = new_capacity;
    }

    rec = &g_records[g_count];
    rec->loop_id = profiler_dup_string(loop_id);
    rec->trip_count = trip_count;
    rec->flops = flops;
    rec->bytes_read = bytes_read;
    rec->bytes_written = bytes_written;
    ++g_count;
}
