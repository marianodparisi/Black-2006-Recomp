/* Synthetic RAM only; executes the authored diagnostic helper verbatim. */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fenv.h>
#include <xmmintrin.h>

#define XBOX_CONTIG_BASE 0x80000000u
#define XBOX_CONTIG_SIZE 0x04000000u
#define RAM_BYTES 0x00400000u
static unsigned char ram[RAM_BYTES], before[RAM_BYTES];
static ptrdiff_t g_xbox_mem_offset;
static void *xbox_GetMemoryBase(void) { return ram; }
static size_t xbox_GetMappedSize(void) { return RAM_BYTES; }
static void *xbox_GetPhysicalMemoryBase(void) { return NULL; }
#define GUEST32(a) (*(uint32_t *)(ram+(a)))
#define BLACK_VA(va) (va) /* fixtures use the canonical (NTSC) layout */
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while(0)
#include "trace.extracted.inc"

static void render_checks(void)
{
    const uint32_t actor=0x10000u, weapon=0x35000u, model=0x36000u;
    const uint32_t effect=0x37000u, entry=0x38000u, settings=0x39000u;
    uint32_t values[12];
    unsigned i;
    GUEST32(actor+0x29Cu)=weapon;
    GUEST32(actor+0x26Cu)=model;
    GUEST32(actor+0x35Cu)=model+0x100u;
    ram[actor+0x3A5u]=1u;
    ram[weapon]=9u;
    ram[weapon+0x108u]=0u;
    ram[model+0x1Au]=2u;
    GUEST32(model+0x124u)=7u;
    black_weapon_attachment_snapshot(1u,actor,values);
    CHECK(values[2] && values[5] && values[6]==9u && !values[7]);
    CHECK(values[9]==model && values[10] && values[11]==2u);
    black_weapon_attachment_snapshot(3u,actor,values);
    CHECK(values[9]==model+0x100u && values[10] && values[11]==7u);
    GUEST32(actor+0x29Cu)=RAM_BYTES-0x108u;
    black_weapon_attachment_snapshot(2u,actor,values);
    CHECK(values[2] && !values[5] && !values[6] && !values[7]);
    GUEST32(actor+0x29Cu)=weapon;
    for(i=0;i<3u;++i) {
        black_weapon_attachment_snapshot(1u,i==0 ? 0u : i==1 ? 0xFFFFFFF0u : RAM_BYTES-0x3AFu,values);
        CHECK(!values[2]);
    }
    GUEST32(effect+0xC4u)=entry;
    GUEST32(entry+0x10u)=settings;
    ram[effect+0xC9u]=255u;
    ram[effect+0xC8u]=255u;
    GUEST32(settings+0x20u)=0x12345678u;
    GUEST32(settings+0x3Cu)=0x7F800001u; /* Signalling NaN remains integer data. */
    black_viewmodel_muzzle_snapshot(effect,values);
    CHECK(values[1] && values[2]==255u && values[3]==255u);
    CHECK(values[6]==0x12345678u && !values[7] && !values[8] && !values[9]);
    ram[effect+0xC9u]=ram[effect+0xC8u]=0u;
    black_viewmodel_muzzle_snapshot(effect,values);
    CHECK(values[7]==0x12345678u && values[9]==0x7F800001u);
    GUEST32(entry+0x10u)=RAM_BYTES-0x4Bu;
    black_viewmodel_muzzle_snapshot(effect,values);
    CHECK(values[1] && !values[6] && !values[7] && !values[8] && !values[9]);
    GUEST32(entry+0x10u)=settings;
    GUEST32(effect+0xC4u)=RAM_BYTES-0x13u;
    black_viewmodel_muzzle_snapshot(effect,values);
    CHECK(values[1] && !values[5]);
    GUEST32(effect+0xC4u)=entry;
    for(i=0;i<3u;++i) {
        black_viewmodel_muzzle_snapshot(i==0 ? 0u : i==1 ? 0xFFFFFFF0u : RAM_BYTES-0xC9u,values);
        CHECK(!values[1]);
    }
    ram[effect+0xC9u]=ram[effect+0xC8u]=255u;
    memcpy(before,ram,sizeof(ram));
    /* Hundreds of identical idle draws produce one record; state transitions
     * still fit. Afterwards exercise the finite budget and all host FP modes. */
    for(i=0;i<300u;++i) {
        black_weapon_attachment_trace(1u,actor);
        black_viewmodel_muzzle_gate(effect);
    }
    CHECK(memcmp(before,ram,sizeof(ram))==0);
    /* More than 32 on-screen owners must not evict unchanged owners and
     * consume the state-transition budget while nothing is happening. */
    for(i=0;i<40u;++i)
        memcpy(ram+actor+0x4000u+0x400u*i,ram+actor,0x3B0u);
    memcpy(before,ram,sizeof(ram));
    for(i=0;i<1200u;++i)
        black_weapon_attachment_trace(2u,actor+0x4000u+0x400u*(i%40u));
    CHECK(memcmp(before,ram,sizeof(ram))==0);
    for(i=0;i<272u;++i) {
        fenv_t a,b;
        unsigned csr=0x1F80u|((i%4)<<13)|(i%64), observed;
        ram[weapon+0x108u]=(uint8_t)((i+1u)%2u);
        ram[effect+0xC9u]=(uint8_t)(i%4u);
        ram[effect+0xC8u]=0u;
        memcpy(before,ram,sizeof(ram));
        fesetenv(FE_DFL_ENV); _mm_setcsr(csr);
        memset(&a,0,sizeof(a)); fegetenv(&a);
        black_weapon_attachment_trace(1u,actor);
        black_viewmodel_muzzle_gate(effect);
        observed=_mm_getcsr(); memset(&b,0,sizeof(b)); fegetenv(&b);
        fesetenv(FE_DFL_ENV); _mm_setcsr(0x1F80u);
        CHECK(observed==csr); CHECK(memcmp(&a,&b,sizeof(a))==0);
        CHECK(memcmp(before,ram,sizeof(ram))==0);
    }
}

static void loadout_checks(int enabled)
{
    const uint32_t core=0x40000u, record=core+0x3F0u, inventory=0x46000u;
    const uint32_t header=0x47000u, entries=0x48000u, config=0x49000u;
    unsigned i;
    GUEST32(0x2D45D4u)=core; GUEST32(0x2D12D0u)=inventory;
    GUEST32(inventory)=header; GUEST32(header+4u)=entries;
    ram[core+0x5AECu]=5u; ram[header+1u]=2u;
    GUEST32(entries+32u)=0xB8230000u;
    GUEST32(entries+36u)=0x54461524u;
    GUEST32(entries+40u)=config;
    GUEST32(config+0x60u)=10u; ram[config+0x64u]=5u;
    CHECK(black_test_weapon_loadout_valid(record));
    CHECK(!black_test_weapon_loadout_valid(record+4u));
    ram[core+0x5AECu]=6u; CHECK(black_test_weapon_loadout_valid(record));
    ram[core+0x5AECu]=0u; CHECK(!black_test_weapon_loadout_valid(record));
    ram[core+0x5AECu]=5u;
    ram[core+0x5CB1u]=1u; CHECK(!black_test_weapon_loadout_valid(record));
    ram[core+0x5CB1u]=0u;
    GUEST32(0x2D45D4u)=RAM_BYTES-0x5CB1u; CHECK(!black_test_weapon_loadout_valid(record));
    GUEST32(0x2D45D4u)=core;
    GUEST32(0x2D12D0u)=0u; CHECK(!black_test_weapon_loadout_valid(record));
    GUEST32(0x2D12D0u)=inventory;
    GUEST32(inventory)=RAM_BYTES-7u; CHECK(!black_test_weapon_loadout_valid(record));
    GUEST32(inventory)=header;
    ram[header+1u]=0u; CHECK(!black_test_weapon_loadout_valid(record));
    ram[header+1u]=128u; CHECK(!black_test_weapon_loadout_valid(record));
    ram[header+1u]=2u;
    GUEST32(header+4u)=RAM_BYTES-63u; CHECK(!black_test_weapon_loadout_valid(record));
    GUEST32(header+4u)=entries;
    memcpy(ram+entries,ram+entries+32u,32u); CHECK(!black_test_weapon_loadout_valid(record));
    memset(ram+entries,0,32u);
    GUEST32(entries+32u)=0u; CHECK(!black_test_weapon_loadout_valid(record));
    GUEST32(entries+32u)=0xB8230000u;
    GUEST32(entries+40u)=RAM_BYTES-0x64u; CHECK(!black_test_weapon_loadout_valid(record));
    GUEST32(entries+40u)=config;
    GUEST32(config+0x60u)=0u; CHECK(!black_test_weapon_loadout_valid(record));
    GUEST32(config+0x60u)=65536u; CHECK(!black_test_weapon_loadout_valid(record));
    GUEST32(config+0x60u)=10u;
    ram[config+0x64u]=10u; CHECK(!black_test_weapon_loadout_valid(record));
    ram[config+0x64u]=5u;
    CHECK(black_test_weapon_loadout_valid(record));
    memcpy(before,ram,sizeof(ram));
    black_test_weapon_loadout(record+4u); /* Wrong actor/record fails closed. */
    CHECK(memcmp(before,ram,sizeof(ram))==0);
    for(i=0;i<256u;++i) {
        fenv_t a,b;
        unsigned csr=0x1F80u|((i%4u)<<13)|(i%64u), observed;
        GUEST32(record)=0x80DB8000u; GUEST32(record+4u)=0x54461523u;
        memcpy(before,ram,sizeof(ram));
        if(enabled) {
            *(uint32_t *)(before+record)=0xB8230000u;
            *(uint32_t *)(before+record+4u)=0x54461524u;
        }
        fesetenv(FE_DFL_ENV); _mm_setcsr(csr);
        memset(&a,0,sizeof(a)); fegetenv(&a);
        black_test_weapon_loadout(record);
        observed=_mm_getcsr(); memset(&b,0,sizeof(b)); fegetenv(&b);
        fesetenv(FE_DFL_ENV); _mm_setcsr(0x1F80u);
        CHECK(observed==csr); CHECK(memcmp(&a,&b,sizeof(a))==0);
        CHECK(memcmp(before,ram,sizeof(ram))==0);
    }
}

static void unit_checks(int enabled)
{
    const uint32_t core=0x40000u, header=0x47000u;
    unsigned i;
    GUEST32(0x2D45D4u)=core;
    GUEST32(core+0x5AF4u)=header;
    ram[core+0x5AECu]=5u; ram[core+0x5AEDu]=1u;
    ram[header+0x11u]=8u;
    CHECK(black_test_stream_unit_valid(core,2u));
    CHECK(!black_test_stream_unit_valid(core+4u,2u));
    CHECK(!black_test_stream_unit_valid(core,0u));
    CHECK(!black_test_stream_unit_valid(core,9u));
    ram[header+0x11u]=128u; CHECK(!black_test_stream_unit_valid(core,2u));
    ram[header+0x11u]=0u; CHECK(!black_test_stream_unit_valid(core,2u));
    ram[header+0x11u]=8u;
    ram[core+0x5AEDu]=0u; CHECK(!black_test_stream_unit_valid(core,2u));
    ram[core+0x5AEDu]=1u;
    GUEST32(core+0x5AF4u)=RAM_BYTES-0x12u; CHECK(!black_test_stream_unit_valid(core,2u));
    GUEST32(core+0x5AF4u)=header;
    for(i=0;i<256u;++i) {
        fenv_t a,b;
        unsigned csr=0x1F80u|((i%4u)<<13)|(i%64u), observed;
        GUEST32(core+0x5AF0u)=0u;
        memcpy(before,ram,sizeof(ram));
        if(enabled) *(uint32_t *)(before+core+0x5AF0u)=2u;
        fesetenv(FE_DFL_ENV); _mm_setcsr(csr);
        memset(&a,0,sizeof(a)); fegetenv(&a);
        black_test_stream_unit(core);
        observed=_mm_getcsr(); memset(&b,0,sizeof(b)); fegetenv(&b);
        fesetenv(FE_DFL_ENV); _mm_setcsr(0x1F80u);
        CHECK(observed==csr); CHECK(memcmp(&a,&b,sizeof(a))==0);
        CHECK(memcmp(before,ram,sizeof(ram))==0);
    }
}

int main(int argc, char **argv)
{
    const uint32_t actor=0x10000u, manager=0x20000u, slots=0x30000u;
    unsigned i;
    g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)ram;
    _putenv(argc>1 ? "RECOMP_BLACK_WEAPON_TRACE=0" : "RECOMP_BLACK_WEAPON_TRACE=1");
    _putenv(argc>1 ? "RECOMP_BLACK_VIEWMODEL_TRACE=0" : "RECOMP_BLACK_VIEWMODEL_TRACE=1");
    _putenv(argc==1 ? "RECOMP_BLACK_TEST_WEAPON=svd" :
            !strcmp(argv[1],"wrong") ? "RECOMP_BLACK_TEST_WEAPON=sVd" : "RECOMP_BLACK_TEST_WEAPON=");
    _putenv(argc==1 ? "RECOMP_BLACK_TEST_UNIT=2" :
            !strcmp(argv[1],"wrong") ? "RECOMP_BLACK_TEST_UNIT=2junk" : "RECOMP_BLACK_TEST_UNIT=");
    GUEST32(0x2D0D64)=manager;
    GUEST32(actor+0x258)=slots;
    GUEST32(actor+0x29C)=0x35000;
    GUEST32(actor+0xC4)=1;
    ram[actor+0x2C4]=2;
    ram[slots+0x19]=255; /* Corrupt slot index must never form a dereference. */
    GUEST32(slots+0xCu)=0x11111111u;
    GUEST32(slots+0x10u)=0x22222222u;
    GUEST32(slots+0x14u)=0x33333333u;
    GUEST32(actor+0x1000u+0x258u)=RAM_BYTES-10u; /* Struct cannot fit in RAM. */
    GUEST32(manager+0xA0)=0x7F800001; /* Signalling NaN stays integer data. */
    memcpy(before,ram,sizeof(ram));
    for(i=0;i<272u;++i) {
        fenv_t a,b;
        unsigned csr=0x1F80u|((i%4)<<13)|(i%64), observed;
        uint32_t object=i%7==0 ? 0xFFFFFFF0u : i%7==1 ? 0u : i%7==2 ? actor+0x1000u : actor;
        fesetenv(FE_DFL_ENV); _mm_setcsr(csr);
        memset(&a,0,sizeof(a)); fegetenv(&a);
        black_weapon_trace(1u+i%4u, i%4==3 ? manager : object);
        observed=_mm_getcsr(); memset(&b,0,sizeof(b)); fegetenv(&b);
        fesetenv(FE_DFL_ENV); _mm_setcsr(0x1F80u);
        CHECK(observed==csr); CHECK(memcmp(&a,&b,sizeof(a))==0);
    }
    CHECK(memcmp(before,ram,sizeof(ram))==0);
    render_checks();
    loadout_checks(argc==1);
    unit_checks(argc==1);
    printf("%u trace state/memory checks passed\n", checks);
    return 0;
}
