/* Actual-source operations; independent ownership/counting oracle. GPU queue
 * synchronization and decoded pixels are checked separately in native tests. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(2);                                                                               \
        }                                                                                          \
    } while (false)
enum { VA_STATUS_SUCCESS, VA_STATUS_ERROR_INVALID_PARAMETER, VA_STATUS_ERROR_ALLOCATION_FAILED };
struct pipe_video_buffer {
    void (*destroy)(struct pipe_video_buffer *);
};
struct pipe_av1_enc_dpb_entry {
    uint32_t id, order_hint;
    struct pipe_video_buffer *buffer;
};
struct decoder {
    struct pipe_video_buffer *(*create_dpb_buffer)(struct decoder *, void *,
                                                   const struct pipe_video_buffer *);
};
typedef struct context {
    struct {
        unsigned base;
        struct {
            struct pipe_av1_enc_dpb_entry dpb[9];
            uint8_t dpb_size, dpb_curr_pic;
        } av1enc;
    } desc;
    struct decoder *decoder;
} vlVaContext;
typedef struct surface {
    struct pipe_video_buffer templat, *buffer;
    vlVaContext *ctx;
    bool is_dpb;
} vlVaSurface;
struct handle {
    uint32_t id;
    vlVaSurface *surface;
};
typedef struct {
    struct handle *htab;
} vlVaDriver;
struct picture {
    uint32_t reconstructed_frame, reference_frames[8];
    uint8_t order_hint;
};
static struct handle handles[16];
static vlVaSurface surfaces[16];
static struct pipe_video_buffer *allocated[64];
static unsigned live, creations, destructions;
static bool fail_allocation;

static void destroy_buffer(struct pipe_video_buffer *buffer)
{
    unsigned at;
    for (at = 0; at < ARRAY_SIZE(allocated) && allocated[at] != buffer; ++at) {
    }
    CHECK(at < ARRAY_SIZE(allocated));
    allocated[at] = nullptr;
    CHECK(live);
    --live;
    ++destructions;
    free_sized(buffer, sizeof(*buffer));
}

static struct pipe_video_buffer *create_buffer(struct decoder *decoder, void *picture,
                                               const struct pipe_video_buffer *model)
{
    (void)decoder;
    (void)picture;
    (void)model;
    if (fail_allocation) {
        fail_allocation = false;
        return nullptr;
    }
    unsigned at;
    for (at = 0; at < ARRAY_SIZE(allocated) && allocated[at]; ++at) {
    }
    CHECK(at < ARRAY_SIZE(allocated));
    struct pipe_video_buffer *buffer = malloc(sizeof(*buffer));
    CHECK(buffer);
    buffer->destroy = destroy_buffer;
    allocated[at] = buffer;
    ++live;
    ++creations;
    return buffer;
}

static vlVaSurface *handle_table_get(struct handle *table, uint32_t id)
{
    for (unsigned i = 0; i < ARRAY_SIZE(handles); ++i)
        if (table[i].id == id)
            return table[i].surface;
    return nullptr;
}

static void vlVaSetSurfaceContext(vlVaDriver *drv, vlVaSurface *surface, vlVaContext *context)
{
    (void)drv;
    surface->ctx = context;
}

#include "vaapi-dpb-operations.inc"

static void audit(const vlVaContext *context)
{
    unsigned owners[64] = {};
    for (unsigned i = 0; i < ARRAY_SIZE(surfaces); ++i)
        if (surfaces[i].buffer) {
            unsigned at;
            for (at = 0; at < ARRAY_SIZE(allocated) && allocated[at] != surfaces[i].buffer; ++at) {
            }
            CHECK(at < ARRAY_SIZE(allocated));
            ++owners[at];
        }
    for (unsigned i = 0; i < ARRAY_SIZE(allocated); ++i)
        CHECK(owners[i] == (allocated[i] ? 1U : 0U));
    for (unsigned i = 0; i < context->desc.av1enc.dpb_size; ++i) {
        const struct pipe_av1_enc_dpb_entry *entry = &context->desc.av1enc.dpb[i];
        if (!entry->buffer) {
            CHECK(!entry->id);
            continue;
        }
        uint32_t owner = entry->id ? entry->id : entry->order_hint;
        vlVaSurface *surface = handle_table_get(handles, owner);
        CHECK(surface && surface->is_dpb && surface->ctx == context &&
              surface->buffer == entry->buffer);
        CHECK(!entry->id || entry->order_hint <= 255);
    }
}

static void destroy_surface(unsigned index)
{
    vlVaSurface *surface = &surfaces[index];
    CHECK(handles[index].surface == surface);
    if (surface->ctx && surface->is_dpb)
        remove_surface(surface, handles[index].id);
    if (surface->buffer)
        surface->buffer->destroy(surface->buffer);
    *surface = (vlVaSurface){};
    handles[index].surface = nullptr;
}

static void close_context(vlVaContext *context)
{
    for (unsigned i = 0; i < ARRAY_SIZE(surfaces); ++i)
        if (surfaces[i].ctx == context)
            surfaces[i].ctx = nullptr;
    release_cached(context);
}

static void issue(vlVaDriver *driver, vlVaContext *context, unsigned target,
                  const unsigned *references, unsigned count)
{
    CHECK(count <= 8);
    struct picture picture = {.reconstructed_frame = handles[target].id, .order_hint = 255};
    for (unsigned i = 0; i < ARRAY_SIZE(picture.reference_frames); ++i)
        picture.reference_frames[i] = i < count ? handles[references[i]].id : UINT32_MAX;
    CHECK(apply(driver, context, &picture) == VA_STATUS_SUCCESS);
    audit(context);
}

int main(void)
{
    for (unsigned i = 0; i < ARRAY_SIZE(handles); ++i)
        handles[i] = (struct handle){i == 0 ? UINT32_MAX - 1 : i + 1, &surfaces[i]};
    struct decoder decoder = {.create_dpb_buffer = create_buffer};
    vlVaContext context = {.decoder = &decoder};
    vlVaDriver driver = {.htab = handles};
    /* Populate the complete AV1 reference domain, then reuse a retired buffer
     * for a new surface handle without increasing physical allocation count. */
    unsigned references[8] = {};
    for (unsigned i = 0; i < 9; ++i) {
        for (unsigned j = 0; j < i; ++j)
            references[j] = j;
        issue(&driver, &context, i, references, i);
    }
    CHECK(live == 9 && creations == 9);
    for (unsigned i = 0; i < 8; ++i)
        references[i] = i + 1;
    issue(&driver, &context, 9, references, 8);
    CHECK(live == 9 && creations == 9 && !surfaces[0].buffer);
    destroy_surface(0);
    audit(&context);
    /* A key retires all earlier references. Returning unused surfaces must
     * release their storage while the current key remains live. */
    issue(&driver, &context, 9, nullptr, 0);
    references[0] = 9;
    issue(&driver, &context, 8, references, 1);
    CHECK(creations == 9); /* Reactivate the cached surface's own buffer. */
    issue(&driver, &context, 9, nullptr, 0);
    destroy_surface(1);
    issue(&driver, &context, 11, references, 1);
    CHECK(live == 8 && creations == 9); /* Fill a hole from another idle slot. */
    issue(&driver, &context, 9, nullptr, 0);
    for (unsigned i = 2; i < 9; ++i)
        destroy_surface(i);
    destroy_surface(11);
    CHECK(live == 1);
    audit(&context);
    references[0] = 9;
    fail_allocation = true;
    struct picture failure = {.reconstructed_frame = handles[10].id, .order_hint = 1};
    for (unsigned i = 0; i < 8; ++i)
        failure.reference_frames[i] = i ? UINT32_MAX : handles[9].id;
    CHECK(apply(&driver, &context, &failure) == VA_STATUS_ERROR_ALLOCATION_FAILED);
    CHECK(!fail_allocation && live == 1 && !surfaces[10].buffer && !surfaces[10].is_dpb);
    audit(&context);
    issue(&driver, &context, 10, references, 1);
    CHECK(live == 2);
    issue(&driver, &context, 10, nullptr, 0);
    vlVaContext other = {.decoder = &decoder};
    struct picture invalid = {.reconstructed_frame = handles[9].id};
    CHECK(apply(&driver, &other, &invalid) == VA_STATUS_ERROR_INVALID_PARAMETER);
    invalid.reconstructed_frame = UINT32_MAX;
    CHECK(apply(&driver, &context, &invalid) == VA_STATUS_ERROR_INVALID_PARAMETER);
    audit(&context);
    /* Close before surface destruction: cached storage is still surface-owned. */
    close_context(&context);
    CHECK(live == 2);
    vlVaContext next = {.decoder = &decoder};
    unsigned before = creations;
    issue(&driver, &next, 9, nullptr, 0);
    CHECK(creations == before + 1 && live == 2);
    close_context(&next);
    destroy_surface(9);
    destroy_surface(10);
    CHECK(live == 0 && creations == destructions);
    puts("Actual Mesa AV1 ownership: full reference set, cross-surface reuse, idle reclamation, "
         "allocation failure and late destruction pass");
}
