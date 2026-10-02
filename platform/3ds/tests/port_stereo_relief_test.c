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

enum { MAX_CELLS = 48 * 48 };

/* A picture of a room, one character per cell:
 *   '.' open ground     '#' solid      'x' past the room's edge
 *   '~' water           '/' a ramp     'n' 'e' 's' 'w' a ledge, named by the
 *                                      side Link lands on (the lower one)
 * The expected heights are a picture too: a digit per cell, or 'a' 'b' 'c' for
 * -1 -2 -3. */
static uint8_t kind_of(char c) {
    switch (c) {
        case '#': return PORT_STEREO_CELL_SOLID;
        case 'x': return PORT_STEREO_CELL_OUTSIDE;
        case '~': return PORT_STEREO_CELL_SUNKEN;
        case '/': return PORT_STEREO_CELL_RAMP;
        case 'n': return PORT_STEREO_CELL_LEDGE_N;
        case 'e': return PORT_STEREO_CELL_LEDGE_E;
        case 's': return PORT_STEREO_CELL_LEDGE_S;
        case 'w': return PORT_STEREO_CELL_LEDGE_W;
        default: return PORT_STEREO_CELL_OPEN;
    }
}

static char digit_of(int height) {
    return height >= 0 ? (char)('0' + height) : (char)('a' - 1 - height);
}

static int relief(const char* const* picture, const char* const* expected, int rows) {
    static uint8_t kind[MAX_CELLS];
    static uint16_t region[MAX_CELLS], queue[MAX_CELLS];
    static int8_t ground[MAX_CELLS], height[MAX_CELLS];
    const int cols = (int)strlen(picture[0]);
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) kind[row * cols + col] = kind_of(picture[row][col]);
    }
    const PortStereoRoom room = { cols, rows, kind, region, queue };
    PortStereo_RoomHeights(&room, NULL, ground, height, NULL);
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
            if (digit_of(height[row * cols + col]) == expected[row][col]) continue;
            printf("row %d col %d: got %c, want %c\n", row, col, digit_of(height[row * cols + col]),
                   expected[row][col]);
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) putchar(digit_of(height[r * cols + c]));
                putchar('\n');
            }
            return 1;
        }
    }
    return 0;
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
        CHECK(relief(picture, expected, 10) == 0);
    }
    /* A wall with no ground below it -- it runs off the bottom of the room,
     * or stands on the room's edge -- has no foot: it is all top. */
    {
        static const char* const picture[] = { "#..#", "#..#", "#..x" };
        static const char* const expected[] = { "4004", "4004", "4000" };
        CHECK(relief(picture, expected, 3) == 0);
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
        CHECK(relief(picture, expected, 6) == 0);
    }
    /* A wide gap is the space between two things, not an opening in one. */
    {
        static const char* const picture[] = {
            "###########", "###.....###", "###.....###", "...........",
        };
        static const char* const expected[] = {
            "22211111222", "11100000111", "11100000111", "00000000000",
        };
        CHECK(relief(picture, expected, 4) == 0);
    }
    /* Water lies one unit under the ground it borders, and a thing standing
     * in it is measured from the water. */
    {
        static const char* const picture[] = { "..#.~~", "..#.~#", "....~~", "x...~~" };
        static const char* const expected[] = { "0010aa", "0010a0", "0000aa", "0000aa" };
        CHECK(relief(picture, expected, 4) == 0);
    }
    /* A plateau: ground ringed by ledges is as high as the cliff that faces
     * the camera is drawn tall -- here two cells, one unit -- and the rim on
     * every side, corners included, is level with it. The field around it
     * stays at 0. This is the yard that used to read as a pit in a rampart. */
    {
        static const char* const picture[] = {
            "..........", "..nnnnnn..", "..w....e..", "..w....e..", "..ssssss..", "..ssssss..", "..........",
        };
        static const char* const expected[] = {
            "0000000000", "0011111100", "0011111100", "0011111100", "0011111100", "0011111100", "0000000000",
        };
        CHECK(relief(picture, expected, 7) == 0);
    }
    /* A taller cliff makes a higher plateau, its face climbs to it, and a
     * thing on the plateau stands on the plateau. */
    {
        static const char* const picture[] = {
            "............", "..nnnnnnnn..", "..w.#....e..", "..w.#....e..", "..w......e..",
            "..ssssssss..", "..ssssssss..", "..ssssssss..", "..ssssssss..",
            "............", "............",
        };
        static const char* const expected[] = {
            "000000000000", "002222222200", "002232222200", "002232222200", "002222222200",
            "002222222200", "002222222200", "001111111100", "001111111100",
            "000000000000", "000000000000",
        };
        CHECK(relief(picture, expected, 11) == 0);
    }
    /* A ramp goes from the level at its top to the level at its bottom. */
    {
        static const char* const picture[] = { "...", "s/s", "s/s", "...", "..." };
        static const char* const expected[] = { "111", "111", "101", "000", "000" };
        CHECK(relief(picture, expected, 5) == 0);
    }
    /* A lake behind ledges that drop into it: the water is one unit down,
     * whichever way the banks face. */
    {
        static const char* const picture[] = { "......", ".e~~w.", ".e~~w.", "......" };
        static const char* const expected[] = { "000000", "00aa00", "00aa00", "000000" };
        CHECK(relief(picture, expected, 4) == 0);
    }
    /* Two banks at different levels share one lake: the water lies under the
     * lower bank, and a far bank no ledge ties to the rest is level ground. */
    {
        static const char* const picture[] = {
            "....~~....", "....~~....", "ssss~~....", "ssss~~....", "....~~....", "....~~....", "....~~....",
            "....~~....", "....~~....", "....~~....", "....~~....", "....~~....", "....~~....",
        };
        static const char* const expected[] = {
            "1111aa0000", "1111aa0000", "1111aa0000", "1111aa0000", "0000aa0000", "0000aa0000", "0000aa0000",
            "0000aa0000", "0000aa0000", "0000aa0000", "0000aa0000", "0000aa0000", "0000aa0000",
        };
        CHECK(relief(picture, expected, 13) == 0);
    }
    puts("port_stereo_relief_test: PASS");
    return 0;
}
