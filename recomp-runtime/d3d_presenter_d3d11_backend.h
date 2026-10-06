#ifndef DOAXBV_RECOMP_D3D_PRESENTER_D3D11_BACKEND_H
#define DOAXBV_RECOMP_D3D_PRESENTER_D3D11_BACKEND_H

#include "d3d_presenter.h"

RecompD3dPresenterError d3d11_backend_create(
    const RecompD3dPresenterConfig *config, RecompD3dPresenter **presenter);
RecompD3dPresenterError d3d11_backend_submit(
    RecompD3dPresenter *presenter, const RecompD3dPresenterCommand *command);
/* Optional idle-time work: creates textures a later draw will need. */
void d3d11_backend_prepare(RecompD3dPresenter *presenter, const RecompD3dPresenterCommand *command);
/* Texel bytes d3d11_backend_prepare would upload; 0 when every texture exists. */
uint64_t d3d11_backend_prepare_bytes(RecompD3dPresenter *presenter, const RecompD3dPresenterCommand *command);
RecompD3dPresenterError d3d11_backend_release_memory(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size);
RecompD3dPresenterError d3d11_backend_destroy(RecompD3dPresenter **presenter);
void d3d11_backend_set_immediate_present(bool enabled);
void d3d11_backend_set_split_presentation(bool enabled);
void d3d11_backend_report_draw_textures();
void d3d11_backend_verify_replay(RecompD3dPresenter *presenter, uint32_t frame, bool replay);

#endif
