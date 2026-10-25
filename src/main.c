#include "capture.h"
#include "control.h"
#include "core.h"
#include "devices.h"
#include "gpu.h"
#include "media.h"
#include "pattern.h"
#include "portal.h"
#include "vk_rgb.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <libavutil/avutil.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <time.h>
#include <tracy/TracyC.h>
#include <unistd.h>
#include <va/va_drmcommon.h>

struct options {
    struct gk_media_options media;
    struct gk_capture_options capture;
    enum gk_backend backend;
    const char *device, *test_source, *stats_path;
    uint64_t frames;
    int width, height;
    int64_t duration, static_interval;
    bool cursor, no_drop, realtime, replay;
    const char *session, *directory, *display_name;
    uint32_t source_types;
    int pipewire_fd;
    uint32_t node;
    struct gk_audio_selection *audio;
    struct gk_audio_track *tracks;
    char default_output[96];
};

static const char *const video_clock_names[] = {"auto", "source", "cycle", "receive"};

static void usage(FILE *f, bool all)
{
    fputs(
        "gpuKey — record a Wayland display or window with GPU AV1\n"
        "Usage: gpukey [FILE.mp4|FILE.mkv] [options]\n"
        "       gpukey replay [SECONDS] [options]\n"
        "       gpukey save [FILE] [--session NAME]\n"
        "       gpukey devices\n\n"
        "  gpukey replay 30               keep the last 30 seconds in RAM\n"
        "  gpukey replay 300 -d DP-1 -f 120 -a MIC -a SINK\n"
        "                                 5-minute 120 Hz replay on DP-1 with two audio tracks\n"
        "  gpukey save                    save a clip while recording continues\n"
        "  gpukey                         choose a source; save a dated MP4 here\n"
        "  gpukey demo.mkv -d DP-1        record display DP-1 directly (or omit name for dialog)\n"
        "  gpukey demo.mkv -f 120         cap capture at 120 frames/second\n"
        "  gpukey demo.mkv -a ID -a ID    record two separate audio tracks\n\n"
        "Everyday options:\n"
        "  -o, --output FILE        output path (also accepted as a positional argument)\n"
        "  -d, --display [NAME]     record display NAME (e.g. DP-1), or open display dialog\n"
        "  -w, --window             choose a window in the desktop sharing dialog\n"
        "  -f, --fps N[/D]          capture cap: 60 by default; e.g. 120 or 60000/1001\n"
        "  -a, --audio ID|NAME      add a device as a separate track; repeat for more\n"
        "  -D, --directory DIR      replay clip directory (current directory)\n"
        "  --duration SECONDS       stop automatically; otherwise Ctrl-C finalizes\n"
        "  --no-cursor              omit the cursor\n"
        "  --session NAME           named replay session (default)\n"
        "  --help-all               hardware, quality, budgets and testing controls\n"
        "  --probe                  inspect available VAAPI AV1 devices\n"
        "  --version                show build and FFmpeg versions\n\n"
        "Audio is off unless selected. 'gpukey devices' lists displays, inputs and monitors.\n"
        "With audio and no filename, a dated Matroska file is chosen for channel support.\n"
        "Native audio channels are preserved; tracks are not mixed. The extension selects\n"
        "MP4 (PCM audio) or Matroska (Opus). FPS caps VFR updates; source time is preferred.\n"
        "Replay uses Matroska, 2s static key reuse, compact GPU memory, and auto-scaled RAM\n"
        "by default; Enter also saves when run in a terminal.\n"
        "Clips include the preceding verified keyframe. Ctrl-C discards unsaved history.\n"
        "Existing files are never overwritten. Normal recording finalizes on Ctrl-C.\n",
        f);
    if (!all)
        return;
    fputs("\nAll controls:\n", f);
    fputs(
        "  --backend vaapi|nvenc|vulkan|software   default: vaapi, no silent codec fallback\n"
        "  --device DEVICE       DRM render node for VAAPI; device index for CUDA/Vulkan\n"
        "  --fps N[/D]           maximum capture rate (60); VFR output\n"
        "  --video-clock auto|source|cycle|receive  PipeWire video timing (auto)\n"
        "                        auto prefers source PTS, otherwise reported receive time\n"
        "  --bits 8|10           coded depth (8), SDR; chroma is 4:2:0\n"
        "  --qp N                native quantizer (32); scales differ by backend\n"
        "  --source monitor|window|any   portal source selection (any)\n"
        "  --no-cursor           request capture with the cursor omitted\n"
        "  --allow-shm           permit shared-memory capture and explicit CPU upload\n"
        "  --shm                 require shared-memory capture, retaining the selected encoder\n"
        "  --audio-node TARGET   add an exact PipeWire input (repeatable)\n"
        "  --audio-monitor TARGET add this exact sink monitor (repeatable)\n"
        "  --audio-codec opus|pcm  Matroska: Opus default, PCM for lossless/lower CPU; MP4: PCM\n"
        "  --duration SECONDS    stop after this recording duration\n"
        "  --static-interval N   unchanged-image repeat interval, seconds (1; up to 3600)\n"
        "                        2 permits verified independent-key reuse when idle\n"
        "  --static-encoding auto|always  reuse eligible keys (auto), or encode every refresh\n"
        "  --container mp4|mkv   fragmented MP4 (default, 1 us) or Matroska (1 ms)\n"
        "  --sink buffered|uring output control or ordinary io_uring challenger\n"
        "  --durable             fsync completed file and parent directory on stop\n"
        "  --overload skip|stop  skip unsubmitted frames (default), or fail on exhaustion\n"
        "  --capture-buffers N   source buffer preference/capacity (at least 2)\n"
        "                        (default: prefer 2, allow up to 8 if required)\n"
        "  --encode-depth N      VAAPI/Vulkan parallel submissions (2; 1 limits overlap)\n"
        "  --packet-readout auto|buffered|available|events  completed VAAPI output (auto)\n"
        "  --gpu-memory retain|compact  keep capacity or release idle GPU buffers (retain)\n"
        "  --surfaces N          owned input capacity (auto; VAAPI follows depth)\n"
        "  --cpu-budget-mib N    shared-memory copy budget (512)\n"
        "  --output-buffer-mib N power-of-two output budget (8)\n"
        "  --stats FILE          write a new JSON measurement file\n"
        "  --replay-memory-mib N live replay budget (256); an export reserves up to as much again\n"
        "  --replay-storage auto|reference|copy  share identical keys (default), adopt only, or "
        "copy\n\n"
        "Synthetic checks (no desktop or microphone capture):\n"
        "  --test-source pattern|static|dmabuf|dmabuf-static\n"
        "  --size WxH            synthetic dimensions (1280x720)\n"
        "  --frames N            synthetic frame count (120)\n"
        "  --test-audio          add a synthetic stereo track (repeatable)\n"
        "  --realtime            pace synthetic input by absolute rational deadlines\n\n"
        "Automation with an already authorized remote:\n"
        "  --pipewire-fd FD --node ID   takes an inherited restricted PipeWire FD\n\n"
        "Existing output files are never overwritten. Ctrl-C drains and finalizes.\n"
        "Untyped RGB defaults to sRGB; tagged HDR, unsupported transforms and source\n"
        "changes stop explicitly.\n",
        f);
}

static bool number(const char *text, uint64_t minimum, uint64_t maximum, uint64_t *value)
{
    if (!text || !*text || *text < '0' || *text > '9')
        return false;
    errno = 0;
    char *end;
    unsigned long long n = strtoull(text, &end, 10);
    if (errno || *end || n < minimum || n > maximum)
        return false;
    *value = n;
    return true;
}

static bool rate(const char *text, uint32_t *num, uint32_t *den)
{
    char *copy = strdup(text);
    if (!copy)
        return false;
    char *slash = strchr(copy, '/');
    if (slash)
        *slash++ = '\0';
    uint64_t n, d = 1;
    bool ok = number(copy, 1, INT32_MAX, &n) && (!slash || number(slash, 1, INT32_MAX, &d));
    free(copy);
    if (!ok || n < d || n > d * 1000000ULL)
        return false;
    *num = (uint32_t)n;
    *den = (uint32_t)d;
    return true;
}

static int invalid_value(const char *option, const char *value, const char *expected)
{
    fprintf(stderr, "Invalid %s '%s'; expected %s.\n", option, value, expected);
    return -1;
}

static bool seconds(const char *text, int64_t *ns)
{
    char *end;
    errno = 0;
    double value = strtod(text, &end);
    if (end == text || errno || *end || !isfinite(value) || value <= 0 ||
        value > (double)(INT64_MAX / GK_NS))
        return false;
    *ns = (int64_t)(value * (double)GK_NS);
    return *ns > 0;
}

static int parse(int argc, char **argv, struct options *o)
{
    *o = (struct options){
        .media = {.bits = 8,
                  .qp = 32,
                  .fps_num = 60,
                  .fps_den = 1,
                  .sink = {.slab_size = 262144, .slabs = 32, .stall_ms = 5000}},
        .capture = {.buffers = 8,
                    .preferred_buffers = 2,
                    .cpu_budget = 512U * 1024 * 1024,
                    .fps_num = 60,
                    .fps_den = 1},
        .backend = GK_VAAPI,
        .frames = 120,
        .width = 1280,
        .height = 720,
        .static_interval = GK_NS,
        .cursor = true,
        .source_types = 3,
        .pipewire_fd = -1,
        .session = "default",
        .directory = ".",
    };
    o->replay = argc > 1 && !strcmp(argv[1], "replay");
    bool replay_duration_set = false;
    if (o->replay) {
        optind = 2;
        o->media.replay.duration_ns = 30 * GK_NS;
        o->media.replay.bytes = 256U * 1024 * 1024;
        o->media.matroska = true;
        o->frames = UINT64_MAX;
        if (argc > 2 && argv[2][0] != '-') {
            if (!seconds(argv[2], &o->media.replay.duration_ns))
                return invalid_value("replay interval", argv[2], "positive seconds");
            optind = 3;
            replay_duration_set = true;
        }
    }
    o->audio = calloc((size_t)argc, sizeof(*o->audio));
    o->tracks = calloc((size_t)argc, sizeof(*o->tracks));
    if (!o->audio || !o->tracks)
        return -1;
    o->capture.audio = o->audio;
    o->media.audio = o->tracks;
    enum {
        BACKEND = 256,
        DEVICE,
        FPS,
        VIDEO_CLOCK,
        BITS,
        QP,
        SOURCE,
        CURSOR,
        SHM,
        AUDIO,
        DURATION,
        CONTAINER,
        SINK,
        DURABLE,
        OVERLOAD,
        BUFFERS,
        SURFACES,
        CPU_BUDGET,
        IO_BUDGET,
        STATS,
        TEST,
        SIZE,
        FRAMES,
        TEST_AUDIO,
        REALTIME,
        REMOTE,
        NODE,
        PROBE,
        VERSION,
        MONITOR,
        STATIC_INTERVAL,
        STATIC_ENCODING,
        FORCE_SHM,
        HELP_ALL,
        DISPLAY,
        WINDOW,
        AUDIO_AUTO,
        LIST_AUDIO,
        AUDIO_CODEC,
        DIRECTORY,
        SESSION,
        REPLAY_MEMORY,
        REPLAY_STORAGE,
        ENCODE_DEPTH,
        PACKET_READOUT,
        GPU_MEMORY
    };
    static const struct option opts[] = {
        {"output", required_argument, nullptr, 'o'},
        {"directory", required_argument, nullptr, 'D'},
        {"session", required_argument, nullptr, SESSION},
        {"replay-memory-mib", required_argument, nullptr, REPLAY_MEMORY},
        {"replay-storage", required_argument, nullptr, REPLAY_STORAGE},
        {"help", no_argument, nullptr, 'h'},
        {"help-all", no_argument, nullptr, HELP_ALL},
        {"display", optional_argument, nullptr, 'd'},
        {"window", no_argument, nullptr, 'w'},
        {"audio", required_argument, nullptr, 'a'},
        {"audio-codec", required_argument, nullptr, AUDIO_CODEC},
        {"list-audio", no_argument, nullptr, LIST_AUDIO},
        {"backend", required_argument, nullptr, BACKEND},
        {"device", required_argument, nullptr, DEVICE},
        {"fps", required_argument, nullptr, 'f'},
        {"video-clock", required_argument, nullptr, VIDEO_CLOCK},
        {"bits", required_argument, nullptr, BITS},
        {"qp", required_argument, nullptr, QP},
        {"source", required_argument, nullptr, SOURCE},
        {"no-cursor", no_argument, nullptr, CURSOR},
        {"allow-shm", no_argument, nullptr, SHM},
        {"shm", no_argument, nullptr, FORCE_SHM},
        {"audio-node", required_argument, nullptr, AUDIO},
        {"duration", required_argument, nullptr, DURATION},
        {"static-interval", required_argument, nullptr, STATIC_INTERVAL},
        {"static-encoding", required_argument, nullptr, STATIC_ENCODING},
        {"audio-monitor", required_argument, nullptr, MONITOR},
        {"container", required_argument, nullptr, CONTAINER},
        {"sink", required_argument, nullptr, SINK},
        {"durable", no_argument, nullptr, DURABLE},
        {"overload", required_argument, nullptr, OVERLOAD},
        {"capture-buffers", required_argument, nullptr, BUFFERS},
        {"surfaces", required_argument, nullptr, SURFACES},
        {"encode-depth", required_argument, nullptr, ENCODE_DEPTH},
        {"packet-readout", required_argument, nullptr, PACKET_READOUT},
        {"gpu-memory", required_argument, nullptr, GPU_MEMORY},
        {"cpu-budget-mib", required_argument, nullptr, CPU_BUDGET},
        {"output-buffer-mib", required_argument, nullptr, IO_BUDGET},
        {"stats", required_argument, nullptr, STATS},
        {"test-source", required_argument, nullptr, TEST},
        {"size", required_argument, nullptr, SIZE},
        {"frames", required_argument, nullptr, FRAMES},
        {"test-audio", no_argument, nullptr, TEST_AUDIO},
        {"realtime", no_argument, nullptr, REALTIME},
        {"pipewire-fd", required_argument, nullptr, REMOTE},
        {"node", required_argument, nullptr, NODE},
        {"probe", no_argument, nullptr, PROBE},
        {"version", no_argument, nullptr, VERSION},
        {},
    };
    int c;
    uint64_t n;
    bool test_audio = false, container_set = false, source_set = false, backend_set = false;
    bool replay_memory_set = false, static_interval_set = false, gpu_memory_set = false;
    while ((c = getopt_long(argc, argv, "ho:D:d::wf:a:", opts, nullptr)) != -1) {
        switch (c) {
        case 'h':
        case HELP_ALL:
            usage(stdout, c == HELP_ALL);
            return 1;
        case LIST_AUDIO:
            return gk_devices_list() < 0 ? -1 : 1;
        case VERSION:
            printf("gpukey %s; FFmpeg %s; C23\n", GK_VERSION, av_version_info());
            return 1;
        case PROBE:
            return gk_gpu_probe() < 0 ? -1 : 1;
        case 'D':
        case DIRECTORY:
            if (!o->replay || !*optarg)
                return invalid_value("--directory", optarg, "a directory in replay mode");
            o->directory = optarg;
            break;
        case SESSION:
            if (!o->replay || !gk_control_valid_session(optarg))
                return invalid_value("--session", optarg,
                                     "1–32 letters/digits, '-' or '_' in replay mode");
            o->session = optarg;
            break;
        case REPLAY_MEMORY:
            if (!o->replay || !number(optarg, 1, SIZE_MAX / 2 / 1048576, &n))
                return invalid_value("--replay-memory-mib", optarg, "a positive replay budget");
            o->media.replay.bytes = (size_t)n * 1048576;
            replay_memory_set = true;
            break;
        case REPLAY_STORAGE:
            if (!o->replay ||
                (strcmp(optarg, "copy") && strcmp(optarg, "auto") && strcmp(optarg, "reference")))
                return invalid_value("--replay-storage", optarg,
                                     "auto, reference or copy in replay mode");
            o->media.replay.copy_packets = !strcmp(optarg, "copy");
            o->media.replay.unique_keys = strcmp(optarg, "auto") != 0;
            break;
        case 'o':
            if (o->replay)
                return fprintf(stderr,
                               "Use --directory for replay clips, or 'gpukey save FILE'.\n"),
                       -1;
            if (o->media.output)
                return fprintf(stderr, "Choose one output path.\n"), -1;
            o->media.output = optarg;
            break;
        case BACKEND:
            backend_set = true;
            if (!strcmp(optarg, "vaapi"))
                o->backend = GK_VAAPI;
            else if (!strcmp(optarg, "nvenc"))
                o->backend = GK_NVENC;
            else if (!strcmp(optarg, "vulkan"))
                o->backend = GK_VULKAN;
            else if (!strcmp(optarg, "software"))
                o->backend = GK_SOFTWARE;
            else
                return -1;
            break;
        case DEVICE:
            o->device = optarg;
            break;
        case 'f':
        case FPS:
            if (!rate(optarg, &o->media.fps_num, &o->media.fps_den))
                return invalid_value("--fps", optarg, "N or N/D between 1 and 1000000");
            o->capture.fps_num = o->media.fps_num;
            o->capture.fps_den = o->media.fps_den;
            break;
        case VIDEO_CLOCK: {
            unsigned i;
            for (i = 0; i < sizeof(video_clock_names) / sizeof(video_clock_names[0]); ++i)
                if (!strcmp(optarg, video_clock_names[i]))
                    break;
            if (i > GK_VIDEO_CLOCK_RECEIVE)
                return invalid_value("--video-clock", optarg, "auto, source, cycle or receive");
            o->capture.video_clock = (enum gk_video_clock)i;
            break;
        }
        case BITS:
            if (!number(optarg, 8, 10, &n) || n == 9)
                return -1;
            o->media.bits = (unsigned)n;
            break;
        case QP:
            if (!number(optarg, 0, 255, &n))
                return -1;
            o->media.qp = (int)n;
            break;
        case 'd':
        case DISPLAY:
        case 'w':
        case WINDOW:
        case SOURCE:
            if (source_set)
                return fprintf(stderr, "Choose one of --display, --window, or --source.\n"), -1;
            source_set = true;
            if (c == 'd' || c == DISPLAY || c == 'w' || c == WINDOW) {
                o->source_types = (c == 'd' || c == DISPLAY) ? 1 : 2;
                if (c == 'd' || c == DISPLAY) {
                    const char *name = optarg;
                    if (!name && optind < argc && argv[optind][0] != '-') {
                        const char *next = argv[optind];
                        int64_t dummy_ns;
                        bool is_output =
                            strchr(next, '.') != nullptr || strchr(next, '/') != nullptr;
                        bool is_replay_sec =
                            o->replay && !replay_duration_set && seconds(next, &dummy_ns);
                        if (!is_output && !is_replay_sec)
                            name = argv[optind++];
                    }
                    if (name && !*name)
                        return invalid_value("--display", name, "a display connector name");
                    o->display_name = name;
                }
                break;
            }
            if (!strcmp(optarg, "monitor"))
                o->source_types = 1;
            else if (!strcmp(optarg, "window"))
                o->source_types = 2;
            else if (!strcmp(optarg, "any"))
                o->source_types = 3;
            else
                return -1;
            break;
        case CURSOR:
            o->cursor = false;
            break;
        case SHM:
            o->capture.allow_shm = true;
            break;
        case FORCE_SHM:
            o->capture.allow_shm = true;
            o->capture.force_shm = true;
            break;
        case 'a':
        case AUDIO_AUTO:
        case AUDIO:
        case MONITOR:
            if (!*optarg)
                return fprintf(stderr, "Audio device is empty; run 'gpukey devices'.\n"), -1;
            o->audio[o->capture.audio_count++] =
                (struct gk_audio_selection){.target = optarg,
                                            .kind = c == AUDIO     ? GK_AUDIO_SOURCE
                                                    : c == MONITOR ? GK_AUDIO_MONITOR
                                                                   : GK_AUDIO_AUTO};
            break;
        case AUDIO_CODEC:
            if (!strcmp(optarg, "pcm"))
                o->media.audio_codec = GK_AUDIO_CODEC_PCM;
            else if (!strcmp(optarg, "opus"))
                o->media.audio_codec = GK_AUDIO_CODEC_OPUS;
            else
                return invalid_value("--audio-codec", optarg, "opus or pcm");
            break;
        case DURATION:
            if (!seconds(optarg, &o->duration))
                return invalid_value("--duration", optarg, "positive seconds");
            break;
        case STATIC_INTERVAL:
            if (!number(optarg, 1, 3600, &n))
                return -1;
            o->static_interval = (int64_t)n * GK_NS;
            static_interval_set = true;
            break;
        case STATIC_ENCODING:
            if (strcmp(optarg, "auto") && strcmp(optarg, "always"))
                return invalid_value("--static-encoding", optarg, "auto or always");
            o->media.encode_repeats = !strcmp(optarg, "always");
            break;
        case CONTAINER:
            container_set = true;
            if (strcmp(optarg, "mp4") && strcmp(optarg, "mkv"))
                return -1;
            o->media.matroska = !strcmp(optarg, "mkv");
            break;
        case SINK:
            if (strcmp(optarg, "buffered") && strcmp(optarg, "uring"))
                return -1;
            o->media.sink.kind = !strcmp(optarg, "uring") ? GK_SINK_URING : GK_SINK_BUFFERED;
            break;
        case DURABLE:
            o->media.sink.durable = true;
            break;
        case OVERLOAD:
            if (strcmp(optarg, "skip") && strcmp(optarg, "stop"))
                return -1;
            o->no_drop = !strcmp(optarg, "stop");
            break;
        case BUFFERS:
            if (!number(optarg, 2, 32768, &n))
                return -1;
            o->capture.buffers = (unsigned)n;
            o->capture.preferred_buffers = (unsigned)n;
            break;
        case SURFACES:
            if (!number(optarg, 1, INT32_MAX, &n))
                return -1;
            o->media.surfaces = (unsigned)n;
            break;
        case PACKET_READOUT:
            if (!strcmp(optarg, "auto"))
                o->media.packet_readout = GK_READOUT_AUTO;
            else if (!strcmp(optarg, "buffered"))
                o->media.packet_readout = GK_READOUT_BUFFERED;
            else if (!strcmp(optarg, "available"))
                o->media.packet_readout = GK_READOUT_AVAILABLE;
            else if (!strcmp(optarg, "events"))
                o->media.packet_readout = GK_READOUT_EVENTS;
            else
                return invalid_value("--packet-readout", optarg,
                                     "auto, buffered, available or events");
            break;
        case GPU_MEMORY:
            if (strcmp(optarg, "retain") && strcmp(optarg, "compact"))
                return invalid_value("--gpu-memory", optarg, "retain or compact");
            o->media.compact_gpu = !strcmp(optarg, "compact");
            gpu_memory_set = true;
            break;
        case ENCODE_DEPTH:
            if (!number(optarg, 1, INT32_MAX, &n))
                return -1;
            o->media.encode_depth = (unsigned)n;
            break;
        case CPU_BUDGET:
            if (!number(optarg, 1, SIZE_MAX / 1048576, &n))
                return -1;
            o->capture.cpu_budget = (size_t)n * 1048576;
            break;
        case IO_BUDGET:
            if (!number(optarg, 1, 8192, &n) || (n & (n - 1)))
                return -1;
            o->media.sink.slabs = (uint32_t)n * 4;
            break;
        case STATS:
            o->stats_path = optarg;
            break;
        case TEST:
            o->test_source = optarg;
            break;
        case SIZE: {
            char *copy = strdup(optarg);
            if (!copy)
                return -1;
            char *x = strchr(copy, 'x');
            uint64_t w, h;
            if (x)
                *x++ = '\0';
            bool ok = x && number(copy, 1, INT32_MAX / 4, &w) && number(x, 1, INT32_MAX, &h);
            free(copy);
            if (!ok)
                return -1;
            o->width = (int)w;
            o->height = (int)h;
            break;
        }
        case FRAMES:
            if (!number(optarg, 1, UINT64_MAX, &o->frames))
                return -1;
            break;
        case TEST_AUDIO:
            test_audio = true;
            o->tracks[o->media.audio_count++] =
                (struct gk_audio_track){.format = GK_AUDIO_STEREO, .title = "Synthetic audio"};
            break;
        case REALTIME:
            o->realtime = true;
            break;
        case REMOTE:
            if (!number(optarg, 0, INT32_MAX, &n))
                return -1;
            o->pipewire_fd = (int)n;
            break;
        case NODE:
            if (!number(optarg, 1, UINT32_MAX - 1, &n))
                return -1;
            o->node = (uint32_t)n;
            break;
        default:
            return -1;
        }
    }
    if (o->replay && !replay_duration_set && optind < argc) {
        if (!seconds(argv[optind], &o->media.replay.duration_ns))
            return invalid_value("replay interval", argv[optind], "positive seconds");
        ++optind;
    }
#ifdef GK_VULKAN_RGB
    if (!backend_set && !o->test_source && !o->capture.force_shm && !o->capture.allow_shm &&
        o->media.bits == 8 && !o->media.surfaces && o->pipewire_fd < 0 && !o->device)
        o->backend = GK_VULKAN;
#endif
    if (o->replay) {
        uint64_t seconds_needed = (uint64_t)((o->media.replay.duration_ns + GK_NS - 1) / GK_NS);
        if (!replay_memory_set && seconds_needed > 30) {
            size_t scaled;
            if (!ckd_mul(&scaled, (size_t)seconds_needed, 8U * 1048576U))
                o->media.replay.bytes = scaled;
        }
        if (!static_interval_set && o->media.replay.duration_ns >= 2 * GK_NS)
            o->static_interval = 2 * GK_NS;
#ifdef AV_HAVE_BUFFER_POOL_TRIM
        if (!gpu_memory_set && !o->test_source &&
            (o->backend == GK_VAAPI || o->backend == GK_VULKAN))
            o->media.compact_gpu = true;
#endif
    }
    if (!o->replay && optind < argc && !o->media.output)
        o->media.output = argv[optind++];
    if (optind != argc)
        return fprintf(stderr, "Unexpected argument '%s'; supply one output path.\n", argv[optind]),
               -1;
    if (o->media.encode_depth && o->backend != GK_VAAPI && o->backend != GK_VULKAN)
        return fprintf(stderr, "--encode-depth requires --backend vaapi or vulkan.\n"), -1;
    if (o->media.compact_gpu && o->backend != GK_VAAPI && o->backend != GK_VULKAN)
        return fprintf(stderr, "--gpu-memory compact requires --backend vaapi or vulkan.\n"), -1;
    unsigned depth = o->media.encode_depth ? o->media.encode_depth : 2;
    if (o->media.surfaces && ((o->backend == GK_VAAPI && o->media.surfaces < depth) ||
                              (o->backend != GK_VAAPI && o->media.surfaces < 2)))
        return fprintf(stderr, "Too few --surfaces for this backend and --encode-depth.\n"), -1;
    if ((test_audio && !o->test_source) || (o->test_source && o->capture.audio_count) ||
        ((o->pipewire_fd >= 0) != (o->node != 0)))
        return fprintf(stderr,
                       "Synthetic audio needs --test-source; real audio needs real capture.\n"
                       "Inherited capture needs both --pipewire-fd and --node.\n"),
               -1;
    if (!o->replay && !o->media.output) {
        if (!container_set && (o->capture.audio_count || test_audio))
            o->media.matroska = true;
        struct timespec now;
        struct tm local;
        if (clock_gettime(CLOCK_REALTIME, &now) < 0 || !localtime_r(&now.tv_sec, &local))
            return -1;
        size_t ntime =
            strftime(o->default_output, sizeof(o->default_output), "gpukey-%Y%m%d-%H%M%S", &local);
        if (!ntime)
            return -1;
        snprintf(o->default_output + ntime, sizeof(o->default_output) - ntime, "-%09ld.%s",
                 now.tv_nsec, o->media.matroska ? "mkv" : "mp4");
        o->media.output = o->default_output;
    }
    const char *extension = o->media.output ? strrchr(o->media.output, '.') : nullptr;
    if (!container_set && !o->replay) {
        if (extension && !strcasecmp(extension, ".mkv"))
            o->media.matroska = true;
        else if (!extension || strcasecmp(extension, ".mp4"))
            return fprintf(stderr, "Use a .mp4 or .mkv output, or set --container explicitly.\n"),
                   -1;
    } else if (extension && ((!strcasecmp(extension, ".mkv") && !o->media.matroska) ||
                             (!strcasecmp(extension, ".mp4") && o->media.matroska)))
        return fprintf(stderr, "Output extension conflicts with --container.\n"), -1;
    if (o->replay && o->static_interval > 2 * GK_NS)
        return fprintf(stderr, "Replay needs --static-interval of at most 2 seconds for regular "
                               "random access.\n"),
               -1;
    o->media.independent_repeats = o->static_interval >= 2 * GK_NS;
    if (!o->test_source)
        o->media.audio_count = o->capture.audio_count;
    if (o->media.audio_codec != GK_AUDIO_CODEC_AUTO && !o->media.audio_count)
        return fprintf(stderr, "--audio-codec needs an --audio selection or --test-audio.\n"), -1;
    if (o->media.audio_codec == GK_AUDIO_CODEC_OPUS && !o->media.matroska)
        return fprintf(stderr, "Opus audio requires .mkv; use PCM for .mp4.\n"), -1;
    if (o->test_source && strcmp(o->test_source, "pattern") && strcmp(o->test_source, "static") &&
        strcmp(o->test_source, "dmabuf") && strcmp(o->test_source, "dmabuf-static"))
        return -1;
    if (o->test_source && o->capture.video_clock != GK_VIDEO_CLOCK_AUTO)
        return fprintf(stderr, "--video-clock applies to PipeWire capture, not --test-source.\n"),
               -1;
    if (o->backend == GK_SOFTWARE && o->media.qp > 63)
        return -1;
    o->capture.stop_on_overload = o->no_drop;
    o->media.measure = o->media.sink.measure = o->stats_path != nullptr;
    return 0;
}

static _Atomic int stop_fd = -1;
static_assert(ATOMIC_INT_LOCK_FREE == 2);

static void request_stop(int signal)
{
    (void)signal;
    int saved_errno = errno;
    uint64_t one = 1;
    int fd = atomic_load_explicit(&stop_fd, memory_order_relaxed);
    if (fd >= 0) {
        ssize_t n;
        do {
            n = write(fd, &one, sizeof(one)); /* Async-signal-safe; EAGAIN is already readable. */
        } while (n < 0 && errno == EINTR);
    }
    errno = saved_errno;
}

static int stop_channel(void)
{
    /* Library constructors (notably Tracy) can create threads before main.
     * A main-thread signal mask cannot make signalfd reliable in that case.
     * Keep this one CLOEXEC fd alive until process exit: a handler on any such
     * thread must never race a close followed by descriptor reuse. */
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0)
        return -errno;
    atomic_store_explicit(&stop_fd, fd, memory_order_relaxed);
    struct sigaction action = {.sa_handler = request_stop, .sa_flags = SA_RESTART};
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGINT);
    sigaddset(&action.sa_mask, SIGTERM);
    if (sigaction(SIGINT, &action, nullptr) < 0 || sigaction(SIGTERM, &action, nullptr) < 0)
        return -errno;
    int ret = pthread_sigmask(SIG_UNBLOCK, &action.sa_mask, nullptr);
    return ret ? -ret : fd;
}

static bool interrupted(int fd)
{
    uint64_t count;
    ssize_t n;
    do {
        n = read(fd, &count, sizeof(count));
    } while (n < 0 && errno == EINTR);
    return n == sizeof(count);
}

static int interactive_input(void)
{
    /* A background job must not consume its shell's input or receive SIGTTIN. */
    return isatty(STDIN_FILENO) && tcgetpgrp(STDIN_FILENO) == getpgrp() ? STDIN_FILENO : -1;
}

static int synthetic(struct options *o, struct gk_gpu *gpu, int signals,
                     struct gk_media_stats *stats)
{
    bool dma = !strncmp(o->test_source, "dmabuf", 6);
    bool still = strstr(o->test_source, "static") != nullptr;
    if (dma && gpu->backend != GK_VAAPI)
        return -ENOTSUP;
    struct gk_video_format format = {
        .width = o->width,
        .height = o->height,
        .pixel_format = AV_PIX_FMT_BGRA,
        .transfer = AVCOL_TRC_IEC61966_2_1,
        .drm_format = DRM_FORMAT_ARGB8888,
        .dmabuf = dma,
    };
    size_t bytes;
    if (ckd_mul(&bytes, (size_t)o->width * 4, (size_t)o->height) || bytes > o->capture.cpu_budget)
        return -ENOMEM;
    uint8_t *pixels = malloc(bytes);
    if (!pixels)
        return -ENOMEM;
    struct gk_image image = {.data = pixels, .size = bytes, .stride = o->width * 4};
    VASurfaceID native = VA_INVALID_ID, imported = VA_INVALID_ID;
    VAImage upload = {.image_id = VA_INVALID_ID};
    VADRMPRIMESurfaceDescriptor desc = {};
    struct gk_media *media = nullptr;
    struct gk_control *control = nullptr;
    int ret = 0;
    if (dma) {
        VASurfaceAttrib attr = {
            .type = VASurfaceAttribPixelFormat,
            .flags = VA_SURFACE_ATTRIB_SETTABLE,
            .value = {.type = VAGenericValueTypeInteger, .value.i = VA_FOURCC_BGRA}};
        VAStatus status = vaCreateSurfaces(gpu->display, VA_RT_FORMAT_RGB32, (unsigned)o->width,
                                           (unsigned)o->height, &native, 1, &attr, 1);
        if (status != VA_STATUS_SUCCESS) {
            ret = -ENOTSUP;
            goto done;
        }
        VAImageFormat vf = {.fourcc = VA_FOURCC_BGRA,
                            .byte_order = VA_LSB_FIRST,
                            .bits_per_pixel = 32,
                            .depth = 32,
                            .red_mask = 0x00ff0000,
                            .green_mask = 0x0000ff00,
                            .blue_mask = 0x000000ff,
                            .alpha_mask = 0xff000000};
        status = vaCreateImage(gpu->display, &vf, o->width, o->height, &upload);
        if (status != VA_STATUS_SUCCESS) {
            ret = -ENOTSUP;
            goto done;
        }
        status = vaExportSurfaceHandle(
            gpu->display, native, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &desc);
        if (status != VA_STATUS_SUCCESS || desc.num_layers != 1 || desc.num_objects > 4) {
            ret = -ENOTSUP;
            goto done;
        }
        format.modifier = desc.objects[0].drm_format_modifier;
        format.drm_format = desc.layers[0].drm_format;
        image.plane_count = desc.layers[0].num_planes;
        for (unsigned i = 0; i < image.plane_count; ++i) {
            unsigned object = desc.layers[0].object_index[i];
            image.planes[i] = (struct gk_plane){.fd = desc.objects[object].fd,
                                                .offset = desc.layers[0].offset[i],
                                                .pitch = desc.layers[0].pitch[i],
                                                .size = desc.objects[object].size};
        }
        ret = gk_gpu_import(gpu, &format, &image, &imported);
        if (ret < 0)
            goto done;
    }
    ret = gk_media_open(&media, gpu, &format, &o->media);
    if (ret < 0)
        goto done;
    if (o->replay) {
        ret = gk_control_open(&control, o->session, o->directory, o->media.matroska,
                              gk_media_replay_fd(media), interactive_input());
        if (ret < 0) {
            int ignored = gk_media_close(&media, 0, stats);
            (void)ignored;
            goto done;
        }
    }
    int64_t origin = gk_now_ns(), endpoint = 0;
    uint64_t audio_position = 0;
    for (uint64_t i = 0; i < o->frames; ++i) {
        int64_t pts, end;
        if (!gk_timestamp(i, o->media.fps_num, o->media.fps_den, &pts) ||
            !gk_timestamp(i + 1, o->media.fps_num, o->media.fps_den, &end)) {
            ret = -EOVERFLOW;
            break;
        }
        if (o->duration && pts >= o->duration)
            break;
        if (o->duration && end > o->duration)
            end = o->duration;
        if (o->realtime) {
            int64_t left;
            while ((left = origin + pts - gk_now_ns()) > 0) {
                struct pollfd fds[] = {{signals, POLLIN, 0}, {gk_control_fd(control), POLLIN, 0}};
                int wait = poll(fds, 2, (int)((left + 999999) / 1000000));
                if (wait < 0 && errno != EINTR) {
                    ret = -errno;
                    break;
                }
                gk_control_service(control, media, gk_now_ns() - origin);
                if (fds[0].revents || ret < 0)
                    break;
            }
        }
        if (ret < 0 || interrupted(signals))
            break;
        if (!still || i == 0) {
            gk_test_pattern(pixels, o->width, o->height, i);
            if (dma) {
                void *mapped = nullptr;
                if (vaMapBuffer(gpu->display, upload.buf, &mapped) != VA_STATUS_SUCCESS) {
                    ret = -EIO;
                    break;
                }
                for (int y = 0; y < o->height; ++y)
                    memcpy((uint8_t *)mapped + upload.offsets[0] + (size_t)y * upload.pitches[0],
                           pixels + (size_t)y * (size_t)image.stride, (size_t)image.stride);
                vaUnmapBuffer(gpu->display, upload.buf);
                if (vaPutImage(gpu->display, native, upload.image_id, 0, 0, (unsigned)o->width,
                               (unsigned)o->height, 0, 0, (unsigned)o->width,
                               (unsigned)o->height) != VA_STATUS_SUCCESS ||
                    vaSyncSurface(gpu->display, native) != VA_STATUS_SUCCESS) {
                    ret = -EIO;
                    break;
                }
            }
        }
        AVFrame *frame;
        ret = gk_media_prepare(media, &image, imported, &frame);
        if (ret >= 0)
            ret = gk_media_submit(media, frame, pts);
        if (ret < 0)
            break;
        endpoint = end;
        if (o->media.audio_count) {
            uint64_t audio_end = (uint64_t)av_rescale(end, 48000, GK_NS);
            float audio[960 * 2];
            while (audio_position < audio_end) {
                unsigned n =
                    audio_end - audio_position > 960 ? 960 : (unsigned)(audio_end - audio_position);
                for (unsigned j = 0; j < n; ++j) {
                    float value = (float)(0.1 * sin((double)(audio_position + j) * 440.0 *
                                                    6.283185307179586 / 48000.0));
                    audio[j * 2] = audio[j * 2 + 1] = audio_position + j == 4800 ? 0.9f : value;
                }
                for (unsigned track = 0; track < o->media.audio_count; ++track) {
                    ret = gk_media_audio(media, track, audio, n,
                                         av_rescale((int64_t)audio_position, GK_NS, 48000));
                    if (ret < 0)
                        break;
                }
                if (ret < 0)
                    break;
                audio_position += n;
            }
            if (ret < 0)
                break;
        }
        gk_control_service(control, media, o->realtime ? gk_now_ns() - origin : endpoint);
    }
    {
        int finished = gk_media_close(&media, endpoint, stats);
        if (ret >= 0)
            ret = finished;
        gk_control_finish(control, &stats->replay.last);
    }
done:
    gk_control_close(&control);
    gk_gpu_unimport(gpu, &imported);
    for (unsigned i = 0; i < desc.num_objects; ++i)
        close(desc.objects[i].fd);
    if (upload.image_id != VA_INVALID_ID)
        vaDestroyImage(gpu->display, upload.image_id);
    if (native != VA_INVALID_ID)
        vaDestroySurfaces(gpu->display, &native, 1);
    free(pixels);
    return ret;
}

struct imported {
    uint32_t generation;
    union {
        VASurfaceID surface;
        AVFrame *frame;
    };
    struct gk_plane planes[4];
    unsigned count;
};

static void retire_imports(struct gk_capture *capture, const struct imported *imports,
                           uint64_t *held, int64_t *arrivals, unsigned *count,
                           struct gk_histogram *leases)
{
    for (unsigned i = 0; i < *count;) {
        uint64_t token = held[i];
        const AVFrame *frame = imports[(uint32_t)token].frame;
        if (av_buffer_get_ref_count(frame->buf[0]) != 1) {
            ++i;
            continue;
        }
        gk_capture_release(capture, token);
        if (arrivals)
            gk_hist_add(leases, (uint64_t)(gk_now_ns() - arrivals[i]));
        --*count;
        held[i] = held[*count];
        if (arrivals)
            arrivals[i] = arrivals[*count];
    }
}

static int consume_audio(struct gk_capture *capture, struct gk_media *media,
                         const struct options *o, int64_t first_pts, int64_t endpoint,
                         size_t budget, bool *work)
{
    struct gk_audio_frame audio;
    for (size_t i = 0; i < budget && gk_capture_audio_next(capture, &audio); ++i) {
        *work = true;
        int ret = 0;
        if (media && first_pts >= 0) {
            unsigned n = audio.frames;
            unsigned rate = o->tracks[audio.track].format.rate;
            int64_t pts = audio.pts - first_pts;
            if (endpoint > 0 && pts >= endpoint)
                n = 0;
            else if (endpoint > 0 && pts + (int64_t)n * GK_NS / rate > endpoint)
                n = (unsigned)av_rescale(endpoint - pts, rate, GK_NS);
            ret = gk_media_audio(media, audio.track, audio.samples, n, pts);
        }
        gk_capture_audio_release(capture, audio.token);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int record(struct options *o, struct gk_gpu *gpu, int signals, struct gk_media_stats *stats,
                  struct gk_capture_stats *capture_stats, uint64_t *skipped,
                  struct gk_histogram *leases)
{
    struct gk_portal *portal = nullptr;
    struct gk_portal_source source = {.fd = o->pipewire_fd, .node = o->node};
    struct gk_capture *capture = nullptr;
    struct gk_media *media = nullptr;
    struct gk_control *control = nullptr;
    uint64_t *held = nullptr;
    int64_t *held_arrivals = nullptr;
    unsigned held_count = 0;
    bool direct = false, have_input = false;
    struct gk_capture_frame input;
    struct imported *imports = calloc(o->capture.buffers, sizeof(*imports));
    if (!imports)
        return -ENOMEM;
    if (gpu->backend != GK_VULKAN)
        for (unsigned i = 0; i < o->capture.buffers; ++i)
            imports[i].surface = VA_INVALID_ID;
    int ret = 0;
    if (source.fd < 0) {
        if (o->display_name) {
            ret = gk_portal_open_display(&portal, &source, o->display_name, o->cursor, signals);
        } else {
            fprintf(stderr, "Select the source in the desktop sharing dialog.\n");
            ret = gk_portal_open(&portal, &source, o->source_types, o->cursor, signals);
        }
        if (ret < 0)
            goto done;
    }
    ret = gk_capture_open(&capture, source, gpu, &o->capture);
    if (ret < 0)
        goto done;
    int64_t first_pts = -1, last_pts = -1, last_arrival = 0, last_source_pts = 0;
    int64_t endpoint = 0, startup = gk_now_ns();
    struct gk_video_format format;
    bool stop = false, duration_reached = false, completion_ready = false, waited = false;
    uint64_t completion_generation = 0;
    while (!stop) {
        bool work = false;
        ret = waited || direct ? gk_media_collect(media, completion_ready) : 0;
        completion_ready = false;
        waited = false;
        if (ret < 0)
            break;
        uint64_t collected_generation = gk_media_completion_generation(media);
        if (collected_generation != completion_generation) {
            /* Collection can recycle an exported fence before the portal's
             * nonblocking revocation dispatch below. Remove stale watches now. */
            gk_portal_clear_completion(portal);
            completion_generation = collected_generation;
        }
        if (direct)
            retire_imports(capture, imports, held, held_arrivals, &held_count, leases);
        gk_drain_event(gk_capture_fd(capture));
        if (interrupted(signals))
            break;
        if (portal && gk_portal_closed(portal)) {
            ret = -ECANCELED;
            break;
        }
        ret = gk_capture_error(capture);
        if (ret < 0)
            break;
        if (!media && gk_capture_format(capture, &format) &&
            gk_capture_audio_formats(capture, o->tracks)) {
            ret = gk_media_open(&media, gpu, &format, &o->media);
            if (ret < 0)
                break;
            direct = gpu->backend == GK_VULKAN && format.dmabuf;
            if (direct) {
                held = calloc(o->capture.buffers, sizeof(*held));
                if (o->stats_path)
                    held_arrivals = calloc(o->capture.buffers, sizeof(*held_arrivals));
                if (!held || (o->stats_path && !held_arrivals)) {
                    ret = -ENOMEM;
                    break;
                }
            }
            /* Negotiation/removal callbacks continue while the encoder opens.
             * Do not announce readiness after an already latched source error. */
            ret = gk_capture_error(capture);
            if (ret < 0)
                break;
            if (o->replay) {
                ret = gk_control_open(&control, o->session, o->directory, o->media.matroska,
                                      gk_media_replay_fd(media), interactive_input());
                if (ret < 0)
                    break;
            } else
                fprintf(stderr, "Recording. Ctrl-C stops and finalizes.\n");
        }
        /* One video job per turn bounds audio starvation without another
         * worker/queue. Busy turns continue directly, without a poll syscall. */
        for (unsigned batch = 0;
             batch < 1 && media && (have_input || (have_input = gk_capture_next(capture, &input)));
             ++batch) {
            have_input = false;
            work = true;
            if (first_pts < 0)
                first_pts = input.pts;
            int64_t pts = input.pts - first_pts;
            if (o->duration && pts >= o->duration) {
                duration_reached = true;
                gk_capture_release(capture, input.token);
                stop = true;
                break;
            }
            if (pts <= last_pts || gk_now_ns() - input.arrival > 250000000) {
                ++*skipped;
                gk_capture_release(capture, input.token);
                if (o->no_drop) {
                    ret = -ENOBUFS;
                    stop = true;
                    break;
                }
                continue;
            }
            if (direct) {
                if (input.image.plane_count != 1) {
                    ret = -ENOTSUP;
                    stop = true;
                    break;
                }
                struct pollfd producer = {.fd = input.image.planes[0].fd, .events = POLLIN};
                int ready = poll(&producer, 1, 0);
                if (ready < 0 && errno != EINTR) {
                    ret = -errno;
                    stop = true;
                    break;
                }
                if (producer.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    ret = -EIO;
                    stop = true;
                    break;
                }
                if (!(producer.revents & POLLIN)) {
                    have_input = true;
                    work = false;
                    break; /* Service audio/control while the producer writes. */
                }
            }
            unsigned index = (uint32_t)input.token;
            struct imported *item = &imports[index];
            uint32_t generation = (uint32_t)(input.token >> 32);
            if (format.dmabuf &&
                (item->generation != generation || item->count != input.image.plane_count ||
                 memcmp(item->planes, input.image.planes, sizeof(item->planes)))) {
                if (direct) {
                    av_frame_free(&item->frame);
                    ret = gk_vk_rgb_import(gpu, &format, &input.image, &item->frame);
                } else {
                    gk_gpu_unimport(gpu, &item->surface);
                    ret = gk_gpu_import(gpu, &format, &input.image, &item->surface);
                }
                if (ret < 0) {
                    gk_capture_release(capture, input.token);
                    stop = true;
                    break;
                }
                item->generation = generation;
                item->count = input.image.plane_count;
                memcpy(item->planes, input.image.planes, sizeof(item->planes));
            }
            AVFrame *frame;
            if (format.dmabuf) {
                if (!gk_capture_begin_submission(capture, input.token)) {
                    ++*skipped;
                    gk_capture_release(capture, input.token);
                    if (o->no_drop) {
                        ret = -ESTALE;
                        stop = true;
                        break;
                    }
                    continue;
                }
                if (direct) {
                    /* Admission precedes revocation, but the retained source
                     * and duplicated import FDs protect its lifetime through
                     * later GPU submission. Codec/mux work must not hold the
                     * PipeWire loop lock or delay removal/audio callbacks. */
                    gk_capture_end_submission(capture);
                    if (held_count == o->capture.buffers) {
                        ret = -EPROTO;
                        stop = true;
                        break;
                    }
                    held[held_count] = input.token;
                    if (held_arrivals)
                        held_arrivals[held_count] = input.arrival;
                    ++held_count;
                    ret = gk_media_submit_imported(media, item->frame, pts);
                } else {
                    ret = gk_media_prepare_begin(media, &input.image, item->surface, &frame);
                    gk_capture_end_submission(capture);
                }
                if (ret >= 0 && !direct)
                    ret = gk_media_prepare_finish(media, frame);
            } else {
                ret = gk_media_prepare(media, &input.image, item->surface, &frame);
            }
            if (!direct && (ret >= 0 || ret == -EAGAIN))
                gk_capture_release(capture, input.token);
            if (o->stats_path && !direct)
                gk_hist_add(leases, (uint64_t)(gk_now_ns() - input.arrival));
            if (ret == -EAGAIN && !o->no_drop) {
                ++*skipped;
                ret = 0;
                continue;
            }
            if (ret < 0) {
                stop = true;
                break;
            }
            if (!direct)
                ret = gk_media_submit(media, frame, pts);
            if (ret < 0) {
                stop = true;
                break;
            }
            last_pts = pts;
            last_source_pts = pts;
            last_arrival = input.arrival;
            endpoint = pts + 1000;
            if (direct)
                retire_imports(capture, imports, held, held_arrivals, &held_count, leases);
        }
        /* Until a video epoch is known, preserve queued audio. Consuming and
         * discarding it here loses the first audio block even without a GPU
         * wait; those samples can belong after the eventual video origin. */
        if (!stop && media && first_pts >= 0)
            ret = consume_audio(capture, media, o, first_pts, o->duration,
                                32U * o->capture.audio_count, &work);
        if (ret < 0)
            stop = true;
        if (stop)
            break;
        int64_t now = gk_now_ns();
        if (last_pts < 0) {
            if (now - startup > 30000000000LL) {
                ret = -ETIMEDOUT;
                break;
            }
        } else {
            endpoint = last_source_pts + now - last_arrival;
            if (o->duration && endpoint >= o->duration) {
                duration_reached = true;
                endpoint = o->duration;
                break;
            }
            /* Repeat our owned last image or an eligible independent key to
             * limit static sample durations and seek gaps without recapture. */
            if (endpoint - last_pts >= o->static_interval) {
                ret = gk_media_repeat(media, endpoint);
                if (ret < 0)
                    break;
                last_pts = endpoint;
                if (direct)
                    retire_imports(capture, imports, held, held_arrivals, &held_count, leases);
            }
        }
        gk_control_service(control, media, endpoint);
        ret = gk_media_error(media);
        if (ret < 0)
            break;
        int timeout = first_pts < 0
                          ? 1000
                          : (int)((o->static_interval - (endpoint - last_pts) + 999999) / 1000000);
        if (o->duration && first_pts >= 0 && o->duration - endpoint < (int64_t)timeout * 1000000)
            timeout = (int)((o->duration - endpoint + 999999) / 1000000);
        if (work)
            continue;
        int completion_fd = gk_media_completion_fd(media);
        uint64_t generation = gk_media_completion_generation(media);
        if (generation != completion_generation) {
            /* Codec calls run on this owner, outside GLib dispatch. Remove
             * registrations for closed/recycled descriptors before polling. */
            gk_portal_clear_completion(portal);
            completion_generation = generation;
        }
        if (portal) {
            int ready = gk_portal_wait_gpu(portal, gk_capture_fd(capture), gk_control_fd(control),
                                           completion_fd,
                                           have_input ? input.image.planes[0].fd : -1, timeout);
            if (ready < 0) {
                ret = ready;
                break;
            }
            completion_ready = ready > 0;
        } else {
            struct pollfd fds[] = {{gk_capture_fd(capture), POLLIN, 0},
                                   {signals, POLLIN, 0},
                                   {gk_control_fd(control), POLLIN, 0},
                                   {completion_fd, POLLIN, 0},
                                   {have_input ? input.image.planes[0].fd : -1, POLLIN, 0}};
            if (poll(fds, 5, timeout) < 0 && errno != EINTR) {
                ret = -errno;
                break;
            }
            completion_ready = fds[3].revents != 0;
        }
        waited = true;
    }
    if (last_pts >= 0) {
        /* A rejected frame at/after the requested boundary proves that the
         * source timeline reached it. A new arrival-time estimate can be
         * shorter when the previous accepted frame had more delivery latency. */
        endpoint = duration_reached ? o->duration : last_source_pts + gk_now_ns() - last_arrival;
        if (o->duration && endpoint > o->duration)
            endpoint = o->duration;
        if (endpoint <= last_pts)
            endpoint = last_pts + 1000;
    }
    gk_capture_stop(capture);
    if (ret >= 0)
        ret = gk_capture_error(capture);
    if (media && first_pts >= 0) {
        bool work = false;
        int drained = consume_audio(capture, media, o, first_pts, endpoint, SIZE_MAX, &work);
        if (ret >= 0)
            ret = drained;
    }
    /* All accepted work has completed its source reads. Stop admission before
     * destroying wrappers; source imports remain live until media retirement. */
    gk_portal_clear_completion(portal);
    {
        int finished = gk_media_close(&media, endpoint, stats);
        if (ret >= 0)
            ret = finished;
        if (direct && ret >= 0)
            retire_imports(capture, imports, held, held_arrivals, &held_count, leases);
        gk_control_finish(control, &stats->replay.last);
    }
    if (first_pts < 0 && ret >= 0)
        ret = -ENODATA;
done:
    gk_capture_stop(capture);
    for (unsigned i = 0; i < o->capture.buffers; ++i)
        if (gpu->backend == GK_VULKAN)
            av_frame_free(&imports[i].frame);
        else
            gk_gpu_unimport(gpu, &imports[i].surface);
    /* On a GPU error, withheld source leases stay withheld through device
     * teardown. A failed completion is never reinterpreted as input release. */
    gk_gpu_close(gpu);
    gk_capture_close(&capture, capture_stats);
    free(imports);
    free(held);
    free(held_arrivals);
    gk_portal_close(&portal);
    gk_control_close(&control);
    return ret;
}

static void histogram(FILE *file, const char *name, const struct gk_histogram *h)
{
    fprintf(file,
            "  \"%s\": {\"count\": %" PRIu64 ", \"mean_ns\": %.1f, "
            "\"p50_bucket_upper_ns\": %" PRIu64 ", \"p99_bucket_upper_ns\": %" PRIu64
            ", \"max_ns\": %" PRIu64 "}",
            name, h->count, h->count ? (double)h->total / (double)h->count : 0.0,
            gk_hist_percentile(h, 50), gk_hist_percentile(h, 99), h->maximum);
}

static int save_command(int argc, char **argv)
{
    const char *session = "default";
    static const struct option opts[] = {
        {"session", required_argument, nullptr, 's'}, {"help", no_argument, nullptr, 'h'}, {}};
    optind = 2;
    int c;
    while ((c = getopt_long(argc, argv, "hs:", opts, nullptr)) != -1) {
        if (c == 'h') {
            puts("Usage: gpukey save [FILE] [--session NAME]\n\n"
                 "Save the running replay buffer while capture continues.\n"
                 "Without FILE, use a dated name in the replay's output directory.\n"
                 "A filename without an extension gets the session's container extension.\n"
                 "Bind 'gpukey save' to a desktop shortcut to save while another app is focused.");
            return 0;
        }
        if (c != 's' || !gk_control_valid_session(optarg)) {
            fputs("Invalid save arguments. Run 'gpukey save --help'.\n", stderr);
            return 2;
        }
        session = optarg;
    }
    if (argc - optind > 1) {
        fputs("Supply one clip filename. Run 'gpukey save --help'.\n", stderr);
        return 2;
    }
    int ret = gk_control_save(session, optind < argc ? argv[optind] : nullptr);
    if (ret < 0) {
        if (ret == -ENOTCONN)
            fprintf(stderr, "No running replay session '%s'. Start 'gpukey replay 30' first.\n",
                    session);
        else if (ret == -EBUSY)
            fputs("A save is already in progress; recording continues.\n", stderr);
        else if (ret == -EAGAIN)
            fputs("The replay buffer is warming up; try again shortly.\n", stderr);
        else
            fprintf(stderr, "Replay save failed: %s. Check the recorder's message for details.\n",
                    strerror(-ret));
    }
    return ret < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    av_log_set_level(AV_LOG_WARNING);
    if (argc > 1 && !strcmp(argv[1], "save"))
        return save_command(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "devices")) {
        if (argc == 3 && (!strcmp(argv[2], "--help") || !strcmp(argv[2], "-h"))) {
            puts(
                "Usage: gpukey devices\n\nList visible PipeWire audio inputs and output monitors.\n"
                "Use an ID or exact node name with repeatable --audio selections.\n"
                "Listing does not start capture. Display selection uses --display.");
            return 0;
        }
        if (argc != 2) {
            fputs("Usage: gpukey devices (no arguments)\n", stderr);
            return 2;
        }
        int listed = gk_devices_list();
        if (listed < 0)
            fprintf(stderr, "Cannot list PipeWire audio devices: %s\n", strerror(-listed));
        return listed < 0 ? 1 : 0;
    }
    if (argc == 2 && !strcmp(argv[1], "help")) {
        usage(stdout, false);
        return 0;
    }
    struct options options;
    int ret = parse(argc, argv, &options);
    if (ret != 0) {
        if (ret < 0)
            fprintf(stderr, "Invalid arguments. Run 'gpukey --help' for examples or '--help-all' "
                            "for all controls.\n");
        free(options.audio);
        free(options.tracks);
        return ret < 0 ? 2 : 0;
    }
    int signals = stop_channel();
    if (signals < 0) {
        free(options.audio);
        free(options.tracks);
        return 1;
    }
    int64_t start = gk_now_ns(); /* Include device setup and teardown in session cost. */
    struct gk_gpu gpu;
    ret = gk_gpu_open(&gpu, options.backend, options.device);
    if (ret < 0) {
        fprintf(stderr, "Requested GPU plan unavailable (%d). --probe lists VAAPI eligibility.\n",
                ret);
        free(options.audio);
        free(options.tracks);
        return 1;
    }
    if (options.replay)
        fprintf(stderr,
                "Rolling %.3f seconds; encoded RAM budget %zu MiB plus one export.\n"
                "Saved clips include the preceding keyframe and the latest completed audio.\n"
                "device: %s\n",
                (double)options.media.replay.duration_ns / (double)GK_NS,
                options.media.replay.bytes / 1048576, gpu.name);
    else
        fprintf(stderr, "Saving to %s\ndevice: %s\n", options.media.output, gpu.name);
    struct gk_media_stats stats = {};
    struct gk_capture_stats capture = {};
    struct gk_histogram leases = {};
    uint64_t skipped = 0;
    if (options.test_source)
        ret = synthetic(&options, &gpu, signals, &stats);
    else
        ret = record(&options, &gpu, signals, &stats, &capture, &skipped, &leases);
    gk_gpu_close(&gpu);
    int64_t elapsed = gk_now_ns() - start;
    if (options.replay)
        fprintf(stderr,
                "Replay stopped: %" PRIu64 " clips saved, %" PRIu64
                " failed; unsaved history discarded.\n",
                stats.replay.saved, stats.replay.failed);
    else
        fprintf(stderr,
                "%s: %" PRIu64 " video inputs, %" PRIu64 " encoded packets; %" PRIu64
                " capture skips, %" PRIu64 " FPS-limited; accepted/written/durable bytes=%" PRIu64
                "/%" PRIu64 "/%" PRIu64 "\n",
                ret < 0 ? "Incomplete recording" : "Completed", stats.submitted, stats.packets,
                skipped + capture.skipped, capture.rate_limited, stats.sink.accepted,
                stats.sink.written, stats.sink.durable);
    if (stats.reused_packets)
        fprintf(stderr, "Reused %llu independent static keys without encoding.\n",
                (unsigned long long)stats.reused_packets);
    if (options.stats_path) {
        FILE *file = fopen(options.stats_path, "wx");
        if (!file) {
            int error = errno;
            perror("create stats");
            if (ret >= 0)
                ret = -error;
        } else {
            struct rusage usage_data;
            getrusage(RUSAGE_SELF, &usage_data);
            int64_t user_us =
                usage_data.ru_utime.tv_sec * INT64_C(1000000) + usage_data.ru_utime.tv_usec;
            int64_t system_us =
                usage_data.ru_stime.tv_sec * INT64_C(1000000) + usage_data.ru_stime.tv_usec;
            fprintf(file,
                    "{\n  \"cpu_user_us\": %" PRId64 ", \"cpu_system_us\": %" PRId64 ",\n"
                    "  \"requested_synthetic_frames\": %" PRIu64 ",\n",
                    user_us, system_us, options.test_source ? options.frames : 0);
            fprintf(file,
                    "  \"audio_input_samples\": %" PRIu64
                    ", \"audio_boundary_silence_samples\": %" PRIu64
                    ", \"audio_corrupt_silence_samples\": %" PRIu64 ",\n",
                    stats.audio_samples, stats.audio_silence, capture.audio_silenced_samples);
            fprintf(file, "  \"audio_tracks\": %u, \"fps_limited\": %" PRIu64 ",\n",
                    options.media.audio_count, capture.rate_limited);
            fprintf(file,
                    "  \"encoder_inputs\": %u, \"encode_depth\": %u, "
                    "\"peak_source_buffers\": %u,\n",
                    stats.encoder_inputs, stats.encode_depth, capture.peak_source_buffers);
            fprintf(file, "  \"completion_readout\": %s,\n",
                    stats.available_readout ? "true" : "false");
            fprintf(file,
                    "  \"completion_events\": %s, \"completion_wakeups\": %" PRIu64
                    ", \"completion_fallbacks\": %" PRIu64 ",\n",
                    stats.completion_events ? "true" : "false", stats.completion_wakeups,
                    stats.completion_fallbacks);
            fprintf(file, "  \"video_clock\": \"%s\",\n",
                    options.test_source ? "synthetic"
                    : capture.video_clock == GK_VIDEO_CLOCK_AUTO
                        ? "unselected"
                        : video_clock_names[capture.video_clock]);
            fprintf(file, "  \"video_boundary_omitted\": %" PRIu64 ",\n",
                    stats.video_boundary_omitted);
            fprintf(file, "  \"reused_packets\": %llu,\n",
                    (unsigned long long)stats.reused_packets);
            fprintf(file,
                    "  \"success\": %s, \"synthetic\": %s, \"elapsed_ns\": %" PRId64
                    ",\n  \"offered\": %" PRIu64 ", \"submitted\": %" PRIu64
                    ", \"packets\": %" PRIu64 ", \"skipped\": %" PRIu64
                    ",\n  \"accepted_bytes\": %" PRIu64 ", \"written_bytes\": %" PRIu64
                    ", \"durable_bytes\": %" PRIu64 ", \"peak_output_reserved_bytes\": %" PRIu64
                    ",\n  \"max_rss_kib\": %ld,\n",
                    ret < 0 ? "false" : "true", options.test_source ? "true" : "false", elapsed,
                    capture.offered, stats.submitted, stats.packets, skipped + capture.skipped,
                    stats.sink.accepted, stats.sink.written, stats.sink.durable,
                    stats.sink.peak_pending, usage_data.ru_maxrss);
            fprintf(
                file,
                "  \"replay_saved\": %" PRIu64 ", \"replay_failed\": %" PRIu64
                ", \"replay_peak_live_bytes\": %" PRIu64 ", \"replay_peak_owned_bytes\": %" PRIu64
                ", \"replay_copied_bytes\": %" PRIu64 ", \"replay_saved_bytes\": %" PRIu64 ",\n",
                stats.replay.saved, stats.replay.failed, stats.replay.peak_live_bytes,
                stats.replay.peak_owned_bytes, stats.replay.copied_bytes, stats.replay.saved_bytes);
            fprintf(file,
                    "  \"replay_shared_keyframes\": %" PRIu64
                    ", \"replay_shared_key_bytes\": %" PRIu64 ",\n",
                    stats.replay.shared_keyframes, stats.replay.shared_key_bytes);
            histogram(file, "prepare", &stats.preparation);
            fputs(",\n", file);
            histogram(file, "encode_and_receive", &stats.submission);
            fputs(",\n", file);
            histogram(file, "source_lease", &leases);
            fputs(",\n", file);
            histogram(file, "sink_completion", &stats.sink.completion);
            fputs("\n}\n", file);
            bool write_failed = ferror(file) != 0;
            int close_status = fclose(file);
            if (ret >= 0 && (write_failed || close_status < 0))
                ret = -EIO;
        }
    }
    if (ret < 0) {
        char description[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, description, sizeof(description));
        fprintf(stderr, "gpukey: %s. Any partial output has been preserved.\n", description);
    }
    free(options.audio);
    free(options.tracks);
    return ret < 0 ? 1 : 0;
}
