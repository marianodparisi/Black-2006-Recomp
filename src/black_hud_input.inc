/* Native in-game hints keep expanded text until dismissed. Keep only the current
 * HUD's pending/active hint, with the original bounded UTF-16 source and the exact
 * last rendered value. sub_00130BD0 measures that text again on every draw, so a
 * refresh before the HUD tick needs no requeue, pause call or animation reset.
 */
#include <fenv.h>
#include "recomp/gen/recomp_types.h" /* TLS register types; generated aliases remain disabled */
extern RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
extern RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;

extern void sub_001314E0_gen(void);

enum { BLACK_HINT_BODY = 200, BLACK_HINT_CONTINUE = 64,
       BLACK_HINT_SCRATCH_UNITS = 6400, BLACK_HINT_GUARD = 32,
       BLACK_HINT_STACK_BYTES = 16384,
       BLACK_HINT_SCRATCH_BYTES = BLACK_HINT_GUARD + BLACK_HINT_SCRATCH_UNITS * 2 +
                                  BLACK_HINT_GUARD + BLACK_HINT_STACK_BYTES };

typedef struct BlackHintField {
    uint16_t raw[BLACK_HINT_BODY], rendered[BLACK_HINT_BODY];
    unsigned capacity;
    int valid, device, confirm_only;
} BlackHintField;

typedef struct BlackHintText {
    uint32_t id;
    BlackHintField body, continuation;
} BlackHintText;

/* Pickup captions are constructed directly from a native control glyph, rather
 * than through113BF0. Retain only this HUD's exact native/rendered first line,
 * its weapon-name second line and the native weapon identity pair. */
typedef struct BlackPickupText {
    uint16_t native[BLACK_HINT_BODY], rendered[BLACK_HINT_BODY], detail[BLACK_HINT_BODY];
    unsigned position;
    uint16_t glyph;
    unsigned char requested, current;
    int valid, device, control;
    char label[48];
} BlackPickupText;

typedef struct BlackHintCache {
    uint32_t hud, owner, scratch;
    int allocation_attempted;
    BlackHintText pending, active;
    BlackPickupText pickup;
} BlackHintCache;

/* The guest HUD and its formatter run on the guest thread. TLS also prevents an
 * unrelated guest worker borrowing another thread's temporary stack or cache. */
static RECOMP_TLS BlackHintCache black_hint_cache;
/* Native control-name localization must not borrow frontend [ENTER]/[ESC]
 * replacements, including when the OHCI thread changes device during formatting. */
static RECOMP_TLS int black_hint_native_formatting;

typedef struct BlackHintGuestState {
    uint32_t regs[10];
    int direction, fp_top_saved, fp_cmp_saved;
    uint16_t fp_control, fp_condition;
    double fp[8];
    RecompXmm simd[8];
    RecompMmx packed[8];
    fenv_t host_environment;
    unsigned host_mxcsr;
} BlackHintGuestState;

static void black_hint_snapshot(BlackHintGuestState *s)
{
    s->regs[0]=g_eax; s->regs[1]=g_ecx; s->regs[2]=g_edx; s->regs[3]=g_ebx;
    s->regs[4]=g_esi; s->regs[5]=g_edi; s->regs[6]=g_esp; s->regs[7]=g_ebp;
    s->regs[8]=g_seh_ebp; s->regs[9]=g_fs_base;
    s->direction=g_df; s->fp_top_saved=g_fp_top; s->fp_cmp_saved=g_fp_cmp;
    s->fp_control=g_fp_control_word; s->fp_condition=g_fp_cc;
    memcpy(s->fp,g_fp_stack,sizeof(s->fp));
    s->simd[0]=g_xmm0; s->simd[1]=g_xmm1; s->simd[2]=g_xmm2; s->simd[3]=g_xmm3;
    s->simd[4]=g_xmm4; s->simd[5]=g_xmm5; s->simd[6]=g_xmm6; s->simd[7]=g_xmm7;
    s->packed[0]=g_mm0; s->packed[1]=g_mm1; s->packed[2]=g_mm2; s->packed[3]=g_mm3;
    s->packed[4]=g_mm4; s->packed[5]=g_mm5; s->packed[6]=g_mm6; s->packed[7]=g_mm7;
    s->host_mxcsr=_mm_getcsr(); fegetenv(&s->host_environment);
}

static void black_hint_restore(const BlackHintGuestState *s)
{
    g_eax=s->regs[0]; g_ecx=s->regs[1]; g_edx=s->regs[2]; g_ebx=s->regs[3];
    g_esi=s->regs[4]; g_edi=s->regs[5]; g_esp=s->regs[6]; g_ebp=s->regs[7];
    g_seh_ebp=s->regs[8]; g_fs_base=s->regs[9];
    g_df=s->direction; g_fp_top=s->fp_top_saved; g_fp_cmp=s->fp_cmp_saved;
    g_fp_control_word=s->fp_control; g_fp_cc=s->fp_condition;
    memcpy(g_fp_stack,s->fp,sizeof(s->fp));
    g_xmm0=s->simd[0]; g_xmm1=s->simd[1]; g_xmm2=s->simd[2]; g_xmm3=s->simd[3];
    g_xmm4=s->simd[4]; g_xmm5=s->simd[5]; g_xmm6=s->simd[6]; g_xmm7=s->simd[7];
    g_mm0=s->packed[0]; g_mm1=s->packed[1]; g_mm2=s->packed[2]; g_mm3=s->packed[3];
    g_mm4=s->packed[4]; g_mm5=s->packed[5]; g_mm6=s->packed[6]; g_mm7=s->packed[7];
    fesetenv(&s->host_environment); _mm_setcsr(s->host_mxcsr);
}

static unsigned black_hint_length(const uint16_t *text, unsigned capacity)
{
    unsigned i;
    for (i=0; i<capacity; ++i) if (!text[i]) return i;
    return capacity;
}

static int black_hint_copy(uint16_t *out, uint32_t source, unsigned capacity)
{
    const uint16_t *text;
    unsigned length;
    if (!black_pc_ram_span(source,capacity*2u)) return 0;
    text=(const uint16_t *)((uintptr_t)g_xbox_mem_offset+source);
    length=black_hint_length(text,capacity);
    if (length==capacity) return 0;
    memset(out,0,BLACK_HINT_BODY*2u);
    memcpy(out,text,(length+1u)*2u);
    return 1;
}

static int black_hint_matches(uint32_t source, const uint16_t *text, unsigned capacity)
{
    const unsigned length=black_hint_length(text,capacity);
    return length<capacity && black_pc_ram_span(source,capacity*2u) &&
        memcmp((const void *)((uintptr_t)g_xbox_mem_offset+source),text,(length+1u)*2u)==0;
}

static int black_hint_current(uint32_t hud)
{
    uint32_t owner;
    BlackHintCache *cache=&black_hint_cache;
    if (!black_pc_ram_span(BLACK_VA(0x002D12E4u),4u)) return 0;
    owner=GUEST32(BLACK_VA(0x002D12E4u));
    if (!black_pc_ram_span(owner,16u) || !black_pc_ram_span(hud,0x2B20u) ||
        hud!=GUEST32(owner+12u)) return 0;
    if (cache->hud!=hud || cache->owner!=owner) {
        memset(&cache->pending,0,sizeof(cache->pending));
        memset(&cache->active,0,sizeof(cache->active));
        memset(&cache->pickup,0,sizeof(cache->pickup));
        cache->hud=hud; cache->owner=owner;
    }
    return 1;
}

static int black_hint_tokens(const uint16_t *text, unsigned capacity)
{
    unsigned i;
    uint32_t map=0;
    for (i=0; i<capacity; ++i) {
        uint16_t character=text[i];
        uint32_t control, name, n;
        if (!character) return 1;
        if (character<0xF001u) continue;
        if (character>0xF025u) return 0; /* native formatter otherwise indexes past its action map */
        if (character==0xF025u) control=16u;
        else {
            if (!map) {
                if (!black_pc_ram_span(BLACK_VA(0x002F3BD4u),4u)) return 0;
                map=GUEST32(BLACK_VA(0x002F3BD4u));
                if (!black_pc_ram_span(map,37u*4u)) return 0;
            }
            control=GUEST32(map+(character-0xF000u)*4u);
        }
        if (control>=28u || !black_pc_ram_span(BLACK_VA(0x002AC290u)+control*4u,4u)) return 0;
        name=GUEST32(BLACK_VA(0x002AC290u)+control*4u);
        for (n=0; n<64u; ++n) {
            unsigned char byte;
            if (!black_pc_ram_span(name+n,1u)) return 0;
            byte=*(const unsigned char *)((uintptr_t)g_xbox_mem_offset+name+n);
            if (!byte) break;
            if (byte>=128u) return 0; /* the native signed-byte CRC expects these ASCII control names */
        }
        if (n==64u) return 0;
    }
    return 0;
}

static BlackHintField *black_hint_capture(uint32_t caller, uint32_t buffer, int device)
{
    BlackHintText *pending=&black_hint_cache.pending;
    BlackHintField *field;
    uint32_t hud=g_ebx, id;
    unsigned capacity;
    if (caller==BLACK_VA(0x0012B4EFu)) capacity=BLACK_HINT_BODY;
    else if (caller==BLACK_VA(0x0012B4E0u)) capacity=BLACK_HINT_CONTINUE;
    else return NULL;
    if (!black_hint_current(hud) ||
        buffer!=hud+(capacity==BLACK_HINT_BODY?0x2114u:0x22C0u)) return NULL;
    id=GUEST32(hud+0x22A4u);
    if (id==0xFFFFFFFFu) return NULL;
    if (pending->id!=id || !pending->body.valid) {
        /* Continue is formatted first. Preserve that same-ID source until body arrives. */
        if (pending->id!=id) memset(pending,0,sizeof(*pending));
        pending->id=id;
    }
    field=capacity==BLACK_HINT_BODY?&pending->body:&pending->continuation;
    memset(field,0,sizeof(*field));
    if (!black_hint_copy(field->raw,buffer,capacity) || !black_hint_tokens(field->raw,capacity)) return NULL;
    field->capacity=capacity; field->confirm_only=capacity==BLACK_HINT_CONTINUE;
    field->device=device;
    return field;
}

static int black_hint_disjoint(uint32_t a, uint32_t na, uint32_t b, uint32_t nb)
{
    return (uint64_t)a+na<=b || (uint64_t)b+nb<=a;
}

static int black_hint_format(const BlackHintField *field, int device, uint16_t *out)
{
    BlackHintCache *cache=&black_hint_cache;
    BlackHintGuestState saved;
    MEMORY_BASIC_INFORMATION mapping;
    uintptr_t tib=(uintptr_t)g_xbox_mem_offset+g_fs_base;
    uint32_t tib_head, text, stack;
    unsigned length, i;
    int result=0;
    const int previous_native_formatting=black_hint_native_formatting;
    if (!field->valid || !black_hint_tokens(field->raw,field->capacity) ||
        !black_pc_ram_span(BLACK_VA(0x002D45D8u),4u) ||
        !black_pc_ram_span(GUEST32(BLACK_VA(0x002D45D8u)),16u) ||
        g_fs_base!=0x4000u ||
        VirtualQuery((void *)tib,&mapping,sizeof(mapping))!=sizeof(mapping) ||
        mapping.State!=MEM_COMMIT || (mapping.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
        ((mapping.Protect&0xFFu)!=PAGE_READWRITE && (mapping.Protect&0xFFu)!=PAGE_EXECUTE_READWRITE) ||
        tib+4u>(uintptr_t)mapping.BaseAddress+mapping.RegionSize) return 0;
    black_hint_snapshot(&saved);
    tib_head=GUEST32(saved.regs[9]);
    __try {
        _mm_setcsr(0x1F80u);
        if (!cache->allocation_attempted) {
            cache->allocation_attempted=1;
            cache->scratch=xbox_HeapAlloc(BLACK_HINT_SCRATCH_BYTES,16u);
        }
        if (cache->scratch && black_pc_ram_span(cache->scratch,BLACK_HINT_SCRATCH_BYTES) &&
            saved.regs[6]>=BLACK_HINT_STACK_BYTES &&
            black_pc_ram_span(saved.regs[6]-BLACK_HINT_STACK_BYTES,BLACK_HINT_STACK_BYTES+8u) &&
            black_hint_disjoint(cache->scratch,BLACK_HINT_SCRATCH_BYTES,cache->hud,0x2B20u) &&
            black_hint_disjoint(cache->scratch,BLACK_HINT_SCRATCH_BYTES,
                saved.regs[6]-BLACK_HINT_STACK_BYTES,BLACK_HINT_STACK_BYTES+8u)) {
            text=cache->scratch+BLACK_HINT_GUARD;
            stack=(cache->scratch+BLACK_HINT_SCRATCH_BYTES-16u)&~15u;
            memset((void *)((uintptr_t)g_xbox_mem_offset+cache->scratch),0xA5,BLACK_HINT_GUARD);
            memset((void *)((uintptr_t)g_xbox_mem_offset+text),0,BLACK_HINT_SCRATCH_UNITS*2u);
            memset((void *)((uintptr_t)g_xbox_mem_offset+text+BLACK_HINT_SCRATCH_UNITS*2u),0xA5,BLACK_HINT_GUARD);
            length=black_hint_length(field->raw,field->capacity);
            memcpy((void *)((uintptr_t)g_xbox_mem_offset+text),field->raw,(length+1u)*2u);
            if (device==PC_DEVICE_KEYBOARD_MOUSE)
                pc_input_expand_prompt((uint16_t *)((uintptr_t)g_xbox_mem_offset+text),field->capacity,field->confirm_only);
            /* At most 199 source units, each native replacement converted with limit32.
             * Even 199*32+1 UTF-16 units fits this separate6400-unit allocation. */
            g_esp=stack-8u; GUEST32(g_esp)=BLACK_VA(0x0012B4EFu); GUEST32(g_esp+4u)=text;
            g_df=0;
            black_hint_native_formatting=1;
            sub_00113BF0_gen();
            length=black_hint_length((const uint16_t *)((uintptr_t)g_xbox_mem_offset+text),BLACK_HINT_SCRATCH_UNITS);
            for (i=0; i<BLACK_HINT_GUARD; ++i)
                if (*(const unsigned char *)((uintptr_t)g_xbox_mem_offset+cache->scratch+i)!=0xA5u ||
                    *(const unsigned char *)((uintptr_t)g_xbox_mem_offset+text+BLACK_HINT_SCRATCH_UNITS*2u+i)!=0xA5u) break;
            if (i==BLACK_HINT_GUARD && length<field->capacity && g_esp==stack-4u) {
                memset(out,0,BLACK_HINT_BODY*2u);
                memcpy(out,(const void *)((uintptr_t)g_xbox_mem_offset+text),length*2u);
                result=1;
            }
        }
    } __finally {
        black_hint_native_formatting=previous_native_formatting;
        /* These formatter/localization/conversion paths install no new persistent
         * TIB state. Preserve the SEH chain head; leave the CRT TLS pointer alone. */
        GUEST32(saved.regs[9])=tib_head;
        black_hint_restore(&saved);
    }
    return result;
}

static void black_hint_refresh_field(BlackHintField *field, uint32_t buffer, int device)
{
    uint16_t formatted[BLACK_HINT_BODY];
    unsigned length;
    if (!field->valid || field->device==device ||
        !black_hint_matches(buffer,field->rendered,field->capacity) ||
        !black_hint_format(field,device,formatted) ||
        !black_hint_matches(buffer,field->rendered,field->capacity)) return;
    length=black_hint_length(formatted,field->capacity);
    memcpy((void *)((uintptr_t)g_xbox_mem_offset+buffer),formatted,(length+1u)*2u);
    memcpy(field->rendered,formatted,sizeof(field->rendered));
    field->device=device;
}

/* The four original presets use X, or A in preset2. Validate the native table's
 * one-byte face glyph instead of borrowing an arbitrary character in a caption.
 * Other custom control-name/null entries retain their original text. */
static int black_pickup_glyph(int *control, uint16_t *glyph)
{
    uint32_t map, value, index, name;
    unsigned char expected, byte;
    if (!black_pc_ram_span(BLACK_VA(0x002F3BD4u),4u)) return 0;
    map=GUEST32(BLACK_VA(0x002F3BD4u));
    if (!black_pc_ram_span(map,37u*4u)) return 0;
    value=GUEST32(map+13u*4u);
    switch (value) {
    case 19u: index=0u; expected=0xB0u; break;
    case 16u: index=1u; expected=0xB2u; break;
    case 17u: index=2u; expected=0xB9u; break;
    case 18u: index=3u; expected=0xB3u; break;
    default: return 0;
    }
    if (!black_pc_ram_span(BLACK_VA(0x002A7DD4u)+index*4u,4u) ||
        GUEST32(BLACK_VA(0x002A7DD4u)+index*4u)!=value ||
        !black_pc_ram_span(BLACK_VA(0x002AB72Cu)+index*4u,4u)) return 0;
    name=GUEST32(BLACK_VA(0x002AB72Cu)+index*4u);
    if (!black_pc_ram_span(name,2u)) return 0;
    byte=*(const unsigned char *)((uintptr_t)g_xbox_mem_offset+name);
    if (byte!=expected || *(const unsigned char *)((uintptr_t)g_xbox_mem_offset+name+1u)) return 0;
    *control=(int)value; *glyph=byte;
    return 1;
}

static int black_pickup_matches(uint32_t hud, const BlackPickupText *text)
{
    return text->valid &&
        *(const unsigned char *)((uintptr_t)g_xbox_mem_offset+hud+0x1Cu)==text->requested &&
        *(const unsigned char *)((uintptr_t)g_xbox_mem_offset+hud+0x1Du)==text->current &&
        black_hint_matches(hud+0x27F0u,text->rendered,BLACK_HINT_BODY) &&
        black_hint_matches(hud+0x2980u,text->detail,BLACK_HINT_BODY);
}

static void black_pickup_refresh(uint32_t hud, int device)
{
    BlackPickupText *text=&black_hint_cache.pickup;
    BlackHintGuestState saved;
    uint16_t native[BLACK_HINT_BODY], formatted[BLACK_HINT_BODY], glyph;
    char label[48];
    unsigned state, length, label_length, i;
    int control, have_glyph, have_label;
    if (!text->valid) return;
    black_hint_snapshot(&saved);
    __try {
        _mm_setcsr(0x1F80u);
        if (!black_hint_current(hud) || !text->valid) return;
        state=*(const unsigned char *)((uintptr_t)g_xbox_mem_offset+hud+0x2B13u);
        if (state>4u || (state==0u &&
            !*(const unsigned char *)((uintptr_t)g_xbox_mem_offset+hud+0x2B10u)) ||
            !black_pickup_matches(hud,text)) {
            memset(text,0,sizeof(*text));
            return;
        }
        memset(label,0,sizeof(label));
        have_glyph=black_pickup_glyph(&control,&glyph);
        if (!have_glyph) { control=text->control; glyph=text->glyph; }
        have_label=have_glyph && pc_input_prompt_text(13,label,sizeof(label));
        label_length=0;
        if (have_label) {
            for (; label_length<sizeof(label); ++label_length) {
                if (!label[label_length]) break;
                if ((unsigned char)label[label_length]>=128u) break;
            }
            have_label=label_length>0u && label_length<sizeof(label) && !label[label_length];
        }
        if (!have_label) { memset(label,0,sizeof(label)); label_length=0; }
        if (text->device==device && text->control==control &&
            !strcmp(text->label,label)) return;
        memcpy(native,text->native,sizeof(native));
        length=black_hint_length(native,BLACK_HINT_BODY);
        if (length==BLACK_HINT_BODY || text->position>=length ||
            native[text->position]!=text->glyph) return;
        native[text->position]=glyph;
        memcpy(formatted,native,sizeof(formatted));
        /* If a changed binding/map cannot expand completely, restore the cached
         * native caption rather than retaining a stale keyboard label. */
        if (device==PC_DEVICE_KEYBOARD_MOUSE && have_label &&
            length-1u+label_length<BLACK_HINT_BODY) {
            for (i=0; i<label_length; ++i)
                formatted[text->position+i]=(uint16_t)(unsigned char)label[i];
            memcpy(formatted+text->position+label_length,native+text->position+1u,
                   (length-text->position)*sizeof(uint16_t)); /* includes NUL */
            length=length-1u+label_length;
        }
        if (!black_pickup_matches(hud,text)) return;
        memcpy((void *)((uintptr_t)g_xbox_mem_offset+hud+0x27F0u),formatted,(length+1u)*2u);
        memcpy(text->native,native,sizeof(native));
        memcpy(text->rendered,formatted,sizeof(formatted));
        memcpy(text->label,label,sizeof(label));
        text->glyph=glyph; text->control=control; text->device=device;
    } __finally {
        black_hint_restore(&saved);
    }
}

/* Called only after132AB7: both native capped200-unit conversions and the two
 * weapon identity writes have completed, before caption animation/layout starts.
 * This plain host helper does not consume the guest stack or any guest flags. */
void black_pickup_text_ready(uint32_t hud)
{
    BlackHintGuestState saved;
    BlackPickupText *text=&black_hint_cache.pickup;
    unsigned length, i, found=BLACK_HINT_BODY, matches=0;
    uint16_t glyph;
    int control, device;
    black_hint_snapshot(&saved);
    __try {
        _mm_setcsr(0x1F80u);
        if (!black_hint_current(hud)) return;
        memset(text,0,sizeof(*text));
        if (*(const unsigned char *)((uintptr_t)g_xbox_mem_offset+hud+0x2B13u)!=0u ||
            !*(const unsigned char *)((uintptr_t)g_xbox_mem_offset+hud+0x2B10u) ||
            !black_pickup_glyph(&control,&glyph) ||
            !black_hint_copy(text->native,hud+0x27F0u,BLACK_HINT_BODY) ||
            !black_hint_copy(text->detail,hud+0x2980u,BLACK_HINT_BODY)) return;
        length=black_hint_length(text->native,BLACK_HINT_BODY);
        for (i=0; i<length; ++i) if (text->native[i]==glyph) { found=i; ++matches; }
        if (matches!=1u) return;
        text->position=found; text->glyph=glyph; text->control=control;
        text->requested=*(const unsigned char *)((uintptr_t)g_xbox_mem_offset+hud+0x1Cu);
        text->current=*(const unsigned char *)((uintptr_t)g_xbox_mem_offset+hud+0x1Du);
        memcpy(text->rendered,text->native,sizeof(text->rendered));
        text->device=PC_DEVICE_CONTROLLER; text->valid=1;
        pc_input_init(); device=(int)pc_input_prompt_device();
        black_pickup_refresh(hud,device);
    } __finally {
        black_hint_restore(&saved);
    }
}

static void black_hint_tick(void)
{
    const uint32_t hud=g_ecx;
    const unsigned caller_csr=_mm_getcsr();
    BlackHintCache *cache=&black_hint_cache;
    uint32_t active_id, pending_id;
    unsigned state;
    int device;
    pc_input_init();
    _mm_setcsr(caller_csr);
    device=(int)pc_input_prompt_device();
    if (black_hint_current(hud)) {
        black_pickup_refresh(hud,device);
        active_id=GUEST32(hud+0x2110u); pending_id=GUEST32(hud+0x22A4u);
        state=*(const unsigned char *)((uintptr_t)g_xbox_mem_offset+hud+0x1Bu);
        if (active_id==0xFFFFFFFFu || state<5u || state>8u)
            memset(&cache->active,0,sizeof(cache->active));
        if (cache->pending.body.valid && active_id==cache->pending.id && pending_id==0xFFFFFFFFu &&
            state>=5u && state<=8u &&
            black_hint_matches(hud+0x1F80u,cache->pending.body.rendered,BLACK_HINT_BODY)) {
            cache->active=cache->pending;
            memset(&cache->pending,0,sizeof(cache->pending));
        }
        if (cache->active.body.valid && active_id==cache->active.id && state>=5u && state<=8u) {
            black_hint_refresh_field(&cache->active.body,hud+0x1F80u,device);
            black_hint_refresh_field(&cache->active.continuation,hud+0x22C0u,device);
        }
        if (cache->pending.body.valid && pending_id==cache->pending.id) {
            black_hint_refresh_field(&cache->pending.body,hud+0x2114u,device);
            black_hint_refresh_field(&cache->pending.continuation,hud+0x22C0u,device);
        } else if (pending_id==0xFFFFFFFFu) memset(&cache->pending,0,sizeof(cache->pending));
    }
    _mm_setcsr(caller_csr);
    sub_001314E0_gen();
}
