#include "discovery.h"

bool receiver_supported(unsigned vendor, unsigned product, unsigned interface) {
    return vendor == 0x046d && (product == 0xc52b || product == 0xc532) && interface == 2;
}

int receiver_choose(const struct receivers *receivers, size_t *index, struct error *err) {
    if (!receivers->count) return fail(err, UC_RECEIVER, 0, 0, "no supported Unifying receiver found");
    if (receivers->count != 1) return fail(err, UC_RECEIVER, 0, 0, "multiple receivers found; select one with --receiver PATH");
    *index = 0;
    return UC_OK;
}

/* Minimal bounds-checked HID item walker. Totals exclude the report ID byte. */
bool receiver_descriptor(const uint8_t *bytes, size_t size) {
    struct globals { uint32_t page, bits, count, id; } g = {0}, stack[16];
    size_t depth = 0;
    uint32_t totals[2][2] = {{0}};
    for (size_t i = 0; i < size;) {
        uint8_t prefix = bytes[i++];
        if (prefix == 0xfe) return false;
        size_t n = prefix & 3u;
        if (n == 3) n = 4;
        if (n > size - i) return false;
        uint32_t value = 0;
        for (size_t j = 0; j < n; j++) value |= (uint32_t)bytes[i + j] << (8 * j);
        i += n;
        unsigned type = (prefix >> 2) & 3u, tag = prefix >> 4;
        if (type == 1) {
            switch (tag) {
            case 0: g.page = value; break;
            case 7: g.bits = value; break;
            case 8:
                if (!value || value > 255) return false;
                g.id = value;
                break;
            case 9: g.count = value; break;
            case 10:
                if (depth == 16) return false;
                stack[depth++] = g;
                break;
            case 11:
                if (!depth) return false;
                g = stack[--depth];
                break;
            default: break;
            }
        } else if (type == 0 && (tag == 8 || tag == 9) && (g.id == 0x10 || g.id == 0x11)) {
            if (g.page != 0xff00 || g.bits > 152 || g.count > 152) return false;
            unsigned report = g.id == 0x10 ? 0u : 1u;
            unsigned direction = tag == 8 ? 0u : 1u;
            totals[report][direction] += g.bits * g.count;
            if (totals[report][direction] > 152) return false;
        }
    }
    return depth == 0 && totals[0][0] == 48 && totals[0][1] == 48 &&
           totals[1][0] == 152 && totals[1][1] == 152;
}
