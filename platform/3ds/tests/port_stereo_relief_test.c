#include "port_stereo_relief.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr)                                                        \
    do {                                                                   \
        if (!(expr)) {                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);         \
            return 1;                                                      \
        }                                                                  \
    } while (0)

/* A picture of a room: '#' solid, '.' open, 'x' past the room's edge. The
 * expected heights are a picture too, one digit per cell. */
static int relief_sink(const char* const* picture, const char* const* expected, int rows, int cap, int wantSink) {
    const int cols = (int)strlen(picture[0]);
    uint8_t kind[32 * 32], out[32 * 32];
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            const char c = picture[row][col];
            kind[row * cols + col] = c == '#' ? PORT_STEREO_CELL_SOLID
                                     : c == 'x' ? PORT_STEREO_CELL_OUTSIDE
                                     : c == '~' ? PORT_STEREO_CELL_SUNKEN
                                                : PORT_STEREO_CELL_OPEN;
        }
    }
    if (PortStereo_ReliefHeights(kind, cols, rows, cap, out) != wantSink) {
        printf("sink: want %d\n", wantSink);
        return 1;
    }
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            if (out[row * cols + col] != (uint8_t)(expected[row][col] - '0')) {
                printf("row %d col %d: got %d, want %c\n", row, col, out[row * cols + col], expected[row][col]);
                for (int r = 0; r < rows; ++r) {
                    for (int c = 0; c < cols; ++c) putchar('0' + out[r * cols + c]);
                    putchar('\n');
                }
                return 1;
            }
        }
    }
    return 0;
}

static int relief(const char* const* picture, const char* const* expected, int rows, int cap) {
    return relief_sink(picture, expected, rows, cap, 0);
}

int main(void) {
    /* A thing climbs from its foot: one unit per two cells, up to the cap. A
     * one-cell fence is the lowest step. Open ground is flat. */
    {
        static const char* const picture[] = {
            "..#.....", "..#.....", "..#.....", "..#.....", "..#.....",
            "..#.....", "..#..#..", "..#..#..", "..#..#.#", "........",
        };
        static const char* const expected[] = {
            "00400000", "00400000", "00400000", "00300000", "00300000",
            "00200000", "00200200", "00100100", "00100101", "00000000",
        };
        CHECK(relief(picture, expected, 10, 4) == 0);
    }
    /* A wall with no ground below it -- it runs off the bottom of the grid,
     * or stands on the room's edge -- has no foot: it is all top. */
    {
        static const char* const picture[] = { "#..#", "#..#", "#..x" };
        static const char* const expected[] = { "4004", "4004", "4000" };
        CHECK(relief(picture, expected, 3, 4) == 0);
    }
    /* The column over a doorway answers to the front it is cut into: it
     * continues the walls on either side instead of starting again at one. */
    {
        static const char* const picture[] = {
            "########", "########", "########", "###..###", "###..###", "........",
        };
        static const char* const expected[] = {
            "33333333", "22222222", "22222222", "11100111", "11100111", "00000000",
        };
        CHECK(relief(picture, expected, 6, 4) == 0);
    }
    /* A wide gap is the space between two things, not an opening in one. */
    {
        static const char* const picture[] = {
            "###########", "###.....###", "###.....###", "...........",
        };
        static const char* const expected[] = {
            "22211111222", "11100000111", "11100000111", "00000000000",
        };
        CHECK(relief(picture, expected, 4, 4) == 0);
    }
    /* Water lies below the ground. The layer is sunk a unit and everything
     * but the water stands a unit up from it, so the ground reads 1 and a
     * thing its own height plus one; a foot stands on a shore as on ground. */
    {
        static const char* const picture[] = { "..#.~~", "..#.~~", "....~~", "x...~~" };
        static const char* const expected[] = { "112100", "112100", "111100", "011100" };
        CHECK(relief_sink(picture, expected, 4, 4, 1) == 0);
    }
    puts("port_stereo_relief_test: PASS");
    return 0;
}
