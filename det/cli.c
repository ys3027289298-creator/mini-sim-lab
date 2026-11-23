/* det/cli.c: shared argv handling; see cli.h. */
#include "cli.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixed default; mixing a constant keeps the default stable across builds. */
uint64_t det_cli_default_seed(void) { return det_mix64(0x5EEDF00DULL); }

void det_cli_fail(const char* msg) {
    fprintf(stderr, "%s\n", msg ? msg : "det: args: invalid arguments");
    exit(2);
}

void det_cli_die(const char* unit, const char* field, const char* fmt, ...) {
    fprintf(stderr, "%s: %s: ", unit ? unit : "det", field ? field : "args");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

static const char* value_after(const char* unit, const char* opt, int argc,
                               char** argv, int i) {
    if (i + 1 >= argc) det_cli_die(unit, opt, "requires a value");
    return argv[i + 1];
}

static int listed(const char* const* names, const char* a) {
    for (int i = 0; names && names[i]; i++)
        if (strcmp(names[i], a) == 0) return 1;
    return 0;
}

DetCli det_cli_common(const char* unit, int argc, char** argv,
                      const char* const* modopts) {
    DetCli cli;
    memset(&cli, 0, sizeof(cli));
    cli.unit = unit ? unit : "det";
    cli.shared = 1;
    cli.seed = det_cli_default_seed();
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--shared") == 0) {
            cli.shared = 1;
        } else if (strcmp(a, "--no-shared") == 0) {
            cli.shared = 0;
        } else if (strcmp(a, "--dump") == 0) {
            cli.dump = value_after(cli.unit, a, argc, argv, i++);
            if (!*cli.dump) det_cli_die(cli.unit, "--dump", "path must not be empty");
        } else if (strcmp(a, "--load") == 0) {
            cli.load = value_after(cli.unit, a, argc, argv, i++);
            if (!*cli.load) det_cli_die(cli.unit, "--load", "path must not be empty");
        } else if (strcmp(a, "--seed") == 0) {
            cli.seed = det_cli_u64(cli.unit, "--seed",
                                   value_after(cli.unit, a, argc, argv, i++));
            cli.seed_given = 1;
        } else if (strcmp(a, "--expect-hash") == 0) {
            cli.compare = det_cli_hex64(cli.unit, "--expect-hash",
                                        value_after(cli.unit, a, argc, argv, i++));
            cli.compare_given = 1;
        } else if (strcmp(a, "--help") == 0) {
            fprintf(stdout,
                    "common: --shared|--no-shared --seed N --dump PATH "
                    "--load PATH --expect-hash HEX\n");
            exit(0);
        } else if (listed(modopts, a)) {
            (void)value_after(cli.unit, a, argc, argv, i++);
        } else if (a[0] == '-' && a[1] != '\0') {
            det_cli_die(cli.unit, a, "unknown option");
        }
    }
    return cli;
}

const char* det_cli_value(const char* unit, const char* opt, int argc,
                          char** argv, int* i) {
    const char* v = value_after(unit, opt, argc, argv, *i);
    (*i)++;
    return v;
}

long det_cli_long(const char* unit, const char* field, const char* s,
                  long lo, long hi) {
    char err[256];
    long v = 0;
    if (det_parse_long(unit, field, s, lo, hi, &v, err, sizeof(err)) != DET_OK)
        det_cli_fail(err);
    return v;
}

double det_cli_double(const char* unit, const char* field, const char* s) {
    char err[256];
    double v = 0;
    if (det_parse_double(unit, field, s, &v, err, sizeof(err)) != DET_OK)
        det_cli_fail(err);
    return v;
}

uint64_t det_cli_u64(const char* unit, const char* field, const char* s) {
    char err[256];
    uint64_t v = 0;
    if (det_parse_u64(unit, field, s, &v, err, sizeof(err)) != DET_OK)
        det_cli_fail(err);
    return v;
}

uint64_t det_cli_hex64(const char* unit, const char* field, const char* s) {
    char err[256];
    uint64_t v = 0;
    if (det_parse_hex64(unit, field, s, &v, err, sizeof(err)) != DET_OK)
        det_cli_fail(err);
    return v;
}

int det_cli_threads(const char* unit, const char* spec, int* out, int max) {
    char err[256];
    int n = 0;
    if (det_parse_threads(unit, spec, out, max, &n, err, sizeof(err)) != DET_OK)
        det_cli_fail(err);
    return n;
}
