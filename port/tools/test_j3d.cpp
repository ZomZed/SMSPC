// Host test for the J3D swap passes and the 64-bit J3D file structs
// (port/include/port_j3d_swap.h, JSU_FILE_PTR). Run through test_j3d.py.
//
//   test_j3d.exe <files.txt> <data root>
//
// For every model ("M path") / animation ("A path") listed in files.txt:
//  - runs port_swap_j3d_model / port_swap_j3d_anm, checks it reports no
//    layout conflicts, that a second call is a no-op (idempotent), and that
//    the SHP1 display lists and TEX1 texel data are byte-identical to the file;
//  - models: reads every block through the real file-facing structs
//    (J3DModelInfoBlock, J3DVertexBlock, ...), the real J3DJointFactory /
//    J3DShapeFactory / J3DMaterialFactory and JUTNameTab, the same way
//    J3DModelLoader does;
//  - animations: loads them with the real J3DAnmLoaderDataBase::load and
//    reads the resulting J3DAnm* objects; also checks that key-frame times in
//    every key table are ordered and inside the animation length.
// Prints one "== <relative path>" section per file in the format of
// test_j3d.py's expected.txt (values read big-endian by the Python side).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#define private public
#define protected public
#include <JSystem/J3D/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DAnmLoader.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DJointFactory.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DMaterialFactory.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DShapeFactory.hpp>
#include <JSystem/JSupport.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <JSystem/ResTIMG.hpp>
#undef private
#undef protected

#include "port_j3d_swap.h"

static int gFailures = 0;
static int gSoft = 0, gAnmHard = 0; // shared-offset diagnostics (port_j3d_swap.h)
static std::string gFile;
#define CHECK(cond, ...)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::printf("FAIL %s: ", gFile.c_str());                           \
			std::printf(__VA_ARGS__);                                          \
			std::printf("\n");                                                 \
			gFailures++;                                                       \
		}                                                                      \
	} while (0)

static std::string L;
static void add(const char* s)
{
	if (!L.empty())
		L += ' ';
	L += s;
}
static void addi(long long v)
{
	char b[32];
	std::snprintf(b, sizeof b, "%lld", v);
	add(b);
}
static void addf(double v)
{
	char b[48];
	std::snprintf(b, sizeof b, "%.9g", v);
	add(b);
}
static void addName(const char* s)
{
	std::string o;
	for (const unsigned char* p = (const unsigned char*)s; p && *p; p++) {
		if (*p >= 0x21 && *p < 0x7F) {
			o += (char)*p;
		} else {
			char b[8];
			std::snprintf(b, sizeof b, "\\x%02x", *p);
			o += b;
		}
	}
	add(o.empty() ? "_" : o.c_str());
}
static void endl()
{
	std::printf("%s\n", L.c_str());
	L.clear();
}

typedef std::map<u32, const JUTDataBlockHeader*> Blocks;
static Blocks walk(const u8* file)
{
	Blocks out;
	const JUTDataFileHeader* fh   = (const JUTDataFileHeader*)file;
	const JUTDataBlockHeader* blk = &fh->mFirstBlock;
	for (u32 i = 0; i < fh->mBlockNum; i++) {
		if ((const u8*)blk + 8 > file + fh->mFileSize)
			break; // some .bmt files declare more blocks than they hold
		out[blk->mType] = blk;
		if (blk->mSize == 0)
			break;
		blk = (const JUTDataBlockHeader*)((const u8*)blk + blk->mSize);
	}
	return out;
}

// End of the table at `start` inside a block: next larger offset or the block end.
static u32 tableEnd(const u32* offs, int n, u32 start, u32 size)
{
	u32 end = size;
	for (int i = 0; i < n; i++)
		if (offs[i] > start && offs[i] < end)
			end = offs[i];
	return end;
}

// ---------------------------------------------------------------- models
static void model(u8* buf, const std::vector<u8>& raw)
{
	int conflicts = 0;
	CHECK(port_swap_j3d_model_ex(buf, &conflicts) == 1, "swap did not run");
	std::vector<u8> once(buf, buf + raw.size());
	CHECK(port_swap_j3d_model(buf) == 0, "second swap not a no-op");
	CHECK(std::memcmp(once.data(), buf, raw.size()) == 0, "second swap changed data");
	CHECK(((JUTDataFileHeader*)buf)->mMagic == 'J3D2', "magic");

	Blocks sec = walk(buf);
	if (sec.count('INF1')) {
		const J3DModelInfoBlock* b = (const J3DModelInfoBlock*)sec['INF1'];
		const u16* h = JSUConvertOffsetToPtr<u16>(b, b->mpHierarchy);
		long long cnt = 0, hs = 0;
		for (;; h += 2) {
			cnt++;
			hs += h[0] + h[1];
			if (h[0] == 0)
				break;
		}
		add("inf1"), addi(b->mFlags), addi(b->mPacketNum), addi(b->mVtxNum), addi(cnt), addi(hs);
		endl();
	}
	if (sec.count('VTX1')) {
		const J3DVertexBlock* b     = (const J3DVertexBlock*)sec['VTX1'];
		const GXVtxAttrFmtList* fl  = JSUConvertOffsetToPtr<GXVtxAttrFmtList>(b, b->mpVtxAttrFmtList);
		std::vector<const GXVtxAttrFmtList*> fmts;
		for (; fl->attr != GX_VA_NULL; fl++)
			fmts.push_back(fl);
		add("vtxfmt"), addi(fmts.size());
		for (auto* f : fmts)
			addi(f->attr), addi(f->cnt), addi(f->type), addi(f->frac);
		endl();
		u32 offs[13];
		offs[0] = b->mpVtxPosArray, offs[1] = b->mpVtxNrmArray, offs[2] = b->mpVtxNBTArray;
		offs[3] = b->mpVtxColorArray[0], offs[4] = b->mpVtxColorArray[1];
		for (int i = 0; i < 8; i++)
			offs[5 + i] = b->mpVtxTexCoordArray[i];
		for (auto* f : fmts) {
			int a = f->attr, slot;
			if (a == GX_VA_POS)
				slot = 0;
			else if (a == GX_VA_NRM)
				slot = 1;
			else if (a == GX_VA_CLR0 || a == GX_VA_CLR1)
				slot = 3 + (a - GX_VA_CLR0);
			else if (a >= GX_VA_TEX0 && a <= GX_VA_TEX7)
				slot = 5 + (a - GX_VA_TEX0);
			else
				continue; // NBT: the Python reference does not read it
			u32 so = offs[slot], end = tableEnd(offs, 13, so, b->mSize);
			const u8* p = (const u8*)b + so;
			add("arr"), addi(a);
			if (a == GX_VA_CLR0 || a == GX_VA_CLR1) {
				static const int esz[6] = { 2, 3, 4, 2, 3, 4 };
				int e = esz[f->type];
				long long n = (end - so) / e, s = 0;
				for (long long k = 0; k < n; k++, p += e) {
					if (f->type == GX_RGB565 || f->type == GX_RGBA4)
						s += *(const u16*)p;
					else if (f->type == GX_RGBA6)
						s += p[0] | (p[1] << 8) | (p[2] << 16); // native (LE) after the swap
					else
						for (int j = 0; j < e; j++)
							s += p[j];
				}
				addi(n), addi(s);
			} else {
				int ncomp = a == GX_VA_POS ? (f->cnt == 0 ? 2 : 3) : a == GX_VA_NRM ? 3 : (f->cnt == 0 ? 1 : 2);
				static const int csz[5] = { 1, 1, 2, 2, 4 };
				int cs = csz[f->type], stride = ncomp * cs;
				long long n = end - so >= (u32)stride ? (end - so) / stride : 0;
				double s = 0, div = (double)(1 << f->frac);
				for (long long k = 0; k < n * ncomp; k++, p += cs) {
					switch (f->type) {
					case GX_U8: s += p[0] / div; break;
					case GX_S8: s += (s8)p[0] / div; break;
					case GX_U16: s += *(const u16*)p / div; break;
					case GX_S16: s += *(const s16*)p / div; break;
					default: s += *(const f32*)p; break;
					}
				}
				addi(n), addf(s);
			}
			endl();
		}
	}
	if (sec.count('EVP1')) {
		const J3DEnvelopBlock* b = (const J3DEnvelopBlock*)sec['EVP1'];
		const u8* cnts           = JSUConvertOffsetToPtr<u8>(b, b->mpWEvlpMixMtxNum);
		const u16* idx           = JSUConvertOffsetToPtr<u16>(b, b->mpWEvlpMixMtxIndex);
		const f32* w             = JSUConvertOffsetToPtr<f32>(b, b->mpWEvlpMixWeight);
		const f32* inv           = (const f32*)JSUConvertOffsetToPtr<Mtx>(b, b->mpInvJointMtx);
		long long sc = 0, si = 0, k = 0;
		int maxj = -1;
		double sw = 0, sinv = 0;
		for (int i = 0; i < b->mWEvlpMtxNum; i++) {
			for (int j = 0; j < cnts[i]; j++) {
				si += idx[k + j];
				sw += w[k + j];
				if (idx[k + j] > maxj)
					maxj = idx[k + j];
			}
			sc += cnts[i];
			k += cnts[i];
		}
		for (int i = 0; i < (maxj + 1) * 12; i++)
			sinv += inv[i];
		add("evp1"), addi(b->mWEvlpMtxNum), addi(sc), addi(si), addf(sw), addi(maxj + 1), addf(sinv);
		endl();
	}
	if (sec.count('DRW1')) {
		const J3DDrawBlock* b = (const J3DDrawBlock*)sec['DRW1'];
		const u8* fl          = JSUConvertOffsetToPtr<u8>(b, b->mpDrawMtxFlag);
		const u16* ix         = JSUConvertOffsetToPtr<u16>(b, b->mpDrawMtxIndex);
		long long sf = 0, sx = 0;
		int full = -1;
		for (int i = 0; i < b->mMtxNum; i++) {
			sf += fl[i];
			sx += ix[i];
			if (full < 0 && fl[i] == 1)
				full = i;
		}
		add("drw1"), addi(b->mMtxNum), addi(sf), addi(sx), addi(full < 0 ? b->mMtxNum : full);
		endl();
	}
	if (sec.count('JNT1')) {
		const J3DJointBlock* b = (const J3DJointBlock*)sec['JNT1'];
		J3DJointFactory f(*b);
		add("jnt1"), addi(b->mJointNum);
		endl();
		for (int i = 0; i < b->mJointNum; i++) {
			const J3DTransformInfo& t = f.getTransformInfo(i);
			add("jnt"), addi(i), addi(f.getKind(i)), addi(f.getScaleCompensate(i));
			addf(t.mScale.x), addf(t.mScale.y), addf(t.mScale.z);
			addi(t.mRotation.x), addi(t.mRotation.y), addi(t.mRotation.z);
			addf(t.mTranslate.x), addf(t.mTranslate.y), addf(t.mTranslate.z), addf(f.getRadius(i));
			addf(f.getMin(i).x), addf(f.getMin(i).y), addf(f.getMin(i).z);
			addf(f.getMax(i).x), addf(f.getMax(i).y), addf(f.getMax(i).z);
			endl();
		}
	}
	if (sec.count('MAT3')) {
		const J3DMaterialBlock* b = (const J3DMaterialBlock*)sec['MAT3'];
		J3DMaterialFactory f(*b);
		const J3DMaterialInitData* init
		    = JSUConvertOffsetToPtr<J3DMaterialInitData>(b, b->mpMaterialInitData);
		JUTNameTab* names = b->mpNameTable != nullptr
		                        ? new JUTNameTab(JSUConvertOffsetToPtr<ResNTAB>(b, b->mpNameTable))
		                        : nullptr;
		add("mat3"), addi(b->mMaterialNum), addi(f.countUniqueMaterials());
		endl();
		for (int i = 0; i < b->mMaterialNum; i++) {
			const J3DMaterialInitData* d = &init[f.getMaterialID(i)];
			J3DGXColorS10 tc             = f.newTevColor(i, 0);
			J3DGXColor mc                = f.newMatColor(i, 0);
			add("mat"), addi(i), addName(names ? names->getName(i) : nullptr);
			addi(f.newTexNo(i, 0)), addi(f.newCullMode(i)), addi(f.newTevStageNum(i));
			addi(f.newColorChanNum(i)), addi(d->mTevKColorSel[0]);
			addi(tc.color.r), addi(tc.color.g), addi(tc.color.b), addi(tc.color.a);
			addi(mc.color.r), addi(mc.color.g), addi(mc.color.b), addi(mc.color.a);
			addi(f.newTexGenNum(i));
			if (d->mTexCoordIdx[0] != 0xFFFF) {
				const J3DTexCoordInfo& c
				    = JSUConvertOffsetToPtr<J3DTexCoordInfo>(b, b->mpTexCoordInfo)[d->mTexCoordIdx[0]];
				addi(c.mTexGenType), addi(c.mTexGenSrc), addi(c.mTexGenMtx);
			} else {
				addi(-1), addi(-1), addi(-1);
			}
			if (d->mTevOrderIdx[0] != 0xFFFF) {
				const J3DTevOrderInfo& c
				    = JSUConvertOffsetToPtr<J3DTevOrderInfo>(b, b->mpTevOrderInfo)[d->mTevOrderIdx[0]];
				addi(c.mTexCoord), addi(c.mTexMap), addi(c.mColorChan);
			} else {
				addi(-1), addi(-1), addi(-1);
			}
			endl();
			if (d->mTexMtxIdx[0] != 0xFFFF) {
				const J3DTexMtxInfo& m
				    = JSUConvertOffsetToPtr<J3DTexMtxInfo>(b, b->mpTexMtxInfo)[d->mTexMtxIdx[0]];
				add("texmtx"), addi(i), addi(m.mProjection), addi(m.mInfo);
				addf(m.mCenter.x), addf(m.mCenter.y), addf(m.mCenter.z);
				addf(m.mSRT.mScaleX), addf(m.mSRT.mScaleY), addi(m.mSRT.mRotation);
				addf(m.mSRT.mTranslationX), addf(m.mSRT.mTranslationY);
				addf(m.mEffectMtx[0][0]), addf(m.mEffectMtx[2][3]);
				endl();
			}
			if (d->mFogIdx != 0xFFFF) {
				const J3DFogInfo& g = JSUConvertOffsetToPtr<J3DFogInfo>(b, b->mpFogInfo)[d->mFogIdx];
				add("fog"), addi(i), addi(g.mType), addi(g.mAdjEnable), addi(g.mCenter);
				addf(g.mStartZ), addf(g.mEndZ), addf(g.mNearZ), addf(g.mFarZ);
				addi(g.mColor.r), addi(g.mColor.g), addi(g.mColor.b), addi(g.mColor.a);
				addi(g.mFogAdjTable[0]), addi(g.mFogAdjTable[9]);
				endl();
			}
			if (d->mNBTScaleIdx != 0xFFFF) {
				const J3DNBTScaleInfo& s
				    = JSUConvertOffsetToPtr<J3DNBTScaleInfo>(b, b->mpNBTScaleInfo)[d->mNBTScaleIdx];
				add("nbt"), addi(i), addi(s.mbHasScale);
				addf(s.mScale.x), addf(s.mScale.y), addf(s.mScale.z);
				endl();
			}
			if (d->mBlendIdx != 0xFFFF) {
				const J3DBlendInfo& s
				    = JSUConvertOffsetToPtr<J3DBlendInfo>(b, b->mpBlendInfo)[d->mBlendIdx];
				add("blend"), addi(i), addi(s.mBlendMode), addi(s.mSrcFactor), addi(s.mDstFactor),
				    addi(s.mLogicOp);
				endl();
			}
			u16 li = *(const u16*)d->field_0x018; // light index 0 (u16[8] in the file)
			if (li != 0xFFFF && b->mpLightInfo != nullptr) {
				const J3DLightInfo& s = JSUConvertOffsetToPtr<J3DLightInfo>(b, b->mpLightInfo)[li];
				add("light"), addi(i), addf(s.mLightPosition.x), addf(s.mLightDirection.z);
				addi(s.mColor.r), addi(s.mColor.g), addi(s.mColor.b), addi(s.mColor.a);
				addf(s.mDistAtten.z);
				endl();
			}
			if (f.mpIndInitData != nullptr) {
				const J3DIndInitData& s = f.mpIndInitData[i];
				add("ind"), addi(i), addi(s.mEnabled), addi(s.mIndTexStageNum);
				addf(s.mIndTexMtxInfo[0].mOffsetMtx[0][0]), addi(s.mIndTexMtxInfo[0].mScaleExp);
				addi(s.mIndTevStageInfo[0].mIndStage);
				endl();
			}
		}
	}
	if (sec.count('SHP1')) {
		const J3DShapeBlock* b = (const J3DShapeBlock*)sec['SHP1'];
		J3DShapeFactory f(*b);
		const J3DShapeInitData* init = JSUConvertOffsetToPtr<J3DShapeInitData>(b, b->mpShapeInitData);
		const u16* index             = JSUConvertOffsetToPtr<u16>(b, b->mpIndexTable);
		const J3DShapeMtxInitData* mi = JSUConvertOffsetToPtr<J3DShapeMtxInitData>(b, b->mpMtxInitData);
		const J3DShapeDrawInitData* di
		    = JSUConvertOffsetToPtr<J3DShapeDrawInitData>(b, b->mpDrawInitData);
		add("shp1"), addi(b->mShapeNum);
		endl();
		for (int i = 0; i < b->mShapeNum; i++) {
			const J3DShapeInitData& s = init[index[i]];
			add("shp"), addi(i), addi(s.mShapeMtxType), addi(f.getMtxGroupNum(i));
			addf(f.getRadius(i)), addf(f.getMin(i).x), addf(f.getMin(i).y), addf(f.getMin(i).z);
			addf(f.getMax(i).x), addf(f.getMax(i).y), addf(f.getMax(i).z);
			std::vector<int> at;
			for (const GXVtxDescList* v = f.getVtxDescList(i); v->attr != GX_VA_NULL; v++)
				at.push_back(v->attr), at.push_back(v->type);
			addi(at.size() / 2);
			for (int x : at)
				addi(x);
			endl();
			for (u32 k = 0; k < f.getMtxGroupNum(i); k++) {
				const J3DShapeMtxInitData& m  = mi[s.mMtxInitDataIndex + k];
				const J3DShapeDrawInitData& d = di[s.mDrawInitDataIndex + k];
				u32 dl                        = (u32)b->mpDisplayListData + d.mDisplayListIndex;
				add("pkt"), addi(i), addi(k), addi(m.mUseMtxIndex), addi(m.mUseMtxCount),
				    addi(m.mFirstUseMtxIndex), addi(d.mDisplayListSize), addi(dl),
				    addi(((const u8*)b)[dl]);
				endl();
			}
		}
		// display lists stay big-endian: the whole DL table is untouched
		u32 offs[8];
		for (int i = 0; i < 8; i++)
			offs[i] = ((const u32*)((const u8*)b + 0x0C))[i];
		u32 start = b->mpDisplayListData, end = tableEnd(offs, 8, start, b->mSize);
		size_t at = (const u8*)b - buf + start;
		CHECK(std::memcmp(buf + at, raw.data() + at, end - start) == 0, "SHP1 display lists changed");
	}
	if (sec.count('TEX1')) {
		const J3DTextureBlock* b = (const J3DTextureBlock*)sec['TEX1'];
		const ResTIMG* t         = JSUConvertOffsetToPtr<ResTIMG>(b, b->mpTextureRes);
		JUTNameTab* names        = b->mpNameTable != nullptr
		                               ? new JUTNameTab(JSUConvertOffsetToPtr<ResNTAB>(b, b->mpNameTable))
		                               : nullptr;
		add("tex1"), addi(b->mTextureNum);
		endl();
		for (int i = 0; i < b->mTextureNum; i++) {
			add("tex"), addi(i), addName(names ? names->getName(i) : nullptr), addi(t[i].format);
			addi(t[i].width), addi(t[i].height), addi(t[i].numColors), addi(t[i].mipmapCount);
			addi(t[i].imageDataOffset);
			endl();
			CHECK((u8)t[i].unk19[0] == PORT_TIMG_SWAPPED, "TIMG %d not marked", i);
			// texels after the last header stay GX order
		}
		size_t hdrEnd = (const u8*)b - buf + b->mpTextureRes + b->mTextureNum * 0x20;
		size_t blkEnd = (const u8*)b - buf + b->mSize;
		size_t nameAt = b->mpNameTable != nullptr ? (const u8*)b - buf + b->mpNameTable : blkEnd;
		size_t texEnd = nameAt > hdrEnd ? nameAt : blkEnd;
		if (b->mTextureNum != 0 && b->mpTextureRes != nullptr)
			CHECK(std::memcmp(buf + hdrEnd, raw.data() + hdrEnd, texEnd - hdrEnd) == 0,
			      "TEX1 texel data changed");
	}
	add("conflicts"), addi(conflicts & 0xFFFF);
	endl();
	gSoft += conflicts >> 16;
}

// ------------------------------------------------------------ animations
static int gBadKeys;

// Key data: count==1 -> one value; else count keys of (time, value, tangent)
// for type 0 or (time, value, in, out) for type 1.
template <typename T> static void checkKeys(const J3DAnmKeyTableBase& k, const T* data, int frameMax)
{
	if (k.mMaxFrame <= 1)
		return;
	if (k.mType > 1) {
		gBadKeys++;
		return;
	}
	int stride  = k.mType == 0 ? 3 : 4;
	double prev = -1e30;
	for (int i = 0; i < k.mMaxFrame; i++) {
		double t = (double)data[k.mOffset + i * stride];
		// (keys past frameMax do occur in shipped files, e.g. sandbomb_water.brk)
		if (t < prev || t < 0 || t > 0x7FFF) {
			gBadKeys++;
			return;
		}
		prev = t;
	}
}

static long long keysum(const J3DAnmKeyTableBase& k) { return k.mMaxFrame + k.mOffset + k.mType; }

static void anm(u8* buf, const std::vector<u8>& raw)
{
	int conflicts = 0;
	CHECK(port_swap_j3d_anm_ex(buf, &conflicts) == 1, "swap did not run");
	// Animations with no entries point all their (empty) tables at one
	// offset, so shared offsets are expected there; only count them.
	gSoft += conflicts >> 16;
	gAnmHard += (conflicts & 0xFFFF) != 0;
	std::vector<u8> once(buf, buf + raw.size());
	CHECK(port_swap_j3d_anm(buf) == 0, "second swap not a no-op");
	CHECK(std::memcmp(once.data(), buf, raw.size()) == 0, "second swap changed data");
	Blocks sec = walk(buf);
	J3DAnmBase* base = J3DAnmLoaderDataBase::load(buf); // real loader (swap is a no-op now)
	CHECK(base != nullptr, "J3DAnmLoaderDataBase::load failed");
	if (!base)
		return;
	gBadKeys = 0;
	if (sec.count('ANK1')) {
		const u8* h            = (const u8*)sec['ANK1'];
		J3DAnmTransformKey* a  = (J3DAnmTransformKey*)base;
		int nj = a->field_0x22, cs = *(const u16*)(h + 0xE), cr = *(const u16*)(h + 0x10),
		    ct = *(const u16*)(h + 0x12);
		long long ts = 0, rs = 0;
		double ss = 0, tt = 0;
		for (int i = 0; i < nj * 3; i++) {
			const J3DAnmTransformKeyTable& k = a->mAnmTable[i];
			ts += keysum(k.mScale) + keysum(k.mRotation) + keysum(k.mTranslate);
			checkKeys(k.mScale, a->mScaleData, a->mMaxFrame);
			checkKeys(k.mRotation, a->mRotData, a->mMaxFrame);
			checkKeys(k.mTranslate, a->mTransData, a->mMaxFrame);
		}
		for (int i = 0; i < cs; i++)
			ss += a->mScaleData[i];
		for (int i = 0; i < cr; i++)
			rs += a->mRotData[i];
		for (int i = 0; i < ct; i++)
			tt += a->mTransData[i];
		add("ank1"), addi(a->mAttribute), addi(a->mDecShift), addi(a->mMaxFrame), addi(nj), addi(cs),
		    addi(cr), addi(ct), addi(ts), addf(ss), addi(rs), addf(tt);
		endl();
	}
	if (sec.count('TTK1')) {
		J3DAnmTextureSRTKey* a = (J3DAnmTextureSRTKey*)base;
		int nm                 = a->mTrackNum / 3;
		long long ts = 0, ms = 0, is = 0, rs = 0;
		double cs = 0, ss = 0, tt = 0;
		for (int i = 0; i < a->mTrackNum; i++) {
			const J3DAnmTransformKeyTable& k = a->mAnmTable[i];
			ts += keysum(k.mScale) + keysum(k.mRotation) + keysum(k.mTranslate);
			checkKeys(k.mScale, a->mScaleData, a->mMaxFrame);
			checkKeys(k.mRotation, a->mRotData, a->mMaxFrame);
			checkKeys(k.mTranslate, a->mTransData, a->mMaxFrame);
		}
		for (int i = 0; i < nm; i++) {
			ms += a->mUpdateMaterialID[i];
			is += a->mUpdateTexMtxID[i];
			cs += a->mSRTCenter[i].x + a->mSRTCenter[i].y + a->mSRTCenter[i].z;
		}
		for (int i = 0; i < a->mScaleNum; i++)
			ss += a->mScaleData[i];
		for (int i = 0; i < a->mRotNum; i++)
			rs += a->mRotData[i];
		for (int i = 0; i < a->mTransNum; i++)
			tt += a->mTransData[i];
		add("ttk1"), addi(a->mAttribute), addi(a->mDecShift), addi(a->mMaxFrame), addi(a->mTrackNum),
		    addi(a->mScaleNum), addi(a->mRotNum), addi(a->mTransNum), addi(ts), addi(ms), addi(is),
		    addf(cs), addf(ss), addi(rs), addf(tt), addi(a->mPostTrackNum);
		addName(nm ? a->mUpdateMaterialName->getName(0) : nullptr);
		endl();
	}
	if (sec.count('TRK1')) {
		J3DAnmTevRegKey* a = (J3DAnmTevRegKey*)base;
		u16 cnt[8]          = { a->mCRegDataCountR, a->mCRegDataCountG, a->mCRegDataCountB,
			                    a->mCRegDataCountA, a->mKRegDataCountR, a->mKRegDataCountG,
			                    a->mKRegDataCountB, a->mKRegDataCountA };
		s16* vals[8]        = { a->mAnmCRegDataR, a->mAnmCRegDataG, a->mAnmCRegDataB, a->mAnmCRegDataA,
			                    a->mAnmKRegDataR, a->mAnmKRegDataG, a->mAnmKRegDataB, a->mAnmKRegDataA };
		long long cts = 0, kts = 0, cms = 0, kms = 0;
		for (int i = 0; i < a->mCRegUpdateMaterialNum; i++) {
			const J3DAnmCRegKeyTable& t = a->mAnmCRegKeyTable[i];
			cts += keysum(t.mRTable) + keysum(t.mGTable) + keysum(t.mBTable) + keysum(t.mATable)
			       + t.mColorId;
			cms += a->mCRegUpdateMaterialID[i];
			checkKeys(t.mRTable, vals[0], a->mMaxFrame), checkKeys(t.mGTable, vals[1], a->mMaxFrame);
			checkKeys(t.mBTable, vals[2], a->mMaxFrame), checkKeys(t.mATable, vals[3], a->mMaxFrame);
		}
		for (int i = 0; i < a->mKRegUpdateMaterialNum; i++) {
			const J3DAnmKRegKeyTable& t = a->mAnmKRegKeyTable[i];
			kts += keysum(t.mRTable) + keysum(t.mGTable) + keysum(t.mBTable) + keysum(t.mATable)
			       + t.mColorId;
			kms += a->mKRegUpdateMaterialID[i];
			checkKeys(t.mRTable, vals[4], a->mMaxFrame), checkKeys(t.mGTable, vals[5], a->mMaxFrame);
			checkKeys(t.mBTable, vals[6], a->mMaxFrame), checkKeys(t.mATable, vals[7], a->mMaxFrame);
		}
		add("trk1"), addi(a->mAttribute), addi(a->mMaxFrame), addi(a->mCRegUpdateMaterialNum),
		    addi(a->mKRegUpdateMaterialNum);
		for (int j = 0; j < 8; j++)
			addi(cnt[j]);
		addi(cts), addi(kts), addi(cms), addi(kms);
		for (int j = 0; j < 8; j++) {
			long long s = 0;
			for (int k = 0; k < cnt[j]; k++)
				s += vals[j][k];
			addi(s);
		}
		addName(a->mCRegUpdateMaterialNum ? a->mCRegUpdateMaterialName->getName(0) : nullptr);
		addName(a->mKRegUpdateMaterialNum ? a->mKRegUpdateMaterialName->getName(0) : nullptr);
		endl();
	}
	if (sec.count('PAK1')) {
		J3DAnmColorKey* a = (J3DAnmColorKey*)base;
		u16 cnt[4]        = { a->unk10, a->unk12, a->unk14, a->unk16 };
		s16* vals[4]      = { a->mColorR, a->mColorG, a->mColorB, a->mColorA };
		long long ts = 0, ms = 0;
		for (int i = 0; i < a->mUpdateMaterialNum; i++) {
			for (int c = 0; c < 4; c++) {
				ts += keysum(a->mTable[i].mColorInfo[c]);
				checkKeys(a->mTable[i].mColorInfo[c], vals[c], a->mMaxFrame);
			}
			ms += a->mUpdateMaterialID[i];
		}
		add("pak1"), addi(a->mAttribute), addi(a->mMaxFrame), addi(a->mUpdateMaterialNum);
		for (int c = 0; c < 4; c++)
			addi(cnt[c]);
		addi(ts), addi(ms);
		for (int c = 0; c < 4; c++) {
			long long s = 0;
			for (int k = 0; k < cnt[c]; k++)
				s += vals[c][k];
			addi(s);
		}
		addName(a->mUpdateMaterialNum ? a->mUpdateMaterialName->getName(0) : nullptr);
		endl();
	}
	if (sec.count('TPT1')) {
		J3DAnmTexPattern* a = (J3DAnmTexPattern*)base;
		long long ts = 0, ns = 0, vs = 0, ms = 0;
		int nvals = 0;
		for (int i = 0; i < a->mUpdateMaterialNum; i++) {
			const J3DAnmTexPatternFullTable& t = a->mAnmTable[i];
			ts += t.mMaxFrame + t.mOffset + t._6;
			ns += t.mTexNo;
			if (t.mMaxFrame + t.mOffset > nvals)
				nvals = t.mMaxFrame + t.mOffset;
			ms += a->mUpdateMaterialID[i];
		}
		for (int i = 0; i < nvals; i++)
			vs += a->mTextureIndex[i];
		add("tpt1"), addi(a->mAttribute), addi(a->mMaxFrame), addi(a->mUpdateMaterialNum),
		    addi(a->field_0x18), addi(ts), addi(ns), addi(nvals), addi(vs), addi(ms);
		addName(a->mUpdateMaterialNum ? a->mUpdateMaterialName->getName(0) : nullptr);
		endl();
	}
	add("badkeys"), addi(gBadKeys);
	endl();
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		std::printf("usage: test_j3d <files.txt> <data root>\n");
		return 2;
	}
	FILE* lf = std::fopen(argv[1], "rb");
	if (!lf)
		return 2;
	std::filesystem::path root(argv[2]);
	char line[1024];
	int nModels = 0, nAnms = 0;
	while (std::fgets(line, sizeof line, lf)) {
		size_t n = std::strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = 0;
		if (n < 3)
			continue;
		std::filesystem::path p(line + 2);
		gFile = std::filesystem::relative(p, root).generic_string();
		FILE* f = std::fopen(p.string().c_str(), "rb");
		if (!f) {
			CHECK(false, "cannot open");
			continue;
		}
		std::vector<u8> raw;
		u8 tmp[65536];
		size_t got;
		while ((got = std::fread(tmp, 1, sizeof tmp, f)) > 0)
			raw.insert(raw.end(), tmp, tmp + got);
		std::fclose(f);
		u8* buf = new u8[raw.size() + 64](); // the loaders keep pointers into it
		std::memcpy(buf, raw.data(), raw.size());
		std::printf("== %s\n", gFile.c_str());
		if (raw.size() < 0x20) { // common/camera/dummy*.bck are empty files
			std::printf("skip\n");
			continue;
		}
		if (line[0] == 'M') {
			model(buf, raw);
			nModels++;
		} else {
			anm(buf, raw);
			nAnms++;
		}
		// buffers are leaked on purpose: anim objects point into them
	}
	std::fclose(lf);
	std::printf("SUMMARY %d models, %d animations, %d failure(s); shared offsets: %d bytes-vs-table, "
	            "%d animation file(s) with differing table kinds\n",
	            nModels, nAnms, gFailures, gSoft, gAnmHard);
	return gFailures ? 1 : 0;
}
