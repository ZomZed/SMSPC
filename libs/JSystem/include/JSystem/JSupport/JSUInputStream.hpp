#ifndef JSU_INPUT_STREAM_H
#define JSU_INPUT_STREAM_H

#include <dolphin/types.h>
#include <JSystem/JSupport/JSUIosBase.hpp>

// Typed multi-byte reads. Stream data is big-endian (GameCube files); the PC
// port reads it byte-swapped. The untyped read(void*, s32) stays a raw copy.
#ifdef SMS_PORT
#define JSU_READ_TYPED(p, size) readSwapped(p, size)
#else
#define JSU_READ_TYPED(p, size) read(p, size)
#endif

class JSUInputStream : public JSUIosBase {
public:
	virtual ~JSUInputStream();
	virtual int getAvailable() const = 0;
	virtual int skip(s32 amount);
	virtual int readData(void* buf, s32 size) = 0;

	u32 read(void* buf, s32 size);
	char* read(char* buf);
	char* readString();
	char* readString(char* buf, u16 len);

#ifdef SMS_PORT
	u32 readSwapped(void* buf, s32 size)
	{
		u32 len = read(buf, size);
		sms_swap_value(buf, size);
		return len;
	}
#endif

	u32 read(s8& p) { return read(&p, sizeof(s8)); }
	u32 read(u8& p) { return read(&p, sizeof(u8)); }
	u32 read(bool& p) { return read(&p, sizeof(bool)); }
	u32 read(s16& p) { return JSU_READ_TYPED(&p, sizeof(s16)); }
	u32 read(u16& p) { return JSU_READ_TYPED(&p, sizeof(u16)); }
	u32 read(s32& p) { return JSU_READ_TYPED(&p, sizeof(s32)); }
	u32 read(u32& p) { return JSU_READ_TYPED(&p, sizeof(u32)); }
	u32 read(s64& p) { return JSU_READ_TYPED(&p, sizeof(s64)); }
	u32 read(u64& p) { return JSU_READ_TYPED(&p, sizeof(u64)); }

	u8 read8b()
	{
		u8 b;
		read(&b, sizeof(u8));
		return b;
	}

	u16 read16b()
	{
		u16 s;
		JSU_READ_TYPED(&s, sizeof(u16));
		return s;
	}

	u32 read32b()
	{
		u32 i;
		JSU_READ_TYPED(&i, sizeof(u32));
		return i;
	}

	/* @fabricated */
	s8 readS8()
	{
		s8 b;
		read(&b, sizeof(s8));
		return b;
	}

	u8 readU8()
	{
		u8 b;
		read(&b, sizeof(u8));
		return b;
	}

	s16 readS16()
	{
		s16 s;
		JSU_READ_TYPED(&s, sizeof(s16));
		return s;
	}

	u16 readU16()
	{
		u16 s;
		JSU_READ_TYPED(&s, sizeof(u16));
		return s;
	}

	s32 readS32()
	{
		s32 i;
		JSU_READ_TYPED(&i, sizeof(s32));
		return i;
	}

	u32 readU32()
	{
		u32 i;
		JSU_READ_TYPED(&i, sizeof(u32));
		return i;
	}

	u64 readU64()
	{
		u64 i;
		JSU_READ_TYPED(&i, sizeof(u64));
		return i;
	}

	/* @fabricated */
	f32 readF32()
	{
		f32 f;
		JSU_READ_TYPED(&f, sizeof(f32));
		return f;
	}

	JSUInputStream& operator>>(bool& p)
	{
		read(&p, sizeof(bool));
		return *this;
	}

	JSUInputStream& operator>>(s8& p)
	{
		read(&p, sizeof(s8));
		return *this;
	}

	JSUInputStream& operator>>(u8& p)
	{
		read(&p, sizeof(u8));
		return *this;
	}

	JSUInputStream& operator>>(s16& p)
	{
		JSU_READ_TYPED(&p, sizeof(s16));
		return *this;
	}

	JSUInputStream& operator>>(u16& p)
	{
		JSU_READ_TYPED(&p, sizeof(u16));
		return *this;
	}

	JSUInputStream& operator>>(s32& p)
	{
		JSU_READ_TYPED(&p, sizeof(s32));
		return *this;
	}

	JSUInputStream& operator>>(u32& p)
	{
		JSU_READ_TYPED(&p, sizeof(u32));
		return *this;
	}

	JSUInputStream& operator>>(f32& p)
	{
		JSU_READ_TYPED(&p, sizeof(f32));
		return *this;
	}
};

#undef JSU_READ_TYPED

#endif
