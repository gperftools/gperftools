// Copyright (c) 2005, Google Inc.
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
// 
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef _BASICTYPES_H_
#define _BASICTYPES_H_

#include "config.h"

// To use this in an autoconf setting, make sure you run the following
// autoconf macros:
//    AC_HEADER_STDC              /* for stdint_h and inttypes_h */
//    AC_CHECK_TYPES([__int64])   /* defined in some windows platforms */

#if defined HAVE_STDINT_H
#include <stdint.h>             // to get uint16_t (ISO naming madness)
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>           // another place uint16_t might be defined
#else
#include <sys/types.h>          // our last best hope
#endif

// Standard typedefs
// All Google code is compiled with -funsigned-char to make "char"
// unsigned.  Google code therefore doesn't need a "uchar" type.
// TODO(csilvers): how do we make sure unsigned-char works on non-gcc systems?
typedef signed char         schar;
typedef int8_t              int8;
typedef int16_t             int16;
typedef int32_t             int32;
#ifdef HAVE___INT64
typedef __int64             int64;
#else
typedef int64_t             int64;
#endif

// NOTE: unsigned types are DANGEROUS in loops and other arithmetical
// places.  Use the signed types unless your variable represents a bit
// pattern (eg a hash value) or you really need the extra bit.  Do NOT
// use 'unsigned' to express "this value should always be positive";
// use assertions for this.

typedef uint8_t            uint8;
typedef uint16_t           uint16;
typedef uint32_t           uint32;
#ifdef HAVE___INT64
typedef unsigned __int64   uint64;
#else
typedef uint64_t           uint64;
#endif

const uint16 kuint16max = (   (uint16) 0xFFFF);
const uint32 kuint32max = (   (uint32) 0xFFFFFFFF);
const uint64 kuint64max = ( (((uint64) kuint32max) << 32) | kuint32max );

const  int8  kint8max   = (   (  int8) 0x7F);
const  int16 kint16max  = (   ( int16) 0x7FFF);
const  int32 kint32max  = (   ( int32) 0x7FFFFFFF);
const  int64 kint64max =  ( ((( int64) kint32max) << 32) | kuint32max );

const  int8  kint8min   = (   (  int8) 0x80);
const  int16 kint16min  = (   ( int16) 0x8000);
const  int32 kint32min  = (   ( int32) 0x80000000);
const  int64 kint64min =  ( ((( int64) kint32min) << 32) | 0 );

// A macro to disallow the evil copy constructor and operator= functions
// This should be used in the private: declarations for a class
#define DISALLOW_EVIL_CONSTRUCTORS(TypeName)    \
  TypeName(const TypeName&);                    \
  void operator=(const TypeName&)

#endif  // _BASICTYPES_H_
