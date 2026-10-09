/* Packet execution with a synthetic backend; no window, GPU or game runner. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static bool fail_high_timer, fail_regular_timer;
static unsigned regular_timers;
static HANDLE WINAPI highTimer(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
static HANDLE WINAPI regularTimer(LPSECURITY_ATTRIBUTES, BOOL, LPCWSTR);
#define CreateWaitableTimerExW highTimer
#define CreateWaitableTimerW regularTimer
#include "d3d_presenter_thread.cpp"
#undef CreateWaitableTimerExW
#undef CreateWaitableTimerW
static HANDLE WINAPI highTimer(LPSECURITY_ATTRIBUTES attributes, LPCWSTR name, DWORD flags, DWORD access)
{ return fail_high_timer ? nullptr : CreateWaitableTimerExW(attributes,name,flags,access); }
static HANDLE WINAPI regularTimer(LPSECURITY_ATTRIBUTES attributes, BOOL manual, LPCWSTR name)
{ ++regular_timers; return fail_regular_timer ? nullptr : CreateWaitableTimerW(attributes,manual,name); }
#include "animation_camera.h"
#include "animation_pose.h"

static std::vector<char> calls;
static bool prepare_throws;
RecompD3dPresenterError d3d11_backend_create(const RecompD3dPresenterConfig *, RecompD3dPresenter **backend)
{ *backend=reinterpret_cast<RecompD3dPresenter *>(1); return RECOMP_D3D_PRESENTER_OK; }
RecompD3dPresenterError d3d11_backend_submit(RecompD3dPresenter *, const RecompD3dPresenterCommand *)
{ calls.push_back('D'); return RECOMP_D3D_PRESENTER_OK; }
RecompD3dPresenterError d3d11_backend_release_memory(RecompD3dPresenter *, uint32_t, uint32_t)
{ calls.push_back('R'); return RECOMP_D3D_PRESENTER_OK; }
RecompD3dPresenterError d3d11_backend_destroy(RecompD3dPresenter **backend)
{ *backend=nullptr; return RECOMP_D3D_PRESENTER_OK; }
void d3d11_backend_set_immediate_present(bool) {}
void d3d11_backend_set_split_presentation(bool) {}
void d3d11_backend_report_draw_textures() {}
void d3d11_backend_verify_replay(RecompD3dPresenter *, uint32_t, bool) {}
uint64_t d3d11_backend_prepare_bytes(RecompD3dPresenter *, const RecompD3dPresenterCommand *) { return 1; }
void d3d11_backend_prepare(RecompD3dPresenter *, const RecompD3dPresenterCommand *)
{ if (prepare_throws) throw std::bad_alloc(); }
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"Replay line %d: %s\n",__LINE__,#c); return 1; } } while (0)
int main()
{
    CHECK(vsync_presents);
    recomp_d3d_presenter_set_immediate_present(true);
    CHECK(!vsync_presents);
    recomp_d3d_presenter_set_immediate_present(false);
    CHECK(vsync_presents);
    const RecompD3dPresenterConfig config{320,240,RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
        RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8};
    /* Reused texture addresses must execute in order once, at either rate. */
    for (double rate : {0.0, 240.0}) {
        PresenterThread release_thread;
        release_thread.pending=1; release_thread.shutdown=true; release_thread.split_rate=rate;
        auto &release_packet=release_thread.packets[0];
        RecompD3dPresenterCommand draw{};
        draw.type=RECOMP_D3D_PRESENTER_COMMAND_DRAW;
        CHECK(release_packet.add(draw)==RECOMP_D3D_PRESENTER_OK);
        CHECK(release_packet.addRelease(0x1000,64)==RECOMP_D3D_PRESENTER_OK);
        CHECK(release_packet.add(draw)==RECOMP_D3D_PRESENTER_OK);
        draw.type=RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
        draw.data.present.swap_counter=1;
        CHECK(release_packet.add(draw)==RECOMP_D3D_PRESENTER_OK);
        release_packet.seal(true); release_packet.published_ms=clock_ms();
        run(release_thread,config);
        CHECK(calls==std::vector<char>({'D','R','D','D'}));
        CHECK(status(release_thread)==RECOMP_D3D_PRESENTER_OK);
        calls.clear();
    }
    PresenterThread thread;
    auto *backend=reinterpret_cast<RecompD3dPresenter *>(1);
    D3dCapturePacket packet;
    RecompD3dPresenterCommand command{};
    command.type=RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    CHECK(packet.add(command)==RECOMP_D3D_PRESENTER_OK);
    CHECK(packet.addRelease(0x1000,64)==RECOMP_D3D_PRESENTER_OK);
    CHECK(packet.add(command)==RECOMP_D3D_PRESENTER_OK);
    packet.seal(true);
    execute(thread,backend,packet,-1,.25f);
    CHECK(calls==std::vector<char>({'D','R','D'}));
    execute(thread,backend,packet,-1,.75f,false);
    CHECK(calls==std::vector<char>({'D','R','D','D','D'}));
    packet.clear(); calls.clear();
    RecompSplitDraw invalid{};
    command.data.draw.split_pose=&invalid;
    command.data.draw.split_pose_size=sizeof invalid;
    CHECK(packet.add(command)==RECOMP_D3D_PRESENTER_OK); packet.seal(true);
    _putenv_s("RECOMP_SPLIT_STRICT","");
    execute(thread,backend,packet,-1,.5f);
    CHECK(status(thread)==RECOMP_D3D_PRESENTER_OK && calls.size()==1);
    calls.clear(); _putenv_s("RECOMP_SPLIT_STRICT","1");
    execute(thread,backend,packet,-1,.5f);
    CHECK(status(thread)==RECOMP_D3D_PRESENTER_INVALID_ARGUMENT && calls.empty());
    _putenv_s("RECOMP_SPLIT_STRICT","");
    thread.status=RECOMP_D3D_PRESENTER_OK; thread.pending=2; thread.split_rate=120;
    command.data.draw.split_pose=nullptr;
    CHECK(thread.packets[1].add(command)==RECOMP_D3D_PRESENTER_OK);
    thread.packets[1].seal(); thread.packets[1].published_ms=1;
    prepare_throws=true;
    CHECK(!prepare_next(thread,backend,clock_ms()+100));
    CHECK(status(thread)==RECOMP_D3D_PRESENTER_OUT_OF_MEMORY);
    active_thread=&thread;
    CHECK(recomp_d3d_presenter_split_enabled());
    thread.split_rate=0;
    CHECK(!recomp_d3d_presenter_split_enabled());
    active_thread=nullptr;
    /* C++ callers must link to the C models. */
    float view[16], projection[16];
    CHECK(!recomp_animation_camera(nullptr,view,projection));
    CHECK(!recomp_animation_pose_blend(nullptr,0,nullptr,nullptr,0,nullptr));
    _putenv_s("RECOMP_SPLIT_RATE","");
    for (const char *trace : {"1","present"}) {
        _putenv_s("RECOMP_SPLIT_TRACE",trace);
        for (const char *limit : {"","0","junk","3"}) {
            _putenv_s("RECOMP_SPLIT_TRACE_LIMIT",limit);
            RecompD3dPresenter *presenter=nullptr;
            CHECK(recomp_d3d_presenter_create(&config,&presenter)==RECOMP_D3D_PRESENTER_OK);
            CHECK(active_thread->split_trace==(std::strcmp(trace,"present")==0));
            CHECK(active_thread->split_trace_limit==(std::strcmp(limit,"3")==0 ? 3 : 1200));
            CHECK(!recomp_d3d_presenter_split_enabled());
            CHECK(recomp_d3d_presenter_destroy(&presenter)==RECOMP_D3D_PRESENTER_OK);
        }
    }
    _putenv_s("RECOMP_SPLIT_RATE","120"); _putenv_s("RECOMP_SPLIT_TRACE","");
    fail_high_timer=true;
    RecompD3dPresenter *presenter=nullptr;
    CHECK(recomp_d3d_presenter_create(&config,&presenter)==RECOMP_D3D_PRESENTER_OK);
    CHECK(regular_timers==1 && active_thread->split_timer!=nullptr);
    CHECK(recomp_d3d_presenter_destroy(&presenter)==RECOMP_D3D_PRESENTER_OK);
    fail_regular_timer=true;
    CHECK(recomp_d3d_presenter_create(&config,&presenter)==RECOMP_D3D_PRESENTER_HOST_FAILURE);
    CHECK(presenter==nullptr && active_thread==nullptr && regular_timers==2);
    std::puts("PASS replay releases, strict fallback, lookahead failure, resolved split and trace bounds");
    return 0;
}
