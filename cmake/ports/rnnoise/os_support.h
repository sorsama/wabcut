/* Wabcut overlay port: RNNoise 0.2's ARM (NEON) and plain-C vector code (src/vec_neon.h, src/vec.h) includes Opus's
   os_support.h, which the RNNoise release does not ship. They use one macro from it, OPUS_CLEAR; RNNoise's own
   common.h has the same operation as RNN_CLEAR. x86-64 builds take src/vec_avx.h and never include this file. */
#ifndef OS_SUPPORT_H
#define OS_SUPPORT_H

#include "common.h"

#define OPUS_CLEAR(dst, n) RNN_CLEAR(dst, n)
#define OPUS_COPY(dst, src, n) RNN_COPY(dst, src, n)
#define OPUS_MOVE(dst, src, n) RNN_MOVE(dst, src, n)

#endif
