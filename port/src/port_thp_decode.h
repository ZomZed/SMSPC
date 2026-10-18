/*
 * port_thp_decode.h - host decoder for THP video frames (port_thp_decode.cpp).
 *
 * Plain C++ with no SDK dependency, so the standalone test tool
 * (port/tools/test_thp.py) builds it as it is.
 */
#ifndef PORT_THP_DECODE_H
#define PORT_THP_DECODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decodes one THP video component (a baseline JPEG, YUV 4:2:0, with the THP
 * conventions: no 0xFF00 byte stuffing in the scan, restart intervals only
 * byte-align) into three GX_TF_I8 textures in the layout THPVideoDecode
 * writes: Y is width x height, U and V are width/2 x height/2, each stored as
 * 8x4-texel tiles of 32 bytes, tiles in row order.
 *
 * `size` bounds the input (reads past it see zero bytes).
 * Returns 0 on success, otherwise a non-zero error code like THPVideoDecode
 * (3 bad syntax, 10 bad precision, 11 unsupported marker, 12 bad component
 * count, 15 missing Huffman table, 19 bad sampling, 20 corrupt scan).
 * If width/height are non-zero they must match the frame header (error 21).
 */
int port_thp_video_decode(const void* data, uint32_t size, uint8_t* tileY, uint8_t* tileU,
                          uint8_t* tileV, uint32_t width, uint32_t height);

#ifdef __cplusplus
}
#endif

#endif
