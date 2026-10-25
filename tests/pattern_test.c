#include "pattern.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

int main(void)
{
    /* Independent scalar formula from the original source, including unsigned
     * wraparound near UINT64_MAX where a naive period-19 tile is incorrect. */
    static constexpr uint8_t colors[8][3] = {{255, 255, 255}, {0, 255, 255}, {255, 255, 0},
                                             {0, 255, 0},     {255, 0, 255}, {0, 0, 255},
                                             {255, 0, 0},     {0, 0, 0}};
    static constexpr int widths[] = {1, 2, 3, 7, 8, 19, 31, 64, 97};
    static constexpr int heights[] = {1, 2, 3, 4, 7, 19, 20, 25, 76, 81, 97, 257};
    static constexpr uint64_t indices[] = {
        0, 1, 18, 19, 255, 256, UINT64_MAX - 100, UINT64_MAX - 1, UINT64_MAX};
    for (size_t w = 0; w < sizeof(widths) / sizeof(*widths); ++w)
        for (size_t h = 0; h < sizeof(heights) / sizeof(*heights); ++h)
            for (size_t n = 0; n < sizeof(indices) / sizeof(*indices); ++n) {
                int width = widths[w], height = heights[h];
                uint64_t index = indices[n];
                uint8_t *pixels = malloc((size_t)width * (size_t)height * 4);
                CHECK(pixels);
                gk_test_pattern(pixels, width, height, index);
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x) {
                        size_t at = ((size_t)y * (size_t)width + (size_t)x) * 4;
                        unsigned bar = (unsigned)((uint64_t)(unsigned)x * 8 / (unsigned)width);
                        for (unsigned c = 0; c < 3; ++c) {
                            uint8_t expected =
                                y < height / 2 ? colors[bar][c] : (uint8_t)((unsigned)x + index);
                            if (y >= height * 3 / 4 && ((unsigned)x + (unsigned)y + index) % 19 < 2)
                                expected = 0;
                            CHECK(pixels[at + c] == expected);
                        }
                        CHECK(pixels[at + 3] == 255);
                    }
                free(pixels);
            }
    puts("pattern: scalar equivalence, odd/small dimensions, row periods and uint64 wrap pass");
    return 0;
}
