/* Reuse the established flat-RAM bridge fixture, replacing only the fake
 * interrupt scope and AOT entry. The production interpreter and bridge are
 * linked unchanged; no commercial ROM or generated game code is required. */
#define main unused_bridge_fixture_main
#define cpu_interrupt_context_enter unused_interrupt_enter
#define cpu_interrupt_context_leave unused_interrupt_leave
#define cpu_interrupt_context_active unused_interrupt_active
#define cpu_dispatch_pc_paired unused_dispatch_paired
#include "bridge_test.c"
#undef main
#undef cpu_interrupt_context_enter
#undef cpu_interrupt_context_leave
#undef cpu_interrupt_context_active
#undef cpu_dispatch_pc_paired
#include "sdd1.h"

static int interrupt_depth;
static RecompReturn bounce_result;
void cpu_interrupt_context_enter(void) { interrupt_depth++; }
void cpu_interrupt_context_leave(void) { interrupt_depth--; }
int cpu_interrupt_context_active(void) { return interrupt_depth != 0; }

RecompReturn cpu_dispatch_pc_paired(CpuState *cpu, uint32 pc24,
                                   uint8 frame_size) {
    CHECK(pc24 == FAKE_AOT, "unexpected AOT target %06X", pc24);
    g_aot_called++;
    cpu->host_return_valid = frame_size;
    /* Model a generated subroutine's unresolved tail into an RTS/RTL suffix. */
    bounce_result = interp_tier_dispatch_tail(cpu, 0x008200, 0x008100,
                                              cpu->S, frame_size);
    return bounce_result;
}

static void prepare_interrupt(void) {
    memset(RAM, 0, MEMSZ);
    init_cpu();
    g_c.emulation = 0;
    g_c.S = 0x01FB;
    cpu_mirrors_to_p(&g_c);
    /* Native NMI frame: P, PC.lo, PC.hi, PB; RTI resumes at $00:9000. */
    RAM[0x01FC] = g_c.P;
    RAM[0x01FD] = 0x00;
    RAM[0x01FE] = 0x90;
    RAM[0x01FF] = 0x00;
    g_aot_called = 0;
    interrupt_depth = 0;
    bounce_result = RECOMP_RETURN_NORMAL;
}

int main(void) {
    RAM = malloc(MEMSZ);
    if (!RAM) return 2;
    for (int frame_size = 2; frame_size <= 3; frame_size++) {
        const uint8_t suffix[] = {0xE6, 0x10, frame_size == 2 ? 0x60 : 0x6B};
        const uint8_t continuation[] = {0xE6, 0x11, 0x40};

        /* An AOT NMI caller still owns its host frame: the fallback must stop
         * at the subroutine return and leave the NMI continuation untouched. */
        prepare_interrupt();
        load(0x8200, suffix, sizeof suffix);
        load(0x8300, continuation, sizeof continuation);
        if (frame_size == 3) RAM[g_c.S--] = 0x00;
        RAM[g_c.S--] = 0x82;
        RAM[g_c.S--] = 0xFF;  /* RTS/RTL -> $8300 */
        cpu_interrupt_context_enter();
        RecompReturn result = interp_tier_dispatch_tail(
            &g_c, 0x008200, 0x008100, g_c.S, (uint8)frame_size);
        printf("direct AOT NMI subroutine, frame=%d\n", frame_size);
        CHECK(result == RECOMP_RETURN_NORMAL, "return=%d", result);
        CHECK(g_c.S == 0x01FB, "subroutine S=%04X expected 01FB", g_c.S);
        CHECK(RAM[0x10] == 1, "suffix did not run once");
        CHECK(RAM[0x11] == 0, "subroutine consumed NMI continuation");
        CHECK(interrupt_depth == 1, "interrupt scope was not preserved");
        cpu_interrupt_context_leave();

        /* An interpreter-owned NMI bounces into AOT, which tails back to the
         * suffix. The owning interpreter must receive an unwind and execute
         * the subroutine return followed by its own continuation and RTI. */
        prepare_interrupt();
        load(0x8200, suffix, sizeof suffix);
        const uint8_t near_handler[] = {0x20, 0x00, 0x81, 0xE6, 0x11, 0x40};
        const uint8_t far_handler[] = {0x22, 0x00, 0x81, 0x00, 0xE6, 0x11, 0x40};
        if (frame_size == 2) load(0x8000, near_handler, sizeof near_handler);
        else load(0x8000, far_handler, sizeof far_handler);
        result = interp_tier_dispatch_interrupt(&g_c, 0x008000);
        printf("interpreter-owned NMI bounce, frame=%d\n", frame_size);
        CHECK(result == RECOMP_RETURN_NORMAL, "return=%d", result);
        CHECK(bounce_result == RECOMP_RETURN_LLE_UNWIND_BASE,
              "tail resumed compiled caller instead of unwinding: %d", bounce_result);
        CHECK(g_aot_called == 1, "AOT calls=%d expected 1", g_aot_called);
        CHECK(RAM[0x10] == 1 && RAM[0x11] == 1,
              "suffix/continuation counts=%d/%d expected 1/1", RAM[0x10], RAM[0x11]);
        CHECK(g_c.S == 0x01FF, "NMI S=%04X expected 01FF", g_c.S);
        CHECK(interrupt_depth == 0, "interrupt scope leaked");
    }

    /* An interrupt root with no paired subroutine frame still exits by RTI. */
    prepare_interrupt();
    RAM[0x8200] = 0x40;
    cpu_interrupt_context_enter();
    RecompReturn result = interp_tier_dispatch_tail(
        &g_c, 0x008200, 0x008100, g_c.S, 0);
    CHECK(result == RECOMP_RETURN_NORMAL, "interrupt root return=%d", result);
    CHECK(g_c.S == 0x01FF, "interrupt root S=%04X expected 01FF", g_c.S);
    CHECK(interrupt_depth == 1, "interrupt root changed enclosing scope");
    cpu_interrupt_context_leave();
    free(RAM);
    printf("%d checks, %d failures\n", g_check, g_fail);
    return g_fail ? 1 : 0;
}
