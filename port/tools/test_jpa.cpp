// Host test for the JPA (.jpa particle) big-endian handling on 64-bit
// (JPAEmitterLoader, JPABaseShape/ExtraShape/SweepShape/ExTexShape, JPADataBlock,
// KFA1 key frames, BEM1/FLD1 stream blocks). Run through test_jpa.py.
//
//   test_jpa.exe <files.txt>
//
// For every .jpa the test runs the real JPAEmitterLoaderDataBase::load on the raw
// big-endian bytes (twice, and checks the file buffer was not modified: the
// shape blocks are read through big-endian accessors, nothing is swapped in
// place), then prints what the real shape objects, the KFA1 copies and the
// BEM1/FLD1 blocks (JPABaseEmitter::loadBaseEmitterBlock, JPABaseField::loadFieldBlock)
// hold. test_jpa.py computes the same values from the bytes in Python.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define private public
#define protected public
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JMath.hpp>
#include <JSystem/JParticle/JPABaseShape.hpp>
#include <JSystem/JParticle/JPAEmitter.hpp>
#include <JSystem/JParticle/JPAEmitterLoader.hpp>
#include <JSystem/JParticle/JPAExTexShape.hpp>
#include <JSystem/JParticle/JPAExtraShape.hpp>
#include <JSystem/JParticle/JPAField.hpp>
#include <JSystem/JParticle/JPAResourceManager.hpp>
#include <JSystem/JParticle/JPASweepShape.hpp>
#undef private
#undef protected

static int gFailures = 0;

// ---- host replacements for the heap / GX / texture pieces the loader touches ----
void* JKRHeap::alloc(u32 size, int align, JKRHeap*)
{
	return _aligned_malloc(size ? size : 1, align > 16 ? align : 16);
}
void* operator new(size_t n, JKRHeap*, int) { return malloc(n ? n : 1); }
void* operator new[](size_t n, JKRHeap*, int) { return malloc(n ? n : 1); }
extern "C" void DCStoreRange(void*, u32) { }
extern "C" void GXInitTexObj(GXTexObj*, void*, u16, u16, GXTexFmt, GXTexWrapMode, GXTexWrapMode,
                             u8)
{
}
extern "C" void GXInitTexObjLOD(GXTexObj*, GXTexFilter, GXTexFilter, f32, f32, f32, u8, u8,
                                GXAnisotropy)
{
}
JMath::TRandom_fast_::TRandom_fast_(u32) { } // static JPAEmitter RNG objects; not used here
void JUTTexture::storeTIMG(const ResTIMG* t) { mTexInfo = t; }
JUTTexture::~JUTTexture() { }

static void hex(const GXColor& c, char* out) { sprintf(out, "%u,%u,%u,%u", c.r, c.g, c.b, c.a); }

static void dumpShapes(JPADataBlockLinkInfo* li)
{
	char a[32], b[32];
	if (JPABaseShape* s = li->getBaseShape()) {
		hex(s->mPrmColor, a);
		hex(s->mEnvColor, b);
		printf("bsp %.9g %.9g %d %d %d %d %d %s %s\n", s->mBaseSizeX, s->mBaseSizeY, s->mLoopOffset,
		       s->mType, s->mDirType, s->mRotType, s->mFlags, a, b);
		printf("bsp2 %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", s->mTexStaticTransX,
		       s->mTexStaticTransY, s->mTexStaticScaleX, s->mTexStaticScaleY, s->mTilingX,
		       s->mTilingY, s->mTexScrollTransX, s->mTexScrollTransY, s->mTexScrollScaleX,
		       s->mTexScrollScaleY, s->mTexScrollRotate);
		printf("bsp3 %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", s->mColLoopOffset,
		       s->mTexLoopOffset, s->mColorRegAnmMaxFrm, s->mTextureAnmKeyNum, s->mTextureIndex,
		       s->unk80, s->unk81, s->unk82, s->unk83, s->unk84, s->unk85, s->unk86, s->unk87,
		       s->unk68);
		printf("bsp4 %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", s->mBlendMode1Index,
		       s->mSrcBlendFactor1Index, s->mDstBlendFactor1Index, s->mBlendOp1Index,
		       s->mAlphaCmpComp0Index, s->mAlphaCmpRef0, s->mAlphaCmpOpIndex,
		       s->mAlphaCmpComp1Index, s->mAlphaCmpRef1, s->mZCompLoc, s->unk77,
		       s->mZCmpFunctionIndex, s->unk79, s->unk7A, s->unk7B);
		printf("bsp5");
		if (s->mTextureIndices)
			for (int i = 0; i < s->mTextureAnmKeyNum; i++)
				printf(" %u", s->mTextureIndices[i]);
		printf("\n");
		for (int k = 0; k < 2; k++) {
			GXColor* t = k ? s->mEnvColors : s->mPrmColors;
			printf(k ? "envtbl" : "prmtbl");
			if (t)
				for (int i = 0; i <= s->mColorRegAnmMaxFrm; i++)
					printf(" %u,%u,%u,%u", t[i].r, t[i].g, t[i].b, t[i].a);
			printf("\n");
		}
	}
	if (JPAExtraShape* e = li->getExtraShape()) {
		printf("esp %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", e->mAlphaInTiming,
		       e->mAlphaOutTiming, e->mAlphaInValue, e->mAlphaBaseValue, e->unk18,
		       e->mAlphaWaveParam1, e->mAlphaWaveParam2, e->mAlphaWaveParam3,
		       e->mAlphaWaveRandom, e->mAlphaIncreaseRate, e->mAlphaDecreaseRate);
		printf("esp2 %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n",
		       e->mRandomScale, e->mScaleInTiming, e->mScaleOutTiming, e->mScaleInValueY, e->unk44,
		       e->mScaleInValueX, e->unk4C, e->mIncreaseRateX, e->mDecreaseRateX,
		       e->mIncreaseRateY, e->mDecreaseRateY, e->mRotateAngle);
		printf("esp3 %.9g %.9g %.9g %.9g %d %d %u %u %u %u %u %u %u %u %u\n", e->mRotateSpeed,
		       e->mRotateRandomSpeed, e->mRotateRandomAngle, e->mRotateDirection, e->mAnmCycleY,
		       e->mAnmCycleX, e->unk78, e->unk79, e->unk7A, e->unk7B, e->unk7C, e->unk7D, e->unk7E,
		       e->unk7F, 0);
	}
	if (JPASweepShape* w = li->getSweepShape()) {
		hex(w->mPrmColor, a);
		hex(w->mEnvColor, b);
		printf("ssp %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %s %s\n", w->unk8,
		       w->mBaseVel, w->mScaleY, w->mScaleX, w->mTiming, w->mVelInhRate, w->mBaseVelRndm,
		       w->unk24, w->mInheritScale, w->mInheritAlpha, w->mRotateSpeed, a, b);
		printf("ssp2 %.9g %d %d %u %u %u %u %u %u %u %u %u %u\n", w->mInheritRGB, w->mLife,
		       w->mRate, w->mType, w->mDirType, w->mRotType, w->mStep, w->mChildrenAffectedByFields,
		       w->unk49, w->mEnableScaleOut, w->mEnableAlphaOut, w->mTextureIndex,
		       w->mEnableRotate);
		printf("ssp3 %u\n", w->unk4E);
	}
	if (JPAExTexShape* x = li->getExTexShape()) {
		printf("etx %u %u %u %u %u %u %d", x->mIndTexMode, x->mExpScale, x->mIndTextureID,
		       x->mSubTextureID, x->mFlags, x->mSecondTexIndex, (int)x->mIndTexMtxID);
		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 3; j++)
				printf(" %.9g", x->mIndTexMtx[i][j]);
		printf("\n");
	}
}

static void dumpBem(JPADataBlock* blk)
{
	JPABaseEmitter* em = (JPABaseEmitter*)calloc(1, sizeof(JPABaseEmitter));
	em->loadBaseEmitterBlock(blk);
	printf("bem %.9g %.9g %.9g %.9g %.9g %.9g %d %d %d %u %u\n", em->mScale.x, em->mScale.y,
	       em->mScale.z, em->mTrans.x, em->mTrans.y, em->mTrans.z, em->mRot.x, em->mRot.y,
	       em->mRot.z, em->mVolumeType, em->mEmitInterval);
	printf("bem2 %u %.9g %.9g %d %d %u %.9g %.9g %d %.9g %.9g %.9g\n", em->mVolumeSubdivision,
	       em->mChildSpawnRate, em->mChildSpawnRateVariance, em->mMaxFrame, em->mStartFrame,
	       em->mVolumeSize, em->mVolumeYawSweep, em->mVolumeMinRadius, em->mBaseLifetime,
	       em->mLifetimeRandomScale, em->mBaseWeight, em->mWeightRandomScale);
	printf("bem3 %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %u %u\n",
	       em->unk1C4, em->unk1CC, em->mBaseAirResistance, em->mAirResistanceVariance, em->unk1FC,
	       em->unk200, em->unk204, em->unk208, em->unk1C8, em->mEmitterDirection.x,
	       em->mEmitterDirection.y, em->mEmitterDirection.z, em->unk1E4, em->mEmitFlags,
	       em->mKeyAnmTypeMask);
	free(em);
}

static void dumpField(JPADataBlock* blk)
{
	JPABaseField f;
	f.loadFieldBlock(blk);
	printf("fld %u %u %u %u %u %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %u\n", f.unk50,
	       f.unk51, f.unk52, f.unk53, f.unk54, f.unk10, f.unk14, f.mMaxDistance, f.unk18.x,
	       f.unk18.y, f.unk18.z, f.unk24.x, f.unk24.y, f.unk24.z, f.unk30, f.unk38);
	printf("fld2 %.9g %.9g %.9g %.9g %.9g\n", f.unk34, f.mFadeInEnd, f.mFadeOutStart,
	       f.mFadeInStart, f.mFadeOutEnd);
}

static std::string load(std::vector<unsigned char>& file)
{
	JPATextureResource* tex = new JPATextureResource(64, nullptr);
	JPAEmitterData* d       = JPAEmitterLoaderDataBase::load(file.data(), nullptr, tex);
	if (!d) {
		printf("FAIL: loader returned null\n");
		gFailures++;
		return "";
	}
	JPADataBlockLinkInfo* li = d->unk0[0];
	printf("link %u %u %u %d %d %d %d\n", li->mFieldNum, li->mTextureNum, li->mKeyframeAnimationNum,
	       li->mBaseShape != nullptr, li->mExtraShape != nullptr, li->mSweepShape != nullptr,
	       li->mExTexShape != nullptr);
	dumpShapes(li);
	dumpBem(li->mBaseEmitterBlock);
	for (int i = 0; i < li->mFieldNum; i++)
		dumpField(li->mFields[i]);
	for (int i = 0; i < li->mKeyframeAnimationNum; i++) {
		u8* raw = (u8*)li->mKeyframeAnimations[i]->mRawData;
		printf("kfa %u %u", raw[0x10], raw[0x12]);
		const float* k = (const float*)(raw + 0x20);
		for (int j = 0; j < raw[0x10] * 4; j++)
			printf(" %.9g", k[j]);
		printf("\n");
	}
	for (int i = 0; i < li->mTextureNum; i++) {
		JPATexture* t = tex->pTexResArray[li->mTextureDataBase[i]];
		const ResTIMG* r = &t->getTextureData()->mResTIMG;
		printf("tex %s %u %u %u %u\n", t->getName(), r->format, r->width, r->height,
		       (unsigned)r->imageDataOffset);
	}
	return "";
}

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	std::ifstream list(argv[1]);
	std::string path;
	int files = 0;
	while (std::getline(list, path)) {
		if (path.empty())
			continue;
		std::ifstream f(path, std::ios::binary);
		std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)),
		                               std::istreambuf_iterator<char>());
		std::vector<unsigned char> orig = buf;
		printf("== %s\n", path.c_str());
		load(buf);
		load(buf); // second load of the same (shared) resource: must give the same values
		if (buf != orig) {
			// JPATexture swaps only its own copy, so the file must be untouched
			printf("FAIL file buffer modified: %s\n", path.c_str());
			gFailures++;
		}
		files++;
	}
	printf("SUMMARY files %d failures %d\n", files, gFailures);
	return gFailures ? 1 : 0;
}


