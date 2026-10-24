# Fixed-function specular and line lists

The D3D11 presenter adds non-local-viewer specular to the existing directional
and point-light material path. Material specular multiplies light specular;
the half-vector uses the light direction and the view matrix's world-space
negative Z axis. Material power controls the highlight. Point specular uses
the same range and attenuation as diffuse. Specular RGB is added after texture
stages, before fog and framebuffer blending; alpha is unchanged. Specular follows
the presenter's per-pixel or Gouraud lighting choice.

The existing lighting admission limits still apply. Local-viewer specular,
spot lights, two-sided lighting and vertex-color material sources remain open.

The XDK 4928 draw adapter reads SPECULARENABLE and LOCALVIEWER at
`0x001f2d24` and `0x001f2d28`. SetMaterial copies 17 floats to device `+0xab0`;
specular starts at float 8 and power is float 16. The active-light list starts
at device `+0x398`, with light specular at float 5. These bindings were checked
in Ghidra against the supported XBE, SHA-256
`053d44e885fa33c1d15d909a533f39dfbd976e97eeaf67e4fdef8438ea7e5c54`.

Indexed draws accept Xbox LINELIST (2). DrawVerticesUP also accepts even line
counts up to 65,536 vertices, for XYZ/diffuse (FVF `0x42`, stride 16) and
XYZRHW/diffuse (`0x44`, stride 20). XYZ lines use world/view/projection;
XYZRHW lines use the existing screen-space conversion. The D3D11 and SDL GPU
presenters draw ordinary one-pixel lines; guest line width and edge
antialiasing are not reproduced. Model-viewer screen masks apply only to quads.

DrawVerticesUP (`0x001e7750`) and DrawIndexedVertices (`0x001e78b0`) still call
the generated D3D8 bodies for driver bookkeeping. Generated setters retain the
lighting/material state consumed at the draw seam. These are temporary library
adapters; wholesale D3D8 replacement remains open work.

Adapter tests cover the 4928 bindings and malformed line inputs. D3D11 readback
tests cover highlight power, ordering before fog, attenuation/range and line
rasterization. Synthetic tests do not establish gameplay or hardware parity.
