#pragma once

// Translator choices fixed at build time.  The code for the other side of a
// choice is kept because removing it would be a large change; the compiler
// drops it.

// FMA contraction (the IR's pass_fma and pass_fma_reduce, the
// fld_arith_arithp fusion's FMA path and the arith_faddp peephole): off.
// Real x87 rounds the product to the precision-control width before the
// add, and Windows processes run at PC=53, where a separate double multiply
// and add is bit-exact and a fused one is not.
inline constexpr bool kFmaContract = false;
