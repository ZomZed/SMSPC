#include <System/PositionHolder.hpp>

#ifndef SMS_PORT // the PC port uses the copy in Application.cpp (US layout)
TNameRefAryT<TStagePositionInfo>* gpPositionHolder;
#endif

void TStagePositionInfo::load(JSUMemoryInputStream& stream)
{
	JDrama::TNameRef::load(stream);
	stream >> unkC.x >> unkC.y >> unkC.z;

	stream.readF32();
	stream.readF32();
	stream.readF32();
	stream.readF32();
	stream.readF32();
	stream.readF32();
}
