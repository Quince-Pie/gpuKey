/* Run the independent media/decode suite with the optional row-cache allocation
 * denied. --wrap affects application references, not allocations within FFmpeg. */
#include "convert.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

void *__real_av_malloc(size_t size);
void *__wrap_av_malloc(size_t size);
int gk_media_suite_main(void);

static unsigned failures;

void *__wrap_av_malloc(size_t size)
{
    if (size == gk_rgb_workspace_size(64)) {
        ++failures;
        return nullptr;
    }
    return __real_av_malloc(size);
}

#define main gk_media_suite_main
#include "media_test.c"
#undef main

int main(void)
{
    int ret = gk_media_suite_main();
    if (ret || !failures)
        return 1;
    printf("conversion fallback: %u denied row caches; media pixels, timing, audio and teardown "
           "pass\n",
           failures);
    return 0;
}
