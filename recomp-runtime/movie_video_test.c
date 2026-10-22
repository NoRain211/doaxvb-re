#include "movie_video.h"

#include <stdio.h>
#include <string.h>

#include "pl_mpeg.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "Movie video: line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

typedef struct Bits {
    uint8_t bytes[512];
    size_t bit;
} Bits;

static void put(Bits *b, uint32_t value, unsigned count)
{
    while (count--) {
        if ((value >> count) & 1u) {
            b->bytes[b->bit >> 3] |= (uint8_t)(0x80u >> (b->bit & 7u));
        }
        b->bit++;
    }
}

static void put_text(Bits *b, const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) put(b, (uint8_t)text[i], 8);
}

static void start(Bits *b, uint8_t code)
{
    b->bit = (b->bit + 7u) & ~(size_t)7u;
    put(b, 0x000001u, 24);
    put(b, code, 8);
}

/* pl_mpeg reads a sequence header only with 136 bytes buffered after it. */
static void end(Bits *b)
{
    b->bit = ((b->bit + 7u) & ~(size_t)7u) + 160u * 8u;
    start(b, 0xB7);
}

static void sequence(Bits *b)
{
    start(b, 0xB3);
    put(b, 16, 12); put(b, 16, 12); put(b, 1, 4); put(b, 3, 4);
    put(b, 0x3FFFF, 18); put(b, 1, 1); put(b, 1, 10); put(b, 0, 3);
}

/* One 16x16 intra picture: every luma pixel is luma, chroma is 128. The
   first luma DC differential is passed as its size code and value bits. */
static void picture(Bits *b, uint32_t size_code, unsigned code_bits, uint32_t value, unsigned value_bits)
{
    start(b, 0x00);
    put(b, 0, 10); put(b, 1, 3); put(b, 0xFFFF, 16); put(b, 0, 1);
    start(b, 0x01);
    put(b, 1, 5); put(b, 0, 1); put(b, 1, 1); put(b, 1, 1);
    put(b, size_code, code_bits); put(b, value, value_bits); put(b, 2, 2);
    for (int i = 0; i < 3; i++) { put(b, 4, 3); put(b, 2, 2); }
    for (int i = 0; i < 2; i++) { put(b, 0, 2); put(b, 2, 2); }
}

/* pl_mpeg holds back a stream's last picture, so callers add one more. */
static int decode(Bits *b, const uint8_t expected[], int count)
{
    end(b);
    /* A growable buffer, as the demuxer feeds the decoder. */
    plm_buffer_t *buffer = plm_buffer_create_with_capacity(b->bit >> 3);
    plm_buffer_write(buffer, b->bytes, b->bit >> 3);
    plm_buffer_signal_end(buffer);
    plm_video_t *video = plm_video_create_with_buffer(buffer, 1);
    plm_video_set_no_delay(video, 1);
    for (int i = 0; i < count; i++) {
        plm_frame_t *frame = plm_video_decode(video);
        CHECK(frame != NULL && frame->width == 16u && frame->height == 16u);
        CHECK(frame->y.data[0] == expected[i] && frame->y.data[255] == expected[i]);
        CHECK(frame->cb.data[0] == 128u && frame->cr.data[63] == 128u);
    }
    plm_video_destroy(video);
    return 0;
}

int main(void)
{
    /* Plain MPEG-1: 8-bit DC, predictor 128, so 200 is +72 (size 7). */
    Bits plain = {{0}, 0};
    const uint8_t plain_expected[] = {200};
    sequence(&plain);
    picture(&plain, 0x3E, 6, 72, 7);
    picture(&plain, 0x3E, 6, 72, 7);
    CHECK(decode(&plain, plain_expected, 1) == 0);

    /* Sofdec: TMPGEnc user data sets 11-bit DC, predictor 1024, so 200 is
       +576 and 40 is -704, both size 10, which plain MPEG-1 cannot code. The
       repeated sequence header must keep the frames and the precision. */
    Bits sofdec = {{0}, 0};
    const uint8_t sofdec_expected[] = {200, 40};
    sequence(&sofdec);
    start(&sofdec, 0xB2);
    put_text(&sofdec, "TMPGEXS\0" "00000028IDCPREC\0" "0000000800000003TMPGEXE", 47);
    picture(&sofdec, 0x1FE, 9, 576, 10);
    sequence(&sofdec);
    picture(&sofdec, 0x1FE, 9, 1023u - 704u, 10);
    picture(&sofdec, 0x1FE, 9, 1023u - 704u, 10);
    CHECK(decode(&sofdec, sofdec_expected, 2) == 0);

    /* The wrapper opens a program stream: a pack, a system header that
       declares no video, as in SFD files, and one video packet. */
    Bits stream = {{0}, 0};
    sequence(&stream);
    start(&stream, 0xB2);
    put_text(&stream, "IDCPREC\0" "0000000800000003", 24);
    picture(&stream, 0x1FE, 9, 576, 10);
    picture(&stream, 0x1FE, 9, 576, 10);
    end(&stream);
    const size_t es_bytes = stream.bit >> 3;
    uint8_t ps[600] = {0x00, 0x00, 0x01, 0xBA, 0x21, 0x00, 0x01, 0x00, 0x01, 0x80, 0x00, 0x03,
        0x00, 0x00, 0x01, 0xBB, 0x00, 0x06, 0x80, 0x00, 0x03, 0x00, 0xE0, 0xFF,
        0x00, 0x00, 0x01, 0xE0, (uint8_t)((es_bytes + 1u) >> 8), (uint8_t)(es_bytes + 1u), 0x0F};
    const size_t header_bytes = 31u;
    CHECK(header_bytes + es_bytes <= sizeof ps);
    memcpy(ps + header_bytes, stream.bytes, es_bytes);
    const char *path = "recomp_movie_video_test.mpg";
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL && fwrite(ps, 1, header_bytes + es_bytes, file) == header_bytes + es_bytes &&
        fclose(file) == 0);
    RecompMovieVideo *video = recomp_movie_video_open(path);
    CHECK(video != NULL);
    RecompMovieFrame frame;
    CHECK(recomp_movie_video_next(video, &frame));
    CHECK(frame.width == 16u && frame.height == 16u && frame.planes[0].stride_bytes == 16u);
    CHECK(frame.planes[0].data[17] == 200u && frame.planes[2].data[0] == 128u);
    recomp_movie_video_close(video);
    remove(path);

    puts("PASS movie video: plain and 11-bit DC, repeated sequence header, program stream");
    return 0;
}
