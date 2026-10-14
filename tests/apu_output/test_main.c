/* Execute the complete production backend against a fake XAudio2 graph. The
 * fake keeps PCM pointers alive until consumption or DestroyVoice, checks
 * graph teardown order, and forbids resource calls from the engine callback. */
#include "apu_xaudio2.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <wchar.h>
#define STDMETHODCALLTYPE
#define WINAPI
#define COINIT_MULTITHREADED 0
#define XAUDIO2_DEFAULT_PROCESSOR 0
#define XAUDIO2_DEFAULT_FREQ_RATIO 2.0f
#define XAUDIO2_COMMIT_NOW 0
#define XAUDIO2_VOICE_NOSAMPLESPLAYED 1
#define WAVE_FORMAT_PCM 1
#define FAILED(x) ((x) < 0)
#define SUCCEEDED(x) ((x) >= 0)
#define RPC_E_CHANGED_MODE ((HRESULT)-2)
typedef long HRESULT;
typedef int32_t LONG;
typedef uint32_t DWORD;
typedef DWORD *LPDWORD;
typedef const wchar_t *LPCWSTR;
typedef void *HANDLE;
typedef void *HMODULE;
typedef int BOOL;
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x800
typedef uint64_t ULONGLONG;
typedef unsigned char BYTE;
typedef struct { unsigned short wFormatTag, nChannels; unsigned long nSamplesPerSec,
    nAvgBytesPerSec; unsigned short nBlockAlign, wBitsPerSample, cbSize; } WAVEFORMATEX;
typedef struct { unsigned BuffersQueued; uint64_t SamplesPlayed; } XAUDIO2_VOICE_STATE;
typedef struct { unsigned AudioBytes; const BYTE *pAudioData; } XAUDIO2_BUFFER;
typedef struct IXAudio2EngineCallback IXAudio2EngineCallback;
typedef struct { void (*OnProcessingPassStart)(IXAudio2EngineCallback *);
    void (*OnProcessingPassEnd)(IXAudio2EngineCallback *);
    void (*OnCriticalError)(IXAudio2EngineCallback *, HRESULT); } IXAudio2EngineCallbackVtbl;
struct IXAudio2EngineCallback { IXAudio2EngineCallbackVtbl *lpVtbl; };
typedef struct IXAudio2SourceVoice IXAudio2SourceVoice;
typedef struct IXAudio2MasteringVoice IXAudio2MasteringVoice;
typedef struct { void (*DestroyVoice)(IXAudio2SourceVoice *); } SourceVtbl;
typedef struct { void (*DestroyVoice)(IXAudio2MasteringVoice *); } MasterVtbl;
struct IXAudio2SourceVoice { SourceVtbl *lpVtbl; };
struct IXAudio2MasteringVoice { MasterVtbl *lpVtbl; };
typedef struct IXAudio2 {
    unsigned id, alive;
    IXAudio2EngineCallback *callback;
    char teardown[8]; unsigned teardown_size;
} IXAudio2;
typedef struct {
    IXAudio2SourceVoice iface;
    IXAudio2 *engine;
    unsigned alive, queued, starts, stops;
    uint64_t played;
    XAUDIO2_BUFFER queue[3];
    int16_t expected[3];
    int16_t snapshot[3][2048];
} FakeSource;
typedef struct { IXAudio2MasteringVoice iface; IXAudio2 *engine; unsigned alive; } FakeMaster;
static IXAudio2 engines[64];
static FakeSource sources[64];
static FakeMaster masters[64];
static unsigned checks, failures, calls, creates, co_inits, co_uninits;
static unsigned fail_stage, fail_submit, fail_start, fail_stop, critical_on_create, critical_on_register;
static DWORD thread_id=42;
static ULONGLONG clock_ms;
static ULONGLONG create_delay_ms;
static HRESULT co_result;
static unsigned avrt_fail, avrt_loads, avrt_frees, avrt_sets, avrt_reverts;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; fprintf(stderr,"Failed line %u: %s\n",(unsigned)__LINE__,#x); } } while(0)
#define SKIP_TAIL_TEST(name) \
    printf("SKIP %s: selected core source has no mixer-tail helpers\n",name)
static void owner_call(void) { ++calls; CHECK(thread_id==42); }
static void event(IXAudio2 *e, char c)
{ CHECK(e->teardown_size<sizeof(e->teardown)-1); e->teardown[e->teardown_size++]=c; }
static void validate(FakeSource *v)
{
    CHECK(v->alive && v->engine->alive);
    for (unsigned j=0;j<v->queued;++j) {
        const int16_t *p=(const int16_t*)v->queue[j].pAudioData;
        CHECK(v->queue[j].AudioBytes<=sizeof(v->snapshot[j]));
        CHECK(!memcmp(p,v->snapshot[j],v->queue[j].AudioBytes));
    }
}
static void destroy_source(IXAudio2SourceVoice *voice)
{
    owner_call(); FakeSource *v=(FakeSource*)voice; validate(v);
    CHECK(!v->engine->callback); event(v->engine,'S');
    v->alive=0; v->queued=0;
}
static void destroy_master(IXAudio2MasteringVoice *voice)
{
    owner_call(); FakeMaster *v=(FakeMaster*)voice;
    CHECK(v->alive && !sources[v->engine->id].alive);
    event(v->engine,'M'); v->alive=0;
}
static SourceVtbl source_vtable={destroy_source};
static MasterVtbl master_vtable={destroy_master};
static HRESULT fake_co_init(void *unused, unsigned mode)
{ (void)unused; (void)mode; owner_call(); ++co_inits; return co_result; }
static void fake_co_uninit(void) { owner_call(); ++co_uninits; }
static HRESULT fake_create(IXAudio2 **out, unsigned flags, unsigned processor)
{
    (void)flags; (void)processor; owner_call(); CHECK(creates<63); ++creates;
    clock_ms+=create_delay_ms;
    *out=NULL; if (fail_stage==1) return -1;
    IXAudio2 *e=&engines[creates]; e->id=creates; e->alive=1; *out=e; return 0;
}
static HRESULT fake_register(IXAudio2 *e, IXAudio2EngineCallback *cb)
{
    owner_call(); CHECK(e->alive && !e->callback);
    if (fail_stage==2) return -1;
    e->callback=cb;
    if (critical_on_register) {
        DWORD previous=thread_id; thread_id=7;
        cb->lpVtbl->OnCriticalError(cb,-56); thread_id=previous;
    }
    return 0;
}
static void fake_unregister(IXAudio2 *e, IXAudio2EngineCallback *cb)
{ owner_call(); CHECK(e->alive && e->callback==cb); e->callback=NULL; event(e,'R'); }
static void fake_release(IXAudio2 *e)
{
    owner_call(); CHECK(e->alive && !e->callback && !sources[e->id].alive && !masters[e->id].alive);
    event(e,'E'); e->alive=0;
}
static HRESULT fake_master(IXAudio2 *e, IXAudio2MasteringVoice **out, unsigned channels,
    unsigned rate, unsigned flags, void *device, void *effects, unsigned category)
{
    (void)flags; (void)device; (void)effects; (void)category; owner_call();
    CHECK(channels==2 && rate==48000); *out=NULL; if (fail_stage==3) return -1;
    FakeMaster *v=&masters[e->id]; v->iface.lpVtbl=&master_vtable; v->engine=e; v->alive=1;
    *out=&v->iface; return 0;
}
static HRESULT fake_source(IXAudio2 *e, IXAudio2SourceVoice **out, WAVEFORMATEX *format,
    unsigned flags, float ratio, void *cb, void *sends, void *effects)
{
    (void)flags; (void)ratio; (void)cb; (void)sends; (void)effects; owner_call();
    CHECK(format->nChannels==2 && format->nSamplesPerSec==48000 && format->nBlockAlign==4);
    *out=NULL; if (fail_stage==4) return -1;
    FakeSource *v=&sources[e->id]; v->iface.lpVtbl=&source_vtable; v->engine=e; v->alive=1;
    *out=&v->iface;
    if (critical_on_create) {
        DWORD previous=thread_id; thread_id=7;
        e->callback->lpVtbl->OnCriticalError(e->callback,-55); thread_id=previous;
    }
    return 0;
}
static void fake_state(IXAudio2SourceVoice *voice, XAUDIO2_VOICE_STATE *state, unsigned flags)
{
    (void)flags; owner_call(); FakeSource *v=(FakeSource*)voice; validate(v);
    state->BuffersQueued=v->queued; state->SamplesPlayed=v->played;
}
static HRESULT fake_submit(IXAudio2SourceVoice *voice, XAUDIO2_BUFFER *b, void *unused)
{
    (void)unused; owner_call(); FakeSource *v=(FakeSource*)voice; validate(v);
    if (fail_submit) return -1;
    CHECK(v->queued<3); if (v->queued>=3) return -1;
    v->queue[v->queued]=*b; v->expected[v->queued]=*(const int16_t*)b->pAudioData;
    memcpy(v->snapshot[v->queued],b->pAudioData,b->AudioBytes); ++v->queued;
    return 0;
}
static HRESULT fake_start(IXAudio2SourceVoice *voice, unsigned flags, unsigned op)
{
    (void)flags; (void)op; owner_call(); FakeSource *v=(FakeSource*)voice; validate(v);
    if (fail_start) return -1; ++v->starts; return 0;
}
static HRESULT fake_stop(IXAudio2SourceVoice *voice, unsigned flags, unsigned op)
{
    (void)flags; (void)op; owner_call(); FakeSource *v=(FakeSource*)voice; validate(v);
    CHECK(!v->queued); if (fail_stop) return -1; ++v->stops; return 0;
}
static LONG atomic_exchange(volatile LONG *p,LONG value) { LONG old=*p; *p=value; return old; }
static LONG atomic_increment(volatile LONG *p) { return ++*p; }
static LONG atomic_compare(volatile LONG *p,LONG value,LONG expected)
{ LONG old=*p; if (old==expected) *p=value; return old; }
static HMODULE fake_load_avrt(LPCWSTR name,HANDLE file,DWORD flags)
{ owner_call(); CHECK(!wcscmp(name,L"avrt.dll") && !file && flags==LOAD_LIBRARY_SEARCH_SYSTEM32); ++avrt_loads; return avrt_fail==1 ? NULL:(HMODULE)(uintptr_t)1; }
static HANDLE fake_av_set(LPCWSTR task,LPDWORD index)
{ owner_call(); CHECK(!wcscmp(task,L"Pro Audio") && *index==0); ++avrt_sets; *index=5; return avrt_fail==3 ? NULL:(HANDLE)(uintptr_t)2; }
static BOOL fake_av_revert(HANDLE task)
{ owner_call(); CHECK(task==(HANDLE)(uintptr_t)2); ++avrt_reverts; return 1; }
typedef void (*FakeProc)(void);
static FakeProc fake_get_proc(HMODULE module,const char *name)
{
    owner_call(); CHECK(module==(HMODULE)(uintptr_t)1);
    if (avrt_fail==2) return NULL;
    if (!strcmp(name,"AvSetMmThreadCharacteristicsW")) return (FakeProc)fake_av_set;
    CHECK(!strcmp(name,"AvRevertMmThreadCharacteristics")); return (FakeProc)fake_av_revert;
}
static BOOL fake_free_avrt(HMODULE module)
{ owner_call(); CHECK(module==(HMODULE)(uintptr_t)1); ++avrt_frees; return 1; }
#define CoInitializeEx fake_co_init
#define CoUninitialize fake_co_uninit
#define GetCurrentThreadId() thread_id
#define GetTickCount64() clock_ms
#define XAudio2Create fake_create
#define IXAudio2_RegisterForCallbacks fake_register
#define IXAudio2_UnregisterForCallbacks fake_unregister
#define IXAudio2_CreateMasteringVoice fake_master
#define IXAudio2_CreateSourceVoice fake_source
#define IXAudio2_Release fake_release
#define IXAudio2SourceVoice_GetState fake_state
#define IXAudio2SourceVoice_SubmitSourceBuffer fake_submit
#define IXAudio2SourceVoice_Start fake_start
#define IXAudio2SourceVoice_Stop fake_stop
#define InterlockedExchange atomic_exchange
#define InterlockedIncrement atomic_increment
#define InterlockedCompareExchange atomic_compare
#define LoadLibraryExW fake_load_avrt
#define GetProcAddress fake_get_proc
#define FreeLibrary fake_free_avrt
#include "backend.inc"

static void reset(void)
{
    thread_id=42; xa2_shutdown();
    memset(engines,0,sizeof(engines)); memset(sources,0,sizeof(sources)); memset(masters,0,sizeof(masters));
    creates=co_inits=co_uninits=calls=0; fail_stage=fail_submit=fail_start=fail_stop=critical_on_create=critical_on_register=0;
    clock_ms=create_delay_ms=0; co_result=0;
    avrt_fail=avrt_loads=avrt_frees=avrt_sets=avrt_reverts=0;
}
static FakeSource *current(void) { CHECK(g_xa2_source!=NULL); return (FakeSource*)g_xa2_source; }
static int packet(int value)
{
    int16_t samples[2048]; for (unsigned i=0;i<2048;++i) samples[i]=(int16_t)value;
    return xa2_submit_samples(samples,1024);
}
static void consume(int expected)
{
    FakeSource *v=current(); validate(v); CHECK(v->queued>0); CHECK(v->expected[0]==expected);
    CHECK(v->queue[0].AudioBytes==1024*4); v->played+=1024;
    --v->queued; memmove(v->queue,v->queue+1,v->queued*sizeof(v->queue[0]));
    memmove(v->expected,v->expected+1,v->queued*sizeof(v->expected[0]));
    memmove(v->snapshot,v->snapshot+1,v->queued*sizeof(v->snapshot[0]));
}
static void critical(HRESULT error)
{
    IXAudio2EngineCallback *cb=g_xa2->callback; unsigned before=calls;
    thread_id=7; cb->lpVtbl->OnCriticalError(cb,error); thread_id=42;
    CHECK(calls==before); /* Callback must only publish atomics. */
}
static Xa2OutputStats stats(void) { Xa2OutputStats s; xa2_get_output_stats(&s); return s; }
static void fifo_and_priming(void)
{
    reset(); CHECK(xa2_init());
    for (unsigned p=1;p<=3;++p) { CHECK(packet(p)); CHECK(current()->starts==(p>=2 ? 1u:0u)); }
    CHECK(stats().submitted_samples==3072); CHECK(packet(9)==XA2_SUBMIT_BACKPRESSURE); CHECK(stats().queue_full==1);
    consume(1); CHECK(packet(4)); consume(2); consume(3); consume(4);
    fail_submit=1; int next=g_xa2_next_buf;
    CHECK(!packet(4)); CHECK(next==g_xa2_next_buf && current()->queued==0);
    CHECK(stats().submitted_samples==4096 && stats().submit_failed==1);
    CHECK(current()->stops==1 && stats().reprimes==1 && !g_xa2_started);
    CHECK(!xa2_submit_samples(NULL,1024)); CHECK(!xa2_submit_samples((int16_t*)g_xa2_bufs,-1));
    CHECK(!xa2_submit_samples((int16_t*)g_xa2_bufs,0));
    fail_submit=0; fail_start=1;
    CHECK(packet(4)); CHECK(packet(4)==XA2_SUBMIT_ACCEPTED); CHECK(!g_xa2_started && stats().start_failed==1);
    fail_start=0; CHECK(packet(4)); CHECK(current()->queued==3 && current()->starts==2);
    consume(4); consume(4); consume(4); fail_stop=1;
    CHECK(packet(4)); CHECK(g_xa2_started && current()->starts==2 && stats().stop_failed==1);
    consume(4); fail_stop=0;
    CHECK(packet(4)); CHECK(!g_xa2_started && current()->queued==1 && stats().reprimes==2);
    CHECK(packet(4)); CHECK(g_xa2_started && current()->starts==3);
    consume(4); consume(4); xa2_shutdown(); CHECK(co_inits==1 && co_uninits==1);
    CHECK(strcmp(engines[1].teardown,"RSME")==0); CHECK(stats().played_samples==10240);
}
static void recover_owned_graph(void)
{
    reset(); CHECK(xa2_init()); CHECK(packet(1)); CHECK(packet(2)); CHECK(packet(3));
    consume(1); CHECK(packet(4)); CHECK(stats().played_samples==1024);
    /* A full healthy voice and silent samples never recreate the graph. */
    for(unsigned i=0;i<5;++i) CHECK(packet(0)==XA2_SUBMIT_BACKPRESSURE);
    CHECK(creates==1 && !stats().recovery_attempts);
    critical(-99); CHECK(packet(5));
    CHECK(strcmp(engines[1].teardown,"RSME")==0 && !sources[1].alive);
    CHECK(current()->queued==1 && !g_xa2_started && current()->expected[0]==5);
    CHECK(stats().critical_errors==1 && stats().critical_error_status==-99);
    CHECK(stats().recoveries==1 && stats().recovery_attempts==1);
    CHECK(stats().submitted_samples==5120 && stats().played_samples==1024);
    CHECK(stats().discarded_samples==3072 && co_inits==1 && !co_uninits);
    CHECK(packet(6)); CHECK(current()->starts==1); consume(5); consume(6);
    CHECK(stats().played_samples==3072); xa2_shutdown(); CHECK(co_uninits==1);
    CHECK(stats().played_samples==3072 && stats().discarded_samples==3072);
}
static void recovery_failures(void)
{
    const char *expected[]={"","","E","RE","RME"};
    for(unsigned stage=1;stage<=4;++stage) {
        reset(); CHECK(xa2_init()); CHECK(packet(10)); CHECK(packet(11));
        critical(-77); fail_stage=stage; CHECK(!packet(12));
        CHECK(xa2_is_active() && !g_xa2_source && co_inits==1 && !co_uninits);
        CHECK(strcmp(engines[1].teardown,"RSME")==0);
        CHECK(strcmp(engines[2].teardown,expected[stage])==0);
        CHECK(stats().recovery_attempts==1 && stats().recovery_failed==1);
        CHECK(stats().discarded_samples==2048);
        fail_stage=0; clock_ms=999; CHECK(!packet(12)); CHECK(creates==2);
        clock_ms=1000; CHECK(packet(12)); CHECK(creates==3 && current()->queued==1);
        CHECK(stats().recovery_attempts==2 && stats().recoveries==1);
        fail_start=1; CHECK(packet(13)==XA2_SUBMIT_ACCEPTED); CHECK(current()->queued==2);
        fail_start=0; CHECK(packet(14)); CHECK(current()->starts==1 && current()->queued==3);
        consume(12); consume(13); consume(14); xa2_shutdown(); CHECK(co_uninits==1);
    }
}
static void com_and_owner(void)
{
    for(unsigned stage=1;stage<=4;++stage) {
        reset(); fail_stage=stage; CHECK(!xa2_init()); CHECK(!xa2_is_active());
        CHECK(co_inits==1 && co_uninits==1 && !g_xa2 && !g_xa2_source && !g_xa2_master);
        xa2_shutdown(); CHECK(co_uninits==1);
    }
    reset(); co_result=1; CHECK(xa2_init()); xa2_shutdown(); CHECK(co_uninits==1); /* S_FALSE lease */
    reset(); co_result=RPC_E_CHANGED_MODE; CHECK(xa2_init()); xa2_shutdown(); CHECK(!co_uninits);
    reset(); co_result=-10; CHECK(!xa2_init()); CHECK(!creates && !co_uninits);
    reset(); CHECK(xa2_init()); unsigned before=calls;
    thread_id=8; CHECK(!xa2_init()); CHECK(!packet(1)); xa2_shutdown();
    CHECK(calls==before && xa2_is_active()); thread_id=42; xa2_shutdown(); CHECK(co_uninits==1);
    /* Shutdown remains balanced even while graph creation is unavailable. */
    reset(); CHECK(xa2_init()); critical(-88); fail_stage=1; CHECK(!packet(1));
    xa2_shutdown(); CHECK(!xa2_is_active() && co_uninits==1);
}

/* Compile the actual APU frame-thread routine, including its paused startup
 * path. Its output init, idle handshake and finalization share this owner. */
#define NUM_SAMPLES_PER_FRAME 32
#define NV_PAPU_SECTL 0
#define NV_PAPU_FECTL 1
#define NV_PAPU_SECTL_XCNTMODE 1
#define NV_PAPU_SECTL_XCNTMODE_OFF 0
#define NV_PAPU_FECTL_FEMETHMODE_TRAPPED 2
#define NV_PAPU_FECTL_FEMETHMODE_HALTED 4
#define GET_MASK(value,mask) ((value)&(mask))
#define qatomic_read(p) (*(p))
#define MCPX_APU_DEVICE(p) ((MCPXAPUState*)(p))
typedef int Error;
typedef struct {
    int lock, cond, idle_cond;
    bool exiting, pause_requested, is_idle, set_irq, trace_output;
    uint32_t regs[2]; unsigned ep_frame_div;
    struct { bool ep_fifo_frame_valid; int16_t frame_buf[256][2]; } monitor;
} MCPXAPUState;
static struct { bool active; } g_test_tone;
static int g_mixer_active_count;
static uint64_t g_apu_frames_full, g_apu_frames_monitor;
static struct {
    int16_t samples[2048][2]; int used;
    uint64_t waited_packets, timed_out_packets, dropped_samples, gated_waits;
} g_monitor_packet;
static int locked, initialized, finalized, idle_signaled;
static char lifecycle[16]; static unsigned lifecycle_count;
static void life(char c) { CHECK(lifecycle_count<sizeof(lifecycle)-1); lifecycle[lifecycle_count++]=c; }
static void qemu_mutex_lock(int *lock) { (void)lock; owner_call(); CHECK(!locked); locked=1; life('L'); }
static void qemu_mutex_unlock(int *lock) { (void)lock; owner_call(); CHECK(locked && finalized); locked=0; life('U'); }
static void mcpx_apu_monitor_init(MCPXAPUState *d, Error **err)
{ (void)d; owner_call(); CHECK(locked && !initialized); initialized=1; *err=NULL; life('I'); CHECK(xa2_init()); }
static void warn_reportf_err(Error *err, const char *message) { (void)err; (void)message; CHECK(0); }
static void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{ (void)d; owner_call(); CHECK(locked && initialized && !finalized); finalized=1; life('F'); xa2_shutdown(); }
static MCPXAPUState *frame_state;
static unsigned wait_calls, wait_action;
static unsigned gate_case, gate_waits, gate_expected_div, throttle_action, irq_publications;
static int16_t gate_snapshot[256][2];
static unsigned tail_frame_mode;
#define QEMU_CLOCK_REALTIME 1
#define MIXER_FRAME_SAMPLES 256
#define WAVEOUT_BUF_SAMPLES 2048
/* The extracted production helper includes a dormant waveOut fallback; these
 * definitions keep that branch type-correct while the fixture exercises XA2. */
#define WAVEOUT_NUM_BUFS 4
#define WHDR_INQUEUE 1
#define WHDR_DONE 2
#define MMSYSERR_NOERROR 0
typedef void *HWAVEOUT;
typedef struct { uint32_t dwFlags; } WAVEHDR;
static struct {
    HWAVEOUT hwo;
    WAVEHDR hdrs[WAVEOUT_NUM_BUFS];
    int16_t bufs[WAVEOUT_NUM_BUFS][WAVEOUT_BUF_SAMPLES][2];
    int next_buf;
    bool initialized;
    int frames_written;
} g_waveout;
static unsigned waveOutWrite(HWAVEOUT hwo, WAVEHDR *hdr, unsigned size)
{ (void)hwo; (void)hdr; (void)size; return MMSYSERR_NOERROR; }
static int64_t qemu_clock_get_us(int type) { CHECK(type==QEMU_CLOCK_REALTIME); return (int64_t)clock_ms*1000; }
static void qemu_cond_timedwait(int *cond,int *lock,int ms)
{
    CHECK(cond==&frame_state->cond && lock==&frame_state->lock && locked);
    CHECK(ms>0 && (ms<=2 || (gate_case && ms==5))); locked=0; ++wait_calls; clock_ms+=ms;
    /* The guest gets the device lock during the wait. Engine consumption is
     * independent; neither must be blocked behind an audio-full packet. */
    CHECK(!locked);
    if (gate_case && ms==5) {
        ++gate_waits; life('G');
        CHECK(frame_state->ep_frame_div==gate_expected_div);
        CHECK(!memcmp(gate_snapshot,frame_state->monitor.frame_buf,sizeof(gate_snapshot)));
        if (gate_case==4) frame_state->pause_requested=true;
        else if (gate_case==5) frame_state->exiting=true;
        else {
            /* The ISR can acquire the released APU lock and clear a trap. */
            frame_state->regs[NV_PAPU_SECTL]=1;
            frame_state->regs[NV_PAPU_FECTL]=0;
        }
    }
    if (wait_action==6) {
        frame_state->pause_requested=true;
        if (clock_ms>=22) { consume(1); wait_action=0; }
    }
    if (wait_action==1 && clock_ms>=22) { consume(1); wait_action=0; }
    if (wait_action==2) { frame_state->pause_requested=true; wait_action=0; }
    if (wait_action==3) { frame_state->exiting=true; wait_action=0; }
    if (wait_action==4) { critical(-123); fail_stage=1; wait_action=0; }
    if (wait_action==5) { critical(-124); wait_action=0; }
    locked=1;
}
#include "packet_wait.inc"
static void production_monitor_unavailable_return(MCPXAPUState *d,
                                                  int packet_samples)
{
#include "monitor_unavailable.inc"
}
#define APU_MIXER_MAX_VOICES 64
typedef struct APUMixerVoice {
    volatile int active, looping;
    const int16_t *pcm_data;
    uint32_t pcm_bytes, num_channels, sample_rate;
    uint64_t play_offset;
    float volume;
} APUMixerVoice;
static APUMixerVoice g_mixer_voices[APU_MIXER_MAX_VOICES];
static int g_mixer_initialized=1, g_mixer_cs;
#define EnterCriticalSection(p) ((void)(p))
#define LeaveCriticalSection(p) ((void)(p))
#define InterlockedDecrement(p) (--(*(p)))
#include "mixer_render.inc"
#undef InterlockedDecrement
#undef LeaveCriticalSection
#undef EnterCriticalSection
static void reset_mixer_tail(void)
{
#ifdef APU_OUTPUT_HAS_MIXER_TAIL
    mixer_output_tail_reset();
#endif
}
static void production_deliver_packet(MCPXAPUState *d,int value)
{
    int packet_samples=1024;
    for (unsigned i=0;i<1024;++i) {
        g_monitor_packet.samples[i][0]=(int16_t)value;
        g_monitor_packet.samples[i][1]=(int16_t)value;
    }
#include "packet_delivery.inc"
}
static void qemu_cond_signal(int *cond)
{ CHECK(cond==&frame_state->idle_cond && initialized && locked && xa2_is_active()); ++idle_signaled; life('S'); }
static void qemu_cond_wait(int *cond,int *lock)
{ CHECK(cond==&frame_state->cond && lock==&frame_state->lock && frame_state->is_idle && locked); life('W'); frame_state->exiting=true; }
static void update_irq(MCPXAPUState *d) { (void)d; CHECK(locked); ++irq_publications; }
/* PIO writers (apu_vp.c) are not part of this fixture. */
static void mcpx_apu_pio_handoff(MCPXAPUState *d) { (void)d; }
static void throttle(MCPXAPUState *d)
{
    CHECK(locked && initialized && !finalized); life('T');
    if (throttle_action==1) d->regs[NV_PAPU_FECTL]=NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
    if (throttle_action==2) d->exiting=true;
    if (throttle_action==3) d->pause_requested=true;
    throttle_action=0;
}
static void se_frame(MCPXAPUState *d) { CHECK(locked && xa2_is_active()); life('V'); d->exiting=true; }
static void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    CHECK(locked && xa2_is_active()); life('A');
    if (!tail_frame_mode) { d->exiting=true; return; }
#ifdef APU_OUTPUT_HAS_MIXER_TAIL
    if ((d->ep_frame_div+1)%8) return;
    bool mixer_was_active=g_mixer_active_count!=0;
    if (mixer_was_active) mixer_output_tail_reset();
    mixer_render(d->monitor.frame_buf,256);
    mixer_output_tail_collect_frame(d,d->monitor.frame_buf,1024,mixer_was_active);
#else
    /* Reproduce the pre-tail monitor's finite-voice accumulation exactly:
     * render a real frame, keep its partial packet, and return before submit. */
    if ((d->ep_frame_div+1)%8) return;
    mixer_render(d->monitor.frame_buf,MIXER_FRAME_SAMPLES);
    memcpy(g_monitor_packet.samples+g_monitor_packet.used,d->monitor.frame_buf,
           MIXER_FRAME_SAMPLES*2*sizeof(int16_t));
    memset(d->monitor.frame_buf,0,sizeof(d->monitor.frame_buf));
    g_monitor_packet.used+=MIXER_FRAME_SAMPLES;
    int packet_samples=xa2_get_buffer_size();
    if (g_monitor_packet.used==packet_samples) {
        monitor_submit_xaudio2(d,(const int16_t*)g_monitor_packet.samples,
                               packet_samples);
        g_monitor_packet.used=0;
    }
#endif
}
#include "frame_thread.inc"
static void configure_host_voice(const int16_t *pcm,unsigned frames)
{
    memset(g_mixer_voices,0,sizeof(g_mixer_voices));
    g_mixer_voices[0].active=1;
    g_mixer_voices[0].pcm_data=pcm;
    g_mixer_voices[0].pcm_bytes=frames*sizeof(*pcm);
    g_mixer_voices[0].num_channels=1;
    g_mixer_voices[0].sample_rate=48000;
    g_mixer_voices[0].volume=1.0f;
    g_mixer_active_count=1;
}
#ifdef APU_OUTPUT_HAS_MIXER_TAIL
static void render_host_block(MCPXAPUState *d,int packet_samples)
{
    bool mixer_was_active=g_mixer_active_count!=0;
    if (mixer_was_active) mixer_output_tail_reset();
    mixer_render(d->monitor.frame_buf,256);
    mixer_output_tail_collect_frame(d,d->monitor.frame_buf,packet_samples,
                                    mixer_was_active);
}
#endif
static void mixer_tail_frame_path(void)
{
    reset(); CHECK(xa2_init()); reset_mixer_tail();
    MCPXAPUState d={0}; frame_state=&d;
    locked=initialized=finalized=idle_signaled=0;
    lifecycle_count=0; memset(lifecycle,0,sizeof(lifecycle));
    memset(&g_monitor_packet,0,sizeof(g_monitor_packet));
    memset(&g_mixer_voices,0,sizeof(g_mixer_voices));
    int16_t pcm[100]; for(unsigned i=0;i<100;++i) pcm[i]=(int16_t)(1000+i);
    configure_host_voice(pcm,100);
    g_test_tone.active=false; g_apu_frames_full=g_apu_frames_monitor=0;
    d.regs[NV_PAPU_SECTL]=NV_PAPU_SECTL_XCNTMODE_OFF;
    d.ep_frame_div=7; d.trace_output=true;
    gate_expected_div=8; gate_case=5; gate_waits=wait_calls=wait_action=0;
    tail_frame_mode=1;
    CHECK(mcpx_apu_frame_thread(&d)==NULL);
    CHECK(g_apu_frames_full==0 && g_apu_frames_monitor==1);
    CHECK(d.ep_frame_div==gate_expected_div && gate_waits==1);
    CHECK(g_monitor_packet.gated_waits==1);
#ifdef APU_OUTPUT_HAS_MIXER_TAIL
    CHECK(g_monitor_packet.used==0);
    CHECK(!mixer_output_tail_pending());
    if (strcmp(lifecycle,"LITATTGFU")!=0)
        fprintf(stderr,"Finite host-tail lifecycle: %s\n",lifecycle);
    CHECK(strcmp(lifecycle,"LITATTGFU")==0);
    CHECK(stats().submitted_samples==2048 && stats().start_failed==0);
    CHECK(sources[1].snapshot[0][0]==1000 && sources[1].snapshot[0][198]==1099);
    for(unsigned i=100;i<1024;++i) {
        CHECK(sources[1].snapshot[0][i*2]==0);
        CHECK(sources[1].snapshot[0][i*2+1]==0);
    }
    for(unsigned i=0;i<2048;++i) CHECK(sources[1].snapshot[1][i]==0);
    CHECK(sources[1].starts==1 && !sources[1].alive && !locked);
#else
    /* Negative control: the old loop renders the finite voice but gates with
     * only one 256-sample fragment retained and no accepted PCM. */
    CHECK(g_monitor_packet.used==MIXER_FRAME_SAMPLES);
    CHECK(g_monitor_packet.samples[0][0]==1000 &&
          g_monitor_packet.samples[99][0]==1099);
    CHECK(stats().submitted_samples==0 && sources[1].starts==0);
    CHECK(strcmp(lifecycle,"LITAGFU")==0);
    CHECK(stats().submitted_samples==2048);
#endif
    tail_frame_mode=0; gate_case=0;
}
static void mixer_tail_native_continuation(void)
{
#ifndef APU_OUTPUT_HAS_MIXER_TAIL
    SKIP_TAIL_TEST("mixer_tail_native_continuation");
#else
    reset(); CHECK(xa2_init()); reset_mixer_tail();
    MCPXAPUState d={0}; memset(&g_monitor_packet,0,sizeof(g_monitor_packet));
    int16_t pcm[100]; for(unsigned i=0;i<100;++i) pcm[i]=(int16_t)(2000+i);
    configure_host_voice(pcm,100); d.ep_frame_div=7;
    for(unsigned i=0;i<256;++i)
        d.monitor.frame_buf[i][0]=d.monitor.frame_buf[i][1]=500;
    render_host_block(&d,1024);
    CHECK(g_mixer_active_count==0 && mixer_output_tail_pending());
    CHECK(g_monitor_packet.used==256 && current()->queued==0);
    for(unsigned block=1;block<4;++block) {
        int16_t value=(int16_t)(1000+block*1000);
        for(unsigned i=0;i<256;++i)
            d.monitor.frame_buf[i][0]=d.monitor.frame_buf[i][1]=value;
        render_host_block(&d,1024);
        CHECK(d.ep_frame_div==7);
        if(block<3) CHECK(g_monitor_packet.used==(int)(block+1)*256 && current()->queued==0);
    }
    CHECK(g_monitor_packet.used==0 && current()->queued==1);
    CHECK(g_mixer_output_tail.prime_pending && mixer_output_tail_pending());
    for(unsigned i=0;i<100;++i) {
        CHECK(current()->snapshot[0][i*2]==(int16_t)(2500+i));
        CHECK(current()->snapshot[0][i*2+1]==(int16_t)(2500+i));
    }
    for(unsigned i=100;i<256;++i)
        CHECK(current()->snapshot[0][i*2]==500 && current()->snapshot[0][i*2+1]==500);
    for(unsigned block=1;block<4;++block) for(unsigned i=0;i<256;++i) {
        int16_t value=(int16_t)(1000+block*1000);
        unsigned at=(block*256+i)*2;
        CHECK(current()->snapshot[0][at]==value && current()->snapshot[0][at+1]==value);
    }
    mixer_output_tail_flush(&d);
    CHECK(d.ep_frame_div==7 && !mixer_output_tail_pending());
    CHECK(current()->queued==2 && current()->starts==1);
    for(unsigned i=0;i<2048;++i) CHECK(current()->snapshot[1][i]==0);
    consume(2500); consume(0); xa2_shutdown();

    /* A trap can interrupt the next native packet after a host packet left
     * one retained source buffer. Tail padding must preserve those samples. */
    reset(); CHECK(xa2_init()); reset_mixer_tail();
    memset(&g_monitor_packet,0,sizeof(g_monitor_packet)); CHECK(packet(1));
    g_mixer_output_tail.pending=true; g_mixer_output_tail.prime_pending=true;
    consume(1); /* The old source drains before this native partial is queued. */
    g_monitor_packet.used=256;
    for(unsigned i=0;i<256;++i)
        g_monitor_packet.samples[i][0]=g_monitor_packet.samples[i][1]=7000;
    mixer_output_tail_flush(&d);
    CHECK(current()->queued==1 && g_mixer_output_tail.prime_pending);
    CHECK(stats().submitted_samples==2048 && mixer_output_tail_pending());
    for(unsigned i=0;i<256;++i)
        CHECK(current()->snapshot[0][i*2]==7000 && current()->snapshot[0][i*2+1]==7000);
    for(unsigned i=256;i<1024;++i)
        CHECK(current()->snapshot[0][i*2]==0 && current()->snapshot[0][i*2+1]==0);
    mixer_output_tail_flush(&d);
    CHECK(current()->queued==2 && current()->starts==1);
    CHECK(stats().submitted_samples==3072 && !mixer_output_tail_pending());
    for(unsigned i=0;i<2048;++i) CHECK(current()->snapshot[1][i]==0);
    consume(7000); consume(0); xa2_shutdown();
#endif
}
static void mixer_tail_one_packet_and_empty_queue(void)
{
#ifndef APU_OUTPUT_HAS_MIXER_TAIL
    SKIP_TAIL_TEST("mixer_tail_one_packet_and_empty_queue");
#else
    reset(); CHECK(xa2_init()); reset_mixer_tail();
    MCPXAPUState d={0}; memset(&g_monitor_packet,0,sizeof(g_monitor_packet));
    int16_t pcm[1024]; for(unsigned i=0;i<1024;++i) pcm[i]=(int16_t)(100+i);
    configure_host_voice(pcm,1024);
    for(unsigned block=0;block<4;++block) render_host_block(&d,1024);
    CHECK(g_mixer_active_count==0 && g_monitor_packet.used==0);
    CHECK(mixer_output_tail_pending() && g_mixer_output_tail.prime_pending);
    CHECK(current()->queued==1 && !g_xa2_started && stats().submitted_samples==1024);
    for(unsigned i=0;i<1024;++i) {
        CHECK(current()->snapshot[0][i*2]==(int16_t)(100+i));
        CHECK(current()->snapshot[0][i*2+1]==(int16_t)(100+i));
    }
    mixer_output_tail_flush(&d);
    CHECK(current()->queued==2 && current()->starts==1);
    for(unsigned i=0;i<2048;++i) CHECK(current()->snapshot[1][i]==0);
    CHECK(!mixer_output_tail_pending());
    consume(100); consume(0); xa2_shutdown();

    /* A packet that has already drained does not receive an orphan prime. */
    reset(); CHECK(xa2_init()); reset_mixer_tail();
    memset(&g_monitor_packet,0,sizeof(g_monitor_packet));
    configure_host_voice(pcm,100);
    render_host_block(&d,1024);
    mixer_output_tail_flush(&d);
    CHECK(current()->queued==1 && g_mixer_output_tail.prime_pending);
    consume(100);
    uint64_t submitted=stats().submitted_samples;
    mixer_output_tail_flush(&d);
    CHECK(current()->queued==0 && stats().submitted_samples==submitted);
    CHECK(!mixer_output_tail_pending() && !g_xa2_started);
    xa2_shutdown();
#endif
}
static void mixer_tail_start_retry(void)
{
#ifndef APU_OUTPUT_HAS_MIXER_TAIL
    SKIP_TAIL_TEST("mixer_tail_start_retry");
#else
    MCPXAPUState d={0}; frame_state=&d; locked=1;
    reset(); CHECK(xa2_init()); reset_mixer_tail();
    memset(&g_monitor_packet,0,sizeof(g_monitor_packet));
    CHECK(packet(1)); g_mixer_output_tail.pending=true;
    g_mixer_output_tail.prime_pending=true; fail_start=1;
    mixer_output_tail_flush(&d);
    CHECK(current()->queued==2 && stats().submitted_samples==2048);
    CHECK(stats().start_failed==1 && g_mixer_output_tail.start_retry_pending);
    fail_start=0; mixer_output_tail_flush(&d);
    CHECK(current()->queued==3 && current()->starts==1);
    CHECK(stats().submitted_samples==3072 && !mixer_output_tail_pending());
    CHECK(current()->expected[0]==1);
    for(unsigned q=1;q<3;++q) for(unsigned i=0;i<2048;++i)
        CHECK(current()->snapshot[q][i]==0);
    consume(1); consume(0); consume(0); locked=0; xa2_shutdown();

    /* A persistent Start failure gets one retry and never duplicates source. */
    reset(); CHECK(xa2_init()); reset_mixer_tail();
    memset(&g_monitor_packet,0,sizeof(g_monitor_packet)); CHECK(packet(1));
    g_mixer_output_tail.pending=true; g_mixer_output_tail.prime_pending=true;
    fail_start=1; mixer_output_tail_flush(&d);
    CHECK(current()->queued==2 && stats().submitted_samples==2048);
    mixer_output_tail_flush(&d);
    CHECK(current()->queued==2 && stats().submitted_samples==2048);
    CHECK(stats().start_failed==2 && g_monitor_packet.dropped_samples==1024);
    CHECK(!mixer_output_tail_pending());
    locked=0; fail_start=0; xa2_shutdown();
#endif
}
static void mixer_tail_backpressure_interrupts(void)
{
#ifndef APU_OUTPUT_HAS_MIXER_TAIL
    SKIP_TAIL_TEST("mixer_tail_backpressure_interrupts");
#else
    MCPXAPUState d={0}; frame_state=&d;
    reset(); CHECK(xa2_init()); reset_mixer_tail();
    CHECK(packet(1)); CHECK(packet(2)); CHECK(packet(3));
    locked=1; wait_calls=0; wait_action=6; clock_ms=0;
    memset(&g_monitor_packet,0,sizeof(g_monitor_packet));
    g_monitor_packet.used=256; g_mixer_output_tail.pending=true;
    mixer_output_tail_flush(&d);
    CHECK(wait_calls==11 && clock_ms==22 && d.pause_requested);
    CHECK(!g_monitor_packet.dropped_samples && !g_monitor_packet.timed_out_packets);
    CHECK(current()->queued==3 && stats().submitted_samples==4096);
    CHECK(!mixer_output_tail_pending());
    consume(2); consume(3); consume(0); locked=0; xa2_shutdown();

    reset(); CHECK(xa2_init()); reset_mixer_tail();
    CHECK(packet(1)); CHECK(packet(2)); CHECK(packet(3));
    memset(&g_monitor_packet,0,sizeof(g_monitor_packet));
    g_monitor_packet.used=256; g_mixer_output_tail.pending=true;
    locked=1; wait_calls=0; wait_action=3; clock_ms=0;
    mixer_output_tail_flush(&d);
    CHECK(d.exiting && wait_calls==1 && clock_ms==2);
    CHECK(g_monitor_packet.dropped_samples==1024 && !mixer_output_tail_pending());
    CHECK(current()->queued==3 && stats().submitted_samples==3072);
    locked=0; wait_action=0; xa2_shutdown();
#endif
}
static void mixer_tail_unavailable_backend(void)
{
#ifndef APU_OUTPUT_HAS_MIXER_TAIL
    SKIP_TAIL_TEST("mixer_tail_unavailable_backend");
#else
    reset(); reset_mixer_tail();
    MCPXAPUState d={0};
    d.regs[NV_PAPU_SECTL]=1; /* Native APU remains enabled while pausing. */
    d.pause_requested=true;
    d.ep_frame_div=5;
    g_monitor_packet.used=MIXER_FRAME_SAMPLES;
    g_mixer_output_tail.pending=true;
    g_mixer_output_tail.prime_pending=true;
    for(unsigned i=0;i<256;++i)
        d.monitor.frame_buf[i][0]=d.monitor.frame_buf[i][1]=1234;
    production_monitor_unavailable_return(&d,WAVEOUT_BUF_SAMPLES);
    CHECK(g_monitor_packet.used==0 && !mixer_output_tail_pending());
    CHECK(d.regs[NV_PAPU_SECTL]==1 && d.pause_requested && d.ep_frame_div==5);
    for(unsigned i=0;i<256;++i)
        CHECK(d.monitor.frame_buf[i][0]==0 && d.monitor.frame_buf[i][1]==0);
#endif
}
static void frame_owner_lifecycle(void)
{
    for(unsigned mode=0;mode<3;++mode) {
        reset(); reset_mixer_tail(); MCPXAPUState d={0}; frame_state=&d;
        locked=initialized=finalized=idle_signaled=0; lifecycle_count=0; memset(lifecycle,0,sizeof(lifecycle));
        g_test_tone.active=mode==2; g_mixer_active_count=0;
        d.pause_requested=mode==0; d.regs[NV_PAPU_SECTL]=mode==1 ? 1:0;
        CHECK(mcpx_apu_frame_thread(&d)==NULL);
        CHECK(strcmp(lifecycle,mode==0 ? "LISWFU":mode==1 ? "LITVFU":"LITAFU")==0);
        CHECK(!locked && initialized && finalized && idle_signaled==(mode==0));
        CHECK(!xa2_is_active() && co_inits==1 && co_uninits==1);
        CHECK(avrt_sets==1 && avrt_reverts==1 && avrt_frees==1);
    }
}
static void gated_frame_chronology(void)
{
    for (unsigned mode=0;mode<10;++mode) {
        reset(); reset_mixer_tail(); MCPXAPUState d={0}; frame_state=&d;
        locked=initialized=finalized=idle_signaled=0; lifecycle_count=0; memset(lifecycle,0,sizeof(lifecycle));
        memset(&g_monitor_packet,0,sizeof(g_monitor_packet));
        d.trace_output=true; d.ep_frame_div=3; gate_expected_div=d.ep_frame_div;
        for (unsigned i=0;i<256;++i) for (unsigned ch=0;ch<2;++ch)
            d.monitor.frame_buf[i][ch]=(int16_t)(1+i*2+ch);
        memcpy(gate_snapshot,d.monitor.frame_buf,sizeof(gate_snapshot));
        d.monitor.ep_fifo_frame_valid=mode==5 || mode==6;
        d.regs[NV_PAPU_SECTL]=mode==1 ? 0:1;
        d.regs[NV_PAPU_FECTL]=mode==2 ? NV_PAPU_FECTL_FEMETHMODE_HALTED:NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
        if (mode>=7) d.regs[NV_PAPU_FECTL]=0;
        g_test_tone.active=mode==6; g_mixer_active_count=mode==5;
        gate_case=mode<=4 ? mode+1:mode==7 ? 1:0;
        gate_waits=wait_calls=wait_action=irq_publications=0;
        throttle_action=mode==7 ? 1:mode==8 ? 2:mode==9 ? 3:0;
        CHECK(mcpx_apu_frame_thread(&d)==NULL);
        const char *expected=mode<=2 ? "LIGTVFU":mode==3 ? "LIGSWFU":mode==4 ? "LIGFU":
            mode<=6 ? "LITAFU":mode==7 ? "LITGTVFU":mode==8 ? "LITFU":"LITSWFU";
        CHECK(!strcmp(lifecycle,expected));
        CHECK(gate_waits==(mode<=4 || mode==7));
        CHECK(g_monitor_packet.gated_waits==gate_waits);
        CHECK(d.ep_frame_div==(mode==5 || mode==6 ? gate_expected_div+1:gate_expected_div));
        CHECK(!memcmp(gate_snapshot,d.monitor.frame_buf,sizeof(gate_snapshot)));
        CHECK(!locked && finalized && !xa2_is_active());
        CHECK(avrt_reverts==1 && co_uninits==1);
        if (mode<=4) CHECK(irq_publications>=1);
    }
    gate_case=throttle_action=0;
    g_test_tone.active=false; g_mixer_active_count=0;
}
static void monitor_backpressure(void)
{
    for (unsigned mode=0;mode<6;++mode) {
        reset(); CHECK(xa2_init()); reset_mixer_tail(); CHECK(packet(1)); CHECK(packet(2)); CHECK(packet(3));
        MCPXAPUState d={0}; frame_state=&d; locked=1; wait_calls=0;
        memset(&g_monitor_packet,0,sizeof(g_monitor_packet));
        g_test_tone.active=false; g_mixer_active_count=0;
        wait_action=mode==0 ? 1:mode==1 ? 0:mode;
        production_deliver_packet(&d,4);
        CHECK(locked && g_monitor_packet.waited_packets==1);
        if (mode==0) {
            CHECK(wait_calls==11 && clock_ms==22);
            CHECK(current()->queued==3 && stats().submitted_samples==4096);
            CHECK(!g_monitor_packet.dropped_samples && !g_monitor_packet.timed_out_packets);
            consume(2); consume(3); consume(4);
        } else if (mode==1) {
            CHECK(wait_calls==32 && clock_ms==64);
            CHECK(g_monitor_packet.timed_out_packets==1 && g_monitor_packet.dropped_samples==1024);
            CHECK(current()->queued==3 && stats().submitted_samples==3072);
            consume(1); consume(2); consume(3);
        } else if (mode==2 || mode==3) {
            CHECK(wait_calls==1 && clock_ms==2);
            CHECK(d.pause_requested==(mode==2) && d.exiting==(mode==3));
            CHECK(!g_monitor_packet.timed_out_packets && g_monitor_packet.dropped_samples==1024);
            CHECK(current()->queued==3 && stats().submitted_samples==3072);
            consume(1); consume(2); consume(3);
        } else if (mode==4) {
            CHECK(wait_calls==1 && clock_ms==2 && !g_xa2_source);
            CHECK(!g_monitor_packet.timed_out_packets && g_monitor_packet.dropped_samples==1024);
            CHECK(stats().discarded_samples==3072 && stats().recovery_failed==1);
        } else {
            CHECK(wait_calls==1 && clock_ms==2);
            CHECK(!g_monitor_packet.dropped_samples && !g_monitor_packet.timed_out_packets);
            CHECK(stats().discarded_samples==3072 && stats().recoveries==1);
            CHECK(current()->queued==1 && current()->expected[0]==4); consume(4);
        }
        locked=0; xa2_shutdown();
    }
    /* A Start failure after submission still transfers PCM ownership. The
     * producer must return once and never submit that accepted packet twice. */
    reset(); CHECK(xa2_init()); reset_mixer_tail(); CHECK(packet(1)); fail_start=1;
    MCPXAPUState d={0}; frame_state=&d; locked=1; wait_calls=wait_action=0;
    memset(&g_monitor_packet,0,sizeof(g_monitor_packet));
    production_deliver_packet(&d,2);
    CHECK(!wait_calls && !g_monitor_packet.dropped_samples && !g_xa2_started);
    CHECK(current()->queued==2 && stats().submitted_samples==2048 && stats().start_failed==1);
    fail_start=0; production_deliver_packet(&d,3);
    CHECK(current()->queued==3 && stats().submitted_samples==3072 && current()->starts==1);
    consume(1); consume(2); consume(3); locked=0; xa2_shutdown();
    reset(); CHECK(xa2_init()); reset_mixer_tail(); frame_state=&d; d.exiting=d.pause_requested=false; locked=1;
    wait_calls=wait_action=0; memset(&g_monitor_packet,0,sizeof(g_monitor_packet)); fail_submit=1;
    production_deliver_packet(&d,1);
    CHECK(!wait_calls && g_monitor_packet.dropped_samples==1024 && current()->queued==0);
    locked=0; xa2_shutdown();
}
static void mmcss_owner(void)
{
    for (unsigned mode=0;mode<4;++mode) {
        reset(); avrt_fail=mode; CHECK(xa2_init());
        CHECK(stats().mmcss_registered==(mode==0));
        CHECK(avrt_loads==1 && avrt_sets==(mode==0 || mode==3));
        unsigned before=avrt_reverts; thread_id=7; xa2_shutdown(); CHECK(avrt_reverts==before);
        thread_id=42; xa2_shutdown();
        CHECK(avrt_reverts==(mode==0) && avrt_frees==(mode!=1));
        CHECK(!stats().mmcss_registered && co_uninits==1);
    }
}
static void critical_during_creation(void)
{
    reset(); CHECK(xa2_init()); critical(-77); critical_on_create=1;
    CHECK(!packet(1)); CHECK(!g_xa2_source && xa2_is_active());
    CHECK(stats().critical_errors==2 && stats().recovery_failed==1 && !stats().recoveries);
    CHECK(strcmp(engines[2].teardown,"RSME")==0);
    clock_ms=999; CHECK(!packet(1)); CHECK(creates==2);
    critical_on_create=0; clock_ms=1000; CHECK(packet(1)); CHECK(creates==3 && stats().recoveries==1);
    xa2_shutdown(); CHECK(co_uninits==1);
    reset(); CHECK(xa2_init()); critical(-77); critical_on_register=1; fail_stage=4;
    CHECK(!packet(1)); CHECK(stats().critical_errors==2 && stats().recovery_failed==1);
    CHECK(strcmp(engines[2].teardown,"RME")==0);
    /* Even a repeated pending signal with no live voice cannot bypass the
     * existing delay. The failing graph is already fully destroyed. */
    atomic_exchange(&g_xa2_critical_pending,1); clock_ms=500; CHECK(!packet(1)); CHECK(creates==2);
    clock_ms=999; CHECK(!packet(1)); CHECK(creates==2);
    clock_ms=1000; CHECK(!packet(1)); CHECK(creates==3 && stats().critical_errors==3);
    clock_ms=1500; CHECK(!packet(1)); CHECK(creates==3);
    fail_stage=critical_on_register=0; clock_ms=2000; CHECK(packet(1)); CHECK(creates==4);
    xa2_shutdown(); CHECK(co_uninits==1);
}
static void retry_deadlines(void)
{
    reset(); CHECK(xa2_init()); critical(-77); CHECK(packet(1)); CHECK(creates==2);
    clock_ms=100; critical(-78); CHECK(!packet(2));
    CHECK(!g_xa2_source && xa2_is_active() && creates==2);
    CHECK(stats().discarded_samples==1024 && stats().recoveries==1);
    clock_ms=999; CHECK(!packet(2)); CHECK(creates==2);
    clock_ms=1000; CHECK(packet(2)); CHECK(creates==3 && stats().recoveries==2);
    consume(2); xa2_shutdown(); CHECK(co_uninits==1);
    reset(); CHECK(xa2_init()); clock_ms=100; critical(-77); fail_stage=1; create_delay_ms=500;
    CHECK(!packet(1)); CHECK(clock_ms==600 && creates==2);
    fail_stage=0; create_delay_ms=0; clock_ms=1000; CHECK(!packet(1)); CHECK(creates==2);
    clock_ms=1599; CHECK(!packet(1)); CHECK(creates==2);
    clock_ms=1600; CHECK(packet(1)); CHECK(creates==3); xa2_shutdown(); CHECK(co_uninits==1);
}
int main(void)
{
    fifo_and_priming(); recover_owned_graph(); recovery_failures(); com_and_owner();
    critical_during_creation(); retry_deadlines(); frame_owner_lifecycle();
    mixer_tail_frame_path(); mixer_tail_native_continuation();
    mixer_tail_one_packet_and_empty_queue(); mixer_tail_start_retry();
    mixer_tail_backpressure_interrupts(); mixer_tail_unavailable_backend();
    monitor_backpressure(); mmcss_owner(); gated_frame_chronology();
    printf("APU output graph: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
}
