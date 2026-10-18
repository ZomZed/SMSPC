#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include <dolphin/os.h>
#include <dolphin/gd.h>
#include <dolphin/gx.h>
#include <macros.h>
#ifdef SMS_ENTRY_TRACE
struct SmsEntryRec {
	const void* packet;
	const void* buffer;
	u32 lr[5];
};
extern SmsEntryRec gSmsEntryRecs[1024];
#endif
#ifdef SMS_A0_PROBE
#include <JSystem/JUtility/JUTGamePad.hpp>
#include <dolphin/pad.h>
extern "C" u8* gx; // GX shadow state (dolphin/src/gx/__gx.h)
extern "C" u32 gSmsDirtyLR;
#define gx_probe gx
#endif

int J3DDrawPacket::sInterruptFlag;

void J3DDisplayListObj::newDisplayList(u32 param_1)
{
	mCapacity = ALIGN_NEXT(param_1, 0x20);
	mpData[0] = new (0x20) char[mCapacity];
	mpData[1] = new (0x20) char[mCapacity];
	mSize     = 0;
}

void J3DDisplayListObj::swapBuffer()
{
	void* tmp = mpData[0];
	mpData[0] = mpData[1];
	mpData[1] = tmp;
}

void J3DDisplayListObj::callDL() { GXCallDisplayList(mpData[0], mSize); }

bool J3DPacket::isSame(J3DMatPacket*) const { return false; }

bool J3DPacket::entry(J3DDrawBuffer*) { return true; }

void J3DPacket::addChildPacket(J3DPacket* packet)
{
	if (mpFirstChild == nullptr) {
		mpFirstChild = packet;
		return;
	}
	packet->mpNext = mpFirstChild;
	mpFirstChild   = packet;
}

void J3DCallBackPacket::draw()
{
	if (mpCallBack != nullptr)
		mpCallBack(this, 0);

	for (J3DPacket* packet = mpFirstChild; packet != nullptr;
	     packet            = packet->getNextPacket())
        packet->draw();

	if (mpCallBack != nullptr)
		mpCallBack(this, 1);
}

J3DDrawPacket::J3DDrawPacket()
{
	mFlags           = 0;
	mpDisplayListObj = nullptr;
}

J3DDrawPacket::~J3DDrawPacket() { }

void J3DDrawPacket::draw()
{
	GXCallDisplayList(mpDisplayListObj->mpData[0], mpDisplayListObj->mSize);
}

void J3DDrawPacket::beginDL()
{
	mpDisplayListObj->swapBuffer();
	sInterruptFlag = OSDisableInterrupts();
	GDInitGDLObj(&mGDList, mpDisplayListObj->mpData[0],
	             mpDisplayListObj->mCapacity);
	GDSetCurrent(&mGDList);
}

u32 J3DDrawPacket::endDL()
{
	GDPadCurr32();
	OSRestoreInterrupts(sInterruptFlag);
	mpDisplayListObj->mSize = mGDList.ptr - mGDList.start;
	GDFlushCurrToMem();
	GDSetCurrent(nullptr);
	return mpDisplayListObj->mSize;
}

void J3DDrawPacket::beginPatch() { beginDL(); }

u32 J3DDrawPacket::endPatch()
{
	OSRestoreInterrupts(sInterruptFlag);
	GDSetCurrent(nullptr);
	return mpDisplayListObj->mSize;
}

J3DMatPacket::J3DMatPacket()
{
	mpMaterial = 0;
	unk3C      = (uintptr_t)-1;
	mTexture   = 0;
	unk44      = 0;
}

J3DMatPacket::~J3DMatPacket() { }

void J3DMatPacket::addShapePacket(J3DShapePacket* packet)
{
	if (mpShapePacket == nullptr) {
		mpShapePacket = packet;
		return;
	}
	packet->setNextPacket(mpShapePacket);
	mpShapePacket = packet;
}

bool J3DMatPacket::isHideAllShapePacket_()
{
	bool ret = true;

	J3DShapePacket* packet = getShapePacket();

	while (packet != nullptr) {
		if (packet->isVisible()) {
			ret = false;
			break;
		}
		packet = (J3DShapePacket*)packet->getNextPacket();
	}

	return ret;
}

void J3DMatPacket::draw()
{
	if (isHideAllShapePacket_())
		return;

	j3dSys.setTexture(mTexture);
	j3dSys.setMatPacket(this);
#ifdef SMS_ENTRY_TRACE
	// port diagnostic: blended material drawn while material loading is
	// switched off (j3dSys flag 2) -> report who entered it, and where.
	if (j3dSys.checkFlag2()) {
		J3DBlend* blend = mpMaterial->getPEBlock()->getBlend();
		if (blend != nullptr && blend->mBlendMode != 0) {
			static const void* sSeen[64];
			static int sSeenNum;
			bool seen = false;
			for (int i = 0; i < sSeenNum; i++)
				if (sSeen[i] == this)
					seen = true;
			if (!seen && sSeenNum < 64) {
				sSeen[sSeenNum++] = this;
				const SmsEntryRec* rec = nullptr;
				for (int i = 0; i < 1024; i++)
					if (gSmsEntryRecs[i].packet == this)
						rec = &gSmsEntryRecs[i];
				if (rec != nullptr)
					OSReport("ENT matpkt=%p mat=%p mode=%d buf=%p by %08x %08x "
					         "%08x %08x %08x\n",
					         this, mpMaterial, mpMaterial->isDrawModeOpaTexEdge(),
					         rec->buffer, rec->lr[0], rec->lr[1], rec->lr[2],
					         rec->lr[3], rec->lr[4]);
				else
					OSReport("ENT matpkt=%p mat=%p mode=%d (no entry record)\n",
					         this, mpMaterial, mpMaterial->isDrawModeOpaTexEdge());
			}
		}
	}
#endif
	mpMaterial->load();

	J3DShapePacket* packet = getShapePacket();

	while (packet != nullptr) {
		packet->draw();
		packet = (J3DShapePacket*)packet->getNextPacket();
	}
}

J3DShapePacket::J3DShapePacket()
{
	mpShape           = nullptr;
	mDrawMatrices     = nullptr;
	mNormMatrices     = nullptr;
	mpCurrentViewNo   = &j3dDefaultViewNo;
	mpVertexPositions = nullptr;
	mpVertexNormals   = nullptr;
	mpVertexColors    = nullptr;
	mVisible          = true;
}

J3DShapePacket::~J3DShapePacket() { }

void J3DShapePacket::draw()
{
	if (mpShape != nullptr && mVisible) {
		if (mpCallBack != nullptr)
			mpCallBack(this, 0);

		j3dSys.setVtxPos(mpVertexPositions);
		j3dSys.setVtxNrm(mpVertexNormals);
		j3dSys.setVtxCol(mpVertexColors);
		mpShape->setDrawMtx(mDrawMatrices);
		mpShape->setNrmMtx(mNormMatrices);
		mpShape->setCurrentViewNoPtr(mpCurrentViewNo);

#ifdef SMS_A0_PROBE
		// port diagnostic: GX keeps "dirty" shadow state that it flushes at
		// the next GXCallDisplayList, which would overwrite what the material
		// display list just set. Log it, and let the D-pad change it.
		{
			u32* dirty  = (u32*)(gx_probe + 0x4F4);
			u32 genMode = *(u32*)(gx_probe + 0x204);
			static int sLogged;
			if (*dirty != 0 && sLogged < 60) {
				OSReport("A0P dirty=%x genMode=%x ncol=%d ntev=%d cull=%d mat=%p "
				         "dirtiedBy=%08x\n",
				         *dirty, genMode, (genMode >> 4) & 7,
				         ((genMode >> 10) & 0xF) + 1, (genMode >> 14) & 3,
				         j3dSys.getMatPacket(), gSmsDirtyLR);
				{
					J3DMatPacket* mp = j3dSys.getMatPacket();
					J3DMaterial* m   = mp->getMaterial();
					J3DBlend* bl     = m->getPEBlock()->getBlend();
					int shapes       = 0;
					for (J3DPacket* p = mp->getShapePacket(); p != nullptr;
					     p            = p->getNextPacket())
						++shapes;
					OSReport("A0P   mat=%p opa=%d stages=%d blend=%d,%d,%d "
					         "k0=%08x shapes=%d flag2=%d thisShape=%p\n",
					         m, m->isDrawModeOpaTexEdge(),
					         m->getTevBlock()->getTevStageNum(),
					         bl ? bl->mBlendMode : -1, bl ? bl->mSrcFactor : -1,
					         bl ? bl->mDstFactor : -1,
					         *(u32*)m->getTevBlock()->getTevKColor(0), shapes,
					         j3dSys.checkFlag2(), this);
				}
				++sLogged;
			}
			u16 btn = JUTGamePad::mPadStatus[0].button;
			static u16 sLastBtn;
			if (btn != sLastBtn) {
				OSReport("A0P buttons=%04x\n", btn);
				sLastBtn = btn;
			}
#ifdef SMS_PROBE_ALWAYS_NODIRTY
			btn |= PAD_BUTTON_DOWN;
#endif
#ifdef SMS_PROBE_ALWAYS_RELOAD
			btn |= PAD_BUTTON_LEFT;
#endif
#ifdef SMS_PROBE_ALWAYS_XF
			{
				GXCmd1u8(GX_CMD_LOAD_XF_REG);
				GXCmd1u16(1);
				GXCmd1u16(0x100C);
				GXCmd1u32(0xFFFFFFFF);
				GXCmd1u32(0xFFFFFFFF);
				GXCmd1u8(GX_CMD_LOAD_XF_REG);
				GXCmd1u16(1);
				GXCmd1u16(0x1010);
				GXCmd1u32(0x0700);
				GXCmd1u32(0x0400);
			}
#endif
			if (btn & PAD_BUTTON_UP)
				*dirty &= ~4; // drop only the pending genMode rewrite
			if (btn & PAD_BUTTON_DOWN)
				*dirty = 0; // drop every pending rewrite
			if (btn & (PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT)) {
				if (btn & PAD_BUTTON_RIGHT)
					*dirty = 0;
				// re-apply the material right before the shape
				j3dSys.getMatPacket()->getDisplayListObj()->callDL();
			}
		}
#endif

		mpShape->draw();

		if (mpCallBack != nullptr)
			mpCallBack(this, 1);
	}
}
