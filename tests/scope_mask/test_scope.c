/* Production source extracted verbatim, synthetic relocated resource graph. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <fenv.h>
#include <stdarg.h>
#include <xmmintrin.h>
#define XBOX_CONTIG_BASE 0x80000000u
#define XBOX_CONTIG_SIZE 0x04000000u
#define RAM_SIZE 0x600000u
static unsigned char ram[RAM_SIZE], before[RAM_SIZE];
static ptrdiff_t g_xbox_mem_offset;
static uint32_t g_esp, black_aspect_bits, registers[10];
static uint32_t guest_xmm[8][4], guest_fp_top;
static double guest_fp_stack[8];
static unsigned calls, checks, returned_csr;
static uint32_t seen[6], seen_shape, seen_mode, seen_return;
static void *xbox_GetMemoryBase(void) { return ram; }
static size_t xbox_GetMappedSize(void) { return RAM_SIZE; }
static void *xbox_GetPhysicalMemoryBase(void) { return NULL; }
static void black_aspect_refresh(void) { }
#define GUEST32(a) (*(uint32_t *)(ram + (a)))
#define BLACK_VA(va) (va) /* fixtures use the canonical (NTSC) layout */
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d %s\n",__LINE__,#x); exit(1); } } while(0)
enum { UI=0x20000, RENDERER=0x30000, RESOURCE=0x70000, TABLE=0x70100,
       ENTRY=0x70200, NAME=0x70300, BLOCK=0x70400, CONSTANTS=0x70500,
       DATA=0x70600, MOVIE=0x71000, CHARS=0x71100, LEAF=0x71200,
       PACK=0x72000, GEOMETRIES=0x72100, SHAPE=0x73000,
       SUBSHAPE=0x73100, VERTICES=0x73200, POINTS=0x73300, STACK=0x80000 };
static unsigned trace_prints;
static char trace_output[32768];
static int trace_fprintf(FILE *stream, const char *format, ...)
{
    char text[4096];
    va_list args;
    int result;
    size_t used = strlen(trace_output), available = sizeof(trace_output)-used-1;
    (void)stream;
    va_start(args,format); result=vsnprintf(text,sizeof(text),format,args); va_end(args);
    if (result>0 && available) strncat(trace_output,text,available);
    ++trace_prints;
    /* Deliberately disturb libc's host FP state to prove the tracer restores it. */
    fesetround(FE_DOWNWARD); _mm_setcsr(0x3FA1u);
    return result;
}
#define fprintf trace_fprintf
#include "scope.extracted.inc"
#undef fprintf

static uint32_t bits(float value) { uint32_t result; memcpy(&result,&value,4); return result; }
static float value(uint32_t word) { float result; memcpy(&result,&word,4); return result; }
static void initialize(uint32_t aspect, int side)
{
    static const float quad[12]={0,8,0,0,8,0,8,0,8,8,0,8};
    unsigned i;
    float offset=aspect==0x40155555u ? 240.0f : aspect==0x40638E39u ? 533.333313f : 106.666664f;
    memset(ram,0,sizeof(ram));
    black_aspect_bits=aspect; g_esp=STACK; calls=0;
    GUEST32(0x2D199Cu)=UI; GUEST32(0x2D67F4u)=RENDERER;
    ram[RENDERER+12]=1;
    GUEST32(0x5629A4u)=bits(640); GUEST32(0x5629C8u)=bits(480);
    GUEST32(UI+0x36B8u+11u*4u)=RESOURCE;
    GUEST32(RESOURCE+8)=1; GUEST32(RESOURCE+12)=TABLE; GUEST32(TABLE)=ENTRY;
    GUEST32(ENTRY)=NAME; memcpy(ram+NAME,"sniperhud",10); GUEST32(ENTRY+4)=BLOCK;
    GUEST32(BLOCK+12)=DATA; GUEST32(BLOCK+16)=CONSTANTS; GUEST32(BLOCK+24)=PACK;
    GUEST32(CONSTANTS+20)=MOVIE-DATA; GUEST32(MOVIE)=9;
    GUEST32(MOVIE+20)=39; GUEST32(MOVIE+24)=CHARS; GUEST32(CHARS)=MOVIE;
    GUEST32(CHARS+25*4)=LEAF; GUEST32(LEAF)=1; GUEST32(LEAF+4)=MOVIE;
    GUEST32(LEAF+16)=bits(8); GUEST32(LEAF+20)=bits(8); GUEST32(LEAF+24)=SHAPE;
    GUEST32(PACK)=0x7244EA15u; GUEST32(PACK+4)=0x96F6A8E2u;
    GUEST32(PACK+8)=10; GUEST32(PACK+12)=GEOMETRIES;
    GUEST32(GEOMETRIES+8*16+4)=0x1E449A94u; GUEST32(GEOMETRIES+8*16+8)=SHAPE;
    GUEST32(SHAPE)=SUBSHAPE; GUEST32(SHAPE+4)=1;
    GUEST32(SUBSHAPE)=0x80000000u; GUEST32(SUBSHAPE+8)=VERTICES;
    GUEST32(VERTICES)=POINTS; GUEST32(VERTICES+4)=6;
    memcpy(ram+POINTS,quad,sizeof(quad));
    /* Native registration keeps shape and glyph callbacks in distinct slots. */
    GUEST32(0x562BD8u)=0x00121B50u; GUEST32(0x562BA8u)=0x00121BC0u;
    GUEST32(STACK)=0x00102D67u; GUEST32(STACK+4)=SHAPE;
    GUEST32(UI+0x3740)=0x41DBFFF0u; GUEST32(UI+0x374C)=0x427A0090u;
    GUEST32(UI+0x3750)=bits((side ? 571.0f : -154.0f)+offset);
    GUEST32(UI+0x3754)=0xC1200000u;
    for(i=0;i<10;++i) registers[i]=0x11223300u+i;
    for(i=0;i<32;++i) guest_xmm[i/4][i%4]=0x7F800001u+i;
    for(i=0;i<8;++i) guest_fp_stack[i]=(double)i+0.25;
    guest_fp_top=7;
}

void sub_00121B50_gen(void)
{
    unsigned i;
    ++calls;
    seen_return=GUEST32(g_esp); seen_shape=GUEST32(g_esp+4); seen_mode=GUEST32(g_esp+8);
    memcpy(seen,ram+UI+0x3740,sizeof(seen));
    for(i=0;i<10;++i) registers[i]^=0x98765432u;
    guest_xmm[3][2]^=0x12345678u; guest_fp_stack[2]=99.5; guest_fp_top=3;
    g_esp+=4;
    /* Original draw owns its returned FP status; restoration must keep it. */
    returned_csr=(_mm_getcsr() & ~0x6000u) | 0x4000u | 0x20u;
    _mm_setcsr(returned_csr);
}

/* Authored adapter for the recovered cdecl shape dispatch contract: mode,
 * geometry, return address are pushed in order, through the shape slot;
 * the original pops its return and the caller removes the two arguments.
 * No translated game body is included in this fixture. */
static void shape_callback_dispatch(void)
{
    uint32_t shape=GUEST32(STACK+4), mode=GUEST32(STACK+8);
    g_esp=STACK+12;
    g_esp-=4; GUEST32(g_esp)=mode;
    g_esp-=4; GUEST32(g_esp)=shape;
    g_esp-=4; GUEST32(g_esp)=0x00102D67u;
    if(GUEST32(0x562BD8u)==0x00121B50u) sub_00121B50();
    else if(GUEST32(0x562BD8u)==0x00121BC0u) sub_00121B50_gen(); /* wrong sibling: no policy */
    else CHECK(0);
    CHECK(g_esp==STACK+4);
    g_esp+=8;
}

static void valid_cases(void)
{
    const uint32_t aspects[2]={0x40155555u,0x40638E39u};
    unsigned a,s,r,i;
    for(a=0;a<2;++a) for(s=0;s<2;++s) for(r=0;r<4;++r) {
        const float width=a ? 1706.666626f : 1120.0f;
        uint32_t ui=0,saved[2],old_csr;
        float inner;
        fenv_t env_before,env_after;
        _mm_setcsr(0x1F80u); initialize(aspects[a],s);
        inner=value(GUEST32(UI+0x3750))+(s ? 0 : 8*value(GUEST32(UI+0x3740)));
        memcpy(before,ram,sizeof(ram));
        old_csr=0x1F80u | (r<<13) | 0x21u | 0x8040u;
        _mm_setcsr(old_csr); fegetenv(&env_before);
        CHECK(black_scope_mask_begin(STACK,&ui,saved));
        fegetenv(&env_after);
        CHECK(ui==UI && _mm_getcsr()==old_csr && !memcmp(&env_before,&env_after,sizeof(env_before)));
        CHECK(guest_fp_top==7 && guest_fp_stack[2]==2.25 && guest_xmm[3][2]==0x7F80000Fu && g_esp==STACK);
        CHECK(fabs((double)(value(GUEST32(UI+0x3750))+(s ? 0 : 8*value(GUEST32(UI+0x3740))))-inner)<0.0002);
        CHECK(s ? value(GUEST32(UI+0x3750))+8*value(GUEST32(UI+0x3740))>=width+0.999f : value(GUEST32(UI+0x3750))<=-1);
        GUEST32(UI+0x3740)=saved[0]; GUEST32(UI+0x3750)=saved[1];
        CHECK(!memcmp(before,ram,sizeof(ram)));
        shape_callback_dispatch();
        CHECK(calls==1 && g_esp==STACK+12 && _mm_getcsr()==returned_csr);
        CHECK(seen_shape==SHAPE && seen_mode==0 && seen_return==0x00102D67u);
        CHECK(!memcmp(before,ram,sizeof(ram)));
        CHECK(seen[0]!=saved[0] && seen[1]==0 && seen[2]==0 && seen[3]==GUEST32(UI+0x374C) && seen[5]==GUEST32(UI+0x3754));
        CHECK(fabs((double)(value(seen[4])+(s ? 0 : 8*value(seen[0])))-inner)<0.0002);
        CHECK(s ? value(seen[4])+8*value(seen[0])>=width+0.999f : value(seen[4])<=-1);
        CHECK(guest_fp_top==3 && guest_fp_stack[2]==99.5 && guest_xmm[3][2]==(0x7F80000Fu^0x12345678u));
        for(i=0;i<10;++i) CHECK(registers[i]==((0x11223300u+i)^0x98765432u));
    }
}

static void callback_route_cases(void)
{
    _mm_setcsr(0x1F80u); initialize(0x40155555u,0);
    /* The identical sibling ABI is insufficient: the actual native shape
     * slot must resolve the authored wrapper. This negative route retains
     * the old uncovered left edge and would fail valid_cases' coverage. */
    GUEST32(0x562BD8u)=0x00121BC0u;
    memcpy(before,ram,sizeof(ram)); shape_callback_dispatch();
    CHECK(calls==1 && g_esp==STACK+12 && _mm_getcsr()==returned_csr);
    CHECK(!memcmp(before,ram,sizeof(ram)) && !memcmp(seen,ram+UI+0x3740,sizeof(seen)));
    CHECK(value(seen[4])>0 && seen_shape==SHAPE && seen_mode==0);
}

static void rejected_cases(void)
{
    const uint32_t aspects[4]={0,0x3FAAAAABu,0x3FE38E39u,0x40155555u};
    unsigned test;
    for(test=0;test<33;++test) {
        uint32_t ui=0,saved[2],csr;
        _mm_setcsr(0x1F80u); initialize(test<3 ? aspects[test] : aspects[3],0);
        switch(test) {
        case 3: ram[NAME]='X'; break;
        case 4: ram[NAME+9]='X'; break;
        case 5: GUEST32(RESOURCE+8)=2; break;
        case 6: GUEST32(TABLE)=RAM_SIZE-4; break;
        case 7: GUEST32(ENTRY+4)=UINT32_MAX; break;
        case 8: GUEST32(BLOCK+16)=RAM_SIZE-20; break;
        case 9: GUEST32(CONSTANTS+20)=UINT32_MAX; break;
        case 10: GUEST32(MOVIE)=5; break;
        case 11: GUEST32(MOVIE+20)=UINT32_MAX; break;
        case 12: GUEST32(CHARS)=LEAF; break;
        case 13: GUEST32(CHARS+25*4)=RAM_SIZE-24; break;
        case 14: GUEST32(LEAF)=6; break;
        case 15: GUEST32(LEAF+4)=MOVIE+4; break;
        case 16: GUEST32(LEAF+24)=SHAPE+4; break;
        case 17: GUEST32(PACK+8)=UINT32_MAX; break;
        case 18: GUEST32(GEOMETRIES+8*16+4)^=1; break;
        case 19: GUEST32(GEOMETRIES+8*16+8)=SHAPE+4; break;
        case 20: GUEST32(SUBSHAPE)=0xFFFFFFFFu; break;
        case 21: GUEST32(SUBSHAPE+4)=1; break;
        case 22: GUEST32(VERTICES+4)=5; break;
        case 23: GUEST32(POINTS+8)=0x7F800001u; break;
        case 24: GUEST32(UI+0x3740)=0x7F800001u; break;
        case 25: GUEST32(UI+0x3750)=0x7F800001u; break;
        case 26: GUEST32(UI+0x3744)=0x80000000u; break;
        case 27: GUEST32(UI+0x3754)=0; break;
        case 28: GUEST32(STACK+8)=1; break;
        case 29: ram[RENDERER+12]=0; break;
        case 30: GUEST32(0x2D199Cu)=RAM_SIZE-0x379Fu; break;
        case 31: GUEST32(0x5629A4u)=0x7F800001u; break;
        case 32: GUEST32(STACK+4)=UI+0x3740u; break;
        }
        memcpy(before,ram,sizeof(ram)); csr=0xBFC1u; _mm_setcsr(csr);
        CHECK(!black_scope_mask_begin(STACK,&ui,saved));
        CHECK(_mm_getcsr()==csr && !memcmp(before,ram,sizeof(ram)));
    }
    for(test=0;test<4;++test) {
        _mm_setcsr(0x1F80u); initialize(aspects[test],test&1);
        if(test==3) ram[NAME]='X';
        memcpy(before,ram,sizeof(ram)); shape_callback_dispatch();
        CHECK(calls==1 && !memcmp(before,ram,sizeof(ram)));
        CHECK(!memcmp(seen,ram+UI+0x3740,sizeof(seen)));
    }
}

static void trace_environment(const char *value)
{
#ifdef _WIN32
    CHECK(!_putenv_s("RECOMP_BLACK_SCOPE_MASK_TRACE",value));
#else
    CHECK(!setenv("RECOMP_BLACK_SCOPE_MASK_TRACE",value,1));
#endif
    black_scope_trace_enabled=-1;
}

static void diagnostic_cases(void)
{
    unsigned i, printed;
    uint32_t csr=0xBFC1u, saved_registers[10], saved_xmm[8][4], saved_top;
    double saved_fp[8];
    fenv_t environment, after_environment;
    const char *disabled[3]={"","0","true"};
    _mm_setcsr(0x1F80u); initialize(0x40155555u,0);
    black_scope_trace_records=0; trace_prints=0; trace_output[0]=0;
    black_scope_trace_entries=black_scope_trace_slot_records=black_scope_trace_tall_records=0;
    for(i=0;i<3;++i) {
        trace_environment(disabled[i]); _mm_setcsr(csr); fegetenv(&environment);
        black_scope_mask_trace(STACK); fegetenv(&after_environment);
        CHECK(!trace_prints && !black_scope_trace_records && _mm_getcsr()==csr);
        CHECK(!black_scope_trace_entries && !black_scope_trace_slot_records && !black_scope_trace_tall_records);
        CHECK(!memcmp(&environment,&after_environment,sizeof(environment)));
    }
    trace_environment("1"); _mm_setcsr(csr);
    ram[NAME]='X'; black_scope_mask_trace(STACK);
    CHECK(trace_prints && !black_scope_trace_records);
    CHECK(strstr(trace_output,"[BLACK_SCOPE_ENTRY]") && strstr(trace_output,"[BLACK_SCOPE_SLOTS]"));
    CHECK(black_scope_trace_entries==1 && black_scope_trace_slot_records==1 && black_scope_trace_tall_records==1);
    CHECK(_mm_getcsr()==csr);
    ram[NAME]='s';
    /* Changed matrix is reported without extending the synthetic draw. */
    GUEST32(UI+0x3740)^=1u; GUEST32(UI+0x3750)^=1u;
    memcpy(before,ram,sizeof(ram)); memcpy(saved_registers,registers,sizeof(registers));
    memcpy(saved_xmm,guest_xmm,sizeof(guest_xmm)); memcpy(saved_fp,guest_fp_stack,sizeof(saved_fp));
    saved_top=guest_fp_top; fegetenv(&environment);
    black_scope_mask_trace(STACK); fegetenv(&after_environment);
    CHECK(black_scope_trace_records==1 && trace_prints>0);
    CHECK(strstr(trace_output,"reject=140") && strstr(trace_output,"affine=") &&
          strstr(trace_output,"leaf25=") && strstr(trace_output,"geometryItems="));
    CHECK(_mm_getcsr()==csr && !memcmp(&environment,&after_environment,sizeof(environment)));
    CHECK(!memcmp(before,ram,sizeof(ram)) && !memcmp(saved_registers,registers,sizeof(registers)));
    CHECK(!memcmp(saved_xmm,guest_xmm,sizeof(guest_xmm)) && !memcmp(saved_fp,guest_fp_stack,sizeof(saved_fp)));
    CHECK(guest_fp_top==saved_top && g_esp==STACK);
    black_scope_trace_entries=4; printed=trace_prints; black_scope_mask_trace(STACK);
    CHECK(trace_prints==printed && black_scope_trace_records==1);
    /* Invalid relocated pointers produce bounded raw evidence, never a read. */
    GUEST32(SUBSHAPE+8)=UINT32_MAX; GUEST32(UI+0x3750)^=2u;
    black_scope_mask_trace(STACK);
    CHECK(black_scope_trace_records==2 && _mm_getcsr()==csr);
    /* Every distinct tuple consumes one slot; subsequent frames cannot flood. */
    for(i=0;i<64;++i) { GUEST32(UI+0x3750)=0x42000000u+i; black_scope_mask_trace(STACK); }
    CHECK(black_scope_trace_records==48 && _mm_getcsr()==csr);
    CHECK(black_scope_trace_tall_records==64);
    for(i=0;i<40;++i) { GUEST32(RESOURCE)=i; black_scope_mask_trace(STACK); }
    CHECK(black_scope_trace_slot_records==32 && _mm_getcsr()==csr);
    printed=trace_prints; GUEST32(UI+0x3750)=0x43000000u; black_scope_mask_trace(STACK);
    CHECK(printed==trace_prints && black_scope_trace_records==48);
    /* The early trace still reports invalid UI/stack independently of the
     * restrictive resource gate, with no invalid access or guest write. */
    black_scope_trace_entries=black_scope_trace_slot_records=0;
    GUEST32(0x2D199Cu)=UINT32_MAX; memcpy(before,ram,sizeof(ram));
    printed=trace_prints; black_scope_mask_trace(UINT32_MAX);
    CHECK(trace_prints>printed && black_scope_trace_entries==1);
    CHECK(!memcmp(before,ram,sizeof(ram)) && _mm_getcsr()==csr);
    /* A new enabled trace still preserves the original callback's returned state. */
    black_scope_trace_records=0; initialize(0x40155555u,0); memcpy(before,ram,sizeof(ram));
    shape_callback_dispatch();
    CHECK(calls==1 && g_esp==STACK+12 && _mm_getcsr()==returned_csr);
    CHECK(!memcmp(before,ram,sizeof(ram)) && seen[4]==0xBF800000u);
    CHECK(guest_fp_top==3 && guest_fp_stack[2]==99.5 && guest_xmm[3][2]==(0x7F80000Fu^0x12345678u));
    trace_environment("");
}

int main(void)
{
    g_xbox_mem_offset=(ptrdiff_t)ram;
    valid_cases(); callback_route_cases(); rejected_cases(); diagnostic_cases(); _mm_setcsr(0x1F80u);
    printf("%u scope mask checks passed\n",checks);
    return 0;
}
