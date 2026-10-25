/* Actual-source transfer and upload-pool lifecycle oracle. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AV_HAVE_BUFFER_POOL_TRIM 1
#define TracyCZoneN(name, label, active) ((void)0)
#define TracyCZoneEnd(name) ((void)0)
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(2);                                                                               \
        }                                                                                          \
    } while (false)

enum {
    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT = 1,
    VK_PIPELINE_STAGE_2_TRANSFER_BIT = 2,
};
enum gk_backend { GK_VAAPI, GK_NVENC, GK_VULKAN, GK_SOFTWARE };

typedef struct AVBufferPool {
    unsigned live_idle;
    unsigned trims;
} AVBufferPool;

typedef struct AVBufferRef {
    uint8_t *data;
    int refs;
} AVBufferRef;

typedef struct AVFrame {
    int format, width, height;
    AVBufferRef *buf[1];
    bool allocated;
} AVFrame;

typedef struct AVVkFrame {
    unsigned id;
} AVVkFrame;

typedef struct VulkanFramesPriv {
    AVBufferPool *tmp;
} VulkanFramesPriv;

typedef struct VulkanDevicePriv {
    int vkctx;
} VulkanDevicePriv;

typedef struct AVHWDeviceContext {
    VulkanDevicePriv *hwctx;
} AVHWDeviceContext;

typedef struct AVHWFramesContext {
    VulkanFramesPriv *hwctx;
    AVHWDeviceContext *device_ctx;
    AVBufferPool *pool;
} AVHWFramesContext;

typedef struct FFVkExecContext {
    AVFrame *frame_dep;
    AVFrame *sw_dep;
    AVBufferRef *buf_dep;
    unsigned submitted, waited, discarded;
} FFVkExecContext;

typedef int VkBufferImageCopy;

struct gk_gpu {
    enum gk_backend backend;
};

struct gk_media {
    struct gk_gpu *gpu;
    struct {
        int width, height;
    } source;
    struct {
        bool compact_gpu, independent_repeats;
    } options;
    struct {
        int sw_pix_fmt;
        void *priv_data;
    } *video;
    AVBufferRef *frames, *imported_hold;
    AVFrame **pool, *staging, *last, *prepared;
    unsigned pool_count, pool_capacity;
    AVFrame *repeat_key;
    struct {
        uint64_t submitted, packets;
    } stats;
};

static bool fail_frame_alloc, fail_frame_buffer, fail_exec_submit;
static unsigned freed_vk_frames, copied_downloads, trim_opt_value;

static void vulkan_frame_free(AVHWFramesContext *hwfc, AVVkFrame *f)
{
    (void)hwfc;
    CHECK(f && f->id == 77U);
    ++freed_vk_frames;
}

static size_t av_buffer_pool_trim(AVBufferPool *pool, size_t keep)
{
    CHECK(pool && keep == 0);
    size_t released = pool->live_idle;
    pool->live_idle = 0;
    ++pool->trims;
    return released;
}

static void av_buffer_unref(AVBufferRef **ref)
{
    if (!ref || !*ref)
        return;
    CHECK((*ref)->refs > 0);
    if (--(*ref)->refs == 0) {
        free_sized(*ref, sizeof(**ref));
    }
    *ref = nullptr;
}

static AVBufferRef *new_ref(int refs)
{
    AVBufferRef *r = malloc(sizeof(*r));
    CHECK(r);
    r->refs = refs;
    return r;
}

static int av_buffer_get_ref_count(const AVBufferRef *ref)
{
    CHECK(ref);
    return ref->refs;
}

static int av_frame_is_writable(const AVFrame *frame)
{
    CHECK(frame && frame->buf[0]);
    return frame->buf[0]->refs == 1;
}

static AVFrame *av_frame_alloc(void)
{
    if (fail_frame_alloc) {
        fail_frame_alloc = false;
        return nullptr;
    }
    AVFrame *f = calloc(1, sizeof(*f));
    CHECK(f);
    return f;
}

static int av_frame_get_buffer(AVFrame *frame, int align)
{
    CHECK(frame && align == 64);
    if (fail_frame_buffer) {
        fail_frame_buffer = false;
        return -ENOMEM;
    }
    frame->buf[0] = new_ref(1);
    frame->allocated = true;
    return 0;
}

static void av_frame_free(AVFrame **frame)
{
    if (!frame || !*frame)
        return;
    av_buffer_unref(&(*frame)->buf[0]);
    free_sized(*frame, sizeof(**frame));
    *frame = nullptr;
}

static void ff_vk_exec_discard_deps(int *vkctx, FFVkExecContext *exec)
{
    (void)vkctx;
    exec->frame_dep = nullptr;
    if (exec->sw_dep) {
        av_buffer_unref(&exec->sw_dep->buf[0]);
        free_sized(exec->sw_dep, sizeof(*exec->sw_dep));
        exec->sw_dep = nullptr;
    }
    av_buffer_unref(&exec->buf_dep);
    ++exec->discarded;
}

static int ff_vk_exec_add_dep_frame(int *vkctx, FFVkExecContext *exec, AVFrame *f, int src_stage,
                                    int dst_stage)
{
    (void)vkctx;
    CHECK(src_stage == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT &&
          dst_stage == VK_PIPELINE_STAGE_2_TRANSFER_BIT);
    exec->frame_dep = f;
    return 0;
}

static int ff_vk_exec_add_dep_sw_frame(int *vkctx, FFVkExecContext *exec, AVFrame *f)
    __attribute__((unused));
static int ff_vk_exec_add_dep_sw_frame(int *vkctx, FFVkExecContext *exec, AVFrame *f)
{
    (void)vkctx;
    AVFrame *clone = av_frame_alloc();
    CHECK(clone && f && f->buf[0]);
    ++f->buf[0]->refs;
    clone->buf[0] = f->buf[0];
    exec->sw_dep = clone;
    return 0;
}

static int ff_vk_exec_add_dep_buf(int *vkctx, FFVkExecContext *exec, AVBufferRef **deps,
                                  int nb_deps, int ref) __attribute__((unused));
static int ff_vk_exec_add_dep_buf(int *vkctx, FFVkExecContext *exec, AVBufferRef **deps,
                                  int nb_deps, int ref)
{
    (void)vkctx;
    CHECK(nb_deps == 1 && ref == 1 && deps[0]);
    ++deps[0]->refs;
    exec->buf_dep = deps[0];
    return 0;
}

static int ff_vk_exec_submit(int *vkctx, FFVkExecContext *exec)
{
    (void)vkctx;
    if (fail_exec_submit) {
        fail_exec_submit = false;
        return -EIO;
    }
    ++exec->submitted;
    return 0;
}

static void ff_vk_exec_wait(int *vkctx, FFVkExecContext *exec)
{
    ++exec->waited;
    ff_vk_exec_discard_deps(vkctx, exec);
}

static int copy_buffer_data(AVHWFramesContext *hwfc, AVBufferRef *buf, AVFrame *swf,
                            VkBufferImageCopy *region, int planes, int upload)
{
    (void)hwfc;
    (void)buf;
    (void)swf;
    (void)region;
    CHECK(planes == 2 && upload == 0);
    ++copied_downloads;
    return 0;
}

static int av_opt_set_int(void *obj, const char *name, int64_t val, int flags)
{
    (void)obj;
    CHECK(!strcmp(name, "async_trim") && flags == 0);
    trim_opt_value = (unsigned)val;
    return 0;
}

static int receive_video(struct gk_media *m)
{
    (void)m;
    return 0;
}

#include "vulkan-upload-operations.inc"

static AVFrame *make_pool_frame(int refs)
{
    AVFrame *f = av_frame_alloc();
    CHECK(f);
    f->buf[0] = new_ref(refs);
    return f;
}

int main(void)
{
    AVBufferPool tmp_pool = {.live_idle = 2};
    VulkanFramesPriv fp = {.tmp = &tmp_pool};
    VulkanDevicePriv dev = {};
    AVHWDeviceContext dev_ctx = {.hwctx = &dev};
    AVHWFramesContext hwfc = {.hwctx = &fp, .device_ctx = &dev_ctx};

    /* 1. Transfer tail must wait on upload, release staging refs, and leave swf writable. */
    AVFrame hwf = {}, swf = {.buf = {new_ref(1)}};
    AVBufferRef *bufs[1] = {new_ref(1)};
    FFVkExecContext exec = {};
    VkBufferImageCopy region = 0;
    CHECK(run_transfer_tail(&hwfc, &exec, &hwf, &swf, bufs, 1, 1, 1, 2, &region) == 0);
    CHECK(swf.buf[0]->refs == 1 && !bufs[0] && exec.submitted == 1 && exec.waited == 1 &&
          exec.discarded == 1 && !exec.frame_dep && !exec.sw_dep && !exec.buf_dep);

    /* Non-host-mapped upload and download. */
    bufs[0] = new_ref(1);
    CHECK(run_transfer_tail(&hwfc, &exec, &hwf, &swf, bufs, 1, 1, 0, 2, &region) == 0);
    CHECK(swf.buf[0]->refs == 1 && !bufs[0] && exec.waited == 2 && copied_downloads == 0);
    bufs[0] = new_ref(1);
    CHECK(run_transfer_tail(&hwfc, &exec, &hwf, &swf, bufs, 1, 0, 0, 2, &region) == 0);
    CHECK(swf.buf[0]->refs == 1 && !bufs[0] && exec.waited == 3 && copied_downloads == 1);

    /* Failed submit must still discard frame_dep and unref bufs. */
    fail_exec_submit = true;
    bufs[0] = new_ref(1);
    CHECK(run_transfer_tail(&hwfc, &exec, &hwf, &swf, bufs, 1, 1, 1, 2, &region) == -EIO);
    CHECK(swf.buf[0]->refs == 1 && !bufs[0] && !exec.frame_dep && exec.discarded == 4);
    av_buffer_unref(&swf.buf[0]);

    /* 2. Frame free callback must trim the temporary transfer staging pool. */
    AVVkFrame vk_frame = {.id = 77U};
    vulkan_frame_free_cb(&hwfc, (uint8_t *)&vk_frame);
    CHECK(freed_vk_frames == 1 && tmp_pool.live_idle == 0 && tmp_pool.trims == 1);

    /* 3. Media upload-pool and CPU-staging lifecycle on Vulkan and VAAPI. */
    AVBufferPool frames_pool = {.live_idle = 3};
    hwfc.pool = &frames_pool;
    AVBufferRef frames_ref = {.data = (uint8_t *)&hwfc, .refs = 1};
    struct {
        int sw_pix_fmt;
        void *priv_data;
    } video = {.sw_pix_fmt = 23};
    struct gk_gpu gpu = {.backend = GK_VULKAN};
    AVFrame *slots[4] = {
        make_pool_frame(1), /* m->last */
        make_pool_frame(2), /* in-flight encoder input */
        make_pool_frame(1), /* writable spare #1 (retained) */
        make_pool_frame(1), /* writable spare #2 (trimmed) */
    };
    struct gk_media m = {
        .gpu = &gpu,
        .source = {.width = 2560, .height = 2880},
        .options = {.compact_gpu = true, .independent_repeats = true},
        .video = (void *)&video,
        .frames = &frames_ref,
        .pool = slots,
        .pool_count = 4,
        .pool_capacity = 4,
        .last = slots[0],
        .stats = {.submitted = 10, .packets = 10},
    };
    release_idle_inputs(&m);
    CHECK(m.pool_count == 3 && frames_pool.trims == 1 && frames_pool.live_idle == 0);
    /* Releasing the in-flight ref leaves two writable spares; one more is trimmed. */
    slots[1]->buf[0]->refs = 1;
    frames_pool.live_idle = 1;
    release_idle_inputs(&m);
    CHECK(m.pool_count == 2 && frames_pool.trims == 2);

    /* If a prepared frame is already present, no extra spare is needed beyond last+prepared. */
    m.prepared = m.pool[1];
    m.pool[m.pool_count++] = make_pool_frame(1);
    release_idle_inputs(&m);
    CHECK(m.pool_count == 2 && frames_pool.trims == 3);
    m.prepared = nullptr;

    /* Staging allocation and failure handling. */
    fail_frame_alloc = true;
    CHECK(ensure_staging(&m) == -ENOMEM && !m.staging);
    fail_frame_buffer = true;
    CHECK(ensure_staging(&m) == -ENOMEM && !m.staging);
    CHECK(ensure_staging(&m) == 0 && m.staging && m.staging->allocated);
    CHECK(m.staging->width == 2560 && m.staging->height == 2880 && m.staging->format == 23);

    /* Standby releases m->last, all writable upload frames, imported_hold, and m->staging. */
    AVFrame key = {};
    m.repeat_key = &key;
    m.imported_hold = new_ref(1);
    frames_pool.live_idle = 2;
    CHECK(release_static_resources(&m) == 0);
    CHECK(!m.last && !m.imported_hold && m.pool_count == 0 && !m.staging && trim_opt_value == 2 &&
          frames_pool.trims == 4 && frames_pool.live_idle == 0);

    /* Restoring after standby succeeds and is idempotent. */
    CHECK(ensure_staging(&m) == 0 && m.staging);
    AVFrame *existing = m.staging;
    CHECK(ensure_staging(&m) == 0 && m.staging == existing);
    av_frame_free(&m.staging);
    CHECK(frames_ref.refs == 1);
    puts("vulkan upload: synchronous transfer, staging trim, 1-spare active reuse and standby "
         "release pass");
    return 0;
}
