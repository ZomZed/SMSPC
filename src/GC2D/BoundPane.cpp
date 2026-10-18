#include <GC2D/BoundPane.hpp>
#include <JSystem/J2D/J2DScreen.hpp>

TBoundPane::TBoundPane(J2DScreen* param_1, u32 param_2)
{
	unk0  = param_1->search(param_2);
	unk4  = unk0->mBounds;
	unk28 = 0.0f;
	unk2C = 0.0f;
	unk30 = 0.0f;
	unk34 = 0.0f;
	unk24 = false;
	unk25 = false;
}

TBoundPane::TBoundPane(JUTTexture*, GXCullMode) { }

void TBoundPane::setPanePosition(s32 param_1, const JUTPoint& param_2,
                                 const JUTPoint& param_3,
                                 const JUTPoint& param_4)
{
	unk28 = 0.0f;
	unk2C = 1.0f / param_1;
	unk38 = param_2;
	unk40 = param_3;
	unk48 = param_4;
	unk24 = true;
}

void TBoundPane::setPaneSize(s32 param_1, const JUTPoint& param_2,
                             const JUTPoint& param_3, const JUTPoint& param_4)
{
	unk30 = 0.0f;
	unk34 = 1.0f / param_1;
	unk50 = param_2;
	unk58 = param_3;
	unk60 = param_4;
	unk25 = true;
}

bool TBoundPane::update()
{
	if (unk24) {
		if (unk28 > 1.0f) {
			unk28 = 1.0f;
			unk24 = false;
		}

		makeNewPosition(unk28, unk38, unk40, unk48, mPositionOffset);
		unk0->move(unk4.x1 + mPositionOffset.x, unk4.y1 + mPositionOffset.y);

		unk28 += unk2C;
	}

	if (unk25) {
		if (unk30 > 1.0f) {
			unk30 = 1.0f;
			unk25 = false;
		}

		makeNewPosition(unk30, unk50, unk58, unk60, mSizeOffset);
		unk0->resize(mSizeOffset.x + unk4.getWidth(),
		             mSizeOffset.y + unk4.getHeight());

		unk30 += unk34;
	}

	bool result = false;
	if (!unk24 && !unk25)
		result = true;

	return result;
}

// quadratic bezier through the three control points, rounded to integers
void TBoundPane::makeNewPosition(f32 t, JUTPoint& start, JUTPoint& control,
                                 JUTPoint& end, JUTPoint& result)
{
	f32 s  = 1.0f - t;
	f32 tt = t * t;
	f32 ss = s * s;
	f32 st = 2.0f * s * t;
	f32 x  = start.x * ss + control.x * st + end.x * tt;
	f32 y  = start.y * ss + control.y * st + end.y * tt;

	x += x > 0.0f ? 0.5f : -0.5f;
	s16 ix = (s16)x;
	y += y > 0.0f ? 0.5f : -0.5f;
	result.x = ix;
	result.y = (s16)y;
}