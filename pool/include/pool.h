
/* pool: fixed-capacity object pool with generation handles, so dangling
   handles are detected instead of silently reused. */
#ifndef POOL_H
#define POOL_H

#define POOL_CAP 16

typedef struct {
    unsigned int index;
    unsigned int gen;
} Handle;

typedef struct {
    int in_use[POOL_CAP];
    unsigned int gen[POOL_CAP];
    int value[POOL_CAP];
    int used;
    int high_water;
} Pool;

void pool_init(Pool* p);
Handle pool_alloc(Pool* p, int v);
int pool_valid(const Pool* p, Handle h);
int pool_get(const Pool* p, Handle h, int* out);
int pool_free(Pool* p, Handle h);

#endif /* POOL_H */
