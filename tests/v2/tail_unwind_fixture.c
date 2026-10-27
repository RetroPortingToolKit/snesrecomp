#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "common_cpu_infra.h"

#define RECOMP_STACK_DEPTH 64
int g_recomp_stack_top;
const char *g_recomp_stack[RECOMP_STACK_DEPTH];
int snes_frame_counter;
/* PRODUCTION_CONTEXT */
/* PRODUCTION_RESOLVERS */

static int remaining_tails, return_kind;
static const int sentinel = RECOMP_RETURN_LLE_UNWIND_BASE - 7;

void RecompStackPop(void) { --g_recomp_stack_top; }

static void enter_frame(uint16_t entry_s, uint8_t hrv) {
    int slot = g_recomp_stack_top++;
    g_recomp_stack[slot] = "synthetic";
    g_cpu_entry_s[slot] = entry_s;
    g_cpu_entry_s_valid[slot] = 1;
    g_cpu_entry_return_frame[slot] = hrv;
    g_cpu_entry_tailcall[slot] = 0;
}

static RecompReturn tail_owner(CpuState *cpu) {
    uint16_t _entry_s = g_cpu_entry_s[g_recomp_stack_top - 1];
    uint8_t _hrv = 2;
    /* GENERATED_TAIL */
}

RecompReturn TailTarget_M1X1(CpuState *cpu) {
    uint16_t entry_s = cpu->S;
    uint8_t hrv = cpu->host_return_valid;
    enter_frame(entry_s, hrv);
    if (!cpu_take_tailcall_return_context(&entry_s, &hrv)) abort();
    g_cpu_entry_s[g_recomp_stack_top - 1] = entry_s;
    if (--remaining_tails > 0) return tail_owner(cpu);
    RecompReturn result = RECOMP_RETURN_NORMAL;
    switch (return_kind) {
    case 0: cpu->S = 0x0197; break; /* RTS of inherited frame. */
    case 1: /* PLA; RTS skips caller: resolve before RTS. */
        cpu->S = 0x0199;
        result = (RecompReturn)cpu_resolve_ancestor_skip(0x0197);
        break;
    case 2: /* Interpreter already consumed the same RTS. */
        cpu->S = 0x0199;
        result = (RecompReturn)cpu_resolve_post_return_skip(cpu->S);
        break;
    case 3: result = (RecompReturn)sentinel; break;
    }
    RecompStackPop();
    return result;
}

int main(void) {
    int failures = 0;
    for (int tails = 1; tails <= 3; ++tails) {
        for (return_kind = 0; return_kind <= 3; ++return_kind) {
            CpuState cpu = {0}; cpu.S = 0x0195;
            g_recomp_stack_top = 0;
            enter_frame(0x0199, 3); /* Root: JSL. */
            enter_frame(0x0197, 2); /* Caller: JSR. */
            enter_frame(0x0195, 2); /* Tail owner: JSR. */
            remaining_tails = tails;
            RecompReturn result = tail_owner(&cpu);
            int expected = return_kind == 0 ? 0 : return_kind == 3 ? sentinel : 1;
            if ((int)result != expected || g_recomp_stack_top != 2) {
                fprintf(stderr, "tails=%d kind=%d result=%d expected=%d depth=%d\n",
                        tails, return_kind, result, expected, g_recomp_stack_top);
                ++failures;
            }
            if (return_kind == 3) continue;
            /* An ordinary JSR caller consumes one SKIP; a tail must not. */
            if (result) result = (RecompReturn)((int)result - 1);
            else cpu.S += 2;
            RecompStackPop();
            if (result != RECOMP_RETURN_NORMAL) ++failures;
            else cpu.S += 3; /* Root continuation executes RTL exactly once. */
            RecompStackPop();
            if (cpu.S != 0x019C || g_recomp_stack_top != 0) ++failures;
        }
    }
    /* A swallowed context must not turn an HLE attribution frame into an
     * eligible guest ancestor, and a consumed context must not leak. */
    g_recomp_stack_top = 0;
    enter_frame(0x0199, 3);
    cpu_tailcall_inherit_return_context(0x0199, 3);
    if (!cpu_take_tailcall_return_context(NULL, NULL)) ++failures;
    if (g_cpu_entry_s_valid[0] || g_cpu_entry_tailcall[0]) ++failures;
    uint16_t entry_s = 0; uint8_t hrv = 0;
    if (cpu_take_tailcall_return_context(&entry_s, &hrv)) ++failures;
    printf("12 tail-chain scenarios, %d failures\n", failures);
    return failures ? 1 : 0;
}
