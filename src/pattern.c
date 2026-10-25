#include "pattern.h"

#include <stddef.h>
#include <string.h>

void gk_test_pattern(uint8_t *pixels, int width, int height, uint64_t index)
{
    static constexpr uint8_t bars[8][3] = {
        {255, 255, 255}, {0, 255, 255}, {255, 255, 0}, {0, 255, 0},
        {255, 0, 255},   {0, 0, 255},   {255, 0, 0},   {0, 0, 0},
    };
    size_t stride = (size_t)width * 4;
    int middle = height / 2, bottom = (int)((int64_t)height * 3 / 4);
    bool periodic = index <= UINT64_MAX - (unsigned)width - (unsigned)height;
    for (int y = 0; y < height; ++y) {
        int from = y > 0 && y < middle            ? 0
                   : y > middle && y < bottom     ? middle
                   : periodic && y >= bottom + 19 ? y - 19
                                                  : -1;
        uint8_t *row = pixels + (size_t)y * stride;
        if (from >= 0) {
            memcpy(row, pixels + (size_t)from * stride, stride);
            continue;
        }
        for (int x = 0; x < width; ++x) {
            uint8_t *p = row + (size_t)x * 4;
            unsigned bar = (unsigned)((uint64_t)(unsigned)x * 8 / (unsigned)width);
            for (unsigned k = 0; k < 3; ++k)
                p[k] = y < middle ? bars[bar][k] : (uint8_t)((unsigned)x + index);
            if (y >= bottom && ((unsigned)x + (unsigned)y + index) % 19 < 2)
                p[0] = p[1] = p[2] = 0;
            p[3] = 255;
        }
    }
}
