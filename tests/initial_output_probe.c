/* Public codec API: bootstrap output must not permanently shrink the FIFO. */
#include "gpu.h"

#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static void run(int initial)
{
    struct gk_gpu gpu;
    CHECK(gk_gpu_open(&gpu, GK_VAAPI, nullptr) == 0);
    AVBufferRef *frames = av_hwframe_ctx_alloc(gpu.device);
    CHECK(frames);
    AVHWFramesContext *fc = (void *)frames->data;
    fc->format = AV_PIX_FMT_VAAPI;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = 320;
    fc->height = 240;
    CHECK(av_hwframe_ctx_init(frames) == 0);
    const AVCodec *codec = avcodec_find_encoder_by_name("av1_vaapi");
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    CHECK(codec && ctx);
    ctx->width = 320;
    ctx->height = 240;
    ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    ctx->sw_pix_fmt = AV_PIX_FMT_NV12;
    ctx->hw_frames_ctx = av_buffer_ref(frames);
    ctx->time_base = (AVRational){1, 60};
    ctx->framerate = (AVRational){60, 1};
    ctx->gop_size = 120;
    ctx->max_b_frames = 0;
    ctx->global_quality = 32;
    CHECK(ctx->hw_frames_ctx && av_opt_set_int(ctx->priv_data, "async_depth", 2, 0) == 0);
    CHECK(av_opt_set_int(ctx->priv_data, "initial_output", initial, 0) == 0);
    CHECK(av_opt_set(ctx->priv_data, "rc_mode", "CQP", 0) == 0);
    CHECK(avcodec_open2(ctx, codec, nullptr) == 0);
    AVFrame *cpu = av_frame_alloc(), *hw = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    CHECK(cpu && hw && packet);
    cpu->format = AV_PIX_FMT_NV12;
    cpu->width = 320;
    cpu->height = 240;
    CHECK(av_frame_get_buffer(cpu, 64) == 0);
    unsigned packets = 0;
    for (int i = 0; i < 2; ++i) {
        for (unsigned p = 0; p < 2; ++p)
            for (unsigned y = 0; y < (p ? 120U : 240U); ++y)
                memset(cpu->data[p] + (size_t)y * (unsigned)cpu->linesize[p],
                       p ? 128 : 16 + i * 219, 320);
        CHECK(av_hwframe_get_buffer(frames, hw, 0) == 0);
        CHECK(av_hwframe_transfer_data(hw, cpu, 0) == 0);
        hw->pts = i;
        CHECK(avcodec_send_frame(ctx, hw) == 0);
        int ret = avcodec_receive_packet(ctx, packet);
        if ((i == 0 && initial) || (i == 1 && !initial)) {
            CHECK(ret == 0 && packet->pts == 0);
            ++packets;
            av_packet_unref(packet);
        } else {
            CHECK(ret == AVERROR(EAGAIN));
            CHECK(vaSyncSurface(gpu.display, (VASurfaceID)(uintptr_t)hw->data[3]) ==
                  VA_STATUS_SUCCESS);
            CHECK(avcodec_receive_packet(ctx, packet) == AVERROR(EAGAIN));
        }
        av_frame_unref(hw);
    }
    CHECK(packets == 1 && avcodec_send_frame(ctx, nullptr) == 0);
    int ret;
    while ((ret = avcodec_receive_packet(ctx, packet)) == 0) {
        CHECK(packet->pts == 1);
        ++packets;
        av_packet_unref(packet);
    }
    CHECK(ret == AVERROR_EOF && packets == 2);
    av_packet_free(&packet);
    av_frame_free(&hw);
    av_frame_free(&cpu);
    avcodec_free_context(&ctx);
    av_buffer_unref(&frames);
    gk_gpu_close(&gpu);
}

int main(void)
{
    run(0);
    run(1);
    puts("initial output: first-picture bootstrap, subsequent FIFO capacity and drain pass");
}
