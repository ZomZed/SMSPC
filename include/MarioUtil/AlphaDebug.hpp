#ifndef MARIOUTIL_ALPHA_DEBUG_HPP
#define MARIOUTIL_ALPHA_DEBUG_HPP

// Diagnostic only (all-source PC-port bring-up): dumps J3D material state and
// the material packet display list of one model via OSReport, prefix "AD>".
// Enabled with SMS_EXTRA_DEFINES="SMS_ALPHA_DEBUG". Not part of the game.
#ifdef SMS_ALPHA_DEBUG

#include <dolphin/os.h>
#include <string.h>
#include <stdio.h>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>

static u32 AD_rd32(const u8* p)
{
	return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | (u32)p[3];
}

static u32 AD_rd16(const u8* p) { return (u32)p[0] << 8 | (u32)p[1]; }

static bool AD_ptrOk(const void* p)
{
	u32 v = (u32)p;
	return v >= 0x80000000 && v < 0x81800000;
}

static void AD_hex(const char* tag, const void* ptr, u32 size)
{
	if (!AD_ptrOk(ptr)) {
		OSReport("AD>   %s: <bad ptr %08x>\n", tag, (u32)ptr);
		return;
	}
	const u8* p = (const u8*)ptr;
	for (u32 i = 0; i < size; i += 16) {
		char buf[16 * 3 + 1];
		int n = 0;
		for (u32 j = 0; j < 16 && i + j < size; ++j) {
			static const char hx[] = "0123456789abcdef";
			buf[n++] = hx[p[i + j] >> 4];
			buf[n++] = hx[p[i + j] & 15];
			buf[n++] = ((j & 3) == 3) ? ' ' : '.';
		}
		buf[n] = 0;
		OSReport("AD>   %s+%02x: %s\n", tag, i, buf);
	}
}

// Walk a GX display list and print every BP / XF / CP write.
// BP "B<reg>=<24-bit value>", XF "X<addr>=<value>", CP "C<reg>=<value>".
static void AD_parseDL(const u8* dl, u32 size)
{
	if (!AD_ptrOk(dl)) {
		OSReport("AD>   DL <bad ptr %08x>\n", (u32)dl);
		return;
	}
	if (size > 0x800)
		size = 0x800;
	char line[200];
	int len    = 0;
	int nOnLine = 0;
	u32 p      = 0;
	while (p < size) {
		u8 op = dl[p];
		char item[48];
		item[0] = 0;
		if (op == 0x00) {
			p += 1;
			continue;
		} else if (op == 0x61) {
			if (p + 5 > size)
				break;
			u32 v = AD_rd32(dl + p + 1);
			sprintf(item, "B%02x=%06x", v >> 24, v & 0xFFFFFF);
			p += 5;
		} else if (op == 0x10) {
			if (p + 5 > size)
				break;
			u32 n    = AD_rd16(dl + p + 1) + 1;
			u32 addr = AD_rd16(dl + p + 3);
			p += 5;
			for (u32 i = 0; i < n && p + 4 <= size; ++i, p += 4) {
				sprintf(item, "X%04x=%08x", addr + i, AD_rd32(dl + p));
				if (len + (int)strlen(item) + 2 >= (int)sizeof(line)
				    || nOnLine >= 6) {
					OSReport("AD>   DL %s\n", line);
					len     = 0;
					nOnLine = 0;
				}
				len += sprintf(line + len, "%s ", item);
				nOnLine++;
			}
			continue;
		} else if (op == 0x08) {
			if (p + 6 > size)
				break;
			sprintf(item, "C%02x=%08x", dl[p + 1], AD_rd32(dl + p + 2));
			p += 6;
		} else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
			sprintf(item, "IDX%02x", op);
			p += 5;
		} else if (op == 0x40) {
			sprintf(item, "CALL%08x", AD_rd32(dl + p + 1));
			p += 9;
		} else if (op == 0x48) {
			sprintf(item, "INVVC");
			p += 1;
		} else {
			sprintf(item, "?op%02x@%x", op, p);
			if (len + (int)strlen(item) + 2 < (int)sizeof(line))
				len += sprintf(line + len, "%s ", item);
			break;
		}
		if (len + (int)strlen(item) + 2 >= (int)sizeof(line) || nOnLine >= 6) {
			OSReport("AD>   DL %s\n", line);
			len     = 0;
			nOnLine = 0;
		}
		len += sprintf(line + len, "%s ", item);
		nOnLine++;
	}
	if (len)
		OSReport("AD>   DL %s\n", line);
	OSReport("AD>   DL end p=%x size=%x\n", p, size);
}

static void AD_dumpMaterial(const char* tag, J3DModel* model, u16 idx, int full)
{
	if (model == nullptr || !AD_ptrOk(model)) {
		OSReport("AD>%s model=null\n", tag);
		return;
	}
	J3DModelData* data = model->getModelData();
	if (data == nullptr || idx >= data->getMaterialNum()) {
		OSReport("AD>%s bad model data / idx %d\n", tag, idx);
		return;
	}
	J3DMaterial* mat = data->getMaterialNodePointer(idx);
	const char* name = "?";
	if (data->getMaterialName() != nullptr)
		name = data->getMaterialName()->getName(idx);
	u8* rm = (u8*)mat;
	OSReport("AD>%s mat#%d '%s' mat=%08x mode(+08)=%08x invalid(+10)=%08x "
	         "id(+18)=%08x f1C=%08x anm(+38)=%08x ownDL(+3C)=%08x\n",
	         tag, idx, name, (u32)mat, *(u32*)(rm + 0x8), *(u32*)(rm + 0x10),
	         *(u32*)(rm + 0x18), *(u32*)(rm + 0x1C), *(u32*)(rm + 0x38),
	         *(u32*)(rm + 0x3C));

	// colour block
	J3DColorBlock* cb = mat->getColorBlock();
	if (AD_ptrOk(cb)) {
		u32 t = cb->getType();
		u8* m0 = (u8*)cb->getMatColor(0);
		u8* m1 = (u8*)cb->getMatColor(1);
		u8* a0 = (u8*)cb->getAmbColor(0);
		u8* a1 = (u8*)cb->getAmbColor(1);
		OSReport("AD>%s  CB type=%c%c%c%c chanNum=%d cull=%d", tag, t >> 24,
		         (t >> 16) & 0xFF, (t >> 8) & 0xFF, t & 0xFF,
		         cb->getColorChanNum(), cb->getCullMode());
		if (AD_ptrOk(m0))
			OSReport(" mat0=%02x%02x%02x%02x", m0[0], m0[1], m0[2], m0[3]);
		if (AD_ptrOk(m1))
			OSReport(" mat1=%02x%02x%02x%02x", m1[0], m1[1], m1[2], m1[3]);
		if (AD_ptrOk(a0))
			OSReport(" amb0=%02x%02x%02x%02x", a0[0], a0[1], a0[2], a0[3]);
		else
			OSReport(" amb0=none");
		if (AD_ptrOk(a1))
			OSReport(" amb1=%02x%02x%02x%02x", a1[0], a1[1], a1[2], a1[3]);
		OSReport("\n");
		OSReport("AD>%s  CB chanCtrl[C0,A0,C1,A1]=", tag);
		for (u32 i = 0; i < 4; ++i) {
			J3DColorChan* ch = cb->getColorChan(i);
			if (AD_ptrOk(ch)) {
				OSReport(" %04x(en%d mat%d amb%d lm%02x df%d at%d)",
				         ch->mChanCtrl, ch->getEnable(), ch->getMatSrc(),
				         ch->getAmbSrc(), ch->getLightMask(), ch->getDiffuseFn(),
				         ch->getAttnFn());
			} else {
				OSReport(" null");
			}
		}
		OSReport("\n");
		if (full)
			AD_hex("CBraw", cb, 0x48);
	} else {
		OSReport("AD>%s  CB bad %08x\n", tag, (u32)cb);
	}

	// PE block
	J3DPEBlock* pe = mat->getPEBlock();
	if (AD_ptrOk(pe)) {
		u32 t = pe->getType();
		OSReport("AD>%s  PE type=%c%c%c%c zCompLoc=%d dither=%d", tag, t >> 24,
		         (t >> 16) & 0xFF, (t >> 8) & 0xFF, t & 0xFF, pe->getZCompLoc(),
		         pe->getDither());
		u8* bl = (u8*)pe->getBlend();
		u8* ac = (u8*)pe->getAlphaComp();
		u8* zm = (u8*)pe->getZMode();
		if (AD_ptrOk(bl))
			OSReport(" blend(type,src,dst,op)=%d,%d,%d,%d", bl[0], bl[1], bl[2],
			         bl[3]);
		else
			OSReport(" blend=none(fixed by block type)");
		if (AD_ptrOk(ac))
			OSReport(" acomp(raw)=%02x%02x%02x%02x", ac[0], ac[1], ac[2], ac[3]);
		if (AD_ptrOk(zm))
			OSReport(" zmode(raw)=%02x%02x", zm[0], zm[1]);
		OSReport("\n");
		if (full)
			AD_hex("PEraw", pe, 0x14);
	} else {
		OSReport("AD>%s  PE bad %08x\n", tag, (u32)pe);
	}

	// TEV block
	J3DTevBlock* tb = mat->getTevBlock();
	if (AD_ptrOk(tb)) {
		OSReport("AD>%s  TEV stages=%d", tag, tb->getTevStageNum());
		for (u32 i = 0; i < 4; ++i) {
			s16* c = (s16*)tb->getTevColor(i);
			if (AD_ptrOk(c))
				OSReport(" c%d=%d,%d,%d,%d", i, c[0], c[1], c[2], c[3]);
		}
		for (u32 i = 0; i < 4; ++i) {
			u8* k = (u8*)tb->getTevKColor(i);
			if (AD_ptrOk(k))
				OSReport(" k%d=%02x%02x%02x%02x", i, k[0], k[1], k[2], k[3]);
		}
		OSReport("\n");
	}

	// material packet (what J3DMaterial::load() actually sends via callDL)
	J3DMatPacket* mp = model->getMatPacket(idx);
	if (AD_ptrOk(mp)) {
		u8* rp = (u8*)mp;
		J3DDisplayListObj* dlo = mp->getDisplayListObj();
		OSReport("AD>%s  MATPKT=%08x next=%08x child=%08x flags(+10)=%08x "
		         "locked=%d dlo=%08x shapePkt=%08x mat=%08x id=%08x tex=%08x\n",
		         tag, (u32)mp, *(u32*)(rp + 4), *(u32*)(rp + 8),
		         *(u32*)(rp + 0x10), mp->isLocked(), (u32)dlo,
		         *(u32*)(rp + 0x34), *(u32*)(rp + 0x38), *(u32*)(rp + 0x3C),
		         *(u32*)(rp + 0x40));
		if (AD_ptrOk(dlo)) {
			OSReport("AD>%s  DLO buf0=%08x buf1=%08x size=%x cap=%x\n", tag,
			         (u32)dlo->mpData[0], (u32)dlo->mpData[1], dlo->mSize,
			         dlo->mCapacity);
			if (full) {
				AD_hex("DL0raw", dlo->mpData[0], 64);
				AD_parseDL((const u8*)dlo->mpData[0], dlo->mSize);
				if (dlo->mpData[1] != dlo->mpData[0]
				    && AD_ptrOk(dlo->mpData[1])) {
					OSReport("AD>%s  (buffer 1:)\n", tag);
					AD_parseDL((const u8*)dlo->mpData[1], dlo->mSize);
				}
			}
		}
	}

	// shared DL owned by the material (J3DMaterial+0x3C), if any
	J3DDisplayListObj* own = *(J3DDisplayListObj**)(rm + 0x3C);
	if (full && AD_ptrOk(own) && own != (AD_ptrOk(mp) ? mp->getDisplayListObj() : 0)) {
		OSReport("AD>%s  OWNDLO buf0=%08x buf1=%08x size=%x\n", tag,
		         (u32)own->mpData[0], (u32)own->mpData[1], own->mSize);
		AD_parseDL((const u8*)own->mpData[0], own->mSize);
	}

	// shape packet (callbacks patch TEV/mat colours here at draw time)
	J3DShape* shape = mat->getShape();
	if (AD_ptrOk(shape)) {
		J3DShapePacket* sp = model->getShapePacket(shape->getIndex());
		if (AD_ptrOk(sp)) {
			u8* rs = (u8*)sp;
			u32 ua = *(u32*)(rs + 0xC);
			OSReport("AD>%s  SHAPEPKT#%d=%08x next=%08x user=%08x cb=%08x "
			         "visible=%d\n",
			         tag, shape->getIndex(), (u32)sp, *(u32*)(rs + 4), ua,
			         *(u32*)(rs + 0x10), sp->isVisible());
			if (AD_ptrOk((void*)ua)) {
				u32* u = (u32*)ua;
				OSReport("AD>%s  USER %08x %08x %08x %08x", tag, u[0], u[1],
				         u[2], u[3]);
				if (u[0] == 0 || u[0] == 1 || u[0] == 6) {
					u8* c = (u8*)u[2];
					if (AD_ptrOk(c))
						OSReport(" -> *col=%02x%02x%02x%02x%02x%02x%02x%02x",
						         c[0], c[1], c[2], c[3], c[4], c[5], c[6],
						         c[7]);
				}
				OSReport("\n");
			}
		}
	}
}

// rate limiter: first 3 calls, then every 120th
static bool AD_shouldPrint(int n) { return n < 3 || (n % 120) == 0; }

#endif // SMS_ALPHA_DEBUG
#endif
