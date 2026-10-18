/*
 * port_gx.h - GX array / texture helpers for the PC port (SMS_PORT only).
 *
 * Shared game/JSystem code includes this under #ifdef SMS_PORT where the SDK call
 * cannot carry what the PC GX needs:
 *
 *  - Vertex, matrix and light arrays need a byte size (Aurora uploads the array to a
 *    GPU buffer) and their byte order. J3D VTX1 arrays are native order after the
 *    J3D swap pass (port/ENDIAN.md), and arrays the game builds on the CPU are native
 *    too, so PORTGXSetArray/PORTGDSetArray mark them little-endian.
 *    PORTGXSetArrayBE is for the few arrays that are raw GameCube bytes.
 *  - J3DLoadArrayBasePtr writes the CP array-base register directly with a 32-bit
 *    physical address; Aurora ignores that register (64-bit pointers), so the port
 *    sends Aurora's own array-base command instead (PORTGXSetArrayBase).
 *  - J3D material display lists point texture maps at images with BP registers
 *    (physical address >> 5); Aurora only resolves textures through its own
 *    LOAD_TEXOBJ/LOAD_TLUT commands, so the port appends those to the display list
 *    (PORTGDLoadTexImage, PORTGDLoadTlut).
 *
 * Implementations: port/src/port_aurora_sdk.cpp (Aurora build). The plain PC build
 * (build_port.py, no SMS_PORT_AURORA) falls back to the SDK calls.
 */
#ifndef PORT_GX_H
#define PORT_GX_H

#ifdef SMS_PORT

#include <dolphin/gx.h>
#include <dolphin/gd.h>

#ifdef SMS_PORT_AURORA

/* Bytes the display-list buffers of J3D need on top of the GameCube sizes for the
 * Aurora commands above (one LOAD_TEXOBJ + LOAD_TLUT per texture map). */
#define PORT_GD_TEX_EXTRA (8 * 0x50)

/* Native-order array of `size` bytes (also sets the stride, like GXSetArray). */
void PORTGXSetArray(GXAttr attr, const void* data, u32 size, u8 stride);
/* Big-endian (raw GameCube) array. */
void PORTGXSetArrayBE(GXAttr attr, const void* data, u32 size, u8 stride);
/* Size for an array whose size the call site does not know: the readable span from
 * `data`, at most 64 KiB. */
u32 PORTGXFallbackArraySize(const void* data);
/* Array base only (the stride stays as set), like a CP array-base register write. */
void PORTGXSetArrayBase(GXAttr attr, const void* data, u32 size);
/* Display-list versions (into the current GDLObj). */
void PORTGDSetArray(GXAttr attr, void* data, u32 size, u8 stride);
void PORTGDSetArrayNone(GXAttr attr, u8 stride);
void PORTGDLoadTexImage(GXTexMapID id, const void* image, u16 width, u16 height,
                        GXTexFmt format, GXBool mipmap, u32 tlut);
void PORTGDLoadTlut(u32 tlut, const void* data, GXTlutFmt format, u16 entries);

#else /* plain PC build: SDK forms */

#define PORT_GD_TEX_EXTRA 0
static inline void PORTGXSetArray(GXAttr attr, const void* data, u32, u8 stride)
{
	GXSetArray(attr, (void*)data, stride);
}
static inline void PORTGXSetArrayBE(GXAttr attr, const void* data, u32, u8 stride)
{
	GXSetArray(attr, (void*)data, stride);
}
static inline void PORTGDSetArray(GXAttr attr, void* data, u32, u8 stride)
{
	GDSetArray(attr, data, stride);
}
static inline void PORTGDSetArrayNone(GXAttr attr, u8 stride)
{
	GDSetArrayRaw(attr, 0, stride);
}

#endif /* SMS_PORT_AURORA */

#endif /* SMS_PORT */

#endif /* PORT_GX_H */
