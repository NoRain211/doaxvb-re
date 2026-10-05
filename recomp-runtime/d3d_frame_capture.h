#ifndef DOAXBV_RECOMP_D3D_FRAME_CAPTURE_H
#define DOAXBV_RECOMP_D3D_FRAME_CAPTURE_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* Both threads use the same immutable capture root. Keep capture output private. */
static inline int recomp_d3d_capture_path(char *path, size_t size,
    uint32_t present, const char *name)
{
    const char *root = getenv("RECOMP_D3D_CAPTURE_ROOT");
    if (root == NULL || *root == '\0') root = "private/captures";
    int n = snprintf(path, size, "%s/%u%s%s", root, present,
        *name ? "/" : "", name);
    return n > 0 && (size_t)n < size;
}

#endif
