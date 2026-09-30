#include <stdint.h>
#include "types.h"

int cmd_copy(uint8_t *dst, uint32_t dst_id, uint32_t src_id, uint32_t clip_id,
             int dst_x1, int dst_y1, int dst_x2, int dst_y2,
             int src_x1, int src_y1) {
    (void)clip_id;
    if (!dst) return 36;
    PUT32(dst + 0, dst_id);
    PUT32(dst + 4, src_id);
    PUT32(dst + 8, clip_id);
    PUT16(dst + 12, (uint16_t)dst_x1);
    PUT16(dst + 14, (uint16_t)dst_y1);
    PUT16(dst + 16, (uint16_t)(dst_x2 - dst_x1));
    PUT16(dst + 18, (uint16_t)(dst_y2 - dst_y1));
    PUT16(dst + 20, (uint16_t)src_x1);
    PUT16(dst + 22, (uint16_t)src_y1);
    return 24;
}