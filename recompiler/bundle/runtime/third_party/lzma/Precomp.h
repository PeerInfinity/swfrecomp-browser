/* Precomp.h -- StdAfx
2013-11-12 : Igor Pavlov : Public domain */

#ifndef __7Z_PRECOMP_H
#define __7Z_PRECOMP_H

/* SWFRecomp local change: single-threaded build. Defined here rather than on
   the command line because the runtime is compiled by half a dozen scripts
   that all just glob the sources; Precomp.h is the first include of every
   LZMA .c file, so this reaches all of them. Keeps LzFindMt.{c,h} (and the
   pthread dependency, which WASM does not want) out of the vendored set. */
#ifndef _7ZIP_ST
#define _7ZIP_ST
#endif

#include "Compiler.h"
/* #include "7zTypes.h" */

#endif
