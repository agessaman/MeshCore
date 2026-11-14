#pragma once

#include <stddef.h>
#include <stdint.h>

// Keep socket headers out of translation units that declare namespace poll.
int pollControlSend(int fd, const uint8_t* data, size_t len);
