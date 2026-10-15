/* Link the production runtime and exercise its shared intrinsic/clock ABI.
 * No retail files, generated game code, graphics or audio device are needed. */
#include <stdio.h>
#include <windows.h>
#include "qemu_shim.h"
#include "dsp56300.h"

int main(void)
{
    LARGE_INTEGER frequency, counter;
    uint32_t program[16] = { 0 }; /* DSP NOP instruction */
    Dsp56300MemoryRegion region = { 0 };
    Dsp56300CreateInfo info = { 0 };
    Dsp56300State state = { 0 };
    Dsp56300Jit *dsp;
    if (ctz32(8u) != 3 || __builtin_clz(8u) != 28) return 1;
    if (muldiv64(123456789ULL, 1000u, 100u) != 1234567890ULL) return 2;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&counter)) return 3;
    region.end = 16;
    region.kind = DSP56300_REGION_BUFFER;
    region.data.buffer.base = program;
    info.memory_map.p_regions = &region;
    info.memory_map.p_count = 1;
    dsp = dsp56300_create(&info);
    if (!dsp) return 4;
    dsp56300_step(dsp);
    dsp56300_get_state(dsp, &state);
    if (state.pc != 1) { dsp56300_destroy(dsp); return 5; }
    dsp56300_destroy(dsp);
    puts("PASS: Linux-built runtime intrinsics, clock and Rust DSP JIT ABI.");
    return 0;
}
