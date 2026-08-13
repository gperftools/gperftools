/* -*- Mode: C; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include <execinfo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(USE_AW_BACKTRACE)
#include "aw-backtrace/aw-backtrace.h"
#endif

#if defined(USE_FP_BACKTRACE)
#include "simple-fp-backtrace.h"
#endif

// Don't pay much attention to Nodes and enums. Here is what is going
// on. We need some test to exercise deep recursion chains that isn't
// excessively synthetic. So we have this "evaluation tree" like
// stuff. Trees we construct are more deep then wide.
//
// We also make recursion eval_ast function "wide' in terms of
// arguments used. To force argument into stack (so push/pop around
// calls; more interesting unwind info). But the more interesting info
// is only exercised when building with -fomit-frame-pointer. Bazel's
// default as at the time of writing is the opposite. I.e. manually
// pass -fno-omit-frame-pointer to exercise that aspect. You may also
// want to build with --copt=-DTESTING_NO_CACHE (skip the cache) and/or
// --copt=-DTESTING_NO_FASTPATH (force the full CFI decoder) to benchmark
// the slow path if that is what you want to see.

typedef enum {
  OP_CONST,
  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_DIV,
  OP_MOD,
  OP_AND,
  OP_OR,
  OP_XOR,
  OP_SHL,
  OP_SHR,
  OP_NOP,
  OP_INC,
  OP_DEC,
  OP_LAST
} OpCode;

typedef struct Node {
  OpCode op;
  uint64_t val;
  struct Node* left;
  struct Node* right;
} Node;

#define BT_SIZE 1024
struct Context;
struct Context {
  uint64_t operations;
  int max_bt_depth;
  void (*leaf_fn)(struct Context* ctx);
  void* backtrace[BT_SIZE];
  int backtrace_size;
};

// __attribute__((noinline)) prevents the compiler from optimizing the call away.
// System V AMD64 ABI passes the first 6 integer/pointer args in registers
// (rdi, rsi, rdx, rcx, r8, r9). Args 7 and 8 are forced onto the stack.
__attribute__((noinline)) uint64_t eval_ast(Node* n,              // 1: rdi
                                            struct Context* ctx,  // 2: rsi
                                            int depth,            // 3: rdx
                                            uint64_t dummy1,      // 4: rcx
                                            uint64_t dummy2,      // 5: r8
                                            uint64_t dummy3,      // 6: r9
                                            uint64_t metric1,     // 7: stack
                                            uint64_t metric2)     // 8: stack
{
  if (!n)
    return 0;
  ctx->operations++;

  if (depth == 0) {
    ctx->leaf_fn(ctx);
    return n->val;
  }

  // large switch statement to inflate function size and test jump tables
  switch (n->op) {
    case OP_ADD:
      return eval_ast(n->left, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 1, metric2 + 2) +
             eval_ast(n->right, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 2, metric2 + 1);
    case OP_SUB:
      return eval_ast(n->left, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 1, metric2 + 2) -
             eval_ast(n->right, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 2, metric2 + 1);
    case OP_MUL:
      return eval_ast(n->left, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 1, metric2 + 2) *
             eval_ast(n->right, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 2, metric2 + 1);
    case OP_DIV: {
      uint64_t rval = eval_ast(n->right, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 2, metric2 + 1);
      uint64_t lval = eval_ast(n->left, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 1, metric2 + 2);
      return rval ? lval / rval : 0;
    }
    case OP_MOD: {
      uint64_t rval = eval_ast(n->right, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 2, metric2 + 1);
      uint64_t lval = eval_ast(n->left, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 1, metric2 + 2);
      return rval ? lval % rval : 0;
    }
    case OP_AND:
      return eval_ast(n->left, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 1, metric2 + 2) &
             eval_ast(n->right, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 2, metric2 + 1);
    case OP_OR:
      return eval_ast(n->left, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 1, metric2 + 2) |
             eval_ast(n->right, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 2, metric2 + 1);
    case OP_XOR:
      return eval_ast(n->left, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 1, metric2 + 2) ^
             eval_ast(n->right, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 2, metric2 + 1);
    case OP_SHL:
      return eval_ast(n->left, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 1, metric2 + 2)
             << (eval_ast(n->right, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 2, metric2 + 1) & 63);
    case OP_SHR:
      return eval_ast(n->left, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 1, metric2 + 2) >>
             (eval_ast(n->right, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 2, metric2 + 1) & 63);
    case OP_INC:
      return eval_ast(n->left, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 1, metric2 + 2) + 1;
    case OP_DEC:
      return eval_ast(n->left, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 1, metric2 + 2) - 1;
    case OP_NOP:
      return eval_ast(n->left, ctx, depth - 1, dummy1, dummy2, dummy3, metric1 + 1, metric2 + 2);
    case OP_CONST:
      return n->val;
    default:
      __builtin_trap();
  }
}

// Builds a deeply nested, unbalanced tree to maximize stack depth without
// causing exponential memory/time overhead.
Node* build_deep_tree(int target_depth) {
  if (target_depth <= 0) {
    Node* leaf = (Node*)malloc(sizeof(Node));
    leaf->op = OP_CONST;
    leaf->val = 42;
    leaf->left = leaf->right = NULL;
    return leaf;
  }
  Node* n = (Node*)malloc(sizeof(Node));
  n->op = (OpCode)(OP_ADD + (target_depth % (int)(OP_LAST - OP_ADD)));
  n->left = build_deep_tree(target_depth - 1);
  n->right = NULL;
  return n;
}

int bench_iters = 10240;

static void do_bench(struct Context* ctx) {
  clock_t time_before;
again:
  time_before = clock();
  for (int i = bench_iters; i > 0; i--) {
#ifdef USE_AW_BACKTRACE
    ctx->backtrace_size = aw_backtrace(NULL, ctx->backtrace, ctx->max_bt_depth, 0);
#elif defined(USE_FP_BACKTRACE)
    ctx->backtrace_size = simple_backtrace(NULL, ctx->backtrace, ctx->max_bt_depth);
#else
    ctx->backtrace_size = backtrace(ctx->backtrace, ctx->max_bt_depth);
#endif
  }
  clock_t time_after = clock();
  double nanos_per_iter = (double)(time_after - time_before) / (double)CLOCKS_PER_SEC / bench_iters * 1e9;
  printf("nanos per iter (for depth of %d): %g\n", ctx->backtrace_size, nanos_per_iter);
  printf("... per unwind step: %g\n", nanos_per_iter / ctx->backtrace_size);

  if (ctx->max_bt_depth > 32) {
    ctx->max_bt_depth = ctx->max_bt_depth / 2;
    goto again;
  }
}

int main(int argc, char** argv) {
  if (argc > 1) {
    bench_iters = atoi(argv[1]);
    if (bench_iters <= 0) {
      printf("bogus bench_iters. Need int, got: %s\n", argv[1]);
      abort();
    }
  }
  int target_depth = 16 << 10;
  Node* root = build_deep_tree(target_depth);
  struct Context ctx = {};
  ctx.max_bt_depth = BT_SIZE;
  ctx.leaf_fn = do_bench;

  uint64_t result = eval_ast(root, &ctx, target_depth, 0x11, 0x22, 0x33, 0x44, 0x55);
  printf("Result: %lu, Ops: %lu\n", result, ctx.operations);
  return 0;
}
