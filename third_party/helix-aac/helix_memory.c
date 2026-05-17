#include "utils/helix_memory.h"

#include <stdlib.h>

void *helix_malloc(int size) {
    return calloc(1, size > 0 ? (size_t)size : 1u);
}

void helix_free(void *ptr) {
    free(ptr);
}
