#include <GC2D/ConsoleStr.hpp>
#include <version.h> // SMS_COMMON_2D_DIR
#ifdef SMS_PORT
#include <port_widescreen.h> // w11: widescreen (Hor+)
#endif
#include <GC2D/BoundPane.hpp>
#include <GC2D/ExPane.hpp>
#include <GC2D/MessageUtil.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>
#include <System/StageUtil.hpp>
#include <System/FlagManager.hpp>
#include <JSystem/J2D/J2DPicture.hpp>
#include <JSystem/J2D/J2DTextBox.hpp>
#include <JSystem/J2D/J2DScreen.hpp>
#include <JSystem/J2D/J2DOrthoGraph.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JParticle/JPAEmitterManager.hpp>
#include <JSystem/JParticle/JPAEmitter.hpp>
#include <JSystem/JUtility/JUTResFont.hpp>
#include <dolphin/gx/GXCull.h>
#include <macros.h>
#include <stdio.h>

extern JPAEmitterManager* gpEmitterManager4D2;

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
// PAL: the letter counts depend on the console language (load()), and the pane
// names have two digits ('go00'..'go10').
#define CONSOLESTR_GO_NUM mGoNum
#define CONSOLESTR_SG_NUM mShineGetNum
#define CONSOLESTR_MS_NUM mMissNum
#define CONSOLESTR_PANE_ID(base, i) ((base) + ((i) / 10 << 8) + (i) % 10)
#else
#define CONSOLESTR_GO_NUM ARRAY_COUNT(unk28)
#define CONSOLESTR_SG_NUM ARRAY_COUNT(unk244)
#define CONSOLESTR_MS_NUM ARRAY_COUNT(unk268)
#define CONSOLESTR_PANE_ID(base, i) ((base) + (i))
#endif

static const char* dummyMactorStringValue1 = "\0\0\0\0\0\0\0\0\0\0\0";
static const char* SMS_NO_MEMORY_MESSAGE   = "メモリが足りません\n";

JUTPoint TConsoleStr::cShineGetRight1(150, -50);
JUTPoint TConsoleStr::cShineGetLeft1(-21, 7);
JUTPoint TConsoleStr::cShineGetRight2(0, 0);
JUTPoint TConsoleStr::cShineGetLeft2(-21, 7);
JUTPoint TConsoleStr::cShineGetRight3(0, 0);
JUTPoint TConsoleStr::cShineGetLeft3(-200, 65);

TConsoleStr::TConsoleStr(const char* name)
    : JDrama::TViewObj(name)
    , unk10(nullptr)
    , unk14(nullptr)
    , unk20(0xB4)
    , unk24(0)
    , unk2A8(0)
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
    , unk2A9(0)
#endif
    , unk2B8(0)
    , unk2BC(STATE_UNK7)
{
}

f32 TConsoleStr::getWipeCloseTime() { return 30.0f / SMSGetVSyncTimesPerSec(); }

void TConsoleStr::load(JSUMemoryInputStream& stream)
{
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
	// Letters of "GO!", "SHINE GET!" and "TOO BAD!" in big_tx_1.blo for each
	// language (English, German, French, Spanish, Italian).
	mGoNum       = 11;
	mShineGetNum = 11;
	mMissNum     = 12;
	switch (TFlagManager::getInstance()->getFlag(MSF_LANGUAGE)) {
	case 0:
		mGoNum       = 3;
		mShineGetNum = 6;
		mMissNum     = 7;
		break;
	case 1:
		mGoNum       = 4;
		mShineGetNum = 11;
		mMissNum     = 12;
		break;
	case 2:
		mGoNum       = 11;
		mShineGetNum = 11;
		mMissNum     = 8;
		break;
	case 3:
		mGoNum       = 10;
		mShineGetNum = 11;
		mMissNum     = 10;
		break;
	case 4:
		mGoNum       = 4;
		mShineGetNum = 10;
		mMissNum     = 7;
		break;
	}
#endif
	JKRArchive* arch = SMSSwitch2DArchive("guide", gArBkConsole);
	JDrama::TViewObj::load(stream);
	unk10 = new J2DSetScreen("big_tx_1.blo", arch);
	unk10->setCullBack(GX_CULL_BACK);
	unk14 = new J2DSetScreen("scenario_demo_1.blo", arch);
	unk14->setCullBack(GX_CULL_BACK);

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
	for (int i = 0; i < mGoNum; ++i) {
#else
	for (int i = 0; i < 3; ++i) {
#endif
		unk28[i] = new TBoundPane(unk10, CONSOLESTR_PANE_ID('go00', i));
		unk28[i]->unk0->hide();
	}

	for (int i = 0; i < CONSOLESTR_SG_NUM; ++i) {
		unk244[i] = new TBoundPane(unk10, CONSOLESTR_PANE_ID('sg00', i));
	}

	for (int i = 0; i < CONSOLESTR_MS_NUM; ++i) {
		unk268[i] = new TBoundPane(unk10, CONSOLESTR_PANE_ID('ms00', i));
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
		mMissRotation[i] = unk268[i]->getPane()->mRotation;
#endif
	}

	for (int i = 0; i < 5; ++i) {
		unk27C[i] = new TExPane(unk10, 're00' + i);
	}

	unk2A0[0] = (J2DTextBox*)unk14->search('\0map');
	SMSMakeTextBuffer(unk2A0[0], 0x80);
	unk2A0[1] = (J2DTextBox*)unk14->search('stry');
	SMSMakeTextBuffer(unk2A0[1], 0x80);

	for (int i = 0; i < 2; ++i) {
		unk290[i] = new TExPane(unk14, 'msk1' + i);
		unk2A0[i]->setFont(gpSystemFont);
	}

	unk298[0] = new TExPane(unk14, 'wp_l');
	unk298[1] = new TExPane(unk14, 'wp_r');

	u32 uVar1     = SMS_getShineStage(gpMarDirector->mMap);
	u32 uVar9     = TFlagManager::getInstance()->getFlag(MSF_EPISODE);
	void* pvVar10 = JKRGetResource(SMS_COMMON_2D_DIR "stagename.bmg");
	unk2A0[0]->setString(SMSGetMessageData(pvVar10, uVar1));

	if (gpMarDirector->mMap != 15) {
		void* pvVar10 = JKRGetResource(SMS_COMMON_2D_DIR "scenarioname.bmg");

		s16 uVar2 = SMS_getShineID(uVar1, uVar9, false);

		const void* puVar15;
		if (pvVar10 == nullptr || uVar2 == -1)
			puVar15 = "";
#ifdef SMS_PORT
		// w10: the airport's shine 0x56 indexes scScenarioNameTable[86], past its
		// 70 entries (original out-of-bounds read, ASan global-buffer-overflow). On
		// the GMSE01 disc build that word is SelectMenu.o's .data @1210[0] = 0
		// (0x803C0CB8; GMSJ01/GMSP01 also land on SelectMenu's @1210), so the game
		// used scenario message 0. Reproduce that value
		// instead of reading whatever global the PC linker placed there.
		else if ((u16)uVar2 >= sizeof(scScenarioNameTable) / sizeof(scScenarioNameTable[0]))
			puVar15 = SMSGetMessageData(pvVar10, 0);
#endif
		else
			puVar15 = SMSGetMessageData(pvVar10, SMS_getNormalStage(uVar2));

		snprintf(unk2A0[1]->getStringPtr(), 0x80, "%s", puVar15);
	}
}

void TConsoleStr::loadAfter()
{
	JDrama::TViewObj::loadAfter();
	for (int i = 0; i < CONSOLESTR_GO_NUM; ++i)
		unk2AC[i] = nullptr;
}

void TConsoleStr::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (cue & CUE_MOVE) {
		if (gpMarDirector->mState != 5) {
			bool bVar6 = false;

			switch (unk2B8) {
			case 1:
				bVar6 = processGo(unk18);
				break;

			case 2:
				if (unk18 == unk20) {
					unk244[0]->setPanePosition(0x28, cShineGetRight1,
					                           cShineGetLeft1, cShineGetLeft1);
					unk244[0]->getPane()->show();
				} else if (unk18 > unk20) {
					bVar6 = processShineGet((unk18 - unk20) * 2.0f);
				}
				break;

			case 3:
				bVar6 = processMiss(unk18 * 2.0f);
				break;

			case 4:
				bVar6 = processScenario(unk18);
				break;

			case 5:
				bVar6 = processReady(unk18 * 2.0f);
				if (unk18 >= 60.0f) {
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
					if (unk18 == 60.0f) {
						for (int i = 0; i < mGoNum; ++i) {
							unk28[i]->getPane()->hide();
							unk28[i]->getPane()->setAlpha(0xff);
						}
					}
#else
					if (unk18 == 60.0f) {
						unk28[0]->getPane()->hide();
						unk28[1]->getPane()->hide();
						unk28[2]->getPane()->hide();
						unk28[0]->getPane()->setAlpha(0xff);
						unk28[1]->getPane()->setAlpha(0xff);
						unk28[2]->getPane()->setAlpha(0xff);
					}
#endif
					bVar6 &= processGo(unk18 - 60.0f);
				}
				break;
			}

			if (unk2BC == STATE_UNK2) {
				bool uVar13 = true;

				for (int i = 0; i < 2; ++i) {
					uVar13 &= unk290[i]->update();
					unk2A0[i]->setAlpha(0xff
					                    - unk290[i]->getPane()->getAlpha());
				}

				if (uVar13) {
					unk2BC = STATE_UNK3;
					unk18  = 0.0f;
				}
			} else if (unk2BC == STATE_UNK8) {
				bool uVar13 = true;

				for (int i = 0; i < 2; ++i)
					uVar13 &= unk290[i]->update();

				if (uVar13) {
					unk2BC = STATE_UNK3;
					unk18  = 0.0f;
				}
			} else if (unk2BC == STATE_UNK5) {
				bool uVar13 = true;
				for (int i = 0; i < 2; ++i)
					uVar13 &= unk298[i]->update();
				if (uVar13) {
					unk2BC = STATE_UNK6;
					bVar6  = true;
					unk298[0]->getPane()->hide();
					unk298[1]->getPane()->hide();
				}
			} else if (unk2BC == STATE_UNK3) {
				unk18 += 1.0f;
				if (unk18 > 60.0f) {
					unk18  = 0.0f;
					unk2BC = STATE_UNK4;
				}
			} else if (unk2BC == STATE_UNK1) {
				if (unk2A8 != 0)
					startCloseWipe(false);
			}

			if (bVar6)
				unk2B8 = 0;

			unk18 += 0.5f;
		}

		if (!unk2A9 && gpMarDirector->mState != TMarDirector::STATE_UNK4) {
			for (int i = 0; i < CONSOLESTR_GO_NUM; ++i)
				if (unk2AC[i])
					unk2AC[i]->stopCreateParticle();
			unk2A9 = true;
		}

		if (unk2A9 && gpMarDirector->mState == TMarDirector::STATE_UNK4) {
			for (int i = 0; i < CONSOLESTR_GO_NUM; ++i)
				if (unk2AC[i])
					unk2AC[i]->playCreateParticle();
			unk2A9 = false;
		}
	}

	if ((cue & CUE_DRAW) && unk2B8 != 0) {
		const JDrama::TRect& rect = graphics->getScissor();

		J2DOrthoGraph local_1a0(graphics->getViewport());
		local_1a0.setup2D();

		if (unk2B8 == 1 && unk18 > 60.0f) {
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
			for (int i = 0; i < mGoNum; ++i) {
#else
			for (int i = 0; i < 3; ++i) {
#endif
				int local_b0[3] = { 4, 10, 20 };

				u8 alpha         = unk28[i]->getPane()->getAlpha();
				u8 trailAlpha    = alpha;
				JUTRect local_a0 = unk28[i]->getPane()->getBounds();

				for (int j = 0; j < 3; ++j) {
					int iVar9       = local_b0[j];
					JUTPoint& point = unk34[i][iVar9];
					if (point.x != 0) {
						trailAlpha = 0.7f * trailAlpha;
						unk28[i]->getPane()->setAlpha(trailAlpha);
						unk28[i]->getPane()->resize(
						    local_a0.getWidth() - iVar9 * 3,
						    local_a0.getHeight() - iVar9 * 3);

						JUTRect local_90 = unk28[i]->getPane()->getBounds();
						((J2DPicture*)unk28[i]->getPane())
						    ->draw(point.x, point.y, local_90.getWidth(),
						           local_90.getHeight(), false, false, false);
					}
				}

				unk28[i]->getPane()->setAlpha(alpha);
				unk28[i]->getPane()->resize(local_a0.getWidth(),
				                            local_a0.getHeight());
			}
		}

		local_1a0.setup2D();
#ifdef SMS_PORT
		// widescreen: the letterbox bars of the stage intro cover the whole wide picture
		PORTWidePaneScope<J2DPane> wideMask1(unk290[0]->getPane());
		PORTWidePaneScope<J2DPane> wideMask2(unk290[1]->getPane());
#endif
		if (unk2B8 == 4) {
			unk14->draw(0, 0, &local_1a0);
		} else {
			unk10->draw(0, 0, &local_1a0);
		}

		local_1a0.setup2D();
		graphics->setScissor(rect);
	}
}

void TConsoleStr::startAppearReady()
{
	if (unk2B8 == 1)
		return;

	unk2B8 = 1;
	unk18  = 0.0f;
	unk2A9 = 0;
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
	for (int i = 0; i < mGoNum; ++i) {
		unk28[i]->unk0->hide();
		unk28[i]->unk0->mAlpha = 0xff;
	}
#else
	unk28[0]->unk0->hide();
	unk28[1]->unk0->hide();
	unk28[2]->unk0->hide();
	unk28[0]->unk0->mAlpha = 0xff;
	unk28[1]->unk0->mAlpha = 0xff;
	unk28[2]->unk0->mAlpha = 0xff;
#endif
}

void TConsoleStr::startAppearGo()
{
	if (unk2B8 == 1)
		return;

	unk2B8 = 1;
	unk18  = 0.0f;
	unk2A9 = 0;
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
	for (int i = 0; i < mGoNum; ++i) {
		unk28[i]->getPane()->hide();
		unk28[i]->getPane()->setAlpha(0xff);
	}
#else
	unk28[0]->getPane()->hide();
	unk28[1]->getPane()->hide();
	unk28[2]->getPane()->hide();
	unk28[0]->getPane()->setAlpha(0xff);
	unk28[1]->getPane()->setAlpha(0xff);
	unk28[2]->getPane()->setAlpha(0xff);
#endif
}

void TConsoleStr::startAppearShineGet()
{
	if (unk2B8 != 0)
		return;

	unk2B8 = 2;
	unk18  = 0.0f;
	unk24  = 0;
}

void TConsoleStr::startAppearMiss()
{
	if (unk2B8 == 3)
		return;

	if (unk2B8 == 1) {
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
		for (int i = 0; i < mGoNum; ++i)
			unk28[i]->getPane()->hide();
#else
		unk28[0]->getPane()->hide();
		unk28[1]->getPane()->hide();
		unk28[2]->getPane()->hide();
#endif
	}

	unk2B8 = 3;
	unk18  = 0.0f;

	for (int i = 0; i < CONSOLESTR_MS_NUM; ++i) {
		unk268[i]->getPane()->hide();
		unk268[i]->getPane()->setAlpha(0);
	}

	unk268[0]->setPanePosition(0x28, JUTPoint(0, -270), JUTPoint(0, -270),
	                           JUTPoint(0, 30));

	unk268[0]->getPane()->show();
}

void TConsoleStr::startAppearScenario()
{
	if (unk2B8 != 0)
		return;

	unk2B8 = 4;
	unk2BC = STATE_UNK0;
	unk18  = 0.0f;
	unk1C  = -200;

	for (int i = 0; i < 2; ++i) {
		unk2A0[i]->setAlpha(0);
		unk290[i]->getPane()->show();
	}

	unk290[0]->setPaneOffset(0x1E, 0.0f, 0.0f, 0.0f,
	                         -(unk290[0]->getInitialBounds().y2 + 1));
	unk290[1]->setPaneOffset(0x1E, 0.0f, 0.0f, 0.0f,
	                         465 - unk290[1]->getInitialBounds().y1);
}

bool TConsoleStr::processReady(int param_1)
{
	bool result = false;

	for (int i = 0; i < 5; ++i) {
		if (param_1 == i * 10) {
			JUTRect local_d8 = unk27C[i]->getPane()->getBounds();

			int width  = local_d8.getWidth();
			int height = local_d8.getHeight();
			unk27C[i]->setCenteredSize(0x1E, width, height, width + 80,
			                           height + 80);
			unk27C[i]->getPane()->show();
			unk27C[i]->getPane()->setAlpha(0);
		} else if (param_1 < i * 10 + 30) {
			unk27C[i]->update();
			u16 alpha = unk27C[i]->getPane()->getAlpha();
			alpha += 9;
			if (alpha > 0xff)
				alpha = 0xff;
			unk27C[i]->getPane()->setAlpha(alpha);
		} else if (param_1 >= i * 10 + 130) {
			if (param_1 == i * 10 + 130) {
				JUTRect local_e8 = unk27C[i]->getPane()->getBounds();

				int width  = local_e8.getWidth();
				int height = local_e8.getHeight();
				unk27C[i]->setCenteredSize(0x1E, width - 20, height - 20, width,
				                           height);
			} else if (param_1 < i * 10 + 160) {
				unk27C[i]->update();
				s16 alpha = unk27C[i]->getPane()->getAlpha();
				alpha -= 9;
				if (alpha < 0)
					alpha = 0;
				unk27C[i]->getPane()->setAlpha(alpha);
			} else if (i == 4) {
				result = true;
			}
		}
	}

	return result;
}

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
// PAL: up to 11 letters, 5 frames apart, flying off in three directions in turn;
// the emitters are stopped and released as soon as their letter has flown off.
bool TConsoleStr::processGo(float param_1)
{
	bool result = false;

	if (param_1 < 90.0f) {
		for (int i = 0; i < mGoNum; ++i) {
			if (param_1 == i * 5) {
				unk28[i]->setPanePosition(0x28, JUTPoint(0, 60),
				                          JUTPoint(0, -40), JUTPoint(0, -40));
				unk28[i]->getPane()->show();
			}
		}

		for (int i = 0; i < mGoNum; ++i) {
			// TODO: unused copy, kept for codegen
			JUTRect bounds = unk28[i]->getPane()->getBounds();
			if (unk28[i]->update()) {
				bool atOrigin
				    = unk28[i]->mPositionOffset.x == 0 && unk28[i]->mPositionOffset.y == 0;
				if (!atOrigin)
					unk28[i]->setPanePosition(0x1E, JUTPoint(0, -40),
					                          JUTPoint(0, -40), JUTPoint(0, 0));
			}
		}
	} else if (!(param_1 < 95.0f)) {
		if (param_1 == 95.0f) {
			for (int i = 0; i < mGoNum; ++i) {
				int dir = i % 3;
				if (dir == 0)
					unk28[i]->setPanePosition(0x50, JUTPoint(0, 0),
					                          JUTPoint(-170, -180),
					                          JUTPoint(-340, -360));
				else if (dir == 1)
					unk28[i]->setPanePosition(0x50, JUTPoint(0, 0),
					                          JUTPoint(0, -220), JUTPoint(0, -440));
				else
					unk28[i]->setPanePosition(0x50, JUTPoint(0, 0),
					                          JUTPoint(160, -180),
					                          JUTPoint(320, -360));
			}

			for (int i = 0; i < mGoNum; ++i) {
				for (int j = 0; j < 16; ++j) {
					JUTRect bounds = unk28[i]->getPane()->getGlobalBounds();
					unk34[i][j].set(bounds.x1, bounds.y1);
				}

				JUTRect bounds = unk28[i]->getPane()->getBounds();
				JGeometry::TVec3<f32> pos(bounds.x1 + bounds.getWidth() * 0.5f,
				                          bounds.y1 + bounds.getHeight() * 0.5f,
				                          0.0f);
				gpEmitterManager4D2->createEmitter(pos, 0x1FD, nullptr, nullptr);
				unk2AC[i] = gpEmitterManager4D2->unkC8[0][0];
			}
		} else if (param_1 < 175.0f) {
			int frame = (int)param_1 % 2;
			for (int i = 0; i < mGoNum; ++i) {
				int alpha = unk28[i]->getPane()->getAlpha() - 4;
				if (alpha < 0)
					alpha = 0;
				JUTRect bounds = unk28[i]->getPane()->getGlobalBounds();
				unk28[i]->getPane()->setAlpha(alpha);
				unk28[i]->getPane()->resize(bounds.getWidth() + 2,
				                            bounds.getHeight() + 2);
				if (unk2AC[i] != nullptr)
					unk2AC[i]->setGlobalTranslation(
					    bounds.x1 + bounds.getWidth() * 0.5f,
					    bounds.y1 + bounds.getHeight() * 0.5f, 0.0f);

				if (unk28[i]->update() && unk2AC[i] != nullptr) {
					unk2AC[i]->stopCreateParticle();
					unk2AC[i] = nullptr;
				}

				if (frame == 0) {
					for (int j = 14; j >= 0; --j)
						unk34[i][j + 1] = unk34[i][j];
					unk34[i][0].set(bounds.x1, bounds.y1);
				}
			}
		} else if (param_1 == 175.0f) {
			for (int i = 0; i < mGoNum; ++i) {
				JUTRect bounds = unk28[i]->getPane()->getBounds();
				unk28[i]->getPane()->resize(bounds.getWidth() - 80,
				                            bounds.getHeight() - 80);
			}
		} else {
			for (int i = 0; i < mGoNum; ++i) {
				unk28[i]->getPane()->hide();
				if (unk2AC[i]) {
					unk2AC[i]->stopCreateParticle();
					unk2AC[i] = nullptr;
				}
			}
			result = true;
		}
	}

	return result;
}
#else
bool TConsoleStr::processGo(float param_1)
{
	bool result = false;

	if (param_1 < 90.0f) {
		for (int i = 0; i < ARRAY_COUNT(unk28); ++i) {
			if (param_1 == i * 10) {
				unk28[i]->setPanePosition(0x28, JUTPoint(0, 60),
				                          JUTPoint(0, -40), JUTPoint(0, -40));
				unk28[i]->getPane()->show();
			}
		}

		for (int i = 0; i < ARRAY_COUNT(unk28); ++i) {
			// TODO: unused copy, kept for codegen
			JUTRect bounds = unk28[i]->getPane()->getBounds();
			if (unk28[i]->update()) {
				bool atOrigin
				    = unk28[i]->mPositionOffset.x == 0 && unk28[i]->mPositionOffset.y == 0;
				if (!atOrigin)
					unk28[i]->setPanePosition(0x1E, JUTPoint(0, -40),
					                          JUTPoint(0, -40), JUTPoint(0, 0));
			}
		}
	} else if (!(param_1 < 95.0f)) {
		if (param_1 == 95.0f) {
			unk28[0]->setPanePosition(0x50, JUTPoint(0, 0),
			                          JUTPoint(-170, -180),
			                          JUTPoint(-340, -360));
			unk28[1]->setPanePosition(0x50, JUTPoint(0, 0), JUTPoint(0, -220),
			                          JUTPoint(0, -440));
			unk28[2]->setPanePosition(0x50, JUTPoint(0, 0), JUTPoint(160, -180),
			                          JUTPoint(320, -360));

			for (int i = 0; i < ARRAY_COUNT(unk28); ++i) {
				for (int j = 0; j < 16; ++j) {
					JUTRect bounds = unk28[i]->getPane()->getGlobalBounds();
					unk34[i][j].set(bounds.x1, bounds.y1);
				}

				JUTRect bounds = unk28[i]->getPane()->getBounds();
				JGeometry::TVec3<f32> pos(bounds.x1 + bounds.getWidth() * 0.5f,
				                          bounds.y1 + bounds.getHeight() * 0.5f,
				                          0.0f);
				gpEmitterManager4D2->createEmitter(pos, 0x1FD, nullptr,
				                                   nullptr);
				unk2AC[i] = gpEmitterManager4D2->unkC8[0][0];
			}
		} else if (param_1 < 175.0f) {
			for (int i = 0; i < ARRAY_COUNT(unk28); ++i) {
				int alpha = unk28[i]->getPane()->getAlpha() - 4;
				if (alpha < 0)
					alpha = 0;
				JUTRect bounds = unk28[i]->getPane()->getGlobalBounds();
				unk28[i]->getPane()->setAlpha(alpha);
				unk28[i]->getPane()->resize(bounds.getWidth() + 2,
				                            bounds.getHeight() + 2);
				unk2AC[i]->setGlobalTranslation(
				    bounds.x1 + bounds.getWidth() * 0.5f,
				    bounds.y1 + bounds.getHeight() * 0.5f, 0.0f);
				unk28[i]->update();

				if ((int)param_1 % 2 == 0) {
					for (int j = 14; j >= 0; --j)
						unk34[i][j + 1] = unk34[i][j];
					unk34[i][0].set(bounds.x1, bounds.y1);
				}
			}
		} else if (param_1 == 175.0f) {
			for (int i = 0; i < ARRAY_COUNT(unk28); ++i) {
				JUTRect bounds = unk28[i]->getPane()->getBounds();
				unk28[i]->getPane()->resize(bounds.getWidth() - 80,
				                            bounds.getHeight() - 80);
			}
		} else {
			for (int i = 0; i < ARRAY_COUNT(unk28); ++i) {
				unk28[i]->getPane()->hide();
				if (unk2AC[i])
					unk2AC[i]->stopCreateParticle();
			}
			result = true;
		}
	}

	return result;
}
#endif

bool TConsoleStr::processShineGet(int param_1)
{
	bool result = true;

	for (int i = 0; i < CONSOLESTR_SG_NUM; ++i) {
		if (param_1 == 6 * i) {
			unk244[i]->getPane()->show();
			unk244[i]->setPanePosition(0x28, cShineGetRight1, cShineGetLeft1,
			                           cShineGetLeft1);
			JUTRect local_74 = unk244[i]->getPane()->getBounds();
			JGeometry::TVec3<f32> local_80(
			    local_74.x1 + local_74.getWidth() * 0.5f,
			    local_74.y1 + local_74.getHeight() * 0.5f, 0.0f);
			gpEmitterManager4D2->createEmitter(local_80, 0x1FE, nullptr,
			                                   nullptr);
		}

		if (param_1 == i * 6 + 40) {
			unk244[i]->setPanePosition(0x14, cShineGetLeft2, cShineGetLeft2,
			                           cShineGetRight2);
		}

		if (param_1 == i * 6 + 60) {
			JUTRect local_94 = unk244[i]->getPane()->getBounds();
			JGeometry::TVec3<f32> local_80(
			    local_94.x1 + local_94.getWidth() * 0.5f,
			    local_94.y1 + local_94.getHeight() * 0.5f, 0.0f);
			gpEmitterManager4D2->createEmitter(local_80, 0x1FF, nullptr,
			                                   nullptr);
		}

		if (param_1 == i * 6 + 200) {
			unk244[i]->setPanePosition(0x28, cShineGetRight3, cShineGetRight3,
			                           cShineGetLeft3);
		}

		if (param_1 < i * 6 + 40) {
			u16 alpha = unk244[i]->getPane()->getAlpha() + 7;
			if (alpha > 0xff)
				alpha = 0xff;
			unk244[i]->getPane()->setAlpha(alpha);
		}

		if (param_1 > i * 6 + 200) {
			s16 alpha = unk244[i]->getPane()->getAlpha();
			alpha -= 7;
			if (alpha < 0)
				alpha = 0;
			unk244[i]->getPane()->setAlpha(alpha);
		}

		if (unk244[i]->update()) {
			if (param_1 > i * 6 + 280) {
				unk244[i]->getPane()->hide();
				result &= true;
			} else {
				result = 0;
			}
		} else {
			result = 0;
		}
	}

	return result;
}

bool TConsoleStr::processMiss(int param_1)
{
	bool result = true;

	for (int i = 0; i < CONSOLESTR_MS_NUM; ++i) {
		if (param_1 == i * 10) {
			unk268[i]->getPane()->show();
			unk268[i]->setPanePosition(0x3C, JUTPoint(0, -270),
			                           JUTPoint(0, -270), JUTPoint(0, 30));
		}

		if (param_1 == i * 10 + 1) {
			JUTRect local_9c = unk268[i]->getPane()->getBounds();
			JGeometry::TVec3<f32> local_a8(
			    local_9c.x1 + local_9c.getWidth() * 0.5f,
			    local_9c.y1 + local_9c.getHeight() * 0.5f, 0.0f);
			gpEmitterManager4D2->createEmitter(local_a8, 0x1F9, nullptr,
			                                   nullptr);
		}

		if (param_1 == i * 10 + 60) {
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
			unk268[i]->getPane()->mRotation = mMissRotation[i];
#else
			unk268[i]->getPane()->mRotation = 0.0f;
#endif
			unk268[i]->setPanePosition(0x28, JUTPoint(0, 30), JUTPoint(0, -80),
			                           JUTPoint(0, -80));
		}

		if (param_1 == i * 10 + 100) {
			unk268[i]->setPanePosition(0x28, JUTPoint(0, -80), JUTPoint(0, -80),
			                           JUTPoint(0, 0));
		}

		if (param_1 == i * 10 + 300) {
			unk268[i]->setPanePosition(0x1E, JUTPoint(0, 0), JUTPoint(0, 0),
			                           JUTPoint(0, 150));
		}

		if (param_1 < i * 10 + 60) {
			u16 alpha = unk268[i]->getPane()->getAlpha();
			alpha += 12;
			if (alpha > 0xff)
				alpha = 0xff;
			unk268[i]->getPane()->setAlpha(alpha);
		}

		if (unk268[i]->update()) {
			if (param_1 > i * 10 + 360) {
				unk268[i]->getPane()->hide();
				result &= true;
			} else {
				result = false;
			}
		} else {
			if (param_1 < i * 10 + 60) {
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
				unk268[i]->getPane()->mRotation
				    = mMissRotation[i] + (i * 10 - param_1) * 6;
#else
				unk268[i]->getPane()->mRotation = (i * 10 - param_1) * 6;
#endif
			}

			result = false;
		}
	}

	return result;
}

bool TConsoleStr::processScenario(int)
{
	if (unk2BC > STATE_UNK0 && unk2BC != STATE_UNK7)
		return 0;

	bool uVar2 = true;
	uVar2 &= unk290[0]->update();
	uVar2 &= unk290[1]->update();

	unk1C += 2;

	if (unk1C > 355) {
		unk1C = 355;
		uVar2 &= true;
	} else {
		uVar2 = false;
	}

	int alpha = unk1C > 100 ? unk1C - 100 : 0;

	unk2A0[0]->setAlpha(alpha);
	unk2A0[1]->setAlpha(alpha);
	if (uVar2)
		unk2BC = STATE_UNK1;

	return false;
}

void TConsoleStr::startCloseWipe(bool param_1)
{
	if (param_1) {
		unk290[0]->getPane()->show();
		unk290[1]->getPane()->show();
		unk2A0[0]->hide();
		unk2A0[1]->hide();

		JUTRect local_74 = unk290[0]->getPane()->getBounds();
		s32 h            = 224;
		unk290[0]->setPaneSize(0x2D, local_74.getWidth(), h,
		                       local_74.getWidth(), 0);
		unk290[0]->setPaneAlpha(0x2D, 255, 0);

		local_74 = unk290[1]->getPane()->getBounds();
		unk290[1]->setPaneOffset(0x2D, 0, h - local_74.y1, 0,
		                         465 - unk290[1]->getInitialBounds().y1);
		unk290[1]->setPaneSize(0x2D, local_74.getWidth(),
		                       465 - unk290[1]->getInitialBounds().y1 + h,
		                       local_74.getWidth(), 0);
		unk290[1]->setPaneAlpha(0x2D, 255, 0);

		unk2BC = STATE_UNK8;
		unk2B8 = 4;
		unk2A8 = 1;
	} else if (unk2BC != STATE_UNK1) {
		unk2A8 = 1;
	} else {
		unk2BC           = STATE_UNK2;
		JUTRect local_88 = unk290[0]->getPane()->getBounds();
		s32 h            = 224;
		unk290[0]->setPaneSize(0x2D, local_88.getWidth(), h,
		                       local_88.getWidth(), local_88.getHeight());
		unk290[0]->setPaneAlpha(0x2D, 255, unk290[0]->getPane()->getAlpha());

		local_88 = unk290[1]->getPane()->getBounds();
		unk290[1]->setPaneOffset(0x2D, 0, h - local_88.y1, 0, 0);
		unk290[1]->setPaneSize(0x2D, local_88.getWidth(), 464 - h,
		                       local_88.getWidth(), local_88.getHeight());
		unk290[1]->setPaneAlpha(0x2D, 255, unk290[1]->getPane()->getAlpha());
	}
}

void TConsoleStr::startOpenWipe()
{
	unk2A8 = 0;
	unk2BC = STATE_UNK5;
	unk290[0]->getPane()->hide();
	unk298[0]->getPane()->show();
	unk290[1]->getPane()->hide();
	unk298[1]->getPane()->show();

	// TODO: TExPane::setPaneAlpha is wrong

	JUTRect local_3c = unk298[0]->getPane()->getBounds();
	unk298[0]->setPaneOffset(0x1E, -local_3c.getWidth(), 0, 0, 0);
	unk298[0]->setPaneAlpha(30, 100, 255);

	local_3c = unk298[1]->getPane()->getBounds();
	unk298[1]->setPaneOffset(0x1E, local_3c.getWidth(), 0, 0, 0);
	unk298[1]->setPaneAlpha(30, 100, 255);
}
