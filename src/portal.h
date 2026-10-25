#ifndef GPUKEY_PORTAL_H
#define GPUKEY_PORTAL_H

#include <stdint.h>

struct gk_portal;
struct gk_portal_source {
    int fd;
    uint32_t node;
    uint64_t serial;
};
/* No restore token is stored. Each invocation obtains portal authority. */
[[nodiscard]] int gk_portal_open(struct gk_portal **out, struct gk_portal_source *source,
                                 uint32_t source_types, bool cursor, int signal_fd);
/* Service revocation on the same context which owns the subscriptions. */
bool gk_portal_closed(struct gk_portal *portal);
/* Integrates capture readiness, cancellation, portal signals and a deadline. */
void gk_portal_wait(struct gk_portal *portal, int capture_fd, int control_fd, int timeout_ms);
/* Completion FDs are borrowed, stable through clear_completion(). Returns 1
 * for completion readiness, 0 for another event, or a negative error. Failed
 * inactive watches also terminate the wait, preventing a poll error spin. */
[[nodiscard]] int gk_portal_wait_completion(struct gk_portal *portal, int capture_fd,
                                            int control_fd, int completion_fd, int timeout_ms);
/* The producer fence FD is borrowed only during this call. */
[[nodiscard]] int gk_portal_wait_gpu(struct gk_portal *portal, int capture_fd, int control_fd,
                                     int completion_fd, int producer_fd, int timeout_ms);
void gk_portal_clear_completion(struct gk_portal *portal);
void gk_portal_close(struct gk_portal **portal);

#endif
