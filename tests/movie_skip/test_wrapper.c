#include <assert.h>
#ifdef NDEBUG
#error Movie wrapper regression assertions must remain enabled in Release builds.
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fenv.h>
#include <xmmintrin.h>
#include "pc_movie_skip.h"
#include "pc_menu.h"

static unsigned char guest[4 * 1024 * 1024];
ptrdiff_t g_xbox_mem_offset;
uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esi, g_edi, g_esp;
static uint64_t active_session, requested_session, subtitle_target_session;
static int active_kind, generated_calls, generated_action, font_loads;
static char subtitle_target_stem[64];
static int black_pc_ram_span(uint32_t va, uint32_t bytes)
{ return va >= 0x10000u && (uint64_t)va + bytes <= sizeof(guest); }
void pc_movie_skip_update(uint64_t session, int kind, int playing)
{ assert(playing); active_session=session; active_kind=kind; }
void pc_movie_skip_clear(void)
{ active_session=0; active_kind=0; requested_session=0; }
int pc_movie_skip_take_request(uint64_t session)
{
    if (session != requested_session) return 0;
    requested_session=0;
    return 1;
}
void pc_movie_skip_subtitle_target(uint64_t session, const char *stem)
{
    subtitle_target_session = session;
    snprintf(subtitle_target_stem, sizeof(subtitle_target_stem), "%s", stem ? stem : "");
}
static int bnm_load_font(int slot, const char *path)
{
    assert(slot == PCM_FONT_SMALL);
    assert(!strcmp(path, "D:\\language\\fonts\\Small.bin"));
    ++font_loads;
    return 1;
}
int pc_menu_has_font(int slot)
{ assert(slot == PCM_FONT_SMALL); return font_loads != 0; }
void sub_000C4570_gen(void)
{
    ++generated_calls;
    g_eax=0x12345678u;
    if (generated_action == 1) guest[g_esi+0x62u]=1;
    if (generated_action == 2) *(uint32_t *)(guest+g_esi+0x4Cu) += 0x100u;
    g_esp += 4u;
}

/* The extracted range also contains the unrelated audio wrapper. Movie tests
 * must never enter it; fail explicitly if these link-only stubs are called. */
int xbox_IrqlTryEnterDispatch(void)
{ fputs("Unexpected audio dispatch acquisition in movie fixture\n", stderr); abort(); }
void xbox_IrqlLeaveDispatch(void)
{ fputs("Unexpected audio dispatch release in movie fixture\n", stderr); abort(); }
void sub_0021C7EF_gen(void)
{ fputs("Unexpected generated audio callback in movie fixture\n", stderr); abort(); }

#define BLACK_VA(va) (va) /* fixtures use the canonical (NTSC) layout */
#include "movie-wrapper-under-test.inc"
#include "movie-font-under-test.inc"

enum { MAIN=0x100000u, MANAGER=MAIN+0x2026Cu, MOVIE=0x150000u,
       DECODER=0x160000u, STACK=0x170000u };

static void movie(const char *stem)
{
    memset(guest,0,sizeof(guest));
    *(uint32_t *)(guest+0x2D45F0u)=MAIN;
    *(uint32_t *)(guest+MANAGER)=MOVIE;
    *(uint32_t *)(guest+MANAGER+0x1A8u)=0x1Cu;
    strcpy((char *)guest+MANAGER+0x84u,stem);
    *(uint32_t *)(guest+MOVIE+0x48u)=0x1Cu;
    *(uint32_t *)(guest+MOVIE+0x4Cu)=DECODER;
    *(uint32_t *)(guest+STACK)=0x209ED8u;
    generated_action=0;
    generated_calls=0;
    g_eax=10; g_ecx=11; g_edx=12; g_ebx=13; g_esi=MOVIE; g_edi=14; g_esp=STACK;
    movie_skip_all=0;
}

static void check_abi(void)
{
    assert(g_ecx==11 && g_edx==12 && g_ebx==13 && g_esi==MOVIE && g_edi==14);
    assert(g_esp==STACK+4u);
}

int main(void)
{
    uint64_t first, next;
    unsigned csr;
    g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)guest;
    movie("LO_n_US");
    sub_000C4570();
    assert(active_kind==PC_MOVIE_SKIP_INTRO && active_session && generated_calls==1);
    assert(subtitle_target_session==active_session && !strcmp(subtitle_target_stem,"LO_n_US"));
    assert(!guest[MOVIE+0x62u]); check_abi();

    movie("03_n");
    fesetround(FE_DOWNWARD);
    csr=_mm_getcsr();
    sub_000C4570();
    assert(fegetround()==FE_DOWNWARD && _mm_getcsr()==csr);
    assert(active_kind==PC_MOVIE_SKIP_CUTSCENE && font_loads==1 && generated_calls==1);
    assert(subtitle_target_session==active_session && !strcmp(subtitle_target_stem,"03_n"));
    assert(g_eax==0x12345678u); check_abi(); first=active_session;
    g_esp=STACK; sub_000C4570();
    assert(active_session==first && font_loads==1);
    requested_session=first; g_esp=STACK; g_eax=10;
    sub_000C4570();
    assert(guest[MOVIE+0x62u] && !active_session && generated_calls==2 && g_eax==10);
    check_abi();

    movie("C_N_Us");
    sub_000C4570();
    assert(active_kind==PC_MOVIE_SKIP_CUTSCENE && active_session!=first);
    assert(subtitle_target_session==active_session && !strcmp(subtitle_target_stem,"C_N_Us"));
    first=active_session;
    generated_action=1; g_esp=STACK;
    sub_000C4570();
    assert(!active_session && guest[MOVIE+0x62u]);
    movie("C_N_Us"); sub_000C4570();
    assert(active_session && active_session!=first); first=active_session;
    generated_action=2; g_esp=STACK; sub_000C4570();
    assert(active_session!=first && active_kind==PC_MOVIE_SKIP_CUTSCENE);

    movie("AK_n");
    *(uint32_t *)(guest+MANAGER+0x198u)=1u;
    movie_skip_all=1;
    sub_000C4570();
    assert(!active_session && !guest[MOVIE+0x62u] && generated_calls==1);
    check_abi();

    movie("05_n"); movie_skip_all=1;
    sub_000C4570();
    assert(guest[MOVIE+0x62u] && !active_session && generated_calls==0 && g_eax==10);
    check_abi();

    movie("07_n");
    *(uint32_t *)(guest+MOVIE+0x48u)=2u;
    sub_000C4570();
    assert(!active_session && !guest[MOVIE+0x62u] && generated_calls==1);
    *(uint32_t *)(guest+MOVIE+0x48u)=0x1Cu;
    g_esp=STACK; sub_000C4570(); first=active_session;
    assert(first && active_kind==PC_MOVIE_SKIP_CUTSCENE);
    guest[MANAGER+0x1B8u]=1; g_esp=STACK; sub_000C4570();
    assert(!active_session && !guest[MOVIE+0x62u]);
    guest[MANAGER+0x1B8u]=0; g_esp=STACK; sub_000C4570();
    next=active_session; assert(next && next!=first);
    requested_session=first; g_esp=STACK; sub_000C4570();
    assert(!guest[MOVIE+0x62u] && active_session==next);
    guest[MOVIE+0x61u]=1; g_esp=STACK; sub_000C4570();
    assert(!active_session && !guest[MOVIE+0x62u]);
    guest[MOVIE+0x61u]=0; g_esp=STACK; sub_000C4570();
    assert(active_session && active_session!=next);
    memset(guest+MANAGER+0x84u,'X',64u); g_esp=STACK; sub_000C4570();
    assert(!active_session && !guest[MOVIE+0x62u]);
    movie("09_n"); g_esi=0xFFFFFFFFu;
    generated_action=0; sub_000C4570();
    assert(!active_session && generated_calls==1);
    puts("PASS: movie wrapper classification, ownership, EOF, reopen, replacement, loop exclusion, lab skip, stale requests, bounded strings and guest ABI/FP preservation");
    return 0;
}
