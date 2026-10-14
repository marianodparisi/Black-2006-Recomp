/* Executes the exact extracted authored camera publisher with synthetic RAM.
 * No game/process/window/input/assets/save access. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fenv.h>
#include <xmmintrin.h>

#define XBOX_CONTIG_BASE 0x80000000u
#define XBOX_CONTIG_SIZE 0x04000000u
#define RAM_BYTES 0x00800000u
static unsigned char ram[RAM_BYTES], contiguous[XBOX_CONTIG_SIZE];
static int primary_available=1, physical_available=1;
static unsigned checks, publishes, observed_csr;
static uint32_t observed_x, observed_y;
static void *xbox_GetMemoryBase(void) { return primary_available ? ram : NULL; }
static size_t xbox_GetMappedSize(void) { return RAM_BYTES; }
static void *xbox_GetPhysicalMemoryBase(void) { return physical_available ? contiguous : NULL; }
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static uint32_t *guest_word(uint32_t address)
{
    if (address >= 0x10000u && (uint64_t)address+4u <= RAM_BYTES && primary_available)
        return (uint32_t *)(ram+address);
    if (address >= XBOX_CONTIG_BASE && (uint64_t)address+4u <= (uint64_t)XBOX_CONTIG_BASE+XBOX_CONTIG_SIZE && physical_available)
        return (uint32_t *)(contiguous+address-XBOX_CONTIG_BASE);
    fprintf(stderr,"Unexpected guest read %08X\n",address); exit(1);
}
#define GUEST32(p) (*guest_word(p))
#define BLACK_VA(va) (va) /* fixtures use the canonical (NTSC) layout */
static void kgpu_set_ao_camera(float x,float y)
{
    ++publishes; memcpy(&observed_x,&x,4); memcpy(&observed_y,&y,4);
    observed_csr=_mm_getcsr();
    /* Deliberately disturb the masked host environment to expose restore gaps. */
    fesetround(FE_DOWNWARD); _mm_setcsr(0xFFBFu);
}
#include "camera.extracted.inc"

static void reset(void)
{
    fesetenv(FE_DFL_ENV); _mm_setcsr(0x1F80u);
    /* Only the fixed renderer root needs resetting; untouched synthetic RAM
     * keeps the 3,072 control/status combinations cheap to run. */
    *(uint32_t *)(ram+0x002D67F4u)=0;
    primary_available=physical_available=1; publishes=0; observed_x=observed_y=0;
}
static void prepare(uint32_t renderer,uint32_t x,uint32_t y)
{
    GUEST32(0x002D67F4u)=renderer;
    GUEST32(renderer+0xD498u)=x; GUEST32(renderer+0xD49Cu)=y;
}
static void check_call(uint32_t camera,unsigned csr,int expected_publishes,uint32_t x,uint32_t y)
{
    fenv_t before,after;
    unsigned returned;
    int same_environment;
    fesetenv(FE_DFL_ENV); _mm_setcsr(csr); memset(&before,0,sizeof(before)); fegetenv(&before);
    black_ao_camera_publish(camera);
    returned=_mm_getcsr(); memset(&after,0,sizeof(after)); fegetenv(&after);
    same_environment=memcmp(&before,&after,sizeof(before))==0;
    /* Compare only after removing pending unmasked state from the test itself. */
    fesetenv(FE_DFL_ENV); _mm_setcsr(0x1F80u);
    CHECK(returned==csr); CHECK(same_environment); CHECK(publishes==(unsigned)expected_publishes);
    if(expected_publishes) { CHECK(observed_x==x); CHECK(observed_y==y); CHECK(observed_csr==0x1F80u); }
}

int main(int argc,char **argv)
{
    const uint32_t valid_x=0x3F3340CDu, valid_y=0x3F06667Bu;
    const uint32_t invalid[]={0u,0x80000000u,0xBF800000u,0x7F800000u,0xFF800000u,0x7FC00001u,0x7F800001u,0xFFFFFFFFu};
    const uint32_t guarded[]={0u,0xFFFCu,0xFFFFu,0xFFFFFFF0u,RAM_BYTES-0xD4EFu,0x84000000u-0xD4EFu,0xF0000000u};
    const uint32_t renderers[]={0x00300000u,XBOX_CONTIG_BASE+0x00020000u,RAM_BYTES-0xD4F0u,XBOX_CONTIG_BASE+XBOX_CONTIG_SIZE-0xD4F0u};
    unsigned i,j,rounding,flags,mode;

    if(argc>1 && strcmp(argv[1],"trace")==0) {
#ifdef _WIN32
        _putenv("RECOMP_AO_CAMERA_TRACE=1");
#else
        putenv("RECOMP_AO_CAMERA_TRACE=1");
#endif
    }
    for(i=0;i<sizeof(renderers)/sizeof(*renderers);++i) {
        for(rounding=0;rounding<4;++rounding) for(flags=0;flags<64;++flags) for(mode=0;mode<3;++mode) {
            unsigned csr=0x1F80u|(rounding<<13)|flags;
            if(mode==1) csr|=0x8040u; /* FTZ+DAZ */
            if(mode==2) csr&=~0x1000u; /* precision unmasked, including pending precision */
            reset();prepare(renderers[i],valid_x,valid_y);
            check_call(renderers[i]+0xD450u,csr,1,valid_x,valid_y);
        }
    }
    for(i=0;i<sizeof(invalid)/sizeof(*invalid);++i) for(j=0;j<2;++j) {
        reset();prepare(0x00300000u,j?valid_x:invalid[i],j?invalid[i]:valid_y);
        check_call(0x0030D450u,0x0FE0u,1,0,0);
    }
    for(i=0;i<sizeof(guarded)/sizeof(*guarded);++i) {
        reset();GUEST32(0x002D67F4u)=guarded[i];check_call(guarded[i]+0xD450u,0x1FBFu,0,0,0);
    }
    reset();prepare(0x00300000u,valid_x,valid_y);check_call(0x0030D4F0u,0x1FBFu,0,0,0); /* secondary */
    reset();prepare(0x00300000u,valid_x,valid_y);check_call(0x0030D490u,0x1FBFu,0,0,0); /* UI */
    reset();primary_available=0;check_call(0x0030D450u,0x1FBFu,0,0,0);
    reset();prepare(XBOX_CONTIG_BASE+0x20000u,valid_x,valid_y);physical_available=0;check_call(XBOX_CONTIG_BASE+0x2D450u,0x1FBFu,0,0,0);
    /* Subnormals/small or huge positive finite values stay safe in publisher;
     * renderer setter independently rejects its geometric bounds. */
    reset();prepare(0x00300000u,1u,0x7F7FFFFFu);check_call(0x0030D450u,0x0FE0u,1,1u,0x7F7FFFFFu);
    printf("{\"status\":\"PASS\",\"checks\":%u,\"configurations\":3072,\"trace\":%s,\"trueX87PendingPrecision\":false}\n",checks,argc>1?"true":"false");
    return 0;
}
