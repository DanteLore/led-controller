// Follows ISO/IEC 18004 for the two versions used. Layout and Reed-Solomon
// steps are after Project Nayuki's QR Code generator (MIT), cut down.

#include "qr.h"
#include <stdint.h>
#include <string.h>

#define FORMAT_LEVEL_M   0   // the format bits for level M are 00
#define MASK             0   // (row + column) even. Any mask decodes; this skips the penalty search
#define EC_MAX          26

// Both versions have one block at level M, so no interleaving
typedef struct { int size, data_codewords, ec_codewords, capacity; } version_t;
static const version_t VERSIONS[] = {
    { 25, 28, 16, 26 },   // version 2
    { 29, 44, 26, 42 },   // version 3
};

static int size;
static bool dark[QR_SIZE_MAX][QR_SIZE_MAX];
static bool reserved[QR_SIZE_MAX][QR_SIZE_MAX];   // finder, timing, alignment and format modules

static void set(int x, int y, bool on)
{
    dark[y][x] = on;
    reserved[y][x] = true;
}

static void finder(int cx, int cy)
{
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || x >= size || y < 0 || y >= size) continue;
            int d = dx * dx > dy * dy ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);   // ring number
            set(x, y, d != 2 && d != 4);
        }
}

static void alignment(int cx, int cy)
{
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            int d = dx * dx > dy * dy ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
            set(cx + dx, cy + dy, d != 1);
        }
}

static void format_bits(void)
{
    int data = FORMAT_LEVEL_M << 3 | MASK;
    int rem = data;
    for (int i = 0; i < 10; i++)
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = (data << 10 | rem) ^ 0x5412;
    #define BIT(i) ((bits >> (i)) & 1)
    // Round the top-left finder
    for (int i = 0; i <= 5; i++) set(8, i, BIT(i));
    set(8, 7, BIT(6));
    set(8, 8, BIT(7));
    set(7, 8, BIT(8));
    for (int i = 9; i < 15; i++) set(14 - i, 8, BIT(i));
    // Split between the other two
    for (int i = 0; i < 8; i++) set(size - 1 - i, 8, BIT(i));
    for (int i = 8; i < 15; i++) set(8, size - 15 + i, BIT(i));
    set(8, size - 8, true);   // always dark
    #undef BIT
}

static uint8_t gf_multiply(uint8_t x, uint8_t y)
{
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return z;
}

static void reed_solomon(const uint8_t *data, int n, uint8_t *ec, int degree)
{
    uint8_t divisor[EC_MAX] = {0};
    divisor[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            divisor[j] = gf_multiply(divisor[j], root);
            if (j + 1 < degree) divisor[j] ^= divisor[j + 1];
        }
        root = gf_multiply(root, 0x02);
    }
    memset(ec, 0, degree);
    for (int i = 0; i < n; i++) {
        uint8_t factor = data[i] ^ ec[0];
        memmove(ec, ec + 1, degree - 1);
        ec[degree - 1] = 0;
        for (int j = 0; j < degree; j++)
            ec[j] ^= gf_multiply(divisor[j], factor);
    }
}

// Mode, length, the bytes, terminator, then the standard padding
static void make_codewords(const char *text, size_t n, uint8_t *out, int count)
{
    memset(out, 0, count);
    int bit = 0;
    #define PUT(value, count) for (int k = (count) - 1; k >= 0; k--, bit++) \
                                  if (((value) >> k) & 1) out[bit >> 3] |= 0x80 >> (bit & 7)
    PUT(0x4, 4);   // byte mode
    PUT(n, 8);
    for (size_t i = 0; i < n; i++) PUT((uint8_t)text[i], 8);
    #undef PUT
    bit += 4;      // terminator: zeros, already there
    int byte = (bit + 7) / 8;
    for (int pad = 0xEC; byte < count; byte++, pad ^= 0xEC ^ 0x11)
        out[byte] = pad;
}

int qr_encode(const char *text, bool modules[QR_SIZE_MAX][QR_SIZE_MAX])
{
    size_t n = strlen(text);
    const version_t *v = &VERSIONS[n <= VERSIONS[0].capacity ? 0 : 1];
    if (n > (size_t)v->capacity) return 0;
    size = v->size;
    memset(dark, 0, sizeof(dark));
    memset(reserved, 0, sizeof(reserved));

    for (int i = 0; i < size; i++) {   // timing lines, then the patterns over them
        set(6, i, i % 2 == 0);
        set(i, 6, i % 2 == 0);
    }
    finder(3, 3);
    finder(size - 4, 3);
    finder(3, size - 4);
    alignment(size - 7, size - 7);
    format_bits();

    uint8_t codewords[44 + EC_MAX];
    int total = v->data_codewords + v->ec_codewords;
    make_codewords(text, n, codewords, v->data_codewords);
    reed_solomon(codewords, v->data_codewords, codewords + v->data_codewords, v->ec_codewords);

    // Zigzag up and down two-column strips from the bottom right, skipping the timing column
    int i = 0;
    for (int right = size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < size; vert++)
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                bool upward = ((right + 1) & 2) == 0;
                int y = upward ? size - 1 - vert : vert;
                if (reserved[y][x]) continue;
                if (i < total * 8) {
                    dark[y][x] = (codewords[i >> 3] >> (7 - (i & 7))) & 1;
                    i++;
                }
                if ((x + y) % 2 == 0) dark[y][x] = !dark[y][x];   // mask 0
            }
    }
    memcpy(modules, dark, sizeof(dark));
    return size;
}
