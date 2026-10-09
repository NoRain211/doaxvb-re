/* Synthetic guest memory and callbacks; no retail data or generated code. */
#define RECOMP_FULL_PROGRAM
#include "animation_probe.c"
#include "animation_split_adapter.c"
#include "animation_track_adapter.c"

#include <stdlib.h>
#include <setjmp.h>
#include <stdarg.h>

RecompRuntime recomp_runtime;
static uint8_t memory[RECOMP_XBOX_RAM_SIZE];
uint8_t *recomp_fast_ram;
static uint32_t test_frame, test_ebp;
static bool split_on;
static unsigned original_calls;
static jmp_buf stop_target;
static char stopped[128];

uint8_t *recomp_memory(uint32_t address, size_t size)
{
    if (address > sizeof memory || size > sizeof memory-address) {
        fprintf(stderr,"Adapter memory out of bounds: address=%u size=%zu\n",address,size);
        abort();
    }
    return memory+address;
}
uint32_t *recomp_memory_u32_checked(uint32_t address) { return (uint32_t *)recomp_memory(address,4); }
uint16_t *recomp_memory_u16(uint32_t address) { return (uint16_t *)recomp_memory(address,2); }
int8_t *recomp_memory_i8(uint32_t address) { return (int8_t *)recomp_memory(address,1); }
void recomp_guest_load(void *out, uint32_t address, size_t size) { memcpy(out,recomp_memory(address,size),size); }
void recomp_guest_store(uint32_t address, const void *in, size_t size) { memcpy(recomp_memory(address,size),in,size); }
uint32_t *recomp_ebp_register(void) { return &test_ebp; }
uint32_t recomp_d3d_frame_adapter_swap_counter(void) { return test_frame; }
bool recomp_d3d_presenter_split_enabled(void) { return split_on; }
void recomp_stop(int code, const char *format, ...)
{
    va_list args; va_start(args,format); vsnprintf(stopped,sizeof stopped,format,args); va_end(args);
    longjmp(stop_target,code ? code : 1);
}
#define ORIGINAL(name) void name(void) { ++original_calls; recomp_runtime.registers.esp += 4; }
ORIGINAL(sub_000AEA60) ORIGINAL(sub_000AEB50) ORIGINAL(sub_000AEBC0) ORIGINAL(sub_000AEC50)
ORIGINAL(sub_000AF050) ORIGINAL(sub_000AEEF0) ORIGINAL(sub_000AF5C0) ORIGINAL(sub_000636D0)
ORIGINAL(sub_000B01E0) ORIGINAL(sub_00023B10) ORIGINAL(sub_0017D520) ORIGINAL(sub_0017D670)

#define CHECK(c) do { if (!(c)) { fprintf(stderr,"Adapter line %d: %s\n",__LINE__,#c); return 1; } } while (0)
static void store_float(uint32_t address, float value) { recomp_guest_store(address,&value,4); }
static void unit(float m[16]) { memset(m,0,64); m[0]=m[5]=m[10]=m[15]=1; }
int main(int argc, char **argv)
{
    bool strict_mode=argc>1 && strcmp(argv[1],"strict")==0;
    bool verify_mode=argc>1 && strcmp(argv[1],"verify")==0;
    _putenv_s("RECOMP_SPLIT_RATE","auto");
    _putenv_s("RECOMP_SPLIT_STRICT",strict_mode ? "1" : "");
    _putenv_s("RECOMP_ANIMATION_TRACKS",verify_mode ? "verify" : "1");
    if (argc>1 && strcmp(argv[1],"off")==0) {
        _putenv_s("RECOMP_ANIMATION_TRACKS","");
        CHECK(recomp_animation_track_lookup_manual(0xaea60)==NULL);
        CHECK(!recomp_animation_split_enabled());
        CHECK(recomp_animation_probe_lookup_manual(0xb01e0)==NULL);
        return 0;
    }
    CHECK(!recomp_animation_split_enabled()); /* raw auto is unresolved/off */
    split_on=true;
    CHECK(recomp_animation_split_enabled());
    /* Rejected host coefficients must execute the original exactly once. */
    const uint32_t entries[3]={0xaea60,0xaeb50,0xaebc0};
    for (unsigned i=0; i<3; ++i) {
        recomp_runtime.registers.ecx=0x1000; recomp_runtime.registers.edx=0x1100;
        recomp_runtime.registers.esp=0x1200;
        RecompFunction entry=recomp_animation_track_lookup_manual(entries[i]);
        CHECK(entry!=NULL);
        if (setjmp(stop_target)==0) {
            entry();
            CHECK(!verify_mode);
            CHECK(original_calls==i+1 && recomp_runtime.registers.esp==0x1204);
        } else {
            CHECK(verify_mode && strcmp(stopped,"animation:invalid-segment")==0);
            CHECK(original_calls==0 && recomp_runtime.registers.esp==0x1200);
        }
    }
    if (verify_mode) return 0;
    /* A 2x2 synthetic grid placed at RAM's end catches an extra cell read. */
    *recomp_memory_u32(0x009ef810)=RECOMP_XBOX_RAM_SIZE-8;
    *recomp_memory_u32(0x009ef800)=2;
    store_float(0x009ef7e4,1); store_float(0x009ef808,1);
    store_float(0x009ef7e8,1); store_float(0x009ef7ec,7);
    for (unsigned i=0; i<4; ++i) *recomp_memory_u16(RECOMP_XBOX_RAM_SIZE-8+i*2)=i*1024;
    CHECK(ground_height(1,.5f)==7 && ground_height(.5f,1)==7);
    CHECK(ground_height(.5f,.5f)==8.5f);
    /* Frame zero finalizes once and becomes the next transition's endpoint. */
    solved_valid[0]=true;
    for (unsigned i=0; i<32; ++i) store_float(0x004d3650+i*64+56,2);
    recomp_animation_probe_finish_split();
    CHECK(pairs[0].net_displacement[1]==2 && solved_bones[0][0].m[14]==2);
    store_float(0x004d3650+56,3);
    recomp_animation_probe_finish_split();
    CHECK(pairs[0].net_displacement[1]==2 && solved_bones[0][0].m[14]==2);
    RecompVisualPose next={0};
    recomp_animation_split_capture(0,1,&next);
    CHECK(pairs[0].net_displacement[0]==2);
    memset(ready,0,sizeof ready);
    /* A paired world with a malformed final column must reach strict fallback. */
    float worlds[4][16]={{0}}, previous[16], view[16], projection[16];
    unit(worlds[0]); unit(view); unit(projection);
    RecompD3dPresenterDrawCommand draw={0}; draw.vertex_bytes=memory+0x100;
    *recomp_memory_u32(0x00a2479c)=1;
    test_frame=1;
    CHECK(!rigid_pair(1,1,draw.vertex_bytes,worlds[0],previous));
    test_frame=2; worlds[0][3]=NAN;
    uint32_t size=0;
    if (setjmp(stop_target)==0) {
        CHECK(recomp_animation_split_draw(&draw,worlds,1,view,projection,&size)==NULL);
        CHECK(!strict_mode);
    } else CHECK(strict_mode && strcmp(stopped,"split:rigid-mismatch")==0);
    /* Deferred palettes must stop using an invalidated or stale actor pair. */
    test_frame=4;
    recomp_animation_split_capture(0,3,&next);
    recomp_animation_split_capture(0,test_frame,&next);
    unit(worlds[0]); unit(worlds[1]);
    uint8_t recipe[16]={0}; float offsets[24][4]={{0}};
    RecompBoneMatrix initial; unit(initial.m);
    recomp_animation_split_palette(0,test_frame,1,2,recipe,offsets,&initial,
        (const RecompBoneMatrix *)worlds);
    RecompSplitDraw *payload=recomp_animation_split_draw(&draw,worlds,2,view,projection,&size);
    CHECK(payload!=NULL && payload->pose && payload->actor==0);
    free(payload);
    recomp_animation_split_invalidate(0);
    CHECK(recomp_animation_split_draw(&draw,worlds,2,view,projection,&size)==NULL);
    recomp_animation_split_capture(0,test_frame+1,&next);
    CHECK(ready[0]);
    CHECK(recomp_animation_split_draw(&draw,worlds,2,view,projection,&size)==NULL);
    puts("PASS animation adapter rejection, resolved split, terrain edge, first-tick finalization and binding invalidation");
    return 0;
}
