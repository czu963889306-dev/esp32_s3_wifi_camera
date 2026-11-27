#include "rtsp_jpeg.h"
#include <string.h>
bool rtsp_jpeg_parse(const uint8_t *d, size_t len, jpeg_scan_t *o) {
    if (!d || !o || len < 4 || d[0] != 255 || d[1] != 216)
        return false;
    memset(o, 0, sizeof(*o));
    size_t pos = 2;
    unsigned tables = 0;
    bool sof = false;
    while (pos + 4 <= len) {
        if (d[pos++] != 255)
            return false;
        while (pos < len && d[pos] == 255)
            pos++;
        if (pos + 3 > len)
            return false;
        uint8_t marker = d[pos++];
        size_t size = ((size_t)d[pos] << 8) | d[pos + 1];
        if (size < 2 || size > len - pos)
            return false;
        const uint8_t *b = d + pos + 2;
        size_t n = size - 2;
        if (marker == 0xdb) {
            size_t k = 0;
            while (k < n) {
                uint8_t id = b[k++];
                if (id > 1 || n - k < 64)
                    return false;
                memcpy(o->quant + 64 * id, b + k, 64);
                k += 64;
                tables |= 1u << id;
            }
        } else if (marker == 0xc0) {
            if (n != 15 || b[0] != 8 || b[5] != 3 || b[6] != 1 || b[8] != 0 || b[9] != 2 ||
                b[10] != 0x11 || b[11] != 1 || b[12] != 3 || b[13] != 0x11 || b[14] != 1)
                return false;
            o->height = (b[1] << 8) | b[2];
            o->width = (b[3] << 8) | b[4];
            if (b[7] != 0x21 && b[7] != 0x22)
                return false;
            o->type = b[7] == 0x22 ? 1 : 0;
            if (!o->width || !o->height || o->width > 2040 || o->height > 2040 || o->width % 8 ||
                o->height % 8)
                return false;
            sof = true;
        } else if (marker == 0xdd || (marker >= 0xc1 && marker <= 0xcf && marker != 0xc4))
            return false;
        else if (marker == 0xda) {
            if (!sof || tables != 3 || n != 10 || b[0] != 3 || b[1] != 1 || b[2] != 0 ||
                b[3] != 2 || b[4] != 0x11 || b[5] != 3 || b[6] != 0x11 || b[7] != 0 || b[8] != 63 ||
                b[9] != 0)
                return false;
            pos += size;
            if (pos + 2 > len || d[len - 2] != 255 || d[len - 1] != 217)
                return false;
            o->scan = d + pos;
            o->len = len - pos - 2;
            return o->len > 0 && o->len < 0x1000000;
        }
        pos += size;
    }
    return false;
}
