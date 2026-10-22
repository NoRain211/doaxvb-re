#ifndef DOAXBV_RECOMP_MOVIE_VIDEO_H
#define DOAXBV_RECOMP_MOVIE_VIDEO_H

#include "movie_color_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native MPEG-1 video for the game's Sofdec movies, decoded by pl_mpeg with
   the Sofdec fixes in pl_mpeg_sofdec.cmake. */
typedef struct RecompMovieVideo RecompMovieVideo;

typedef struct RecompMovieFrame {
    RecompMoviePlane planes[3]; /* Y, U, V, as recomp_movie_color_convert takes them */
    uint32_t width;
    uint32_t height;
    double time; /* seconds */
} RecompMovieFrame;

/* Opens an MPEG-1 program stream such as an SFD file; NULL if it has no video. */
RecompMovieVideo *recomp_movie_video_open(const char *path);
/* Decodes the next frame in display order; its planes stay valid until the
   next call. Returns false at the end of the stream. */
bool recomp_movie_video_next(RecompMovieVideo *video, RecompMovieFrame *frame);
void recomp_movie_video_close(RecompMovieVideo *video);

#ifdef __cplusplus
}
#endif

#endif
