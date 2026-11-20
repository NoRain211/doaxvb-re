/* C interface of dlss_bridge.dll (dlss_bridge.cpp); the presenter loads it
   with LoadLibrary and resolves these names, so it needs no NVIDIA headers. */
#ifndef DLSS_BRIDGE_H
#define DLSS_BRIDGE_H

#include <d3d11.h>

#ifdef DLSS_BRIDGE_BUILD
#define DLSS_BRIDGE_API __declspec(dllexport)
#else
#define DLSS_BRIDGE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum DlssBridgeMode {
    DLSS_BRIDGE_DLAA = 0,
    DLSS_BRIDGE_QUALITY = 1,
    DLSS_BRIDGE_BALANCED = 2,
    DLSS_BRIDGE_PERFORMANCE = 3,
    DLSS_BRIDGE_ULTRA_PERFORMANCE = 4,
};

typedef struct DlssBridgeFrame {
    ID3D11Resource *color;   // render-sized scene colour
    ID3D11Resource *depth;   // render-sized depth, D24S8 typeless
    ID3D11Resource *motion;  // render-sized R16G16_FLOAT, render pixels, current to previous
    ID3D11Resource *output;  // output-sized, unordered-access colour
    float jitter_x, jitter_y; // render pixels
    unsigned render_width, render_height;
    int reset;               // 1 after a camera cut or resize
    float frame_ms;
} DlssBridgeFrame;

typedef int (*DlssBridgeInit)(ID3D11Device *device, const wchar_t *data_directory);
typedef int (*DlssBridgeRenderSize)(unsigned output_width, unsigned output_height, int mode,
    unsigned *render_width, unsigned *render_height);
typedef int (*DlssBridgeCreate)(ID3D11DeviceContext *context, unsigned render_width,
    unsigned render_height, unsigned output_width, unsigned output_height, int mode);
typedef int (*DlssBridgeEvaluate)(ID3D11DeviceContext *context, const DlssBridgeFrame *frame);
typedef void (*DlssBridgeShutdown)(void);
typedef const char *(*DlssBridgeError)(void);

#ifdef DLSS_BRIDGE_BUILD
DLSS_BRIDGE_API int dlss_bridge_init(ID3D11Device *device, const wchar_t *data_directory);
DLSS_BRIDGE_API int dlss_bridge_render_size(unsigned output_width, unsigned output_height, int mode,
    unsigned *render_width, unsigned *render_height);
DLSS_BRIDGE_API int dlss_bridge_create(ID3D11DeviceContext *context, unsigned render_width,
    unsigned render_height, unsigned output_width, unsigned output_height, int mode);
DLSS_BRIDGE_API int dlss_bridge_evaluate(ID3D11DeviceContext *context, const DlssBridgeFrame *frame);
DLSS_BRIDGE_API void dlss_bridge_shutdown(void);
DLSS_BRIDGE_API const char *dlss_bridge_error(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
