/* det/cli.h: shared argv handling for the module evidence drivers.
 *
 * Every failure is reported as "<module>: <option>: <reason>" on stderr and
 * exits 2, so an invalid parameter always names the module that rejected it.
 * Unknown options are rejected rather than ignored; module-specific options
 * are declared up front so the base parser knows to leave them alone. */
#ifndef DET_CLI_H
#define DET_CLI_H

#include <stdint.h>

#include "detcontext.h"

typedef struct {
    const char* unit;          /* module name used to locate errors */
    int shared;                /* 1 unless --no-shared */
    const char* dump;          /* --dump PATH, may be NULL */
    const char* load;          /* --load PATH, may be NULL */
    uint64_t seed;             /* --seed, or the fixed module default */
    int seed_given;
    uint64_t compare;          /* --expect-hash HEX, valid if compare_given */
    int compare_given;
} DetCli;

/* `modopts` is a NULL-terminated list of module option names that take a
 * value (e.g. {"--frames", NULL}); the base parser validates them for
 * presence but leaves parsing to the module. */
DetCli det_cli_common(const char* unit, int argc, char** argv,
                      const char* const* modopts);

/* The default seed: a fixed constant, never the clock or the environment. */
uint64_t det_cli_default_seed(void);

/* Prints a message already formatted as "<module>: <field>: <reason>". */
void det_cli_fail(const char* msg) __attribute__((noreturn));
void det_cli_die(const char* unit, const char* field, const char* fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));

/* The value following option argv[i]; advances *i past it. */
const char* det_cli_value(const char* unit, const char* opt, int argc,
                          char** argv, int* i);

/* Parsed values; all exit 2 via det_cli_die on bad input. */
long det_cli_long(const char* unit, const char* field, const char* s,
                  long lo, long hi);
double det_cli_double(const char* unit, const char* field, const char* s);
uint64_t det_cli_u64(const char* unit, const char* field, const char* s);
uint64_t det_cli_hex64(const char* unit, const char* field, const char* s);
/* "1,2,4" -> ascending thread counts; returns the count. */
int det_cli_threads(const char* unit, const char* spec, int* out, int max);

#endif /* DET_CLI_H */
