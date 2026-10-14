/* Synthetic draw queues and shader constants; no game assets or instructions. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fenv.h>
#include <xmmintrin.h>
#ifndef _MSC_VER
#undef __declspec
#define __declspec(x) __thread
#endif
#define XBOX_CONTIG_BASE 0x80000000u
#define XBOX_CONTIG_SIZE 0x04000000u
#define RAM_BYTES 0x00400000u
static unsigned char ram[RAM_BYTES];
static ptrdiff_t g_xbox_mem_offset;
static void *xbox_GetMemoryBase(void) { return ram; }
static size_t xbox_GetMappedSize(void) { return RAM_BYTES; }
static void *xbox_GetPhysicalMemoryBase(void) { return NULL; }
#define GUEST32(a) (*(uint32_t *)(ram+(a)))
#define BLACK_VA(va) (va) /* fixtures use the canonical (NTSC) layout */
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
#include "viewmodel.extracted.inc"

static const uint32_t actor=0x11000u, renderer=0x20000u, upload=0x30000u;
static const float matrix[16]={2,-3,0.5f,0, -4,5,0.25f,-0.0f, 0,0,1,-1, 1,2,3,4};
static void set_scale(float ratio)
{
    uint32_t bits;
    memcpy(&bits,&ratio,4);
    fesetenv(FE_DFL_ENV); _mm_setcsr(0x1F80u);
    black_viewmodel_scale(0x3F800000u,bits);
}
static void expect_upload(float ratio)
{
    unsigned i;
    for(i=0;i<16;++i) {
        float actual, expected=i<8 ? matrix[i]*ratio : matrix[i];
        memcpy(&actual,ram+upload+4*i,4);
        CHECK(memcmp(&actual,&expected,4)==0);
    }
}
int main(int argc,char **argv)
{
    unsigned i;
    uint32_t previous;
    const float expected=argc>1 ? 1.0f : 2.0f;
    (void)argv;
    g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)ram;
    _putenv(argc>1 ? "RECOMP_BLACK_VIEWMODEL=0" : "RECOMP_BLACK_VIEWMODEL=1");
    GUEST32(0x2D67F4)=renderer;
    GUEST32(renderer+0xD590)=renderer+0xD450;
    set_scale(2.0f);
    black_viewmodel_owner(actor);
    for(i=0;i<256u;++i) {
        fenv_t a,b;
        unsigned csr=0x1F80u|((i%4)<<13)|(i%64), observed;
        memcpy(ram+upload,matrix,sizeof(matrix));
        fesetenv(FE_DFL_ENV); _mm_setcsr(csr);
        memset(&a,0,sizeof(a)); fegetenv(&a);
        previous=black_viewmodel_draw_begin(actor+0x2C8u);
        black_viewmodel_constants(upload);
        black_viewmodel_draw_end(previous);
        observed=_mm_getcsr(); memset(&b,0,sizeof(b)); fegetenv(&b);
        CHECK(observed==csr); CHECK(memcmp(&a,&b,sizeof(a))==0);
        fesetenv(FE_DFL_ENV); _mm_setcsr(0x1F80u);
        expect_upload(expected);
        CHECK(!black_vm_active);
    }
    /* Shared material/shader programs on a different actor never compensate. */
    memcpy(ram+upload,matrix,sizeof(matrix));
    previous=black_viewmodel_draw_begin(actor+0x4000u+0x2C8u);
    black_viewmodel_constants(upload); black_viewmodel_draw_end(previous);
    expect_upload(1.0f);
    /* A nested world draw cannot inherit the surrounding player's ownership. */
    previous=black_viewmodel_draw_begin(actor+0x2C8u);
    {
        uint32_t nested=black_viewmodel_draw_begin(actor+0x4000u+0x2C8u);
        black_viewmodel_constants(upload); expect_upload(1.0f);
        black_viewmodel_draw_end(nested);
    }
    black_viewmodel_constants(upload); expect_upload(expected);
    black_viewmodel_draw_end(previous);
    /* Queue reset and secondary-camera passes invalidate owner provenance. */
    black_viewmodel_owner(0u);
    memcpy(ram+upload,matrix,sizeof(matrix));
    previous=black_viewmodel_draw_begin(actor+0x2C8u);
    black_viewmodel_constants(upload); black_viewmodel_draw_end(previous); expect_upload(1.0f);
    black_viewmodel_owner(actor);
    GUEST32(renderer+0xD590)=renderer+0xD4F0u;
    previous=black_viewmodel_draw_begin(actor+0x2C8u);
    black_viewmodel_constants(upload); black_viewmodel_draw_end(previous); expect_upload(1.0f);
    GUEST32(renderer+0xD590)=renderer+0xD450u;
    /* The native player's immediate muzzle flash uses a separate upload. Its
     * exact caller, actor owner and primary view must all match. Other effects
     * must not inherit projection even inside a nested first-person draw. */
    GUEST32(0x2D45D4u)=actor-0x30u;
    for(i=0;i<256u;++i) {
        fenv_t a,b;
        unsigned csr=0x1F80u|((i%4)<<13)|(i%64), observed;
        uint32_t active=i%2;
        black_vm_active=active;
        memcpy(ram+upload,matrix,sizeof(matrix));
        fesetenv(FE_DFL_ENV); _mm_setcsr(csr);
        memset(&a,0,sizeof(a)); fegetenv(&a);
        black_viewmodel_muzzle_constants(upload,0x0016B49Eu);
        observed=_mm_getcsr(); memset(&b,0,sizeof(b)); fegetenv(&b);
        CHECK(observed==csr); CHECK(memcmp(&a,&b,sizeof(a))==0);
        CHECK(black_vm_active==active);
        fesetenv(FE_DFL_ENV); _mm_setcsr(0x1F80u);
        expect_upload(expected);
    }
    memcpy(ram+upload,matrix,sizeof(matrix));
    black_vm_active=1u;
    black_viewmodel_muzzle_constants(upload,0x0016B49Fu);
    expect_upload(1.0f); CHECK(black_vm_active==1u);
    GUEST32(0x2D45D4u)=actor;
    black_viewmodel_muzzle_constants(upload,0x0016B49Eu);
    expect_upload(1.0f); CHECK(black_vm_active==1u);
    GUEST32(0x2D45D4u)=0xFFFFFFF0u;
    black_viewmodel_muzzle_constants(upload,0x0016B49Eu);
    expect_upload(1.0f); CHECK(black_vm_active==1u);
    GUEST32(0x2D45D4u)=actor-0x30u;
    GUEST32(renderer+0xD590)=renderer+0xD4F0u;
    black_viewmodel_muzzle_constants(upload,0x0016B49Eu);
    expect_upload(1.0f); CHECK(black_vm_active==1u);
    GUEST32(renderer+0xD590)=renderer+0xD450u;
    black_viewmodel_owner(0u);
    black_viewmodel_muzzle_constants(upload,0x0016B49Eu);
    expect_upload(1.0f); CHECK(black_vm_active==1u);
    black_viewmodel_owner(actor);
    black_vm_active=0u;
    /* Invalid constants and overflow are all-or-nothing, with no partial write. */
    for(i=0;i<3;++i) {
        uint32_t copy[16];
        memcpy(ram+upload,matrix,sizeof(matrix));
        GUEST32(upload+12)=i==0 ? 0x7F800001u : i==1 ? 0x7F800000u : 0x7F7FFFFFu;
        memcpy(copy,ram+upload,sizeof(copy));
        previous=black_viewmodel_draw_begin(actor+0x2C8u);
        black_viewmodel_constants(upload); black_viewmodel_constants(0xFFFFFFF0u);
        black_viewmodel_draw_end(previous);
        black_viewmodel_muzzle_constants(upload,0x0016B49Eu);
        black_viewmodel_muzzle_constants(0xFFFFFFF0u,0x0016B49Eu);
        CHECK(!memcmp(copy,ram+upload,sizeof(copy)));
    }
    /* Switching back to Original FOV removes the previous frame's ratio. */
    black_viewmodel_scale(0u,0u);
    memcpy(ram+upload,matrix,sizeof(matrix));
    previous=black_viewmodel_draw_begin(actor+0x2C8u);
    black_viewmodel_constants(upload); black_viewmodel_draw_end(previous); expect_upload(1.0f);
    printf("%u first-person ownership/projection checks passed\n",checks);
    return 0;
}
