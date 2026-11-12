#ifndef ED25519_SECURE_ZERO_H
#define ED25519_SECURE_ZERO_H

#include <stddef.h>

// Volatile stores keep secret cleanup from being removed before memory is released.
static inline void ed25519_secure_zero(void *data, size_t size) {
    volatile unsigned char *p = (volatile unsigned char *)data;
    while (size--) *p++ = 0;
}

#endif
