#include "render_fixes.h"

/* Address after the replaced "FLD [ECX+4]; FSTP [EAX+ESI+4]". */
static void *g_trail_vertex_resume;

void trail_vertex_set_resume(void *resume) {
    g_trail_vertex_resume = resume;
}

/*
 * Jumped to (not called) from the trail vertex builder. Registers are the
 * builder's: [EBP-4] = points left (n - 1 - i), [EBP-0x24] = float(n - 1),
 * EDX = module data (+0xC = texture V scale), EAX + ESI + 4 = the second
 * vertex's V. These are the same instructions the builder used for the first
 * vertex's V, so the value is bit-identical; XMM0 is dead here (the loop
 * reloads it).
 */
__attribute__((naked)) void trail_vertex_v_stub(void) {
    __asm__ volatile(
        "cvtsi2ssl -4(%%ebp), %%xmm0\n\t"
        "divss -0x24(%%ebp), %%xmm0\n\t"
        "mulss 0xc(%%edx), %%xmm0\n\t"
        "movss %%xmm0, 4(%%eax,%%esi)\n\t"
        "jmp *%c0"
        :
        : "i"(&g_trail_vertex_resume));
}
