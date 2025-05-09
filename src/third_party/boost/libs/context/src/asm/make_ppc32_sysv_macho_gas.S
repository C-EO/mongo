/*
            Copyright Oliver Kowalke 2009.
   Distributed under the Boost Software License, Version 1.0.
      (See accompanying file LICENSE_1_0.txt or copy at
          http://www.boost.org/LICENSE_1_0.txt)
*/

/******************************************************
 *                                                     *
 *  -------------------------------------------------  *
 *  |  0  |  1  |  2  |  3  |  4  |  5  |  6  |  7  |  *
 *  -------------------------------------------------  *
 *  |  0  |  4  |  8  |  12 |  16 |  20 |  24 |  28 |  *
 *  -------------------------------------------------  *
 *  |    F14    |    F15    |    F16    |    F17    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  8  |  9  |  10 |  11 |  12 |  13 |  14 |  15 |  *
 *  -------------------------------------------------  *
 *  |  32 |  36 |  40 |  44 |  48 |  52 |  56 |  60 |  *
 *  -------------------------------------------------  *
 *  |    F18    |    F19    |    F20    |    F21    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  16 |  17 |  18 |  19 |  20 |  21 |  22 |  23 |  *
 *  -------------------------------------------------  *
 *  |  64 |  68 |  72 |  76 |  80 |  84 |  88 |  92 |  *
 *  -------------------------------------------------  *
 *  |    F22    |    F23    |    F24    |    F25    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  24 |  25 |  26 |  27 |  28 |  29 |  30 |  31 |  *
 *  -------------------------------------------------  *
 *  |  96 | 100 | 104 | 108 | 112 | 116 | 120 | 124 |  *
 *  -------------------------------------------------  *
 *  |    F26    |    F27    |    F28    |    F29    |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  32 |  33 |  34 |  35 |  36 |  37 |  38 |  39 |  *
 *  -------------------------------------------------  *
 *  | 128 | 132 | 136 | 140 | 144 | 148 | 152 | 156 |  *
 *  -------------------------------------------------  *
 *  |    F30    |    F31    |   fpscr   | R13 | R14 |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  40 |  41 |  42 |  43 |  44 |  45 |  46 |  47 |  *
 *  -------------------------------------------------  *
 *  | 160 | 164 | 168 | 172 | 176 | 180 | 184 | 188 |  *
 *  -------------------------------------------------  *
 *  | R15 | R16 | R17 | R18 | R19 | R20 | R21 | R22 |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  48 |  49 |  50 |  51 |  52 |  53 |  54 |  55 |  *
 *  -------------------------------------------------  *
 *  | 192 | 196 | 200 | 204 | 208 | 212 | 216 | 220 |  *
 *  -------------------------------------------------  *
 *  | R23 | R24 | R25 | R26 | R27 | R28 | R29 | R30 |  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  56 |  57 |  58 |  59 |  60 |  61 |  62 |  63 |  *
 *  -------------------------------------------------  *
 *  | 224 | 228 | 232 | 236 | 240 | 244 | 248 | 252 |  *
 *  -------------------------------------------------  *
 *  | R31 |hiddn|  CR |  LR |  PC |bchai|linkr| FCTX|  *
 *  -------------------------------------------------  *
 *  -------------------------------------------------  *
 *  |  64 |                                         |  *
 *  -------------------------------------------------  *
 *  | 256 |                                         |  *
 *  -------------------------------------------------  *
 *  | DATA|                                         |  *
 *  -------------------------------------------------  *
 *                                                     *
 *******************************************************/

.text
.private_extern _make_fcontext
.globl _make_fcontext
.align 2
_make_fcontext:
    ; save return address into R6
    mflr  r6

    ; first arg of make_fcontext() == top address of context-function
    ; shift address in R3 to lower 16 byte boundary
    clrrwi  r3, r3, 4

    ; reserve space for context-data on context-stack
    ; including 64 byte of linkage + parameter area (R1 % 16 == 0)
    subi  r3, r3, 336

    ; third arg of make_fcontext() == address of context-function
    ; store as trampoline's R31
    stw  r5, 224(r3)

    ; set back-chain to zero
    li   r0, 0
    stw  r0, 244(r3)

    mffs  f0  ; load FPSCR
    stfd  f0, 144(r3)  ; save FPSCR

    ; compute address of returned transfer_t
    addi  r0, r3, 252
    mr    r4, r0
    stw   r4, 228(r3)

    ; load LR
    mflr  r0
    ; jump to label 1
    bcl  20, 31, L1
L1:
    ; load LR into R4
    mflr  r4
    ; compute abs address of trampoline, use as PC
    addi  r5, r4, lo16(Ltrampoline - L1)
    stw  r5, 240(r3)
    ; compute abs address of label finish
    addi  r4, r4, lo16(Lfinish - L1)
    ; restore LR
    mtlr  r0
    ; save address of finish as return-address for context-function
    ; will be entered after context-function returns
    stw  r4, 236(r3)

    ; restore return address from R6
    mtlr  r6

    blr  ; return pointer to context-data

Ltrampoline:
    ; We get R31 = context-function, R3 = address of transfer_t,
    ; but we need to pass R3:R4 = transfer_t.
    mtctr  r31
    lwz  r4, 4(r3)
    lwz  r3, 0(r3)
    bctr

Lfinish:
    ; load address of _exit into CTR
    bcl  20, 31, L2
L2:
    mflr  r4
    addis  r4, r4, ha16(Lexitp - L2)
    lwz  r4, lo16(Lexitp - L2)(r4)
    mtctr  r4
    ; exit code is zero
    li  r3, 0
    ; exit application
    bctr

.const_data
.align 2
Lexitp:
    .long  __exit
