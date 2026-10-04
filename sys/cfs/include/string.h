/* SPDX-License-Identifier: ISC
 *
 * string.h -- kernel stand-in for the core's memory primitives (libkern).
 */

#ifndef CFS_KERNEL_STRING_H
#define CFS_KERNEL_STRING_H

#include <sys/types.h>

int(memcmp)(const void *left, const void *right, size_t length);
void *(memcpy)(void *destination, const void *source, size_t length);
void *(memmove)(void *destination, const void *source, size_t length);
void *(memset)(void *destination, int value, size_t length);

#endif /* CFS_KERNEL_STRING_H */
