#ifndef MULTICORE_RENDER_FIXES_H
#define MULTICORE_RENDER_FIXES_H

#include "common.h"

/*
 * M3a: the trail vertex builder copied each point's V texture coordinate
 * from its first vertex to its second by reading it back from the locked GPU
 * vertex buffer. The stub recomputes V with the game's own instructions
 * instead (bit-identical) and jumps back.
 */
void trail_vertex_set_resume(void *resume);
void trail_vertex_v_stub(void);

#endif
