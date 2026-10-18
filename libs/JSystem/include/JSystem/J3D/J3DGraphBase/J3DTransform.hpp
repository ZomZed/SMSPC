#ifndef J3DTRANSFORM_H
#define J3DTRANSFORM_H

#include <dolphin/mtx.h>

typedef f32 Mtx33[3][3];

struct J3DTextureSRTInfo;

struct J3DTransformInfo {
	/* 0x00 */ Vec mScale;
	/* 0x0C */ S16Vec mRotation;
	/* 0x14 */ Vec mTranslate;

	inline J3DTransformInfo& operator=(const J3DTransformInfo& b)
	{
		mScale.x     = b.mScale.x;
		mScale.y     = b.mScale.y;
		mScale.z     = b.mScale.z;
		mRotation.x  = b.mRotation.x;
		mRotation.y  = b.mRotation.y;
		mRotation.z  = b.mRotation.z;
		mTranslate.x = b.mTranslate.x;
		mTranslate.y = b.mTranslate.y;
		mTranslate.z = b.mTranslate.z;
		return *this;
	}
}; // Size: 0x20

extern J3DTransformInfo const j3dDefaultTransformInfo;
extern Vec const j3dDefaultScale;
extern Mtx const j3dDefaultMtx;
extern f32 PSMulUnit01[];

f32 J3DCalcZValue(MtxPtr m, Vec v);
bool J3DPSCalcInverseTranspose(MtxPtr, ROMtxPtr);
void J3DGetTranslateRotateMtx(J3DTransformInfo const&, Mtx);
void J3DGetTranslateRotateMtx(s16, s16, s16, f32, f32, f32, Mtx);
void J3DGetTextureMtx(const J3DTextureSRTInfo&, Vec, MtxPtr);
void J3DGetTextureMtxOld(const J3DTextureSRTInfo&, Vec, MtxPtr);
void J3DGetTextureMtxMaya(const J3DTextureSRTInfo&, MtxPtr);
void J3DGetTextureMtxMayaOld(const J3DTextureSRTInfo&, MtxPtr);
void J3DScaleNrmMtx33(ROMtxPtr, const Vec&);
void J3DMtxProjConcat(MtxPtr, MtxPtr, MtxPtr);
void J3DPSMtx33Copy(ROMtxPtr src, ROMtxPtr dst);
void J3DPSMtx33CopyFrom34(MtxPtr src, ROMtxPtr dst);
void J3DPSMtxArrayCopy(MtxPtr, MtxPtr, u32);
void J3DMTXConcatArrayIndexedSrc(const float (*)[4], const float (*)[3][4],
                                 const u16*, float (*)[3][4], u32);
void J3DPSMtxArrayConcat(Mtx, Mtx, Mtx, u32);

inline void J3DPSMulMtxVec(register MtxPtr mtx, register Vec* vec,
                           register Vec* dst)
{
#ifdef SMS_PORT // plain C version of the paired-single code below
    f32 vx = vec->x, vy = vec->y, vz = vec->z;
    dst->x = (mtx[0][2] * vz + mtx[0][0] * vx) + (mtx[0][3] * 1.0f + mtx[0][1] * vy);
    dst->y = (mtx[1][2] * vz + mtx[1][0] * vx) + (mtx[1][3] * 1.0f + mtx[1][1] * vy);
    dst->z = (mtx[2][2] * vz + mtx[2][0] * vx) + (mtx[2][3] * 1.0f + mtx[2][1] * vy);
#elif defined(__MWERKS__) // clang-format off
    asm {
        psq_l f0, 0(vec), 0, 0
        psq_l f2, 0(mtx), 0, 0
        psq_l f1, 8(vec), 1, 0
        ps_mul f4, f2, f0
        psq_l f3, 8(mtx), 0, 0
        ps_madd f5, f3, f1, f4
        psq_l f8, 16(mtx), 0, 0
        ps_sum0 f6, f5, f6, f5
        psq_l f9, 24(mtx), 0, 0
        ps_mul f10, f8, f0
        psq_st f6, 0(dst), 1, 0
        ps_madd f11, f9, f1, f10
        psq_l f2, 32(mtx), 0, 0
        ps_sum0 f12, f11, f12, f11
        psq_l f3, 40(mtx), 0, 0
        ps_mul f4, f2, f0
        psq_st f12, 4(dst), 1, 0
        ps_madd f5, f3, f1, f4
        ps_sum0 f6, f5, f6, f5
        psq_st f6, 8(dst), 1, 0
    }
#endif // clang-format on
}

inline void J3DPSMulMtxVec(register MtxPtr mtx, register S16Vec* vec,
                           register S16Vec* dst)
{
#ifdef SMS_PORT // plain C version (GQR7 taken as s16, scale 0)
    f32 vx = (f32)vec->x, vy = (f32)vec->y, vz = (f32)vec->z;
    f32 rx = (mtx[0][2] * vz + mtx[0][0] * vx) + (mtx[0][3] * 1.0f + mtx[0][1] * vy);
    f32 ry = (mtx[1][2] * vz + mtx[1][0] * vx) + (mtx[1][3] * 1.0f + mtx[1][1] * vy);
    f32 rz = (mtx[2][2] * vz + mtx[2][0] * vx) + (mtx[2][3] * 1.0f + mtx[2][1] * vy);
    dst->x = (s16)__sms_f32_to_s16(rx);
    dst->y = (s16)__sms_f32_to_s16(ry);
    dst->z = (s16)__sms_f32_to_s16(rz);
#elif defined(__MWERKS__) // clang-format off
    asm {
        psq_l f0, 0(vec), 0, 7
        psq_l f2, 0(mtx), 0, 0
        psq_l f1, 4(vec), 1, 7
        ps_mul f4, f2, f0
        psq_l f3, 8(mtx), 0, 0
        ps_madd f5, f3, f1, f4
        psq_l f8, 16(mtx), 0, 0
        ps_sum0 f6, f5, f6, f5
        psq_l f9, 24(mtx), 0, 0
        ps_mul f10, f8, f0
        psq_st f6, 0(dst), 1, 7
        ps_madd f11, f9, f1, f10
        psq_l f2, 32(mtx), 0, 0
        ps_sum0 f12, f11, f12, f11
        psq_l f3, 40(mtx), 0, 0
        ps_mul f4, f2, f0
        psq_st f12, 2(dst), 1, 7
        ps_madd f5, f3, f1, f4
        ps_sum0 f6, f5, f6, f5
        psq_st f6, 4(dst), 1, 7
    }
#endif // clang-format on
}

inline void J3DPSMulMtxVec(register ROMtxPtr mtx, register Vec* vec,
                           register Vec* dst)
{
#ifdef SMS_PORT // plain C version of the paired-single code below
    f32 vx = vec->x, vy = vec->y;
    f32 vz = vec->z + PSMulUnit01[0];
    f32 one = 1.0f + PSMulUnit01[1];
    dst->x = (mtx[0][2] * vz + mtx[0][0] * vx) + (1.0f * one + mtx[0][1] * vy);
    dst->y = (mtx[1][2] * vz + mtx[1][0] * vx) + (1.0f * one + mtx[1][1] * vy);
    dst->z = (mtx[2][2] * vz + mtx[2][0] * vx) + (1.0f * one + mtx[2][1] * vy);
#elif defined(__MWERKS__) // clang-format off
    asm {
        addis r6, r0, PSMulUnit01@ha
        psq_l f0, 0(vec), 0, 0
        addi r6, r6, PSMulUnit01@l
        psq_l f2, 0(mtx), 0, 0
        psq_l f13, 0(r6), 0, 0
        psq_l f1, 8(vec), 1, 0
        ps_add f1, f13, f1
        psq_l f3, 8(mtx), 1, 0
        ps_mul f4, f2, f0
        psq_l f8, 12(mtx), 0, 0
        ps_madd f5, f3, f1, f4
        ps_sum0 f6, f5, f6, f5
        psq_l f9, 20(mtx), 1, 0
        ps_mul f10, f8, f0
        psq_st f6, 0(dst), 1, 0
        ps_madd f11, f9, f1, f10
        psq_l f2, 24(mtx), 0, 0
        ps_sum0 f12, f11, f12, f11
        psq_l f3, 32(mtx), 1, 0
        ps_mul f4, f2, f0
        psq_st f12, 4(dst), 1, 0
        ps_madd f5, f3, f1, f4
        ps_sum0 f6, f5, f6, f5
        psq_st f6, 8(dst), 1, 0
    }
#endif // clang-format on
}

inline void J3DPSMulMtxVec(register ROMtxPtr mtx, register S16Vec* vec,
                           register S16Vec* dst)
{
#ifdef SMS_PORT // plain C version (GQR7 taken as s16, scale 0)
    f32 vx = (f32)vec->x, vy = (f32)vec->y;
    f32 vz = (f32)vec->z + PSMulUnit01[0];
    f32 one = 1.0f + PSMulUnit01[1];
    f32 rx = (mtx[0][2] * vz + mtx[0][0] * vx) + (1.0f * one + mtx[0][1] * vy);
    f32 ry = (mtx[1][2] * vz + mtx[1][0] * vx) + (1.0f * one + mtx[1][1] * vy);
    f32 rz = (mtx[2][2] * vz + mtx[2][0] * vx) + (1.0f * one + mtx[2][1] * vy);
    dst->x = (s16)__sms_f32_to_s16(rx);
    dst->y = (s16)__sms_f32_to_s16(ry);
    dst->z = (s16)__sms_f32_to_s16(rz);
#elif defined(__MWERKS__) // clang-format off
    asm {
        addis r6, r0, PSMulUnit01@ha
        psq_l f0, 0(vec), 0, 7
        addi r6, r6, PSMulUnit01@l
        psq_l f2, 0(mtx), 0, 0
        psq_l f13, 0(r6), 0, 0
        psq_l f1, 4(vec), 1, 7
        ps_add f1, f13, f1
        psq_l f3, 8(mtx), 1, 0
        ps_mul f4, f2, f0
        psq_l f8, 12(mtx), 0, 0
        ps_madd f5, f3, f1, f4
        ps_sum0 f6, f5, f6, f5
        psq_l f9, 20(mtx), 1, 0
        ps_mul f10, f8, f0
        psq_st f6, 0(dst), 1, 7
        ps_madd f11, f9, f1, f10
        psq_l f2, 24(mtx), 0, 0
        ps_sum0 f12, f11, f12, f11
        psq_l f3, 32(mtx), 1, 0
        ps_mul f4, f2, f0
        psq_st f12, 2(dst), 1, 7
        ps_madd f5, f3, f1, f4
        ps_sum0 f6, f5, f6, f5
        psq_st f6, 4(dst), 1, 7
    }
#endif // clang-format on
}

#endif
