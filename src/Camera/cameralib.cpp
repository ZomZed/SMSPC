#include <dolphin/types.h>
#include <Camera/cameralib.hpp>
#include <JSystem/JMath.hpp>
#include <MarioUtil/MathUtil.hpp>

// TODO: This macro should probably be consolidated elsewhere
#define ABS(x) ((x) >= 0 ? (x) : -(x))

const JGeometry::TVec3<f32> CLBConstUpVec(0.0f, 1.0f, 0.0f);

static const f32 SHORTANGLE_TO_DEGREES = 0.005493164f; // 360/65536
static const f32 DEGREES_TO_RADIANS    = 0.017453294f; // pi/180

// TODO: These are very fake
static void normalizeInner1(JGeometry::TVec3<f32>& vec) { vec.normalize(); }
static void normalizeInner2(JGeometry::TVec3<f32>& vec)
{
	normalizeInner1(vec);
}

static inline f32 XZ1(const JGeometry::TVec3<f32>& a, const JGeometry::TVec3<f32>& b)
{
	f32 dx = a.x - b.x;
	f32 dz = a.z - b.z;
	return MsSqrtf(dx * dx + dz * dz);
}
static inline f32 XZ0(const JGeometry::TVec3<f32>& a, const JGeometry::TVec3<f32>& b)
{
	return XZ1(a, b);
}
static inline f32 XZ2(const JGeometry::TVec3<f32>& a, const JGeometry::TVec3<f32>& b)
{
	return XZ0(a, b);
}

static inline void RotXInline(JGeometry::TVec3<f32>& v, s16 angle)
{
	f32 sinX = JMASSin(angle);
	f32 cosX = JMASCos(angle);
	f32 y    = v.y;
	v.y      = y * cosX - v.z * sinX;
	v.z      = y * sinX + v.z * cosX;
}

static inline void RotYInline(JGeometry::TVec3<f32>& v, s16 angle)
{
	f32 sinY = JMASSin(angle);
	f32 cosY = JMASCos(angle);
	f32 x    = v.x;
	v.x      = x * cosY + v.z * sinY;
	v.z      = -x * sinY + v.z * cosY;
}

static inline void RotateAboutAxis(const JGeometry::TVec3<f32>& param_axis,
                                   f32 angle, JGeometry::TVec3<f32>* vec)
{
	JGeometry::TRotation3<TMtx33f> mtxT;

	mtxT.identity();
	mtxT.setRotate(param_axis, angle);
	CLBMultTranspose33(mtxT, *vec);
}

// Same as RotateAboutAxis, but with the source copy as a local of this
// helper (CLBCalcNearNinePos keeps it above the matrix on the stack).
static inline void RotateAboutAxisNine(const JGeometry::TVec3<f32>& param_axis,
                                       f32 angle, JGeometry::TVec3<f32>& vec)
{
	JGeometry::TRotation3<TMtx33f> mtxT;
	JGeometry::TVec3<f32> src;

	mtxT.identity();
	mtxT.setRotate(param_axis, angle);
	src = vec;
	CLBMultTranspose33(mtxT, src, vec);
}

void CLBCalc2DFPos(JGeometry::TVec2<f32>* out_ndc_pos, const f32 (*proj_mtx)[4],
                   const f32 (*view_mtx)[4], const Vec& world_pos,
                   u32* out_depth, bool disable_z_clip)
{
	Vec camSpacePos;

	MTXMultVec((MtxPtr)view_mtx, (Vec*)&world_pos, &camSpacePos);

	if (camSpacePos.z == 0.0f) {
		out_ndc_pos->x = out_ndc_pos->y = 10000.0f;
		return;
	}

	f32 perspectiveFactor = 1.0f / -camSpacePos.z;

	f32 z = proj_mtx[2][2] * camSpacePos.z + proj_mtx[2][3];
	z *= perspectiveFactor;
	if (!disable_z_clip && (z > 0.0f || z < -1.0f)) {
		out_ndc_pos->x = out_ndc_pos->y = 10000.0f;
		return;
	}

	f32 x = proj_mtx[0][0] * camSpacePos.x + proj_mtx[0][2] * camSpacePos.z;
	f32 y = proj_mtx[1][1] * camSpacePos.y + proj_mtx[1][2] * camSpacePos.z;
	out_ndc_pos->set(x * perspectiveFactor, y * perspectiveFactor);

	if (out_depth != nullptr)
		*out_depth = CLBLinearInbetween<u32>(0, 0xffffff, z + 1.0f);
}

BOOL CLBChaseAngleDecrease(s16* value, s16 desired, s16 ratio)
{
	if (ratio == 0) {
		*value = desired;
	} else {
		s16 newValue = *value - desired;
		newValue -= newValue / ratio;
		newValue += desired;
		if (newValue == *value)
			return false;

		*value = newValue;
	}

	if (*value == desired)
		return false;

	return true;
}

BOOL CLBChaseDecrease(f32* value, f32 desired, f32 ratio, f32 threshold)
{
	if (ratio > 1.0f)
		ratio = 1.0f;

	*value += ratio * (desired - *value);

	// You'd think this would just be
	// "return ABS(*dstValue - targetValue) > ABS(threshold);"
	// but this gives an exact match
	if (ABS(*value - desired) <= ABS(threshold))
		return false;

	return true;
}

BOOL CLBChaseSpecialDecrease(f32* value, f32 desired, f32 ratio, f32 speed)
{
	if (ratio > 1.0f)
		ratio = 1.0f;

	f32 actualSpeed = ratio * (desired - *value);

	if (CLBAbs(actualSpeed) < CLBAbs(speed))
		actualSpeed = speed;

	return CLBChaseGeneralConstantSpecifySpeed(value, desired, actualSpeed);
}

void CLBCrossToPolar(const Vec& origin, const Vec& in, f32* out_radius,
                     s16* out_pitch, s16* out_yaw)
{
	f32 dx = in.x - origin.x;
	f32 dy = in.y - origin.y;
	f32 dz = in.z - origin.z;

	*out_radius = MsSqrtf(dx * dx + dy * dy + dz * dz);

	*out_pitch = matan(MsSqrtf(dx * dx + dz * dz), dy);
	*out_yaw   = matan(dz, dx);
}

void CLBPolarToCross(const Vec& origin, Vec* out, f32 radius, s16 vAngle,
                     s16 hAngle)
{
	out->x = origin.x + radius * JMASCos(vAngle) * JMASSin(hAngle);
	out->y = origin.y + radius * JMASSin(vAngle);
	out->z = origin.z + radius * JMASCos(vAngle) * JMASCos(hAngle);
}

void CLBRevisionLookatByAngleX(s16 vAngleMin, s16 vAngleMax, const Vec& origin,
                               Vec* inOut)
{
	f32 radius;
	s16 vAngle;
	s16 hAngle;

	CLBCrossToPolar(origin, *inOut, &radius, &vAngle, &hAngle);
	vAngle = MsClamp(vAngle, vAngleMin, vAngleMax);
	CLBPolarToCross(origin, inOut, radius, vAngle, hAngle);
}

void CLBRotatePosAndUp(s16 sAngle1, s16 sAngle2,
                       const JGeometry::TVec3<f32>& axis1,
                       const JGeometry::TVec3<f32>& axis2,
                       const JGeometry::TVec3<f32>& offset,
                       JGeometry::TVec3<f32>* param_6,
                       JGeometry::TVec3<f32>* param_7)

{
	f32 angle1 = sAngle1 * SHORTANGLE_TO_DEGREES * DEGREES_TO_RADIANS;
	f32 angle2 = sAngle2 * SHORTANGLE_TO_DEGREES * DEGREES_TO_RADIANS;

	JGeometry::TVec3<f32> v1 = *param_6 - offset;
	RotateAboutAxis(axis1, -angle1, &v1);
	*param_6 = offset + v1;
	RotateAboutAxis(axis1, -angle1, param_7);

	JGeometry::TVec3<f32> v2 = *param_6 - offset;
	RotateAboutAxis(axis2, -angle2, &v2);
	*param_6 = offset + v2;
	RotateAboutAxis(axis2, -angle2, param_7);
}

bool CLBIsPointInCube(const Vec& param_1, const Vec& param_2,
                      const Vec& param_3, const Vec& param_4)
{
	// TODO: Why is so much of this copy-pasted from CLBCalcPointInCubeRatio?

	f32 dx = param_1.x - param_2.x;
	f32 dy = param_1.y - param_2.y;
	f32 dz = param_1.z - param_2.z;

	bool result = false;

	if (param_3.y != 0.0f || param_3.x != 0.0f || param_3.z != 0.0f) {
		if (param_3.z != 0.0f) {
			s16 zAngle = CLBDegToShortAngle(-param_3.z);
			f32 cosZ   = JMASCos(zAngle);
			f32 sinZ   = JMASSin(zAngle);

			f32 dySinZ = dy * sinZ;

			dy = dx * sinZ + dy * cosZ;
			dx = dx * cosZ - dySinZ;
		}

		if (param_3.y != 0.0f) {
			s16 yAngle = CLBDegToShortAngle(-param_3.y);
			f32 cosY   = JMASCos(yAngle);
			f32 sinY   = JMASSin(yAngle);

			f32 dzSinY = dz * sinY;

			dz = -dx * sinY + dz * cosY;
			dx = dx * cosY + dzSinY;
		}

		if (param_3.x != 0.0f) {
			s16 xAngle = CLBDegToShortAngle(-param_3.x);
			f32 cosX   = JMASCos(xAngle);
			f32 sinX   = JMASSin(xAngle);

			f32 dzSinX = dz * sinX;

			dz = dy * sinX + dz * cosX;
			dy = dy * cosX - dzSinX;
		}
	}

	if ((-param_4.x * 0.5f < dx && dx < param_4.x * 0.5f)
	    && (0.0f < dy && dy < param_4.y)
	    && (-param_4.z * 0.5f < dz && dz < param_4.z * 0.5f)) {
		result = true;
	}

	return result;
}

void CLBCalcPointInCubeRatio(const Vec& param_1, const Vec& param_2,
                             const Vec& param_3, const Vec& param_4,
                             f32* param_5, f32* param_6, f32* param_7)
{
	f32 dx = param_1.x - param_2.x;
	f32 dy = param_1.y - param_2.y;
	f32 dz = param_1.z - param_2.z;

	if (param_3.z != 0) {
		s16 zAngle = CLBDegToShortAngle(-param_3.z);
		f32 cosZ   = JMASCos(zAngle);
		f32 sinZ   = JMASSin(zAngle);

		f32 dySinZ = dy * sinZ;

		dy = dx * sinZ + dy * cosZ;
		dx = dx * cosZ - dySinZ;
	}

	if (param_3.y != 0) {
		s16 yAngle = CLBDegToShortAngle(-param_3.y);
		f32 cosY   = JMASCos(yAngle);
		f32 sinY   = JMASSin(yAngle);

		f32 dzSinY = dz * sinY;

		dz = -dx * sinY + dz * cosY;
		dx = dx * cosY + dzSinY;
	}

	if (param_3.x != 0) {
		s16 xAngle = CLBDegToShortAngle(-param_3.x);
		f32 cosX   = JMASCos(xAngle);
		f32 sinX   = JMASSin(xAngle);

		f32 dzSinX = dz * sinX;

		dz = dy * sinX + dz * cosX;
		dy = dy * cosX - dzSinX;
	}

	if (param_5 != nullptr) {
		*param_5 = CLBCalcRatio(-param_4.x * 0.5f, param_4.x * 0.5f, dx);
	}

	if (param_6 != nullptr) {
		*param_6 = CLBCalcRatio(0.0f, param_4.y, dy);
	}

	if (param_7 != nullptr) {
		*param_7 = CLBCalcRatio(-param_4.z * 0.5f, param_4.z * 0.5f, dz);
	}
}

void CLBCalcRotateZXYTranslateMatrix(MtxPtr mtx, const Vec& rotate,
                                     const Vec& translate)
{
	s16 sAngleX = CLBDegToShortAngle(rotate.x);
	s16 sAngleY = CLBDegToShortAngle(rotate.y);
	s16 sAngleZ = CLBDegToShortAngle(rotate.z);

	f32 sinX = JMASSin(sAngleX);
	f32 cosX = JMASCos(sAngleX);
	f32 sinY = JMASSin(sAngleY);
	f32 cosY = JMASCos(sAngleY);
	f32 sinZ = JMASSin(sAngleZ);
	f32 cosZ = JMASCos(sAngleZ);

	mtx[0][0] = cosY * cosZ + sinY * sinX * sinZ;
	mtx[0][1] = -sinZ * cosY + cosZ * sinY * sinX;
	mtx[0][2] = sinY * cosX;
	mtx[0][3] = translate.x;

	mtx[1][0] = cosX * sinZ;
	mtx[1][1] = cosX * cosZ;
	mtx[1][2] = -sinX;
	mtx[1][3] = translate.y;

	mtx[2][0] = -sinY * cosZ + sinZ * cosY * sinX;
	mtx[2][1] = sinY * sinZ + cosZ * cosY * sinX;
	mtx[2][2] = cosY * cosX;
	mtx[2][3] = translate.z;
}

void CLBCalcScaleTranslateMatrix(MtxPtr mtx, const Vec& scale,
                                 const Vec& translate)
{
	mtx[0][0] = scale.x;
	mtx[0][1] = 0.0f;
	mtx[0][2] = 0.0f;
	mtx[0][3] = translate.x;

	mtx[1][0] = 0.0f;
	mtx[1][1] = scale.y;
	mtx[1][2] = 0.0f;
	mtx[1][3] = translate.y;

	mtx[2][0] = 0.0f;
	mtx[2][1] = 0.0f;
	mtx[2][2] = scale.z;
	mtx[2][3] = translate.z;
}

void CLBCalcNearNinePos(JGeometry::TVec3<f32>* out_grid, S16Vec* out_euler,
                        const JGeometry::TVec3<f32>& origin,
                        const JGeometry::TVec3<f32>& lookat, s16 roll,
                        f32 near_dist, const JGeometry::TVec2<f32>& near_dims)
{
	// TODO(94%): remaining diffs are load CSE of local_68/local_74 across the
	// out_grid stores (ROM keeps y/z in registers), two src.x reloads in the
	// rotate inlines, and two 12-byte dead stack slots.

	JGeometry::TVec3<f32> fVar19;
	JGeometry::TVec3<f32> fVar16;
	JGeometry::TVec3<f32> local_68;
	JGeometry::TVec3<f32> local_74;
	JGeometry::TVec3<f32> local_80;
	JGeometry::TVec3<f32> local_90;
	JGeometry::TVec3<f32> fVar22;
	JGeometry::TVec3<f32> local_a8;

	local_a8.set(lookat.x - origin.x, lookat.y - origin.y, lookat.z - origin.z);
	normalizeInner2(local_a8);

	// Center point
	out_grid[4].scaleAdd(near_dist, origin, local_a8);

	f32 xzDistance = XZ2(origin, lookat);
	out_euler->x   = -matan(xzDistance, origin.y - lookat.y);
	out_euler->y   = matan(origin.z - lookat.z, origin.x - lookat.x);
	out_euler->z   = roll;

	local_68.set(0.0f, 1.0f, 0.0f);
	local_74.set(1.0f, 0.0f, 0.0f);

	// We already did this, so maybe we're calling another function here?
	local_80.set(lookat.x - origin.x, lookat.y - origin.y, lookat.z - origin.z);
	normalizeInner1(local_80);

	f32 rollRad = out_euler->z * SHORTANGLE_TO_DEGREES * DEGREES_TO_RADIANS;

	// Basically transform the up/right vectors from cam space into world space.
	// TODO: Definitely inlines...

	{
		RotXInline(local_68, out_euler->x);
		RotYInline(local_68, out_euler->y);

		RotateAboutAxisNine(local_80, rollRad, local_68);
	}

	{
		RotXInline(local_74, out_euler->x);
		RotYInline(local_74, out_euler->y);

		RotateAboutAxisNine(local_80, rollRad, local_74);
	}

	f32 fVar3 = near_dims.y * 0.5f;
	f32 fVar5 = near_dims.x * 0.5f;
	f32 fVar6 = -fVar3;
	f32 fVar7 = -fVar5;

	fVar19.x      = local_68.x * fVar3;
	out_grid[1].x = out_grid[4].x + fVar19.x;
	fVar19.y      = local_68.y * fVar3;
	out_grid[1].y = out_grid[4].y + fVar19.y;
	fVar19.z      = local_68.z * fVar3;
	out_grid[1].z = out_grid[4].z + fVar19.z;
	out_grid[7].x = out_grid[4].x + local_68.x * fVar6;
	out_grid[7].y = out_grid[4].y + local_68.y * fVar6;
	out_grid[7].z = out_grid[4].z + local_68.z * fVar6;
	fVar22.x      = local_74.x * fVar7;
	out_grid[3].x = out_grid[4].x + fVar22.x;
	fVar22.y      = local_74.y * fVar7;
	out_grid[3].y = out_grid[4].y + fVar22.y;
	fVar22.z      = local_74.z * fVar7;
	out_grid[3].z = out_grid[4].z + fVar22.z;
	fVar16.x      = local_74.x * fVar5;
	out_grid[5].x = out_grid[4].x + fVar16.x;
	fVar16.y      = local_74.y * fVar5;
	out_grid[5].y = out_grid[4].y + fVar16.y;
	fVar16.z      = local_74.z * fVar5;
	out_grid[5].z = out_grid[4].z + fVar16.z;

	// Anything below here could be part of CLBCalcNearFourPos?

	f32 halfPlaneDiagonal = MsSqrtf(fVar3 * fVar3 + fVar5 * fVar5);

	local_90.x = fVar22.x;
	local_90.y = fVar22.y;
	local_90.z = fVar22.z;
	local_90.add(fVar19);
	MsVECNormalize(&local_90, &local_90);

	out_grid[0].scaleAdd(halfPlaneDiagonal, out_grid[4], local_90);
	local_90.negate();
	out_grid[8].scaleAdd(halfPlaneDiagonal, out_grid[4], local_90);

	local_90.x = fVar16.x;
	local_90.y = fVar16.y;
	local_90.z = fVar16.z;
	local_90.add(fVar19);
	MsVECNormalize(&local_90, &local_90);

	out_grid[2].scaleAdd(halfPlaneDiagonal, out_grid[4], local_90);
	local_90.negate();
	out_grid[6].scaleAdd(halfPlaneDiagonal, out_grid[4], local_90);
}
