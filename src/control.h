#ifndef GPUKEY_CONTROL_H
#define GPUKEY_CONTROL_H

#include "media.h"

struct gk_control;
/* input_fd is borrowed; -1 disables Enter-to-save. Session names are local
 * identifiers, and the endpoint is available only to the current user. */
[[nodiscard]] int gk_control_open(struct gk_control **out, const char *session,
                                  const char *directory, bool matroska, int replay_fd,
                                  int input_fd);
int gk_control_fd(const struct gk_control *control);
void gk_control_service(struct gk_control *control, struct gk_media *media, int64_t endpoint_ns);
/* Close after media drain; an accepted export's final result is still reported. */
void gk_control_finish(struct gk_control *control, const struct gk_replay_result *last);
void gk_control_close(struct gk_control **control);
[[nodiscard]] int gk_control_save(const char *session, const char *path);
bool gk_control_valid_session(const char *name);

#endif
