/* SPDX-License-Identifier: Unlicense */

#ifndef BFS_MACROS_H
#define BFS_MACROS_H

/*
 * Portable compiler-attribute helpers shared across bfs.
 *
 * Self-contained: no includes, no libc or kernel dependencies, so it is
 * safe in userland, under -D_KERNEL, and in freestanding builds.
 */

/*
 * BFS_NO_SANITIZE_THREAD marks a single function so ThreadSanitizer skips
 * instrumenting its plain (non-atomic) loads and stores. Happens-before
 * tracking via __tsan_func_entry/exit is still emitted, so synchronization
 * established by callers is unaffected. Use only on pure compute over
 * thread-private memory (e.g. block checksums of a caller-owned buffer),
 * never on code that touches shared state or establishes ordering.
 *
 * Note: no_sanitize also acts as an inlining barrier. Marking a leaf alone
 * forces it out-of-line and leaves its callers instrumented, so to silence
 * a hot helper chain mark the whole span with the region macros below.
 *
 * No-op where the attribute is unsupported; harmless when TSan is off.
 */
#if defined(__has_attribute)
#  if __has_attribute(no_sanitize)
#    define BFS_NO_SANITIZE_THREAD __attribute__((no_sanitize("thread")))
#  endif
#endif
#ifndef BFS_NO_SANITIZE_THREAD
#  define BFS_NO_SANITIZE_THREAD
#endif

/*
 * BFS_NO_SANITIZE_THREAD_BEGIN / _END apply the above to every function
 * defined between them (clang region attribute). This is the form to use
 * when a whole translation unit or section is pure compute, since it also
 * covers functions added later and sidesteps the inlining-barrier caveat.
 *
 * No-op on compilers without clang's attribute pragma.
 */
#if defined(__clang__)
#  define BFS_NO_SANITIZE_THREAD_BEGIN \
       _Pragma("clang attribute push (__attribute__((no_sanitize(\"thread\"))), apply_to = function)")
#  define BFS_NO_SANITIZE_THREAD_END _Pragma("clang attribute pop")
#else
#  define BFS_NO_SANITIZE_THREAD_BEGIN
#  define BFS_NO_SANITIZE_THREAD_END
#endif

/*
 * BFS_NO_FRAME_INLINE marks a function so the optimizer neither optimizes
 * it nor inlines other functions into its frame. Use on a dispatcher that
 * calls many large-frame functions in sequence: at -O2 the compiler would
 * otherwise inline the callees and sum their stack frames (and any
 * sanitizer redzones) into one oversized frame, overflowing the stack.
 * Keeping the dispatcher unoptimized leaves the callees out-of-line, each
 * with its own short-lived frame, while every other function stays -O2.
 *
 * No-op where unsupported.
 */
#if defined(__has_attribute)
#  if __has_attribute(optnone)
#    define BFS_NO_FRAME_INLINE __attribute__((optnone))
#  endif
#endif
#ifndef BFS_NO_FRAME_INLINE
#  define BFS_NO_FRAME_INLINE
#endif

#endif /* BFS_MACROS_H */
