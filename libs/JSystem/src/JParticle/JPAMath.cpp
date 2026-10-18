#include <JSystem/JParticle/JPAMath.hpp>
#include <JSystem/JMath.hpp>
#include <math.h>

f32 JPASqrtf(f32 x)
{
	if (x > 0.0f) {
		return x * (f32)__frsqrte(x);
	}
	return 0.0f;
}

// TODO: all these vec math funcs are equivalent (I think), but matching them
// will be quite the ordeal.

void JPAGetXYZRotateMtx(s16 x, s16 y, s16 z, MtxPtr dst)
{
	f32* ptr = &dst[0][0];

	*ptr++ = JMASCos(z) * JMASCos(y);
	*ptr++ = -JMASSin(z) * JMASCos(x) + JMASCos(z) * JMASSin(y) * JMASSin(x);
	*ptr++ = JMASSin(z) * JMASSin(x) + JMASCos(z) * JMASSin(y) * JMASCos(x);
	*ptr++ = 0.0f;

	*ptr++ = JMASSin(z) * JMASCos(y);
	*ptr++ = JMASCos(z) * JMASCos(x) + JMASSin(z) * JMASSin(y) * JMASSin(x);
	*ptr++ = -JMASCos(z) * JMASSin(x) + JMASSin(z) * JMASSin(y) * JMASCos(x);
	*ptr++ = 0.0f;

	*ptr++ = -JMASSin(y);
	*ptr++ = JMASCos(y) * JMASSin(x);
	*ptr++ = JMASCos(y) * JMASCos(x);
	*ptr++ = 0.0f;
}

void JPAGetXYRotateMtx(s16 x, s16 y, MtxPtr dst)
{
	f32* ptr = &dst[0][0];

	*ptr++ = JMASCos(y);
	*ptr++ = JMASSin(y) * JMASSin(x);
	*ptr++ = JMASSin(y) * JMASCos(x);
	*ptr++ = 0.0f;

	*ptr++ = 0.0f;
	*ptr++ = JMASCos(x);
	*ptr++ = -JMASSin(x);
	*ptr++ = 0.0f;

	*ptr++ = -JMASSin(y);
	*ptr++ = JMASCos(y) * JMASSin(x);
	*ptr++ = JMASCos(y) * JMASCos(x);
	*ptr++ = 0.0f;
}

void JPAGetScaleXYRotateMtx(JGeometry::TVec3<f32>&, s16, s16, MtxPtr) { }

void JPAGetYZRotateMtx(s16 y, s16 z, MtxPtr dst)
{
	f32* ptr = &dst[0][0];

	*ptr++ = JMASCos(z) * JMASCos(y);
	*ptr++ = -JMASSin(z);
	*ptr++ = JMASCos(z) * JMASSin(y);
	*ptr++ = 0.0f;

	*ptr++ = JMASSin(z) * JMASCos(y);
	*ptr++ = JMASCos(z);
	*ptr++ = JMASSin(z) * JMASSin(y);
	*ptr++ = 0.0f;

	*ptr++ = -JMASSin(y);
	*ptr++ = 0.0f;
	*ptr++ = JMASCos(y);
	*ptr++ = 0.0f;
}

void JPAGetScaleYZRotateMtx(JGeometry::TVec3<f32>&, s16, s16, MtxPtr) { }

void JPAGetYRotateMtx(s16 y, MtxPtr dst)
{
	f32* ptr = &dst[0][0];

	*ptr++ = JMASCos(y);
	*ptr++ = 0.0f;
	*ptr++ = JMASSin(y);
	*ptr++ = 0.0f;

	*ptr++ = 0.0f;
	*ptr++ = 1.0f;
	*ptr++ = 0.0f;
	*ptr++ = 0.0f;

	*ptr++ = -JMASSin(y);
	*ptr++ = 0.0f;
	*ptr++ = JMASCos(y);
	*ptr++ = 0.0f;
}

void JPAGetZRotateMtx(s16 z, MtxPtr dst)
{
	f32* ptr = &dst[0][0];

	*ptr++ = JMASCos(z);
	*ptr++ = JMASSin(z);
	*ptr++ = 0.0f;
	*ptr++ = 0.0f;

	*ptr++ = -JMASSin(z);
	*ptr++ = JMASCos(z);
	*ptr++ = 0.0f;
	*ptr++ = 0.0f;

	*ptr++ = 0.0f;
	*ptr++ = 0.0f;
	*ptr++ = 1.0f;
	*ptr++ = 0.0f;
}

void JPAVecToRotaMtx(MtxPtr dst, JGeometry::TVec3<f32> a,
                     JGeometry::TVec3<f32> b)
{
	JGeometry::TVec3<f32> axis;
	(void)&axis; // keeps axis in memory: the target stores it and reloads it
	axis.cross(a, b);

	f32 ax  = axis.x;
	f32 ay  = axis.y;
	f32 az  = axis.z;
	f32 sq  = ax * ax + ay * ay + az * az;
	f32 cos = a.dot(b);
	f32 sin = JGeometry::TUtil<f32>::sqrt(sq);

	if (sin <= JGeometry::TUtil<f32>::epsilon()) {
		axis.zero();
	} else {
		axis.scale(__fres(sin));
	}

	f32 x = axis.x;
	f32 y = axis.y;
	f32 z = axis.z;
	f32 oneMinusCos = 1.0f - cos;

	dst[0][0] = cos * (1.0f - x * x) + x * x;
	dst[0][1] = x * y * oneMinusCos + z * sin;
	dst[0][2] = x * z * oneMinusCos - y * sin;
	dst[0][3] = 0.0f;

	dst[1][0] = y * x * oneMinusCos - z * sin;
	dst[1][1] = cos * (1.0f - y * y) + y * y;
	dst[1][2] = y * z * oneMinusCos + x * sin;
	dst[1][3] = 0.0f;

	dst[2][0] = z * x * oneMinusCos + y * sin;
	dst[2][1] = z * y * oneMinusCos - x * sin;
	dst[2][2] = cos * (1.0f - z * z) + z * z;
	dst[2][3] = 0.0f;
}

f32 JPAConvertFixToFloat(s16 param_1)
{
	if (param_1 == 0x7fff)
		return 1.0f;

	f32 f0 = (float)param_1 * (1.0f / 32768.0f) * 100000.0f;
	int r5 = (int)f0;
	int r0 = (int)f0 % 10;
	if (r0 >= 5) {
		r5 += (10 - r0);
	} else {
		r5 -= r0;
	}
	return (float)r5 * 1e-05f;
}

s16 JPAConvertFloatToFix(f32)
{
#ifdef SMS_PORT
	return 0; // UNUSED stub, not in the original binary
#endif
}

void JPAConvertFixVecToFloatVec(JGeometry::TVec3<f32>& param_1,
                                const JGeometry::TVec3<s16>& param_2)
{
	param_1.x = JPAConvertFixToFloat(param_2.x);
	param_1.y = JPAConvertFixToFloat(param_2.y);
	param_1.z = JPAConvertFixToFloat(param_2.z);
}

void JPAConvertFloatVecToFixVec(JGeometry::TVec3<s16>&,
                                const JGeometry::TVec3<f32>&)
{
}

void JPABound(JGeometry::TVec3<f32>&, const JGeometry::TVec3<f32>&,
              const JGeometry::TVec3<f32>&, f32, f32)
{
}

void JPAGetSVecElement(MtxPtr, JGeometry::TVec3<f32>&) { }

void JPAGetRMtxSTVecElement(MtxPtr param_1, MtxPtr param_2,
                            JGeometry::TVec3<f32>& param_3,
                            JGeometry::TVec3<f32>& param_4)
{
	param_3.x = std::sqrtf(param_1[0][0] * param_1[0][0]
	                       + param_1[1][0] * param_1[1][0]
	                       + param_1[2][0] * param_1[2][0]);
	param_3.y = std::sqrtf(param_1[0][1] * param_1[0][1]
	                       + param_1[1][1] * param_1[1][1]
	                       + param_1[2][1] * param_1[2][1]);
	param_3.z = std::sqrtf(param_1[0][2] * param_1[0][2]
	                       + param_1[1][2] * param_1[1][2]
	                       + param_1[2][2] * param_1[2][2]);

	MTXIdentity(param_2);
	if (param_3.x != 0.0f) {
		param_2[0][0] = param_1[0][0] / param_3.x;
		param_2[1][0] = param_1[1][0] / param_3.x;
		param_2[2][0] = param_1[2][0] / param_3.x;
	}
	if (param_3.y != 0.0f) {
		param_2[0][1] = param_1[0][1] / param_3.y;
		param_2[1][1] = param_1[1][1] / param_3.y;
		param_2[2][1] = param_1[2][1] / param_3.y;
	}
	if (param_3.z != 0.0f) {
		param_2[0][2] = param_1[0][2] / param_3.z;
		param_2[1][2] = param_1[1][2] / param_3.z;
		param_2[2][2] = param_1[2][2] / param_3.z;
	}

	param_4.set(param_1[0][3], param_1[1][3], param_1[2][3]);
}

void JPAGetRMtxTVecElement(MtxPtr param_1, MtxPtr param_2,
                           JGeometry::TVec3<f32>& param_3)
{
	JGeometry::TVec3<f32> scale;

	scale.x = std::sqrtf(param_1[0][0] * param_1[0][0]
	                     + param_1[1][0] * param_1[1][0]
	                     + param_1[2][0] * param_1[2][0]);
	scale.y = std::sqrtf(param_1[0][1] * param_1[0][1]
	                     + param_1[1][1] * param_1[1][1]
	                     + param_1[2][1] * param_1[2][1]);
	scale.z = std::sqrtf(param_1[0][2] * param_1[0][2]
	                     + param_1[1][2] * param_1[1][2]
	                     + param_1[2][2] * param_1[2][2]);

	MTXIdentity(param_2);
	if (scale.x != 0.0f) {
		param_2[0][0] = param_1[0][0] / scale.x;
		param_2[1][0] = param_1[1][0] / scale.x;
		param_2[2][0] = param_1[2][0] / scale.x;
	}
	if (scale.y != 0.0f) {
		param_2[0][1] = param_1[0][1] / scale.y;
		param_2[1][1] = param_1[1][1] / scale.y;
		param_2[2][1] = param_1[2][1] / scale.y;
	}
	if (scale.z != 0.0f) {
		param_2[0][2] = param_1[0][2] / scale.z;
		param_2[1][2] = param_1[1][2] / scale.z;
		param_2[2][2] = param_1[2][2] / scale.z;
	}

	param_3.set(param_1[0][3], param_1[1][3], param_1[2][3]);
}

void JPAGetRMtxElement(MtxPtr, MtxPtr) { }

void JPAGetRTMtxElement(MtxPtr, MtxPtr) { }

void JPARadVecToSVec(JGeometry::TVec3<f32>&, JGeometry::TVec3<s16>&) { }

f32 JPAGetKeyFrameValue(f32 time, u16 frame_num, f32* frames)
{
	return JPAGetKeyFrameInterpolation<f32>(time, frame_num, frames);
}
