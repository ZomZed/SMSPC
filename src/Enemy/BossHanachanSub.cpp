#include <Enemy/BossHanachanSub.hpp>
#include <Player/ModelWaterManager.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <Camera/cameralib.hpp>
#include <System/MarDirector.hpp>
#include <JSystem/JMath.hpp>

// fabricated
// Rotates the XZ vector (x, z) around the Y axis by rotY degrees.
inline void BHSRotateXZ(f32 x, f32 z, f32 rotY, f32* out_x, f32* out_z)
{
	s16 angle = CLBDegToShortAngle(rotY);
	f32 c     = JMASCos(angle);
	f32 s     = JMASSin(angle);
	*out_x    = x * c + z * s;
	*out_z    = -x * s + z * c;
}

// fabricated
// Scalar variant of MsGetRotFromZaxisY for a vector in the XZ plane.
inline f32 BHSGetRotFromZaxisY(f32 x, f32 z)
{
	if (z == 0.0f) {
		if (x >= 0.0f)
			return 90.0f;
		else
			return -90.0f;
	}

	if (z >= 0.0f)
		return (360.0f / 65536.0f) * matan(z, x);
	return 180.0f - (f32)(matan(-z, x) * (360.0f / 65536.0f));
}

f32 BHSCalcCentrifugalForce(const JGeometry::TVec3<f32>& pos,
                            const JGeometry::TVec3<f32>& prev_pos,
                            const JGeometry::TVec3<f32>& prev_prev_pos,
                            f32 rot_y)
{
	f32 dz   = pos.z - prev_pos.z;
	f32 dx   = pos.x - prev_pos.x;
	f32 dist = dx * dx + dz * dz;
	if (dist < 0.001f)
		return 0.0f;

	f32 prevDz = prev_pos.z - prev_prev_pos.z;
	f32 prevDx = prev_pos.x - prev_prev_pos.x;
	if (prevDx * prevDx + prevDz * prevDz < 0.001f)
		return 0.0f;

	s16 angle     = CLBDegToShortAngle(BHSGetRotFromZaxisY(dx, dz));
	s16 prevAngle = CLBDegToShortAngle(BHSGetRotFromZaxisY(prevDx, prevDz));
	if (angle == prevAngle)
		return 0.0f;

	int diff  = angle - prevAngle;
	f32 ratio = (1.0f / 32768.0f) * CLBAbs<int>(diff);
	if (ratio >= 0.5f)
		return 0.0f;

	f32 force = dist * ratio;
	if (diff < 0)
		force = -force;

	return force * JMASCos(angle - CLBDegToShortAngle(rot_y));
}

void BHSCalcRevisionDistXZByRotateZ(f32 rot_y, f32 rot_z, f32 length,
                                    f32* out_x, f32* out_z)
{
	f32 dist = length * rot_z;
	BHSRotateXZ(dist, 0.0f, rot_y, out_x, out_z);
}

void TWaterHitActor::onWaterHitCounter() { mHitCounter = 0x3C; }

BOOL TWaterHitActor::receiveMessage(THitActor* sender, u32 message)
{
	BOOL result = FALSE;
	if (message == HIT_MESSAGE_SPRAYED_BY_WATER) {
		if (gpMarDirector->isThing())
			mHitCounter = 0;
		else
			mHitCounter = 0x3C;
		result = TRUE;
	}
	return result;
}

TSphereLink::TSphereLink(u16 num, const JGeometry::TVec3<f32>& pos,
                         f32 length, f32 radius, f32 param_3, f32 param_4,
                         f32 param_5, f32 rot_y)
{
	unk0  = num;
	unk4  = new TSpherePoint[num];
	unk8  = param_3;
	unkC  = param_4;
	unk10 = radius;
	unk14 = param_5;
	unk18 = rot_y;

	f32 offsetX = MsSin(unk18) * length;
	f32 offsetZ = MsCos(unk18) * length;
	for (int i = 0; i < unk0; ++i) {
		TSpherePoint& point = unk4[i];
		JGeometry::TVec3<f32> pointPos;
		if (i == 0) {
			pointPos = pos;
		} else {
			pointPos = unk4[i - 1].unkC;
			pointPos.x -= offsetX;
			pointPos.z -= offsetZ;
		}
		point.unkC = pointPos;
		point.unk0 = point.unkC;
		point.unk18.set(0.0f, 0.0f, 0.0f);
		point.unk24 = length;
		point.unk28 = 0.0f;
	}
}

// fabricated wrapper: the ROM calls isIllegalData out of line here
static inline bool IsLegalWrap0(const TBGCheckData* data)
{
	return data->isLegal();
}
static inline bool IsLegalWrap(const TBGCheckData* data)
{
	return IsLegalWrap0(data);
}

void TSphereLink::execMapCollision_(JGeometry::TVec3<f32>* pos)
{
	gpMap->isTouchedOneWallAndMoveXZ(&pos->x, pos->y, &pos->z, unk10);
	const TBGCheckData* data;
	f32 groundY = gpMap->checkGroundIgnoreWaterSurface(pos->x, pos->y + unk10,
	                                                   pos->z, &data);
	if (data != nullptr && IsLegalWrap(data) && pos->y < groundY)
		pos->y = groundY;
}

void TSphereLink::moveHead(const JGeometry::TVec3<f32>& head)
{
	for (int i = 0; i < unk0; ++i) {
		unk4[i].unkC.y += unkC;
		unk4[i].unkC += unk4[i].unk18;
	}

	unk4[0].unkC = head;
	execMapCollision_(&unk4[0].unkC);

	for (int i = 1; i < unk0; ++i) {
		TSpherePoint& point     = unk4[i];
		TSpherePoint& prev      = unk4[i - 1];
		JGeometry::TVec3<f32> d = point.unkC - prev.unkC;
		if (d.isZero())
			d.set(0.0f, 1.0f, 0.0f);
		else
			VECNormalize(&d, &d);
		d.scale(point.unk24);
		point.unkC = prev.unkC + d;
		execMapCollision_(&point.unkC);
	}

	for (int i = 0; i < unk0; ++i) {
		f32 scale               = unk8;
		JGeometry::TVec3<f32> v;
		v             = unk4[i].unkC - unk4[i].unk0;
		unk4[i].unk18 = scale * v;
		unk4[i].unk0            = unk4[i].unkC;
	}
}

BOOL TSphereLink::setDegreeZAndRevisionPosXZ(int index, f32 degree)
{
	BOOL changed        = FALSE;
	TSpherePoint& point = unk4[index];
	f32 oldDegree       = point.unk28;
	if (oldDegree != degree) {
		point.unk28 = degree;
		changed     = TRUE;

		f32 rotY;
		if (index == 0)
			rotY = unk18;
		else {
			JGeometry::TVec3<f32> diff = unk4[index - 1].unkC - point.unkC;
			rotY = MsWrap(MsGetRotFromZaxisY(diff), 0.0f, 360.0f);
		}

		f32 x, z;
		BHSCalcRevisionDistXZByRotateZ(rotY, degree - oldDegree, unk14, &x,
		                               &z);
		point.unkC.x += x;
		point.unkC.z += z;
	}
	return changed;
}
