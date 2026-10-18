#include <System/ParamInst.hpp>
#include <JSystem/JGeometry.hpp>

template <typename T> void TParamT<T>::load(JSUMemoryInputStream& stream)
{
	// TODO: fakematch
	u8 discard[16];

	stream.read(&discard[8], 4);
	stream.read(&value, sizeof(T));
#ifdef SMS_PORT
	// raw read of big-endian file data: swap per scalar (u8 none, s16, s32,
	// f32, TVec3<f32> = 3 x f32)
	sms_swap_param_value(&value, (int)sizeof(T));
#endif
};

template class TParamT<u8>;
template class TParamT<s16>;
template class TParamT<s32>;
template class TParamT<f32>;
template class TParamT<JGeometry::TVec3<f32> >;
