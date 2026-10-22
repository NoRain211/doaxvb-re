#include "movie_video.h"

#include <stdio.h>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

/* SFD system headers declare no video stream, so the demuxer has to find it
   in the packets; this covers the first video packets. */
enum { PROBE_BYTES = 5u << 20 };

RecompMovieVideo *recomp_movie_video_open(const char *path)
{
    plm_t *plm = plm_create_with_filename(path);

    if (plm == NULL) {
        return NULL;
    }
    plm_set_audio_enabled(plm, 0);
    if (plm_get_num_video_streams(plm) == 0) {
        plm_probe(plm, PROBE_BYTES);
    }
    if (plm_get_num_video_streams(plm) == 0) {
        plm_destroy(plm);
        return NULL;
    }
    return (RecompMovieVideo *)plm;
}

static RecompMoviePlane movie_plane(const plm_plane_t *plane)
{
    return (RecompMoviePlane){plane->data, (size_t)plane->width * plane->height, plane->width};
}

bool recomp_movie_video_next(RecompMovieVideo *video, RecompMovieFrame *frame)
{
    /* shortcut: pl_mpeg never returns a stream's last picture (its ring buffer
       loses the end mark when it compacts), so each movie ends one frame early;
       fix it in pl_mpeg's buffer if a movie's final frame turns out to matter. */
    plm_frame_t *decoded = plm_decode_video((plm_t *)video);

    if (decoded == NULL) {
        return false;
    }
    frame->planes[0] = movie_plane(&decoded->y);
    frame->planes[1] = movie_plane(&decoded->cb);
    frame->planes[2] = movie_plane(&decoded->cr);
    frame->width = decoded->width;
    frame->height = decoded->height;
    frame->time = decoded->time;
    return true;
}

void recomp_movie_video_close(RecompMovieVideo *video)
{
    if (video != NULL) {
        plm_destroy((plm_t *)video);
    }
}
