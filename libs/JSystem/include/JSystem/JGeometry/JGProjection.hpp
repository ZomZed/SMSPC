#ifndef JG_PROJECTION_HPP
#define JG_PROJECTION_HPP

#include <JSystem/JGeometry/JGPosition3.hpp>

namespace JGeometry {

// This class is from SMG. Inheritance is a wild guess on my part,
// but it makes stuff match and kind of makes sense conceptually?
// A projective transform (TProjection) IS an affine transform (TPosition)
// IS alinear transform (TRotation).
template <class T> class TProjection3 : public TPosition3<T> {
public:
	TProjection3() { }

	// w5 (2026-10-15): only caller is TBathWaterMeshRenderer::prerender (n = 0).
	// The zero chains store [3][x] first in the ROM. The ROM's m03/m13 use an fmadds with a
	// 0.0f addend (real form unknown; "+ n" would fake it, so it is left out).
	void orthographic(f32 t, f32 b, f32 l, f32 r, f32 n, f32 f)
	{
		this->mMtx[0][0] = 2.0f / (r - l);
		this->mMtx[0][3] = -0.5f * this->mMtx[0][0] * (r + l);
		this->mMtx[1][1] = 2.0f / (t - b);
		this->mMtx[1][3] = -0.5f * this->mMtx[1][1] * (t + b);
		this->mMtx[2][2] = -1.0f / (f - n);
		this->mMtx[2][3] = this->mMtx[2][2] * f;
		this->mMtx[0][2] = this->mMtx[1][2] = this->mMtx[3][2] = 0.0f;
		this->mMtx[0][1] = this->mMtx[2][1] = this->mMtx[3][1] = 0.0f;
		this->mMtx[1][0] = this->mMtx[2][0] = this->mMtx[3][0] = 0.0f;
		this->mMtx[3][3]                                       = 1.0f;
	}
};

} // namespace JGeometry

#endif
