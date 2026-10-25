/* Audit the linked library, including hardware backends unavailable locally.
 * This is a build-feature check, not a hardware capability claim. */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <stdio.h>

int main(void)
{
    static const char *const encoders[] = {"av1_vaapi",  "av1_nvenc", "av1_vulkan",
                                           "libaom-av1", "libopus",   "pcm_f32le"};
    bool complete = true;
    for (unsigned i = 0; i < sizeof(encoders) / sizeof(encoders[0]); ++i) {
        bool found = avcodec_find_encoder_by_name(encoders[i]) != nullptr;
        printf("encoder %s %s\n", encoders[i], found ? "available" : "missing");
        complete &= found;
    }
    static const char *const decoders[] = {"libdav1d", "opus", "pcm_f32le"};
    for (unsigned i = 0; i < sizeof(decoders) / sizeof(decoders[0]); ++i) {
        bool found = avcodec_find_decoder_by_name(decoders[i]) != nullptr;
        printf("decoder %s %s\n", decoders[i], found ? "available" : "missing");
        complete &= found;
    }
    static const char *const muxers[] = {"matroska", "mp4"};
    for (unsigned i = 0; i < sizeof(muxers) / sizeof(muxers[0]); ++i) {
        bool found = av_guess_format(muxers[i], nullptr, nullptr) != nullptr;
        printf("muxer %s %s\n", muxers[i], found ? "available" : "missing");
        complete &= found;
    }
    static const char *const demuxers[] = {"matroska", "mov"};
    for (unsigned i = 0; i < sizeof(demuxers) / sizeof(demuxers[0]); ++i) {
        bool found = av_find_input_format(demuxers[i]) != nullptr;
        printf("demuxer %s %s\n", demuxers[i], found ? "available" : "missing");
        complete &= found;
    }
    AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_AV1);
    printf("parser av1 %s\n", parser ? "available" : "missing");
    complete &= parser != nullptr;
    av_parser_close(parser);
    unsigned types = 0;
    enum AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
    while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
        if (type == AV_HWDEVICE_TYPE_VAAPI)
            types |= 1;
        if (type == AV_HWDEVICE_TYPE_CUDA)
            types |= 2;
        if (type == AV_HWDEVICE_TYPE_VULKAN)
            types |= 4;
    }
    printf("hardware-contexts vaapi,cuda,vulkan %s\n", types == 7 ? "available" : "missing");
    return complete && types == 7 ? 0 : 1;
}
