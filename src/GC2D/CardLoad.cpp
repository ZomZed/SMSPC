#include <GC2D/CardLoad.hpp>
#include <stdio.h>
#include <macros.h>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/J2D/J2DScreen.hpp>
#include <JSystem/J2D/J2DPicture.hpp>
#include <JSystem/J2D/J2DTextBox.hpp>
#include <JSystem/J2D/J2DOrthoGraph.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <JSystem/JParticle/JPAEmitterManager.hpp>
#include <JSystem/JParticle/JPAEmitter.hpp>
#include <System/MarioGamePad.hpp>
#include <System/Application.hpp>
#include <System/StageUtil.hpp>
#include <System/MarDirector.hpp>
#include <System/FlagManager.hpp>
#include <MSound/MSound.hpp>
#include <GC2D/Option.hpp>
#include <GC2D/ExPane.hpp>
#include <GC2D/MessageUtil.hpp>
#include <MoveBG/MapObjOption.hpp>
#include <Player/Mario.hpp>
#include <Camera/CameraOption.hpp>
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
#include <JSystem/J2D/J2DWindow.hpp>
#include <JSystem/J2D/J2DPrint.hpp>
#endif

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <M3DUtil/InfectiousStrings.hpp>

TCardLoad* gpCardLoad;

// TODO: where should this go?
extern JPAEmitterManager* gpEmitterManager4D2;

// The US version reads the "Corrupt" and "New" file labels from
// loadmessage.bmg; JP copies string literals.
#ifdef VERSION_GMSP01
#define CARDLOAD_SET_BROKEN(box, len) setMessage(box, len, 0)
#define CARDLOAD_SET_NEW(box, len) setMessage(box, len, 0x1B)
#else
#define CARDLOAD_SET_BROKEN(box, len) \
	strncpy((box)->getStringPtr(), "こわれています", len)
#define CARDLOAD_SET_NEW(box, len) strncpy((box)->getStringPtr(), "NEW", len)
#endif

// Other US-only differences in changeScene().
#ifdef VERSION_GMSP01
#define CARDLOAD_PROGRESS_US35 PROGRESS_UNK35
#define CARDLOAD_FORMAT_NO PROGRESS_UNK35
#define CARDLOAD_CREATE_MSG_STEP 3
#else
#define CARDLOAD_FORMAT_NO PROGRESS_UNK4
#define CARDLOAD_CREATE_MSG_STEP 4
#endif

// After formatting, PAL keeps the message up for drawMessage()'s timed step 3; JP/US
// jump straight to step 4.
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
#define CARDLOAD_FORMAT_MSG_STEP 3
#else
#define CARDLOAD_FORMAT_MSG_STEP 4
#endif

// "PRESS START" letter timing in titleDraw() (frames; PAL waits longer).
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
#define CARDLOAD_TITLE_SHOW_TIME 600
#define CARDLOAD_TITLE_HIDE_TIME 400
#else
#define CARDLOAD_TITLE_SHOW_TIME 500
#define CARDLOAD_TITLE_HIDE_TIME 300
#endif

static void clearBookmark(u32 bm)
{
	JSUMemoryOutputStream stream;
	gpCardManager->getWriteStream(&stream);
	gpCardManager->writeBlock(bm);
}

static void loadBookmark()
{
	JSUMemoryInputStream stream;
	gpCardManager->getReadStream(&stream);
	TFlagManager::getInstance()->load(stream);
}

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
// PAL loadmessage_<language>.bmg has one more entry than the US file at 0x1C, so the
// IDs from 0x1C up are one higher.
u32 TCardLoad::cMessageID[] = {
	0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x10010,    0x10014,    0x10015,
	0x10005,    0x10006,    0x10007,    0x1001A,    0x10023,    0x10019,
	0x10013,    0x10011,    0x10002,    0x1000B,    0x10012,    0x10021,
	0x10017,    0x1001E,    0xFFFFFFFF, 0x10009,    0xFFFFFFFF, 0xFFFFFFFF,
	0xFFFFFFFF, 0xFFFFFFFF, 0x10008,    0xFFFFFFFF, 0x1001F,    0x10004,
	0x10003,    0x1000C,    0x10022,    0x10018,    0x10001,    0xFFFFFFFF,
	0x1000A,    0x1000A,    0x10016,    0x10020,    0xFFFFFFFF, 0xFFFFFFFF,
	0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x1000F,    0xFFFFFFFF, 0x1001D,
	0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x1000E,
};

// Texture names below /option/timg/<language>/ (loadLangTexture()).
const char* TCardLoad::cToOptionFileName     = "select_options.bti";
const char* TCardLoad::cBMarkNewFileName     = "select_new.bti";
const char* TCardLoad::cWindowYesFileName    = "select_yes.bti";
const char* TCardLoad::cWindowNoFileName     = "select_no.bti";
const char* TCardLoad::cScoreTotalFileName   = "select_total.bti";
const char* TCardLoad::cBMarkMenuFileName[4] = {
	"select_start.bti",
	"select_copy.bti",
	"select_erase.bti",
	"select_score.bti",
};
const char* TCardLoad::cScoreStageFileName[9] = {
	"stage_bianco.bti", "stage_ricco.bti",      "stage_mamma.bti",
	"stage_pinna.bti",  "stage_sirena.bti",     "stage_monte.bti",
	"stage_mare.bti",   "stage_dolpicTown.bti", "stage_airport.bti",
};
#elif defined(VERSION_GMSP01)
// US loadmessage.bmg has extra entries, so most IDs differ from JP.
u32 TCardLoad::cMessageID[] = {
	0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x10010,    0x10014,    0x10015,
	0x10005,    0x10006,    0x10007,    0x1001A,    0x10022,    0x10019,
	0x10013,    0x10011,    0x10002,    0x1000B,    0x10012,    0x10020,
	0x10017,    0x1001D,    0xFFFFFFFF, 0x10009,    0xFFFFFFFF, 0xFFFFFFFF,
	0xFFFFFFFF, 0xFFFFFFFF, 0x10008,    0xFFFFFFFF, 0x1001E,    0x10004,
	0x10003,    0x1000C,    0x10021,    0x10018,    0x10001,    0xFFFFFFFF,
	0x1000A,    0x1000A,    0x10016,    0x1001F,    0xFFFFFFFF, 0xFFFFFFFF,
	0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x1000F,    0xFFFFFFFF, 0x1001C,
	0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x1000E,
};
#else
u32 TCardLoad::cMessageID[] = {
	0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x1000E,    0x10012,    0x10013,
	0x10004,    0x10005,    0x10006,    0x10018,    0x1001F,    0x10017,
	0x10011,    0x1000F,    0x10001,    0x1000A,    0x10010,    0x1001D,
	0x10015,    0x1001A,    0xFFFFFFFF, 0x10008,    0xFFFFFFFF, 0xFFFFFFFF,
	0xFFFFFFFF, 0xFFFFFFFF, 0x10007,    0xFFFFFFFF, 0x1001B,    0x10003,
	0x10002,    0x1000B,    0x1001E,    0x10016,    0x10000,    0xFFFFFFFF,
	0x10009,    0x10009,    0x10014,    0x1001C,    0xFFFFFFFF, 0xFFFFFFFF,
	0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x1000D,    0xFFFFFFFF, 0x10019,
	0x0,        0x0,        0x0,        0x0,        0x0,
};
#endif

TCardLoad::TCardLoad(const char* name)
    : JDrama::TViewObj(name)
    , unk10(5)
    , unk14(10)
    , unk1C(PROGRESS_UNK0)
    , unk28(nullptr)
    , unk2C(nullptr)
    , unk30(0)
    , unkA0(nullptr)
    , unkA4(0)
    , unkA8(60.0f)
    , unkAC(nullptr)
    , unkB8(0)
    , unkBC(0)
    , unk25C(nullptr)
    , unk270(nullptr)
    , unk274(0)
    , unk275(0)
    , unk284(0)
    , unk288(nullptr)
    , unk524(nullptr)
    , unk538(nullptr)
    , unk53C(nullptr)
    , unk754(0)
{
	gpCardLoad = this;
}

void TCardLoad::changePattern(J2DPicture* picture, s16 period, u32 frame)
{
	if (frame % period == 0) {
		if ((frame / period) % 2 == 0) {
			picture->setBlendKonstColor(0.0f, 1.0f, 0.0f, 0.0f);
			picture->setBlendKonstAlpha(0.0f, 1.0f, 0.0f, 0.0f);
		} else {
			picture->setBlendKonstColor(1.0f, 0.0f, 0.0f, 0.0f);
			picture->setBlendKonstAlpha(1.0f, 0.0f, 0.0f, 0.0f);
		}
	}
}

void TCardLoad::load(JSUMemoryInputStream& stream)
{
	JDrama::TViewObj::load(stream);

	if (gpMarDirector->mMap != 15)
		return;

	JKRArchive* optionVolume = (JKRArchive*)JKRFileLoader::getVolume("option");

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
	// PAL: messages and the score screen for all five languages; changeLanguage()
	// picks the current ones.
	mLoadMessage[0] = JKRGetResource("/option/loadmessage_en.bmg");
	mLoadMessage[1] = JKRGetResource("/option/loadmessage_ge.bmg");
	mLoadMessage[2] = JKRGetResource("/option/loadmessage_fr.bmg");
	mLoadMessage[3] = JKRGetResource("/option/loadmessage_sp.bmg");
	mLoadMessage[4] = JKRGetResource("/option/loadmessage_it.bmg");

	unk28 = new J2DSetScreen("load.blo", optionVolume);
	unk28->setCullBack(GX_CULL_BACK);
	mScoreScreen[0] = new J2DSetScreen("load_score_en.blo", optionVolume);
	mScoreScreen[1] = new J2DSetScreen("load_score_ge.blo", optionVolume);
	mScoreScreen[2] = new J2DSetScreen("load_score_fr.blo", optionVolume);
	mScoreScreen[3] = new J2DSetScreen("load_score_sp.blo", optionVolume);
	mScoreScreen[4] = new J2DSetScreen("load_score_it.blo", optionVolume);
	unk2C = mScoreScreen[0];
	unk2C->setCullBack(GX_CULL_BACK);
	unk754 = new TOptionControl;
	unk754->load();

	int language = TFlagManager::getInstance()->getFlag(MSF_LANGUAGE);
	const char* titleName;
	switch (language) {
	default:
		titleName = "title_1.blo";
		break;
	case 1:
		titleName = "title_1_ge.blo";
		break;
	case 2:
		titleName = "title_1_fr.blo";
		break;
	case 3:
		titleName = "title_1_sp.blo";
		break;
	case 4:
		titleName = "title_1_it.blo";
		break;
	}
	unk34 = new J2DSetScreen(titleName, optionVolume);
	unk34->setCullBack(GX_CULL_BACK);
#else
	unkA0 = JKRGetResource("/option/loadmessage.bmg");

	unk28 = new J2DSetScreen("load.blo", optionVolume);
	unk28->setCullBack(GX_CULL_BACK);
	unk2C = new J2DSetScreen("load_score.blo", optionVolume);
	unk2C->setCullBack(GX_CULL_BACK);
	unk754 = new TOptionControl;
	unk754->load();

	unk34 = new J2DSetScreen("title_1.blo", optionVolume);
	unk34->setCullBack(GX_CULL_BACK);
#endif

	for (int i = 0; i < 10; ++i) {
		char buffer[0x28];
		snprintf(buffer, 0x28, "/option/timg/coin_number_%d_min.bti", i);
		unkC8[i] = new JUTTexture((const ResTIMG*)JKRGetResource(buffer));
	}

	setupTitleScreen();

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
	// PAL: the "PRESS START" letters come from the language's texture folder.
	const char* letterDirs[]
	    = { "/option/timg/", "/option/timg/ge/", "/option/timg/fr/",
		    "/option/timg/sp/", "/option/timg/it/" };
	const char* letterDir = letterDirs[language];
	for (int i = 0; i < mTitleLetterNum; ++i) {
		char buffer[0x100];
		snprintf(buffer, 0xFF, "%stitle_start_%02d.bti", letterDir, i + 1);
		JUTTexture* letter
		    = new JUTTexture((const ResTIMG*)JKRGetResource(buffer));
		((J2DPicture*)unk34->search('s_00' + ((i + 1) / 10) * 0x100
		                            + (i + 1) % 10))
		    ->changeTexture(letter->getTexInfo(), 0);
	}
#endif

	unk208 = unk34->search('ROOT');

	unkF0 = new TExPane(unk34, 'titl');
	unkF4 = new TExPane(unk34, 'nint');

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
	for (int i = 0; i < mTitleLetterNum; ++i) {
#else
	for (int i = 0; i < 11; ++i) {
#endif
		int key;
		if (i < 9)
			key = 's_01' + i;
		else
			key = 's_00' + ((i + 1) / 10) * 0x100 + (i + 1) % 10;

		unkF8[i] = new TExPane(unk34, key);
		unkF8[i]->getPane()->setAlpha(0);
		unk124[i] = unkF8[i]->getPane()->getBounds();
	}

	for (int i = 0; i < ARRAY_COUNT(unk1D4); ++i) {
		int key;
		if (i < 9)
			key = 'p_01' + i;
		else
			key = 'p_00' + ((i + 1) / 10) * 0x100 + (i + 1) % 10;

		unk1D4[i] = new TExPane(unk34, key);

		((J2DPicture*)unk1D4[i]->getPane())->mBlack = 0x00FFFF00;

		unk1D4[i]->setPaneAlpha(20, 0xff, 0);
	}

	unk25C = unk28->search('yaji');
	unk260 = unk25C->getBounds();
	unk270 = unk28->search('\0s_1');

	unk25C->show();
	unk270->show();
	unk25C->setAlpha(0);
	unk288 = new TExPane(unk28, '\0w_0');
	unk28C = unk288->getPane()->getBounds();

#ifdef VERSION_GMSP01
	unk29C = (J2DTextBox*)unk28->search('m_0a');
	makeBuffer(unk29C, 0x100);

	unk2A0 = (J2DTextBox*)unk28->search('m_0b');
	makeBuffer(unk2A0, 0x100);
#else
	unk29C = (J2DTextBox*)unk28->search('m_0a');
	makeBuffer(unk29C, 0x80);

	unk2A0 = (J2DTextBox*)unk28->search('m_0b');
	makeBuffer(unk2A0, 0x80);
#endif

	JUTTexture* pJVar5 = new JUTTexture(
	    (const ResTIMG*)JKRGetResource("/option/timg/load_sun_1.bti"));
	for (int i = 0; i < 3; ++i) {
		unk2A4[i] = new TExPane(unk28, '\0w_1' + i);

		unk2C0[i] = (J2DTextBox*)unk28->search('m_1a' + 2 * i);
		makeBuffer(unk2C0[i], 0x80);

		unk2CC[i] = (J2DTextBox*)unk28->search('m_1b' + 2 * i);
		makeBuffer(unk2CC[i], 0x80);

		unk2D8[i] = unk28->search('shn0' + i);
		unk2E4[i] = (J2DPicture*)unk28->search('n_0a' + i * 0x100);
		unk2F0[i] = (J2DPicture*)unk28->search('n_0b' + i * 0x100);
		unk2FC[i] = unk28->search('shp0' + i);
		unk308[i] = (J2DPicture*)unk28->search('np0a' + i * 0x100);
		unk314[i] = (J2DPicture*)unk28->search('np0b' + i * 0x100);
		unk320[i] = (J2DPicture*)unk28->search('np0c' + i * 0x100);
		unk32C[i] = (J2DPicture*)unk28->search('cra1' + i * 0x100);

		unk32C[i]->insert(pJVar5, unk32C[i]->mTextureNum, 0.0f);
		unk28->search('cra2' + i * 0x100)->hide();
	}

	unk2B0 = unk2A4[0]->getPane()->getBounds();

	for (int i = 0; i < 3; ++i) {
		unk33C[i] = new TExPane(unk28, '\0w_4' + i);
		unk348[i] = unk33C[i]->getPane()->getBounds();

		u32 key = 's_2a' + i;
		for (int j = 0; j < 4; ++j) {
			unk378[i][j] = new TExPane(unk28, key + j * 0x100);
			unk3A8[i][j] = unk378[i][j]->getPane()->getBounds();
		}
	}

	unk468 = new TExPane(unk28, '\0w_7');
	unk46C = unk468->getPane()->getBounds();

	unk47C = (J2DTextBox*)unk28->search('m_2a');
	makeBuffer(unk47C, 0x400);

	unk480 = (J2DTextBox*)unk28->search('m_2b');
	makeBuffer(unk480, 0x400);

	for (int i = 0; i < 2; ++i) {
		unk484[i] = new TExPane(unk28, 's_6a' + i * 0x100);
		unk48C[i] = unk484[i]->getPane()->getBounds();
	}

	unk4AC = new TExPane(unk28, '\0w_8');
	unk4B0 = unk4AC->getPane()->getBounds();

	unk4C0 = (J2DTextBox*)unk28->search('m_4a');
	makeBuffer(unk4C0, 0x400);

	unk4C4 = (J2DTextBox*)unk28->search('m_4b');
	makeBuffer(unk4C4, 0x400);

	unk4C8 = unk28->search('\0t_0');

	for (int i = 0; i < 3; ++i)
		unk4CC[i] = unk28->search('\0t_1' + i);

	for (int i = 0; i < 2; ++i) {
		unk4D8[i] = new TExPane(unk28, 's_6b' + i * 0x100);
		unk4E0[i] = unk4D8[i]->getPane()->getBounds();
	}

	unk500 = unk28->search('f_sh');
	unk510 = unk28->search('f_si');

	for (int i = 0; i < 3; ++i)
		unk504[i] = (J2DPicture*)unk28->search('n_3a' + i);

	for (int i = 0; i < 2; ++i)
		unk514[i] = (J2DPicture*)unk28->search('n_4a' + i);

	unk51C = (J2DTextBox*)unk28->search('m_3a');
	makeBuffer(unk51C, 0x14);

	unk520 = (J2DTextBox*)unk28->search('m_3b');
	makeBuffer(unk520, 0x14);

	unk524 = new TExPane(unk28, '\0w_9');
	unk528 = unk524->getPane()->getBounds();

	unk538 = (J2DTextBox*)unk28->search('m_5a');
	makeBuffer(unk538, 0x400);

	unk53C = (J2DTextBox*)unk28->search('m_5b');
	makeBuffer(unk53C, 0x400);
	unk53C->hide();

	for (int i = 0; i < 3; ++i)
		unk540[i] = unk28->search('s_0a' + i);

	unk54C = new TExPane(unk28, 'w_11');
	unk550 = unk54C->getPane()->getBounds();

	unk560 = (J2DTextBox*)unk28->search('m_7a');
	makeBuffer(unk560, 0x400);

	unk564 = (J2DTextBox*)unk28->search('m_7b');
	makeBuffer(unk564, 0x400);
	unk564->hide();

	unk568 = new TExPane(unk28, 'w_10');
	unk56C = unk568->getPane()->getBounds();

	unk57C = (J2DTextBox*)unk28->search('m_6a');
	makeBuffer(unk57C, 0x400);

	unk580 = (J2DTextBox*)unk28->search('m_6b');
	makeBuffer(unk580, 0x400);

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
	loadLangTexture();
	changeLanguage(TFlagManager::getInstance()->getFlag(MSF_LANGUAGE));
}

// PAL: switches the load messages, the score screen and the translated pictures of
// the file select and score screens (after the language option changes).
void TCardLoad::changeLanguage(int language)
{
	unkA0 = mLoadMessage[language];
	unk2C = mScoreScreen[language];
	resetScoreScreenObjects();

	((J2DPicture*)unk28->search('\0s_1'))
	    ->changeTexture(mOptionTex[language]->getTexInfo(), 0);
	((J2DPicture*)unk28->search('s_6a'))
	    ->changeTexture(mYesTex[language]->getTexInfo(), 0);
	((J2DPicture*)unk28->search('s_7a'))
	    ->changeTexture(mNoTex[language]->getTexInfo(), 0);
	((J2DPicture*)unk28->search('s_6b'))
	    ->changeTexture(mYesTex[language]->getTexInfo(), 0);
	((J2DPicture*)unk28->search('s_7b'))
	    ->changeTexture(mNoTex[language]->getTexInfo(), 0);

	for (int i = 0; i < 3; ++i) {
		((J2DPicture*)unk28->search('s_0a' + i))
		    ->changeTexture(mNewTex[language]->getTexInfo(), 0);
		for (int j = 0; j < 4; ++j)
			((J2DPicture*)unk28->search('s_2a' + i + j * 0x100))
			    ->changeTexture(mBookmarkMenuTex[j][language]->getTexInfo(),
			                    0);
	}

	for (int i = 0; i < 9; ++i)
		((J2DPicture*)unk2C->search('st_0' + i))
		    ->changeTexture(mStageTex[i][language]->getTexInfo(), 0);

	((J2DPicture*)unk2C->search('s_tl'))
	    ->changeTexture(mTotalTex[language]->getTexInfo(), 0);
}

void TCardLoad::loadLangTexture()
{
	static const char* langArray[] = { "", "ge/", "fr/", "sp/", "it/" };

	for (int i = 0; i < 5; ++i) {
		char buffer[0x100];
		snprintf(buffer, 0xFF, "/option/timg/%s%s", langArray[i],
		         cToOptionFileName);
		mOptionTex[i] = new JUTTexture((const ResTIMG*)JKRGetResource(buffer));
	}

	for (int j = 0; j < 4; ++j) {
		for (int i = 0; i < 5; ++i) {
			char buffer[0x100];
			snprintf(buffer, 0xFF, "/option/timg/%s%s", langArray[i],
			         cBMarkMenuFileName[j]);
			mBookmarkMenuTex[j][i]
			    = new JUTTexture((const ResTIMG*)JKRGetResource(buffer));
		}
	}

	for (int i = 0; i < 5; ++i) {
		char buffer[0x100];
		snprintf(buffer, 0xFF, "/option/timg/%s%s", langArray[i],
		         cBMarkNewFileName);
		mNewTex[i] = new JUTTexture((const ResTIMG*)JKRGetResource(buffer));
	}

	for (int i = 0; i < 5; ++i) {
		char buffer[0x100];
		snprintf(buffer, 0xFF, "/option/timg/%s%s", langArray[i],
		         cWindowYesFileName);
		mYesTex[i] = new JUTTexture((const ResTIMG*)JKRGetResource(buffer));
	}

	for (int i = 0; i < 5; ++i) {
		char buffer[0x100];
		snprintf(buffer, 0xFF, "/option/timg/%s%s", langArray[i],
		         cWindowNoFileName);
		mNoTex[i] = new JUTTexture((const ResTIMG*)JKRGetResource(buffer));
	}

	for (int j = 0; j < 9; ++j) {
		for (int i = 0; i < 5; ++i) {
			char buffer[0x100];
			snprintf(buffer, 0xFF, "/option/timg/%s%s", langArray[i],
			         cScoreStageFileName[j]);
			mStageTex[j][i]
			    = new JUTTexture((const ResTIMG*)JKRGetResource(buffer));
		}
	}

	for (int i = 0; i < 5; ++i) {
		char buffer[0x100];
		snprintf(buffer, 0xFF, "/option/timg/%s%s", langArray[i],
		         cScoreTotalFileName);
		mTotalTex[i] = new JUTTexture((const ResTIMG*)JKRGetResource(buffer));
	}
}

// PAL: the score-screen pane lookups of the US load(), redone whenever unk2C
// switches to another language's screen.
void TCardLoad::resetScoreScreenObjects()
{
#endif
	int local_90[] = { 2, 3, 4, 5, 6, 8, 7 };

	for (int i = 0; i < 7; ++i) {
		unk584[i].unk0 = (J2DPicture*)unk2C->search('st_0' + i);
		for (int j = 0; j < 3; ++j)
			unk584[i].unk4[j]
			    = (J2DPicture*)unk2C->search('n_0a' + i * 0x100 + j);

		int tmp = local_90[i];

		for (int j = 0; j < 8; ++j) {
			unk584[i].unk10[j] = unk2C->search('sh0a' + i * 0x100 + j);

			if (!SMS_isGetShine(tmp, j, false))
				unk584[i].unk10[j]->hide();
		}

		for (int j = 0; j < 2; ++j) {
			unk584[i].unk30[j] = unk2C->search('sh0i' + i * 0x100 + j);
			unk584[i].unk30[j]->hide();
		}

		unk584[i].unk38 = unk2C->search('sh0k' + i * 0x100);
		unk584[i].unk38->hide();
	}

	for (int i = 0; i < 3; ++i) {
		unk728[i] = unk2C->search('\0t_1' + i);
		unk728[i]->hide();
	}

	unk740 = unk2C->search('\0t_p');
	unk744 = unk2C->search('s_tl');
	unk748 = (J2DPicture*)unk2C->search('\0n_a');
	unk74C = (J2DPicture*)unk2C->search('\0n_b');
	unk750 = (J2DPicture*)unk2C->search('\0n_c');
}

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
void TCardLoad::setupTitleScreen()
{
	// "PRESS START" in the current language.
	switch (TFlagManager::getInstance()->getFlag(MSF_LANGUAGE)) {
	default:
		mTitleLetterNum = 11;
		break;
	case 1:
		mTitleLetterNum = 11;
		break;
	case 2:
		mTitleLetterNum = 15;
		break;
	case 3:
		mTitleLetterNum = 12;
		break;
	case 4:
		mTitleLetterNum = 10;
		break;
	}

	for (int i = 0; i < mTitleLetterNum; ++i) {
		unk20C[i] = 25 * i + 150;
		unk222[i] = 4;
	}

	for (int i = 0; i < ARRAY_COUNT(unk22E); ++i) {
		unk22E[i] = 400 * i;
		unk248[i] = 4;
	}
}

// PAL: fits a message window around its text (translations differ in length) and
// keeps it centred.
void TCardLoad::resizeBox(J2DWindow* window, J2DTextBox* text_a,
                          J2DTextBox* text_b)
{
	const JUTRect& bounds = window->getBounds();
	const char* str       = text_a->getStringPtr();
	JUTRect old           = bounds;

	J2DPrint print((JUTFont*)gpSystemFont, 0);
	J2DPrint::TSize size;
	print.getSize(size, str);

	JUTRect contents = window->getContentsBounds();
	JUTRect inner    = contents;
	int innerW       = inner.getWidth();
	int innerH       = inner.getHeight();
	window->resize(10.0f + (size.unk0 + (old.getWidth() - innerW)),
	               10.0f + (size.unk4 + (old.getHeight() - innerH)));

	JUTRect now = bounds;
	text_a->resize(10.0f + size.unk0, 10.0f + size.unk4);
	text_b->resize(10.0f + size.unk0, 10.0f + size.unk4);
	window->add((old.x2 - now.x2) / 2, (old.y2 - now.y2) / 2);
}
#else
void TCardLoad::setupTitleScreen()
{
	for (int i = 0; i < ARRAY_COUNT(unk20C); ++i) {
		unk20C[i] = 25 * i + 150;
		if (i > 4)
			unk20C[i] += 20;
		unk222[i] = 4;
	}

	for (int i = 0; i < ARRAY_COUNT(unk22E); ++i) {
		unk22E[i] = 400 * i;
		unk248[i] = 4;
	}
}
#endif

void TCardLoad::setupScoreScreen()
{
	int iVar2 = TFlagManager::getInstance()->getFlag(MSF_SHINE_COUNT);
	if (iVar2 < 100) {
		unk748->changeTexture(unkC8[iVar2 / 10]->getTexInfo(), 0);
		unk74C->changeTexture(unkC8[iVar2 % 10]->getTexInfo(), 0);
		unk750->hide();
	} else {
		unk748->changeTexture(unkC8[iVar2 / 100]->getTexInfo(), 0);
		unk74C->changeTexture(unkC8[(iVar2 - 100) / 10]->getTexInfo(), 0);
		unk750->changeTexture(unkC8[(iVar2 - 100) % 10]->getTexInfo(), 0);
		unk750->show();
	}

	u16 iVar8      = 0;
	int local_90[] = { 2, 3, 4, 5, 6, 7, 8 };

	for (int i = 0; i < 7; ++i) {
		u8 shineCount = 0;

		if (TFlagManager::getInstance()->getBool(MSF_VISITED_BASE
		                                         + local_90[i]))
			unk584[i].unk0->show();
		else
			unk584[i].unk0->hide();

		for (int j = 0; j < 8; ++j)
			if (SMS_isGetShine(local_90[i], j, false))
				++shineCount;

		iVar8 += shineCount;

		for (int j = 0; j < 8; ++j) {
			J2DPane* pane = unk584[i].unk10[j];
			if (!pane)
				continue;

			if (j < shineCount)
				pane->show();
			else
				pane->hide();
		}

		for (int j = 0; j < 2; ++j) {
			if (SMS_isGetShine(local_90[i], j + 1, true)) {
				iVar8 += 1;
				unk584[i].unk30[j]->show();
			} else {
				unk584[i].unk30[j]->hide();
			}
		}

		if (SMS_isGetShine(local_90[i], 0, true)) {
			iVar8 += 1;
			unk584[i].unk38->show();
		} else {
			unk584[i].unk38->hide();
		}

		int iVar3 = TFlagManager::getInstance()->getFlag(MSF_COIN_RECORD_BASE
		                                                 + local_90[i]);
		if (iVar3 > 999)
			iVar3 = 999;
		if (iVar3 < 100) {
			unk584[i].unk4[2]->hide();
			unk584[i].unk4[0]->changeTexture(unkC8[iVar3 / 10]->getTexInfo(),
			                                 0);
			unk584[i].unk4[1]->changeTexture(unkC8[iVar3 % 10]->getTexInfo(),
			                                 0);
		} else {
			unk584[i].unk4[0]->changeTexture(unkC8[iVar3 / 100]->getTexInfo(),
			                                 0);
			u8 iVar4 = iVar3 - (iVar3 / 100) * 100;
			unk584[i].unk4[1]->changeTexture(unkC8[iVar4 / 10]->getTexInfo(),
			                                 0);
			unk584[i].unk4[2]->changeTexture(unkC8[iVar4 % 10]->getTexInfo(),
			                                 0);
			unk584[i].unk4[2]->show();
		}

		unk584[i].unk0->setAlpha(0);
	}

	u16 asdf = 0;
	if (TFlagManager::getInstance()->getBool(MSF_SHINE_AIRSTRIP))
		asdf += 1;
	if (TFlagManager::getInstance()->getBool(MSF_SHINE_AIRSTRIP_REDS))
		asdf += 1;
	iVar8 += asdf;
	if (asdf > 9)
		asdf = 9;
	const ResTIMG* pRVar4 = unkC8[asdf % 10]->getTexInfo();
	((J2DPicture*)unk2C->search('\0n_f'))->changeTexture(pRVar4, 0);

	if (TFlagManager::getInstance()->getBool(MSF_PLAZA_VISITED))
		unk2C->search('st_7')->show();
	else
		unk2C->search('st_7')->hide();
	unk2C->search('st_7')->setAlpha(255);
	if (SMS_isGetShine(1, 0, true)) {
		unk2C->search('sh7a')->show();
		iVar8 += 1;
	} else {
		unk2C->search('sh7a')->hide();
	}

	int thing = TFlagManager::getInstance()->getFlag(MSF_COIN_RECORD_DELFINO);
	if (thing > 999)
		thing = 999;

	if (thing < 100) {
		unk2C->search('n_7c')->hide();
		((J2DPicture*)unk2C->search('n_7a'))
		    ->changeTexture(unkC8[thing / 10]->getTexInfo(), 0);
		((J2DPicture*)unk2C->search('n_7b'))
		    ->changeTexture(unkC8[thing % 10]->getTexInfo(), 0);
	} else {
		unk2C->search('n_7c')->show();
		((J2DPicture*)unk2C->search('n_7a'))
		    ->changeTexture(unkC8[thing / 100]->getTexInfo(), 0);
		thing -= thing / 100 * 100;
		((J2DPicture*)unk2C->search('n_7b'))
		    ->changeTexture(unkC8[thing / 10]->getTexInfo(), 0);
		((J2DPicture*)unk2C->search('n_7c'))
		    ->changeTexture(unkC8[thing % 10]->getTexInfo(), 0);
	}

	int asdf2 = TFlagManager::getInstance()->getFlag(MSF_SHINE_COUNT) - iVar8;
	if (asdf2 > 100)
		asdf2 = 99;
	if (asdf2 < 0)
		asdf2 = 0;
	((J2DPicture*)unk2C->search('\0n_d'))
	    ->changeTexture(unkC8[asdf2 / 10]->getTexInfo(), 0);
	((J2DPicture*)unk2C->search('\0n_e'))
	    ->changeTexture(unkC8[asdf2 % 10]->getTexInfo(), 0);

	unk744->setAlpha(0);
	unk740->setAlpha(0);
}

#ifdef SMS_PORT_AURORA
extern "C" void port_mp_block_spawn(void* blockA); // port_mp_block.cpp
extern "C" void port_mp_block_hit(int which);
extern "C" int port_mp_block_panned(void);
extern "C" void port_mp_block_dump_screen(void* root);
extern "C" void port_mp_block_set_2d(int alpha);
extern "C" void port_mp_block_draw2d(const void* graph);
#endif

void TCardLoad::loadAfter()
{
	JDrama::TNameRef::loadAfter();
	unk278[0] = static_cast<TFileLoadBlock*>(
	    JDrama::TNameRefGen::search("ロードブロックＡ"));
	unk278[1] = static_cast<TFileLoadBlock*>(
	    JDrama::TNameRefGen::search("ロードブロックＢ"));
	unk278[2] = static_cast<TFileLoadBlock*>(
	    JDrama::TNameRefGen::search("ロードブロックＣ"));
	unk284 = static_cast<TMapObjOptionWall*>(
	    JDrama::TNameRefGen::search("オプション用壁"));
#ifdef SMS_PORT_AURORA
	port_mp_block_spawn(unk278[0]); // the multiplayer block (port_mp_block.cpp)
#endif
}

#ifdef SMS_PORT_AURORA
// port/src/port_mp_game.cpp (w10): multiplayer entry (Mario runs off the left of the
// save-block screen) and free-mode start. Does nothing unless multiplayer is used.
extern "C" void port_mp_cardload_perform(int marioHasControl);
#endif

void TCardLoad::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (cue & CUE_MOVE) {
#ifdef SMS_PORT_AURORA
		port_mp_cardload_perform(
		    unk14 == 0 && !unk38->checkFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT));
		if (unk14 == 0 && unk1C == PROGRESS_UNK2)
			port_mp_block_dump_screen(unk28->search('ROOT'));
		{ // the multiplayer block: the 2D file select fades out while the camera is over it
			static bool sMpFaded;
			const bool panned = port_mp_block_panned() != 0;
			if (unk14 == 0 && (panned || sMpFaded)) {
				J2DPane* root = unk28->search('ROOT');
				int alpha     = root->getAlpha() + (panned ? -24 : 24);
				alpha         = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
				root->setAlpha(alpha);
				unk25C->setAlpha(alpha < unk25C->getAlpha() ? alpha : unk25C->getAlpha());
				// the text boxes ("Select data.", "New") keep their own alpha: hide it all
				if (alpha == 0)
					root->hide();
				else
					root->show();
				port_mp_block_set_2d(255 - alpha); // the multiplayer section's own boxes
				sMpFaded = panned || alpha < 255;
			}
		}
#endif
		switch (unk14) {
		case 0: {
			changeScene();
			int alpha = unk25C->getAlpha();
			if (unk275 && alpha < 255) {
				alpha += 8;
				if (alpha > 255)
					alpha = 255;
				unk25C->setAlpha(alpha);
			}

			if (!unk275 && alpha > 0) {
				alpha -= 8;
				if (alpha < 0)
					alpha = 0;
				unk25C->setAlpha(alpha);
			}

			if (alpha != 0) {
				// TODO: copy-pasta from TArrowControl::calcMoveX? inline?
				int iVar9 = unk274;
				int iVar3 = unk274 < 50 ? iVar9 : 100 - iVar9;
				f32 fVar1 = iVar3 / 50.0f;
				f32 fVar2 = 1.0f - fVar1;
				s16 iVar8 = fVar1 * 8.0f * fVar1 + fVar2 * -8.0f * fVar2
				            + fVar1 * fVar2;
				unk25C->setBounds(JUTRect(unk260.x1, unk260.y1,
				                          unk260.x2 + iVar8, unk260.y2));
				unk274 += 1;
				if (unk274 > 100)
					unk274 = 0;
			}

			if (unk1C == 19 || unk1C == 12 || unk1C == 13 || unk1C == 3
			    || unk1C == 4 || unk1C == 5 || unk1C == 45 || unk1C == 16
#ifdef VERSION_GMSP01
			    || unk1C == 0x35
#endif
			) {
				unk284->offCollision();
				if (!(gpCameraOption->unk0 & 1)) {
					if (unk10 == 2)
						unk10 = 3;
					if (unk10 == 5)
						unk14 = 6;
				}
			}
		} break;

		case 4: {
			int rc = gpCardManager->getLastStatus();
			if (rc != CARD_RESULT_BUSY) {
				if (rc == CARD_RESULT_READY) {
					if (unkB8 == 0) {
						loadBookmark();
						setupScoreScreen();
						unkB8 = 1;
					}
				} else {
					unk14 = 0;
					unk1C = PROGRESS_UNK3;
					unk10 = 1;
				}
			}

			if (gpCameraOption->unk16 != 0)
				unkB8 = 0;

			if (unkB8 != 0) {
				J2DPane* root1 = unk2C->search('ROOT');
				J2DPane* root2 = unk28->search('ROOT');

				bool done  = true;
				int alpha1 = root1->getAlpha() + 4;
				if (alpha1 > 255)
					alpha1 = 255;
				else
					done = false;
				root1->setAlpha(alpha1);
				for (int i = 0; i < 7; ++i)
					unk584[i].unk0->setAlpha(alpha1);
				unk744->setAlpha(alpha1);
				unk740->setAlpha(alpha1);

				int alpha2 = root2->getAlpha() - 4;
				if (alpha2 < 0)
					alpha2 = 0;
				else
					done = false;
				root2->setAlpha(alpha2);

				if (done)
					unk14 = 1;
			}
		} break;

		case 5: {
			J2DPane* root1 = unk2C->search('ROOT');
			J2DPane* root2 = unk28->search('ROOT');

			bool done = true;

			// Fade the score screen out and the file select back in
			// (the reverse of case 4).
			int alpha1 = root1->getAlpha() - 4;
			if (alpha1 < 0)
				alpha1 = 0;
			else
				done = false;
			root1->setAlpha(alpha1);
			for (int i = 0; i < 7; ++i)
				unk584[i].unk0->setAlpha(alpha1);
			unk744->setAlpha(alpha1);
			unk740->setAlpha(alpha1);

			int alpha2 = root2->getAlpha() + 4;
			if (alpha2 > 255)
				alpha2 = 255;
			else
				done = false;
			root2->setAlpha(alpha2);

			if (done) {
				if (unkB8) {
					gpCameraOption->moveToDown();
					unkB8 = 0;
				} else if (gpCameraOption->unk16 == 0) {
					unk14 = 0;
					unk1C = PROGRESS_UNK2;
					gpCardManager->getBookmarkInfos(unk40);
					unk10 = 0;
				}
			}
		} break;
		case 1:
			if (unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_A)
			    || unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_B)) {
				SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_CANCEL_COMMON, 0,
				                                   nullptr, 0);
				unkB8 = 1;
				unk14 = 5;
			}
			break;

		case 6:
			if (unk754->movementCard2Option()) {
				unk38->offFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
				unk14 = 2;
			} else if (gpCameraOption->unk0 & 1) {
				unk14 = 7;
			}
			break;

		case 2:
			if (unk754->movementOption()) {
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
				changeLanguage(
				    TFlagManager::getInstance()->getFlag(MSF_LANGUAGE));
#endif
				unk14 = 7;
			}
			break;

		case 7:
			if (!(gpCameraOption->unk0 & 1)) {
				unk14 = 6;
			} else {
				if (unk754->movementOption2Card()) {
					unk284->onCollision();
					unk14 = 0;
					unk38->onFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
					if (unk754->isChangedSetting()) {
						gpCardManager->getBookmarkInfos(unk40);
						unk1C = PROGRESS_UNK32;
						unk10 = 0;
					} else {
						gpCardManager->getBookmarkInfos(unk40);
						unk1C = PROGRESS_UNK2;
						unk10 = 0;
					}
					unk754->resetChangedSetting();
				}
			}
			break;

		case 8: {
			int alpha = (s16)(unk208->getAlpha() - 4);
			if (alpha < 0)
				alpha = 0;
			unk208->setAlpha(alpha);
			if (gpCameraOption->unkE == 0) {
				MSBgm::startBGM(MSD_BGM_BOSSPAKU_DEMO);
				unk1C = PROGRESS_UNK30;
				unk14 = 0;
			}
		} break;

		case 3:
			titleDraw();
			if (unk18 >= 4) {
				if (unk18 >= 4 && unkBC >= 100 && gpCameraOption->unkA == 0
				    && (unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_A)
				        || unk38->getTrigger() & 0x1000)) {
					SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_DECIDE, 0,
					                                   nullptr, 0);
					gpCameraOption->moveToLoadFromTitle();
					gpMarioOriginal->waitingStart(nullptr, 0.0f);
					unk14 = 8;
				}
				unkF0->update();
				unkF4->update();
				unkBC += 1;
			} else if (unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_A)
			           || unk38->getTrigger() & 0x1000) {
				unkF0->setPaneAlpha(10, 255, unkF0->getPane()->getAlpha());
				unkF4->setPaneAlpha(10, 255, unkF4->getPane()->getAlpha());
				for (int i = 0; i < ARRAY_COUNT(unk1D4); ++i)
					unk1D4[i]->getPane()->setAlpha(0);
				unk258 = 0;
				if (unk18 < 4)
					unk18 = 4;
			}

			if (unkC0 / 120.0f > 45.0f) {
				if (TFlagManager::getInstance()->getBool(MSF_UNK3001C)) {
					gpMarDirector->fireStreamingMovie(9);
					TFlagManager::getInstance()->setBool(false, MSF_UNK3001C);
					unkC0 = 0;
				} else {
					gpMarDirector->fireStreamingMovie(12);
					TFlagManager::getInstance()->setBool(true, MSF_UNK3001C);
					unkC0 = 0;
				}
			}
			break;

		case 9:
			if (gpCameraOption->unkA == 0)
				unk14 = 3;

			if (unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_A)
			    || unk38->getTrigger() & 0x1000) {
				unkF0->setPaneAlpha(10, 255, unkF0->getPane()->getAlpha());
				unkF4->setPaneAlpha(10, 255, unkF4->getPane()->getAlpha());
				for (int i = 0; i < ARRAY_COUNT(unk1D4); ++i)
					unk1D4[i]->getPane()->setAlpha(0);
				unk258 = 0;
				unkBC  = 0;
				unk18  = 4;
				unk14  = 3;
			}
			titleDraw();
			break;

		case 10:
			MSBgm::startBGM(MSD_BGM_CHUBOSS2);
			unk38->onFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
			gpMarioOriginal->offFlag(MARIO_FLAG_HAS_FLUDD);
			unk14 = 9;
			unk18 = 1;
			unkF0->getPane()->setAlpha(0);
			unkF4->getPane()->setAlpha(0);
			unk258 = 0;
			unkC0  = 0;
			unk208->show();
			break;
		}

		if (unkC0 < 0x0FFFFFFF)
			unkC0 += 1;
	}

	if (cue & CUE_DRAW) {
		const JDrama::TRect& rect = graphics->getScissor();
		J2DOrthoGraph graph(graphics->getViewport());
		graph.setup2D();
		switch (unk14) {
		case 0:
			unk28->draw(0, 0, &graph);
#ifdef SMS_PORT_AURORA
			port_mp_block_draw2d(&graph);
#endif
			break;
		case 1:
			unk2C->draw(0, 0, &graph);
			break;
		case 2:
			unk754->draw(&graph);
			break;
		case 4:
		case 5:
			unk2C->draw(0, 0, &graph);
			graph.setup2D();
			unk28->draw(0, 0, &graph);
			break;
		case 3:
		case 8:
		case 9:
			unk34->draw(0, 0, &graph);
			break;
		}

		graphics->setScissor(rect);
	}
}

// TODO: retail compiles this switch to an eight entry jump table, with the
// slots for cases 5 through 7 pointing at the default, while we get a compare
// chain. That one difference accounts for nearly all of the remaining diff --
// the rest is register allocation downstream of it. The case set matches
// retail (our chain does range-check 5 through 7, so they are not being
// pruned), the body layout order matches, and the frame is 0x20 bytes smaller
// than retail's. Typing the state as an enum spanning 0-7, moving the empty
// cases to the front, and making the field unsigned all fail to flip it.
bool TCardLoad::titleDraw()
{
	switch (unk18) {
	case 0:
		unk18 = 1;
		break;

	case 1:
		for (int i = 0; i < ARRAY_COUNT(unk1D4); ++i) {
			switch (unk248[i]) {
			case 4:
				if (unk258 > unk22E[i]) {
					unk1D4[i]->getPane()->show();
					JUTRect bounds = unk1D4[i]->getPane()->getBounds();
					unk1D4[i]->setPaneAlpha(40, 180, 0);
					unk248[i] = 0;
				}
				break;

			case 0:
				if (unk1D4[i]->update()) {
					unk248[i] = 1;
					unk22E[i] = 0;
				}
				break;

			case 1:
				if (i == ARRAY_COUNT(unk1D4) - 1) {
					unk258 = 0;
					for (int j = 0; j < ARRAY_COUNT(unk1D4); ++j)
						unk1D4[j]->setPaneAlpha(40, 255, 180);
					unk18 = 3;
				}
				break;
			}

			++unk258;
		}
		break;

	case 3:
		if (unk258 > 160) {
			bool any = true;
			for (int i = 0; i < ARRAY_COUNT(unk1D4); ++i) {
				any &= unk1D4[i]->update();
				JUtility::TColor col
				    = ((J2DPicture*)unk1D4[i]->getPane())->mBlack;
				u16 r = col.r + 7;
				if (r > 255)
					r = 255;

				((J2DPicture*)unk1D4[i]->getPane())->mBlack
				    = (r << 24) + 0xFFFF00;
			}

			if (any) {
				for (int i = 0; i < ARRAY_COUNT(unk1D4); ++i)
					unk1D4[i]->setPaneAlpha(140, 0, 255);
				unk258 = 0;
				unk18  = 2;
			}
		}
		++unk258;
		break;

	case 2: {
		u16 alpha = unkF0->getPane()->getAlpha() + 1;
		if (alpha > 255) {
			alpha    = 255;
			bool any = true;
			for (int i = 0; i < ARRAY_COUNT(unk1D4); ++i)
				any &= unk1D4[i]->update();
			if (any) {
				unkBC = 0;
				unk18 = 4;
			}
		}
		unkF0->getPane()->setAlpha(alpha);
		unkF4->getPane()->setAlpha(alpha);
	} break;

	case 4: {
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
		for (int i = 0; i < mTitleLetterNum; ++i) {
#else
		for (int i = 0; i < 11; ++i) {
#endif
			switch (unk222[i]) {
			case 0:
				if (unkF8[i]->update()) {
					unk222[i] = 1;
					unk20C[i] = 0;
				}
				break;

			case 1:
				unk20C[i] += 1;
				if (unk20C[i] > CARDLOAD_TITLE_SHOW_TIME) {
					JUTRect bounds = unkF8[i]->getPane()->getBounds();
					unkF8[i]->setPaneAlpha(25, 0, 255);
					unk222[i] = 2;
				}
				break;

			case 2:
				if (unkF8[i]->update()) {
					unk20C[i] = 0;
					unk222[i] = 3;
				}
				break;

			case 3:
				++unk20C[i];
				if (unk20C[i] > CARDLOAD_TITLE_HIDE_TIME) {
					JUTRect local_124 = unkF8[i]->getPane()->getBounds();

					unkF8[i]->setCenteredSize(
					    25, local_124.getWidth() * 2, local_124.getHeight() * 2,
					    local_124.getWidth(), local_124.getHeight());

					unkF8[i]->setPaneAlpha(25, 255, 0);
					unk222[i] = 0;
				}
				break;

			case 4:
				if (unk258 > unk20C[i]) {
					JUTRect local_124 = unk124[i];
					unkF8[i]->setCenteredSize(
					    25, local_124.getWidth(), local_124.getHeight(),
					    local_124.getWidth() * 2, local_124.getHeight() * 2);
					unkF8[i]->setPaneAlpha(25, 255, 0);
					unk222[i] = 0;
					unk20C[i] = 0;
				}
				break;
			}
		}
		++unk258;
	} break;

	case 5:
	case 6:
	case 7:
		break;
	}

	return unk18 > 4;
}

void TCardLoad::makeBuffer(J2DTextBox* text_box, int size)
{
	SMSMakeTextBuffer(text_box, size);
	text_box->setFont((JUTFont*)gpSystemFont);
}

TEProgress TCardLoad::changeMode(s32 param_1)
{
	gpCardManager->probe();
	TEProgress result = PROGRESS_UNK4;
	switch (param_1) {
	case CARD_RESULT_NOCARD:
		result = PROGRESS_UNK3;
		break;
	case CARD_RESULT_WRONGDEVICE:
	case -0x80:
		result = PROGRESS_UNK4;
		break;
	case CARD_RESULT_IOERROR:
		result = PROGRESS_UNK2D;
		break;
	case -0x100:
		result = PROGRESS_UNK5;
		break;
	case CARD_RESULT_BROKEN:
		result = PROGRESS_UNK6;
		break;
	case CARD_RESULT_ENCODING:
		result = PROGRESS_UNK7;
		break;
	case CARD_RESULT_INSSPACE:
		result = PROGRESS_UNKC;
		break;
	case CARD_RESULT_NOENT:
		result = PROGRESS_UNKD;
		break;
	case CARD_RESULT_NOFILE:
		result = PROGRESS_UNKE;
		break;
	}
	return result;
}

void TCardLoad::setMessage(J2DTextBox* text_box, s32 param_2, int param_3)
{
	strncpy(text_box->getStringPtr(), SMSGetMessageData(unkA0, param_3),
	        param_2);
}

s8 TCardLoad::waitForChoice(TEProgress param_1, TEProgress param_2, int param_3)
{
	s8 result = -1;

	switch (unk10) {
	case 0:
		setMessage(unk47C, 0x400, (u16)cMessageID[unk1C]);
		setMessage(unk480, 0x400, (u16)cMessageID[unk1C]);

		unk468->getPane()->show();

		if (param_3 == 1) {
			((J2DPicture*)unk484[0]->getPane())->mWhite = 0xFFFFFFFF;
			((J2DPicture*)unk484[1]->getPane())->mWhite = 0x00FF00FF;
		} else {
			((J2DPicture*)unk484[1]->getPane())->mWhite = 0xFFFFFFFF;
			((J2DPicture*)unk484[0]->getPane())->mWhite = 0x00FF00FF;
		}
		unk480->hide();
		unk484[0]->getPane()->hide();
		unk484[1]->getPane()->hide();
		unk468->setCenteredSize(20, unk46C.getWidth(), unk46C.getHeight(), 0,
		                        0);

		unkB7 = param_3;
		unk10 = 1;
		break;

	case 1:
		if (unk468->update()) {
			unk480->show();
			unk484[0]->getPane()->show();
			unk484[1]->getPane()->show();
			unkC4 = 0;
			unk10 = 2;
		}
		break;

	case 2: {
		s8 old = unkB7;
		if (unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_LEFT)) {
			unkB7 = 0;
		} else if (unk38->checkFrameMeaning(
		               TMarioGamePad::MEANING_MENU_RIGHT)) {
			unkB7 = 1;
		} else if (unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_A)) {
			if (old == 0)
				SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_SELECT_COMMON, 0,
				                                   nullptr, 0);
			else
				SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_CANCEL_COMMON, 0,
				                                   nullptr, 0);
			unk10 = 3;
		} else if (unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_B)) {
			unkB7 = 1;
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_CANCEL_COMMON, 0,
			                                   nullptr, 0);
			unk10 = 3;
		}

		if (unkC4 == 4) {
			unk484[unkB7]->setCenteredSize(40, unk48C[unkB7].getWidth() * 1.5f,
			                               unk48C[unkB7].getHeight() * 1.5f,
			                               unk48C[unkB7].getWidth(),
			                               unk48C[unkB7].getHeight());
			JUTRect bounds = unk484[unkB7]->getPane()->mGlobalBounds;
			JGeometry::TVec3<f32> local_108;
			local_108.x = bounds.x1 + bounds.getWidth() * 0.5f;
			local_108.y = bounds.y1 + bounds.getHeight() * 0.5f;
			local_108.z = 0.0f;
			gpEmitterManager4D2->createEmitter(local_108, 0x1FA, nullptr,
			                                   nullptr);
			unkAC = gpEmitterManager4D2->unkC8[0][0];
			unkAC->setRotation(0, 0, DEG2SHORTANGLE(12));
			unkAC->setEmitterScale(JGeometry::TVec3<f32>(0.9f, 1.0f, 0.1f));
		} else if (unkC4 == 44) {
			unk484[unkB7]->setCenteredSize(40, unk48C[unkB7].getWidth(),
			                               unk48C[unkB7].getHeight(),
			                               unk48C[unkB7].getWidth() * 1.5f,
			                               unk48C[unkB7].getHeight() * 1.5f);
		}

		int thing = unkC4 < 40 ? unkC4 : 80 - unkC4;
		thing     = (thing * 255.0f) / 40.0f;
		((J2DPicture*)unk484[unkB7]->getPane())->mWhite
		    = (thing << 24) + 0xFF00FF;
		unk484[unkB7]->update();
		unkC4 += 1;
		if (unkC4 > 80)
			unkC4 = 0;
		if (old != unkB7) {
			if (unkAC != nullptr)
				gpEmitterManager4D2->forceDeleteEmitter(unkAC);

			unkC4 = 0;

			unk484[old]->getPane()->mBounds = unk48C[old];
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_E3_MENU_CURSOR, 0,
			                                   nullptr, 0);
			((J2DPicture*)unk484[old]->getPane())->mWhite   = 0xFFFFFFFF;
			((J2DPicture*)unk484[unkB7]->getPane())->mWhite = 0x00FF00FF;
		}
	} break;

	case 3:
		if (unkB7 == -1)
			unkB7 = 2;
		unk468->setCenteredSize(20, 0, 0, unk46C.getWidth(),
		                        unk46C.getHeight());
		unk480->hide();
		unk484[0]->getPane()->hide();
		unk484[1]->getPane()->hide();
		unk484[0]->getPane()->mBounds = unk48C[0];
		unk484[1]->getPane()->mBounds = unk48C[1];

		unk10 = 4;
		break;

	case 4:
		if (unk468->update()) {
			unk468->getPane()->hide();
			unk10 = 5;
		}
		break;

	case 5:
		if (unkB7 == 0)
			unk1C = param_1;
		else
			unk1C = param_2;
		result = unkB7;
		break;
	}

	return result;
}

s8 TCardLoad::waitForChoiceBM(TEProgress param_1, TEProgress param_2,
                              int param_3)
{
	s8 result = -1;

	switch (unk10) {
	case 0:
		setMessage(unk4C0, 0x400, (u16)cMessageID[unk1C]);
		setMessage(unk4C4, 0x400, (u16)cMessageID[unk1C]);

		unk4AC->getPane()->show();

		if (param_3 == 1) {
			((J2DPicture*)unk4D8[0]->getPane())->mWhite = 0xFFFFFFFF;
			unk4D8[0]->getPane()->hide();
			((J2DPicture*)unk4D8[1]->getPane())->mWhite = 0x00FF00FF;
			unk4D8[1]->getPane()->hide();
		} else {
			((J2DPicture*)unk4D8[1]->getPane())->mWhite = 0xFFFFFFFF;
			unk4D8[1]->getPane()->hide();
			((J2DPicture*)unk4D8[0]->getPane())->mWhite = 0x00FF00FF;
			unk4D8[0]->getPane()->hide();
		}
		unk500->hide();
		unk510->hide();
		unk520->hide();
		unk4C4->hide();
		unk4C8->hide();
		for (int i = 0; i < 3; ++i)
			unk4CC[i]->hide();
		unk4AC->setCenteredSize(20, unk4B0.getWidth(), unk4B0.getHeight(), 0,
		                        0);

		unkB7 = param_3;
		unk10 = 1;
		break;

	case 1:
		if (unk4AC->update()) {
			unk4C4->show();
			unk4D8[0]->getPane()->show();
			unk4D8[1]->getPane()->show();
			unk4C8->show();
			unk4CC[unkB0]->show();
			TCardBookmarkInfo& info = unk40[unkB0];
			if (info.unk0 == 1) {
				CARDLOAD_SET_BROKEN(unk51C, 0x14);
				CARDLOAD_SET_BROKEN(unk520, 0x14);
				unk520->show();
				unk500->hide();
				unk510->hide();
			} else if (info.unk18 == 0) {
				CARDLOAD_SET_NEW(unk51C, 0x14);
				CARDLOAD_SET_NEW(unk520, 0x14);
				unk520->show();
				unk500->hide();
				unk510->hide();
			} else {
				unk520->hide();
				TCardBookmarkInfo* bm2 = &unk40[unkB0];
				u16 score              = bm2->unk1C;
				if (score > 999)
					score = 999;
				if (score < 100) {
					unk500->hide();
					unk510->show();
					unk514[0]->changeTexture(unkC8[score / 10]->getTexInfo(),
					                         0);
					unk514[1]->changeTexture(unkC8[score % 10]->getTexInfo(),
					                         0);
				} else {
					unk500->show();
					unk510->hide();
					unk504[0]->changeTexture(unkC8[score / 100]->getTexInfo(),
					                         0);
					score -= int(score * 0.01f) * 100;
					unk504[1]->changeTexture(unkC8[score / 10]->getTexInfo(),
					                         0);
					unk504[2]->changeTexture(unkC8[score % 10]->getTexInfo(),
					                         0);
				}
			}
			unkC4 = 0;
			unk10 = 2;
		}
		break;

	case 2: {
		s8 old = unkB7;
		if (unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_LEFT)) {
			unkB7 = 0;
		} else if (unk38->checkFrameMeaning(
		               TMarioGamePad::MEANING_MENU_RIGHT)) {
			unkB7 = 1;
		} else if (unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_A)) {
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_SELECT_COMMON, 0,
			                                   nullptr, 0);
			unk10 = 3;
		} else if (unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_B)) {
			unkB7 = 1;
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_CANCEL_COMMON, 0,
			                                   nullptr, 0);
			unk10 = 3;
		}

		if (unkC4 == 4) {
			unk4D8[unkB7]->setCenteredSize(40, unk4E0[unkB7].getWidth() * 1.5f,
			                               unk4E0[unkB7].getHeight() * 1.5f,
			                               unk4E0[unkB7].getWidth(),
			                               unk4E0[unkB7].getHeight());
			JUTRect bounds = unk4D8[unkB7]->getPane()->mGlobalBounds;
			JGeometry::TVec3<f32> local_108;
			local_108.x = bounds.x1 + bounds.getWidth() * 0.5f;
			local_108.y = bounds.y1 + bounds.getHeight() * 0.5f;
			local_108.z = 0.0f;
			gpEmitterManager4D2->createEmitter(local_108, 0x1FA, nullptr,
			                                   nullptr);
			unkAC = gpEmitterManager4D2->unkC8[0][0];
			unkAC->setRotation(0, 0, DEG2SHORTANGLE(12));
			unkAC->setEmitterScale(JGeometry::TVec3<f32>(0.9f, 1.0f, 0.1f));
		} else if (unkC4 == 44) {
			unk4D8[unkB7]->setCenteredSize(40, unk4E0[unkB7].getWidth(),
			                               unk4E0[unkB7].getHeight(),
			                               unk4E0[unkB7].getWidth() * 1.5f,
			                               unk4E0[unkB7].getHeight() * 1.5f);
		}

		int thing = unkC4 < 40 ? unkC4 : 80 - unkC4;
		thing     = (thing * 255.0f) / 40.0f;
		((J2DPicture*)unk4D8[unkB7]->getPane())->mWhite
		    = (thing << 24) + 0xFF00FF;
		unk4D8[unkB7]->update();
		unkC4 += 1;
		if (unkC4 > 80)
			unkC4 = 0;
		if (old != unkB7) {
			if (unkAC != nullptr)
				gpEmitterManager4D2->forceDeleteEmitter(unkAC);

			unkC4 = 0;

			unk4D8[old]->getPane()->mBounds = unk4E0[old];
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_E3_MENU_CURSOR, 0,
			                                   nullptr, 0);
			((J2DPicture*)unk4D8[old]->getPane())->mWhite   = 0xFFFFFFFF;
			((J2DPicture*)unk4D8[unkB7]->getPane())->mWhite = 0x00FF00FF;
		}
	} break;

	case 3: {
		unk4AC->setCenteredSize(20, 0, 0, unk4B0.getWidth(),
		                        unk4B0.getHeight());
		unk4C8->hide();
		unk500->hide();
		unk510->hide();
		unk520->hide();
		unk4C4->hide();
		unk4D8[0]->getPane()->hide();
		unk4D8[1]->getPane()->hide();
		unk4D8[0]->getPane()->mBounds = unk4E0[0];
		unk4D8[1]->getPane()->mBounds = unk4E0[1];

		unk10 = 4;
	} break;

	case 4:
		if (unk4AC->update()) {
			unk4AC->getPane()->hide();
			unk10 = 5;
		}
		break;

	case 5:
		if (unkB7 == 0)
			unk1C = param_1;
		else
			unk1C = param_2;
		result = unkB7;
		break;
	}

	return result;
}

s8 TCardLoad::waitForAnyKey(TEProgress progress)
{
	s8 result = -1;

	switch (unk10) {
	case 0: {
		setMessage(unk57C, 0x400, (u16)cMessageID[unk1C]);
		setMessage(unk580, 0x400, (u16)cMessageID[unk1C]);

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
		resizeBox((J2DWindow*)unk568->getPane(), unk580, unk57C);
		unk580->hide();
		unk568->getPane()->show();
		JUTRect rect = unk568->getPane()->getBounds();
		unk568->setCenteredSize(20, rect.getWidth(), rect.getHeight(), 0, 0);
#else
		unk580->hide();
		unk568->getPane()->show();
		unk568->setCenteredSize(20, unk56C.getWidth(), unk56C.getHeight(), 0,
		                        0);
#endif
		unk10 = 1;
	} break;

	case 1:
		if (unk568->update()) {
			unk580->show();
			unkB4 = 0;
			unk10 = 2;
		}
		break;

	case 2:
		if (unkB4 > 600
		    || unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_A
		                                | TMarioGamePad::MEANING_MENU_B)) {
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_SELECT_COMMON, 0,
			                                   nullptr, 0);
			unk10 = 3;
		} else {
			++unkB4;
		}
		break;

	case 3: {
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
		JUTRect rect = unk568->getPane()->getBounds();
		unk568->setCenteredSize(20, 0, 0, rect.getWidth(), rect.getHeight());
#else
		unk568->setCenteredSize(20, 0, 0, unk56C.getWidth(),
		                        unk56C.getHeight());
#endif
		unk580->hide();
		unkB4 = 0;
		unk10 = 4;
	} break;

	case 4:
		if (unk568->update()) {
			unk568->getPane()->hide();
			unk10 = 5;
		}
		break;

	case 5:
		result = 0;
		unk1C  = progress;
		break;
	}

	return result;
}

s8 TCardLoad::waitForAnyKeyBM(TEProgress param_1)
{
	s8 result = -1;

	switch (unk10) {
	case 0:
		setMessage(unk4C0, 0x400, (u16)cMessageID[unk1C]);
		setMessage(unk4C4, 0x400, (u16)cMessageID[unk1C]);

		unk4AC->getPane()->show();
		unk500->hide();
		unk510->hide();
		unk520->hide();
		unk4C4->hide();
		unk4C8->hide();
		for (int i = 0; i < 3; ++i)
			unk4CC[i]->hide();

		unk4AC->setCenteredSize(20, unk4B0.getWidth(), unk4B0.getHeight(), 0,
		                        0);
		unk10 = 1;
		break;

	case 1:
		if (unk4AC->update()) {
			unk4C4->show();
			unk4C8->show();
			unk4CC[unkB0]->show();
			if (unk1C == PROGRESS_UNK27)
				unkB0 = unkB1;
			TCardBookmarkInfo& info = unk40[unkB0];
			if (info.unk0 == 1) {
				CARDLOAD_SET_BROKEN(unk51C, 0x14);
				CARDLOAD_SET_BROKEN(unk520, 0x14);
				unk520->show();
				unk500->hide();
				unk510->hide();
			} else if (info.unk18 == 0) {
				CARDLOAD_SET_NEW(unk51C, 0x14);
				CARDLOAD_SET_NEW(unk520, 0x14);
				unk520->show();
				unk500->hide();
				unk510->hide();
			} else {
				unk500->show();
				unk520->hide();
				TCardBookmarkInfo* bm2 = &unk40[unkB0];
				u16 score              = bm2->unk1C;
				if (score > 999)
					score = 999;
				if (score < 100) {
					unk500->hide();
					unk510->show();
					unk514[0]->changeTexture(unkC8[score / 10]->getTexInfo(),
					                         0);
					unk514[1]->changeTexture(unkC8[score % 10]->getTexInfo(),
					                         0);
				} else {
					unk500->show();
					unk510->hide();
					unk504[0]->changeTexture(unkC8[score / 100]->getTexInfo(),
					                         0);
					score -= int(score * 0.01f) * 100;
					unk504[1]->changeTexture(unkC8[score / 10]->getTexInfo(),
					                         0);
					unk504[2]->changeTexture(unkC8[score % 10]->getTexInfo(),
					                         0);
				}
			}
			unkB4 = 0;
			unk10 = 2;
		}
		break;

	case 2: {
		if (unkB4 > 600
		    || unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_A
		                                | TMarioGamePad::MEANING_MENU_B)) {
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_SELECT_COMMON, 0,
			                                   nullptr, 0);
			unk10 = 3;
		} else {
			++unkB4;
		}
	} break;

	case 3:
		unk4AC->setCenteredSize(20, 0, 0, unk4B0.getWidth(),
		                        unk4B0.getHeight());
		unk4C4->hide();
		unk500->hide();
		unk510->hide();
		unk520->hide();
		unk4C8->hide();
		unkB4 = 0;
		unk10 = 4;
		break;

	case 4:
		if (unk4AC->update()) {
			unk4AC->getPane()->hide();
			unk10 = 5;
		}
		break;

	case 5:
		result = 0;
		unk1C  = param_1;
		break;
	}

	return result;
}

s8 TCardLoad::waitForStart(TEProgress param_1)
{
	s8 result = -1;

	switch (unk10) {
	case 0:
		setMessage(unk538, 0x400, (u16)cMessageID[unk1C]);
		setMessage(unk53C, 0x400, (u16)cMessageID[unk1C]);
#ifdef VERSION_GMSP01
		setMessage(unk560, 0x400, 13);
		setMessage(unk564, 0x400, 13);
#else
		setMessage(unk560, 0x400, 12);
		setMessage(unk564, 0x400, 12);
#endif

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
		resizeBox((J2DWindow*)unk524->getPane(), unk53C, unk538);
		unk524->getPane()->show();
		{
			JUTRect rect = unk524->getPane()->getBounds();
			unk524->setCenteredSize(20, rect.getWidth(), rect.getHeight(), 0,
			                        0);
		}
#else
		unk524->getPane()->show();
		unk524->setCenteredSize(20, unk528.getWidth(), unk528.getHeight(), 0,
		                        0);
#endif
		unk564->hide();
		if (unk1C == 12 || unk1C == 13) {
			unk54C->getPane()->show();
			unk54C->setCenteredSize(20, unk550.getWidth(), unk550.getHeight(),
			                        0, 0);
		}
		unk10 = 1;
		break;

	case 1: {
		bool allDone = true;
		allDone &= unk524->update();
		allDone &= unk54C->update();
		if (allDone) {
			unk10 = 2;
			unk53C->show();
			unk564->show();
			for (int i = 0; i < 3; ++i)
				unk540[i]->show();
			unkB0 = -1;
		}
	} break;

	case 2:
		if (unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_A)
		    || unkB0 != -1)
			unk10 = 3;
		break;

	case 3:
		unk53C->hide();
		for (int i = 0; i < 3; ++i)
			unk540[i]->hide();
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
		{
			JUTRect rect = unk524->getPane()->getBounds();
			unk524->setCenteredSize(20, 0, 0, rect.getWidth(),
			                        rect.getHeight());
		}
#else
		unk524->setCenteredSize(20, 0, 0, unk528.getWidth(),
		                        unk528.getHeight());
#endif
		unk564->hide();
		unk54C->setCenteredSize(20, 0, 0, unk550.getWidth(),
		                        unk550.getHeight());
		unk10 = 4;
		break;

	case 4: {
		bool allDone = true;
		allDone &= unk524->update();
		allDone &= unk54C->update();
		if (allDone) {
			unk524->getPane()->hide();
			unk54C->getPane()->hide();
			unk10 = 5;
		}
	} break;

	case 5:
		unk1C  = param_1;
		result = 0;
		break;
	}

	return result;
}

s8 TCardLoad::drawMessage(TEProgress param_1)
{
	switch (unk10) {
	case 0:
		setMessage(unk57C, 0x400, (u16)cMessageID[unk1C]);
		setMessage(unk580, 0x400, (u16)cMessageID[unk1C]);

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
		resizeBox((J2DWindow*)unk568->getPane(), unk580, unk57C);
		unk568->getPane()->show();
		unk580->hide();
		{
			JUTRect rect = unk568->getPane()->getBounds();
			unk568->setCenteredSize(20, rect.getWidth(), rect.getHeight(), 0,
			                        0);
		}
#else
		unk568->getPane()->show();
		unk580->hide();
		unk568->setCenteredSize(20, unk56C.getWidth(), unk56C.getHeight(), 0,
		                        0);
#endif
		unk10 = 1;
		break;

	case 1:
		if (unk568->update()) {
			unk580->show();
			unkB4 = 0;
			unk10 = 2;
		}
		break;

	case 3:
		if (unkB4 > 180) {
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
			JUTRect rect = unk568->getPane()->getBounds();
			unk568->setCenteredSize(20, 0, 0, rect.getWidth(),
			                        rect.getHeight());
#else
			unk568->setCenteredSize(20, 0, 0, unk56C.getWidth(),
			                        unk56C.getHeight());
#endif
			unk580->hide();
			unk10 = 4;
		}
		break;

	case 4:
		if (unk568->update()) {
			unk568->getPane()->hide();
			unk10 = 5;
		}
		break;

	case 5:
		unk1C = param_1;
		break;
	}

	unkB4 += 1;

	return -1;
}

s8 TCardLoad::drawMessageBM(TEProgress param_1)
{
	s8 result = -1;

	switch (unk10) {
	case 0:
		setMessage(unk4C0, 0x400, (u16)cMessageID[unk1C]);
		setMessage(unk4C4, 0x400, (u16)cMessageID[unk1C]);

		unk4AC->getPane()->show();
		unk500->hide();
		unk520->hide();
		unk4C4->hide();
		unk4C8->hide();
		unk4CC[0]->hide();
		unk4CC[1]->hide();
		unk4CC[2]->hide();

		unk4AC->setCenteredSize(20, unk4B0.getWidth(), unk4B0.getHeight(), 0,
		                        0);
		unk10 = 1;
		break;

	case 1:
		if (unk4AC->update()) {
			unk4C4->show();
			unk4C8->show();
			unk4CC[unkB0]->show();
			TCardBookmarkInfo* bm = &unk40[unkB0];
			if (bm->unk0 == 1) {
				CARDLOAD_SET_BROKEN(unk51C, 20);
				CARDLOAD_SET_BROKEN(unk520, 20);
				unk520->show();
				unk500->hide();
			} else if (bm->unk18 == 0) {
				CARDLOAD_SET_NEW(unk51C, 20);
				CARDLOAD_SET_NEW(unk520, 20);
				unk520->show();
				unk500->hide();
			} else {
				unk520->hide();
				TCardBookmarkInfo* bm2 = &unk40[unkB0];
				u16 score              = bm2->unk1C;
				(void)score; // TODO: hack, regswap
				if (score > 999)
					score = 999;
				if (score < 100) {
					unk500->hide();
					unk510->show();
					unk514[0]->changeTexture(unkC8[score / 10]->getTexInfo(),
					                         0);
					unk514[1]->changeTexture(unkC8[score % 10]->getTexInfo(),
					                         0);
				} else {
					unk500->show();
					unk510->hide();
					unk504[0]->changeTexture(unkC8[score / 100]->getTexInfo(),
					                         0);
					score -= int(score * 0.01f) * 100;
					unk504[1]->changeTexture(unkC8[score / 10]->getTexInfo(),
					                         0);
					unk504[2]->changeTexture(unkC8[score % 10]->getTexInfo(),
					                         0);
				}
			}
			unkB4 = 0;
			unk10 = 2;
		}
		break;

	case 3:
		if (unkB4 > 180) {
			unk4AC->setCenteredSize(20, 0, 0, unk4B0.getWidth(),
			                        unk4B0.getHeight());

			unk4C8->hide();
			unk500->hide();
			unk510->hide();
			unk520->hide();
			unk4C4->hide();
			unk10 = 4;
		}
		break;

	case PROGRESS_UNK4:
		if (unk4AC->update()) {
			unk4AC->getPane()->hide();
			unk10 = 5;
		}
		break;

	case 5:
		unk1C  = param_1;
		result = 0;
		break;
	}

	unkB4 += 1;

	return result;
}

s8 TCardLoad::selectBookmark(TEProgress param_1, TEProgress param_2,
                             bool param_3)
{
	s8 result = -1;

	switch (unk10) {
	case 0:
		unk338 = 0;
		unk2A0->hide();
		unk288->getPane()->show();
		unk288->setCenteredSize(30, unk28C.getWidth(), unk28C.getHeight(), 0,
		                        0);
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
		// PAL: same messages, one entry later in loadmessage_<language>.bmg.
		if (unk1C == PROGRESS_UNK1C) {
			unk278[unkB1]->makeBlockRock();
			setMessage(unk29C, 0xFF, 0x1F);
			setMessage(unk2A0, 0xFF, 0x1F);
		} else {
			setMessage(unk29C, 0xFF, 0x1E);
			setMessage(unk2A0, 0xFF, 0x1E);
		}
#elif defined(VERSION_GMSP01)
		// US: "Where do you want to copy this data to?" / "Select data."
		if (unk1C == PROGRESS_UNK1C) {
			unk278[unkB1]->makeBlockRock();
			setMessage(unk29C, 0xFF, 0x1E);
			setMessage(unk2A0, 0xFF, 0x1E);
		} else {
			setMessage(unk29C, 0xFF, 0x1D);
			setMessage(unk2A0, 0xFF, 0x1D);
		}
#else
		if (unk1C == PROGRESS_UNK1C) {
			unk278[unkB1]->makeBlockRock();
			setMessage(unk29C, 30, 0x1B);
			setMessage(unk2A0, 30, 0x1B);
		} else {
			setMessage(unk29C, 30, 0x1A);
			setMessage(unk2A0, 30, 0x1A);
		}
#endif

		for (int i = 0; i < 3; ++i) {
			if (unk2A4[i]->getPane()->isVisible())
				continue;

			unk2CC[i]->hide();
			unk2D8[i]->hide();
			unk2FC[i]->hide();
			unk2A4[i]->getPane()->show();
			unk2A4[i]->setCenteredSize(30, unk2B0.getWidth(),
			                           unk2B0.getHeight(), 0, 0);
			unk32C[i]->hide();
		}

		unk10 = 1;
		break;

	case 1: {
		bool done = true;
		done &= unk288->update();
		for (int i = 0; i < 3; ++i)
			done &= unk2A4[i]->update();
		if (done) {
			unk2A0->show();
			for (int i = 0; i < 3; ++i) {
				if (unk40[i].unk0 == 1) {
					CARDLOAD_SET_BROKEN(unk2C0[i], 20);
					CARDLOAD_SET_BROKEN(unk2CC[i], 20);
					unk2CC[i]->show();
					unk2D8[i]->hide();
				} else if (unk40[i].unk18 == 0) {
					CARDLOAD_SET_NEW(unk2C0[i], 20);
					CARDLOAD_SET_NEW(unk2CC[i], 20);
					unk2CC[i]->show();
					unk2D8[i]->hide();
				} else {
					if (unk40[i].unk1C >= 120)
						unk32C[i]->show();

					unk2CC[i]->hide();

					u16 score = unk40[i].unk1C > 999 ? 999 : unk40[i].unk1C;
					if (score < 100) {
						unk2E4[i]->changeTexture(
						    unkC8[score / 10]->getTexInfo(), 0);
						unk2F0[i]->changeTexture(
						    unkC8[score % 10]->getTexInfo(), 0);
						unk2D8[i]->show();
						unk2FC[i]->hide();
					} else {
						unk308[i]->changeTexture(
						    unkC8[score / 100]->getTexInfo(), 0);
						score -= int(score * 0.01f) * 100;
						unk314[i]->changeTexture(
						    unkC8[score / 10]->getTexInfo(), 0);
						unk320[i]->changeTexture(
						    unkC8[score % 10]->getTexInfo(), 0);
						unk2FC[i]->show();
						unk2D8[i]->hide();
					}
				}
			}
			unkB0 = -1;
			unk10 = 2;
		}
	} break;

	case 2:
		if (param_2 != 0 && unk38->getTrigger() & 0x200)
			unk10 = 3;
		if (unkB0 != -1)
			unk10 = 3;
		break;

	case 3:
		unk38->onFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
		unk2A0->hide();
		unk288->setCenteredSize(30, 0, 0, unk28C.getWidth(),
		                        unk28C.getHeight());
		for (int i = 0; i < 3; ++i)
			unk32C[i]->hide();
		for (int i = 0; i < 3; ++i) {
			if (i != unkB0 || unk1C == PROGRESS_UNK1C
			    || unk40[unkB0].unk0 == 1) {
				unk2CC[i]->hide();
				unk2D8[i]->hide();
				unk2FC[i]->hide();
				unk2A4[i]->setCenteredSize(30, 0, 0, unk2B0.getWidth(),
				                           unk2B0.getHeight());
			}
		}
		unk10 = 4;
		break;

	case 4: {
		bool done = true;
		done &= unk288->update();
		for (int i = 0; i < 3; ++i)
			done &= unk2A4[i]->update();
		if (done) {
			unk288->getPane()->hide();
			if (param_3) {
				for (int i = 0; i < 3; ++i)
					if (i != unkB0 || unk1C == PROGRESS_UNK1C
					    || unk40[unkB0].unk0 == 1)
						unk2A4[i]->getPane()->hide();
			}
			unk10 = 5;
		}
	} break;

	case 5:
		if (unk1C == PROGRESS_UNK1C)
			unk278[unkB1]->makeBlockNormal();
		result = unkB0;
		if (unk14 == 0)
			unk38->offFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
		if (unkB0 != -1)
			unk1C = param_1;
		else
			unk1C = param_2;
		break;
	}

	for (int i = 0; i < ARRAY_COUNT(unk32C); ++i)
		changePattern(unk32C[i], 72, unk338);

	if (unk338 < 144)
		unk338 += 1;
	else
		unk338 = 0;

	return result;
}

s8 TCardLoad::selectFunction()
{
	s8 result = -1;
	s8 bVar1  = unkB6;

	switch (unk10) {
	case 0: {
		unk33C[unkB0]->getPane()->show();
		for (int i = 0; i < 4; ++i)
			unk378[unkB0][i]->getPane()->hide();
		JUTRect rect = unk348[unkB0];
		unk33C[unkB0]->setPaneSize(30, rect.getWidth(), rect.getHeight(),
		                           rect.getWidth(), 0);
		unk33C[unkB0]->setPaneOffset(30, 0, 0, 0, rect.getHeight());

		if (!unk2A4[0]->getPane()->isVisible()) {
			for (int i = 0; i < 3; ++i)
				if (i == unkB0 && !unk2A4[i]->getPane()->isVisible()) {
					unk2CC[i]->hide();
					unk2D8[i]->hide();
					unk2FC[i]->hide();
					unk2A4[i]->getPane()->show();
					unk2A4[i]->setCenteredSize(30, unk2B0.getWidth(),
					                           unk2B0.getHeight(), 0, 0);
				}
		}

		unk10 = 1;
	} break;

	case 1: {
		bool done = true;
		for (int i = 0; i < ARRAY_COUNT(unk2A4); ++i)
			done &= unk2A4[i]->update();
		done &= unk33C[unkB0]->update();
		if (done) {
			for (int i = 0; i < 4; ++i) {
				unk378[unkB0][i]->getPane()->show();

				unk378[unkB0][i]->updateCenteredSize(
				    1, unk3A8[unkB0][i].getWidth(),
				    unk3A8[unkB0][i].getHeight());

				unk378[unkB0][i]->update();
				if (unk40[unkB0].unk18 == 0) {
					if (i > 0)
						((J2DPicture*)unk378[unkB0][i]->getPane())->mWhite
						    = 0xFFFFFF7F;
				} else {
					((J2DPicture*)unk378[unkB0][i]->getPane())->mWhite
					    = 0xFFFFFFFF;
				}
			}

			((J2DPicture*)unk378[unkB0][unkB6]->getPane())->mWhite = 0x00FF00FF;
			for (int i = 0; i < 3; ++i) {
				if (unk40[i].unk0 == 1) {
					// The retail code writes the bookmark-window text boxes
					// here, not unk2C0[i]/unk2CC[i] (original behaviour).
					CARDLOAD_SET_BROKEN(unk51C, 20);
					CARDLOAD_SET_BROKEN(unk520, 20);
					unk2CC[i]->show();
					unk2D8[i]->hide();
				} else if (unk40[i].unk18 == 0) {
					CARDLOAD_SET_NEW(unk51C, 20);
					CARDLOAD_SET_NEW(unk520, 20);
					unk2CC[i]->show();
					unk2D8[i]->hide();
				} else {
					unk2CC[i]->hide();

					u16 score = unk40[i].unk1C;

					if (score < 100) {
						unk2E4[i]->changeTexture(
						    unkC8[score / 10]->getTexInfo(), 0);
						unk2F0[i]->changeTexture(
						    unkC8[score % 10]->getTexInfo(), 0);
						unk2D8[i]->show();
					} else {
						unk2FC[i]->show();
					}
				}
			}

			unkC4 = 0;
			unk10 = 2;
		}
	} break;

	case 2:
		if (unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_A)) {
			if (bVar1 == 0)
				SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_DECIDE_COMMON, 0,
				                                   nullptr, 0);
			else
				SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_SELECT_COMMON, 0,
				                                   nullptr, 0);
			unk10 = 3;
		} else if (unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_DOWN)) {
			TCardBookmarkInfo* bm = &unk40[unkB0];
			if (bm->unk18 == 0)
				unkB6 = 0;
			else
				unkB6 = bVar1 + 1;
			if (unkB6 > 3)
				unkB6 = 0;
		} else if (unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_UP)) {
			TCardBookmarkInfo* bm = &unk40[unkB0];
			if (bm->unk18 == 0)
				unkB6 = 0;
			else
				unkB6 = bVar1 - 1;
			if (unkB6 < 0)
				unkB6 = 3;
		} else if (unk38->checkFrameMeaning(TMarioGamePad::MEANING_MENU_B)) {
			unkB6 = -1;
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_CANCEL_COMMON, 0,
			                                   nullptr, 0);
			unk10 = 3;
		}

		if (unkB6 != -1) {
			if (unkC4 == 4) {
				unk378[unkB0][unkB6]->setCenteredSize(
				    40, unk3A8[unkB0][unkB6].getWidth() * 1.5f,
				    unk3A8[unkB0][unkB6].getHeight() * 1.5f,
				    unk3A8[unkB0][unkB6].getWidth(),
				    unk3A8[unkB0][unkB6].getHeight());

				JUTRect rect
				    = unk378[unkB0][unkB6]->getPane()->getGlobalBounds();
				JGeometry::TVec3<f32> pos(rect.x1 + rect.getWidth() / 2.0f,
				                          rect.y1 + rect.getHeight() / 2.0f,
				                          0.0f);
				gpEmitterManager4D2->createEmitter(pos, 0x1FA, nullptr,
				                                   nullptr);
				unkAC = gpEmitterManager4D2->unkC8[0][0];
				unkAC->setRotation(0, 0, DEG2SHORTANGLE(12));
				unkAC->setEmitterScale(JGeometry::TVec3<f32>(0.9f, 1.0f, 0.1f));
			} else if (unkC4 == 44) {
				unk378[unkB0][unkB6]->setCenteredSize(
				    40, unk3A8[unkB0][unkB6].getWidth(),
				    unk3A8[unkB0][unkB6].getHeight(),
				    unk3A8[unkB0][unkB6].getWidth() * 1.5f,
				    unk3A8[unkB0][unkB6].getHeight() * 1.5f);
			}

			int r = unkC4;
			r     = r < 40 ? r : 80 - r;
			r     = (r * 255.0f) / 40.0f;
			((J2DPicture*)unk378[unkB0][unkB6]->getPane())->mWhite
			    = (r << 24) + 0xFF00FF;
			unk378[unkB0][unkB6]->update();
			unkC4 += 1;
			if (unkC4 > 80)
				unkC4 = 0;
		}

		if (unkB6 != bVar1 && unkB6 != -1) {
			if (unkAC != nullptr)
				gpEmitterManager4D2->forceDeleteEmitter(unkAC);
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_E3_MENU_CURSOR, 0,
			                                   nullptr, 0);

			unkC4 = 0;

			unk378[unkB0][bVar1]->getPane()->setBounds(unk3A8[unkB0][bVar1]);

			((J2DPicture*)unk378[unkB0][bVar1]->getPane())->mWhite = 0xFFFFFFFF;
			((J2DPicture*)unk378[unkB0][unkB6]->getPane())->mWhite = 0x00FF00FF;
		}
		break;

	case 3: {
		for (int i = 0; i < 4; ++i)
			unk378[unkB0][i]->getPane()->hide();
		JUTRect rect = unk348[unkB0];
		unk33C[unkB0]->updatePaneSize(30, rect.getWidth(), 0);
		unk33C[unkB0]->updatePaneOffset(30, 0, rect.getHeight());

		unk10 = 4;
		for (int i = 0; i < 3; ++i) {
			if (i == unkB0 && unkB6 != -1 && unkB6 != 1) {
				unk2CC[i]->hide();
				unk2D8[i]->hide();
				unk2FC[i]->hide();
				unk2A4[i]->setCenteredSize(30, 0, 0, unk2B0.getWidth(),
				                           unk2B0.getHeight());
			}
		}
	} break;

	case 4: {
		bool done = true;
		done &= unk33C[unkB0]->update();
		for (int i = 0; i < ARRAY_COUNT(unk2A4); ++i)
			done &= unk2A4[i]->update();
		if (done) {
			unk33C[unkB0]->getPane()->hide();
			for (int i = 0; i < 3; ++i)
				if (unkB0 == i && unkB6 != -1 && unkB6 != 1)
					unk2A4[i]->getPane()->hide();
			unk10 = 5;
		}
	} break;

	case 5:
		if (bVar1 != -1) {
			unk1C  = PROGRESS_UNK1B;
			result = bVar1;
		} else {
			unk1C = PROGRESS_UNK13;
		}
		break;
	}

	return result;
}

void TCardLoad::setSelected(u8 param_1)
{
#ifdef SMS_PORT_AURORA
	if (param_1 == 3 || param_1 == 4) { // the online / customization blocks (port_mp_block.cpp)
		if (unk14 == 0)
			port_mp_block_hit(param_1);
		return;
	}
#endif
	if (unk14 == 0) {
		SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_COIN_APPEAR, 0, nullptr,
		                                   0);
		unkB0 = param_1;
		setupScoreScreen();
	}
}

void TCardLoad::changeScene()
{
	TEProgress prevUnk1C = unk1C;
	int prevUnk0         = unk10;
	switch (prevUnk1C) {
	case PROGRESS_UNK30:
		gpCardManager->readOptionBlock();
		unk1C = PROGRESS_UNK31;
		break;

	case PROGRESS_UNK31: {
		int rc = gpCardManager->getLastStatus();
		if (rc != CARD_RESULT_BUSY) {
			if (rc == CARD_RESULT_READY) {
				JSUMemoryInputStream stream;
				gpCardManager->getOptionReadStream(&stream);
				TFlagManager::getInstance()->loadOption(stream);
				unk1C = PROGRESS_UNK0;
			} else {
				unk1C = changeMode(rc);
			}
		} else {
			gpCardManager->probe();
		}
	} break;

	case PROGRESS_UNK0:
		unk38->onFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
		gpCardManager->getBookmarkInfos(unk40);
		unk1C = PROGRESS_UNK2;
		break;

	case PROGRESS_UNK1:
		gpCardManager->unmount();
		break;

	case PROGRESS_UNK2: {
		int rc = gpCardManager->getLastStatus();
		if (rc != CARD_RESULT_BUSY) {
			if (rc == CARD_RESULT_READY) {
				unk10 = 1;
				unk1C = PROGRESS_UNK13;
			} else {
				unk1C = changeMode(rc);
			}
		}
	} break;

	case PROGRESS_UNK3: {
		int rc = gpCardManager->getLastStatus();
		if (rc == CARD_RESULT_NOCARD) {
			waitForStart(PROGRESS_UNK1A);
			gpCardManager->probe();
		} else {
			if (unk10 < 3)
				unk10 = 3;
			if (waitForStart(PROGRESS_UNK2) != -1)
				gpCardManager->getBookmarkInfos(unk40);
		}
	} break;

	case PROGRESS_UNK4:
	case PROGRESS_UNK5:
	case PROGRESS_UNKC:
	case PROGRESS_UNKD:
	case PROGRESS_UNK10:
	case PROGRESS_UNK2D:
#ifdef VERSION_GMSP01
	case CARDLOAD_PROGRESS_US35:
#endif
	{
		int rc = gpCardManager->getLastStatus();
		if (rc != CARD_RESULT_NOCARD) {
			waitForStart(PROGRESS_UNK1A);
			gpCardManager->probe();
		} else {
			if (unk10 < 3)
				unk10 = 3;
			if (waitForStart(PROGRESS_UNK2) != -1)
				gpCardManager->getBookmarkInfos(unk40);
		}
	} break;

	case PROGRESS_UNK6:
	case PROGRESS_UNK7: {
		int rc = gpCardManager->getLastStatus();
		if (rc == CARD_RESULT_READY) {
			waitForChoice(PROGRESS_UNK8, CARDLOAD_FORMAT_NO, 1);
			gpCardManager->probe();
		} else if (rc != CARD_RESULT_BUSY) {
			if (unk10 < 3)
				unk10 = 3;
			if (waitForChoice(PROGRESS_UNK8, CARDLOAD_FORMAT_NO, 1) != -1)
				unk1C = changeMode(rc);
		}
	} break;

	case PROGRESS_UNK8: {
		int rc = gpCardManager->getLastStatus();
		if (rc == CARD_RESULT_READY) {
			s8 choice = waitForChoice(PROGRESS_UNK9, CARDLOAD_FORMAT_NO, 1);
			if (choice == 0)
				gpCardManager->format();
			if (choice == -1)
				gpCardManager->probe();
		} else if (rc != CARD_RESULT_BUSY) {
			if (unk10 < 3)
				unk10 = 3;
			if (waitForChoice(PROGRESS_UNK9, CARDLOAD_FORMAT_NO, 1) != -1)
				unk1C = changeMode(rc);
		}
	} break;

	case PROGRESS_UNK9: {
		int rc = gpCardManager->getLastStatus();
		if (rc != CARD_RESULT_BUSY) {
			if (rc == CARD_RESULT_READY) {
				if (unk10 == 2)
					unk10 = CARDLOAD_FORMAT_MSG_STEP;
				drawMessage(PROGRESS_UNKA);
				gpCardManager->probe();
				if (unk10 == 5)
					SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_DECIDE, 0,
					                                   nullptr, 0);
			} else {
				if (unk10 == 2)
					unk10 = 3;
				drawMessage(PROGRESS_UNKB);
			}
		} else {
			gpCardManager->probe();
			drawMessage(PROGRESS_UNK0);
		}
	} break;

#ifdef VERSION_GMSP01
	// US: after a failed format/create, go back through the error states.
	case PROGRESS_UNKA:
		if (waitForAnyKey(PROGRESS_UNK2) != -1)
			gpCardManager->getBookmarkInfos(unk40);
		break;

	case PROGRESS_UNKB:
		if (waitForAnyKey(PROGRESS_UNK2) != -1)
			unk1C = changeMode(gpCardManager->getLastStatus());
		break;
#else
	case PROGRESS_UNKA:
	case PROGRESS_UNKB:
		if (waitForAnyKey(PROGRESS_UNK2) != -1)
			gpCardManager->getBookmarkInfos(unk40);
		break;
#endif

	case PROGRESS_UNK1A: {
		s8 choice = waitForChoice(PROGRESS_UNK29, PROGRESS_UNK2, 0);
		if (choice == 1)
			gpCardManager->getBookmarkInfos(unk40);
		else if (choice == 0)
			TFlagManager::getInstance()->firstStart();
	} break;

	case PROGRESS_UNKE: {
		int rc = gpCardManager->getLastStatus();
		if (rc == CARD_RESULT_READY) {
			s8 c = waitForChoice(PROGRESS_UNKF, PROGRESS_UNK10, 0);
			if (c == 0)
				gpCardManager->createFile();
			if (c == -1)
				gpCardManager->probe();
		} else if (rc != CARD_RESULT_BUSY) {
			if (unk10 < 3)
				unk10 = 3;
			if (waitForChoice(PROGRESS_UNKF, PROGRESS_UNK4, 0) != -1)
				unk1C = changeMode(rc);
		}
	} break;

	case PROGRESS_UNKF: {
		int rc = gpCardManager->getLastStatus();
		if (rc != CARD_RESULT_BUSY) {
			if (rc == CARD_RESULT_READY) {
				if (unk10 == 2)
					unk10 = CARDLOAD_CREATE_MSG_STEP;
				drawMessage(PROGRESS_UNK11);
				gpCardManager->probe();
				if (unk10 == 5)
					SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_DECIDE, 0,
					                                   nullptr, 0);
			} else {
				if (unk10 == 2)
					unk10 = CARDLOAD_CREATE_MSG_STEP;
				drawMessage(PROGRESS_UNK12);
			}
		} else {
			gpCardManager->probe();
			drawMessage(PROGRESS_UNK0);
		}
	} break;

#ifdef VERSION_GMSP01
	case PROGRESS_UNK11:
		if (waitForAnyKey(PROGRESS_UNK2) != -1)
			gpCardManager->getBookmarkInfos(unk40);
		break;

	case PROGRESS_UNK12:
		if (waitForAnyKey(PROGRESS_UNK2) != -1)
			unk1C = changeMode(gpCardManager->getLastStatus());
		break;
#else
	case PROGRESS_UNK11:
	case PROGRESS_UNK12:
		if (waitForAnyKey(PROGRESS_UNK2) != -1)
			gpCardManager->getBookmarkInfos(unk40);
		break;
#endif

	case PROGRESS_UNK13: {
		if (unk24 != 0)
			selectBookmark(PROGRESS_UNK2, changeMode(unk24), true);
		int rc = gpCardManager->probe();
		if (rc == CARD_RESULT_READY) {
			unkB6 = 0;
			s8 bm = selectBookmark(PROGRESS_UNK1B, PROGRESS_UNK0, true);
			if (bm != -1 && unk40[bm].unk0 == 1)
				unk1C = PROGRESS_UNK1E;
		} else if (rc != CARD_RESULT_BUSY) {
			unk24 = rc;
			if (unk10 == 2)
				unk10 = 3;
		}
	} break;

	case PROGRESS_UNK1C: {
		int rc = gpCardManager->probe();
		if (rc == CARD_RESULT_READY) {
			s8 choice = selectBookmark(PROGRESS_UNK15, PROGRESS_UNK2, 1);
			if (choice != -1) {
				unkB2 = choice;

				TCardBookmarkInfo* bm = &unk40[unkB2];
				if (bm->unk0 == 1)
					unk1C = PROGRESS_UNK22;
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
				// PAL: copying onto an empty file skips the overwrite question.
				else if (bm->unk18 == 0) {
					gpCardManager->readBlock(unkB1);
					unk1C = PROGRESS_UNK24;
				}
#endif
			}
			if (unk1C == PROGRESS_UNK2)
				gpCardManager->getBookmarkInfos(unk40);
		} else if (rc != CARD_RESULT_BUSY) {
			TEProgress next = changeMode(rc);
			selectBookmark(next, next, true);
			if (unk10 == 2)
				unk10 = 3;
		}
	} break;

	case PROGRESS_UNK1B: {
		int rc = gpCardManager->getLastStatus();
		if (rc == CARD_RESULT_READY) {
			s8 c = selectFunction();
			gpCardManager->probe();
			switch (c) {
			case 0: {
				SMSGetApplication()->mSaveFile = unkB0;
				TCardBookmarkInfo* bm          = &unk40[unkB0];
				if (bm->unk18 == 0) {
					TFlagManager::getInstance()->firstStart();
					unk1C = PROGRESS_UNK29;
				} else {
					gpCardManager->readBlock(unkB0);
					unk1C = PROGRESS_UNK28;
				}
			} break;

			case 1:
				unkB1 = unkB0;
				unk1C = PROGRESS_UNK1C;
				break;

			case 2:
				unk1C = PROGRESS_UNK1D;
				break;

			case 3:
				for (int i = 0; i < 3; ++i)
					if (i == unkB0)
						unk728[i]->show();
					else
						unk728[i]->hide();
				unk2C->search('ROOT')->setAlpha(0);
				gpCameraOption->moveToUp();
				gpCardManager->readBlock(unkB0);
				unk14 = 4;
				unkB8 = 0;
				break;
			}
		} else if (rc != CARD_RESULT_BUSY) {
			if (unk378[unkB0][0]->getPane()->isVisible()) {
				for (int i = 0; i < 4; ++i)
					unk378[unkB0][i]->getPane()->hide();
				JUTRect local_6c = unk348[unkB0];
				unk33C[unkB0]->updatePaneSize(30, local_6c.getWidth(), 0);
				unk33C[unkB0]->updatePaneOffset(30, 0, local_6c.getHeight());
				unk10 = 4;
			}

			if (selectFunction() != -1)
				unk1C = changeMode(rc);
		}
	} break;

	case PROGRESS_UNK1E: {
		int rc = gpCardManager->getLastStatus();
		if (rc == CARD_RESULT_READY) {
			s8 choice = waitForChoiceBM(PROGRESS_UNK1F, PROGRESS_UNK2, 1);
			gpCardManager->probe();
			if (choice == 0) {
				clearBookmark(unkB0);
			} else if (choice == 1) {
				gpCardManager->getBookmarkInfos(unk40);
				unk1C = PROGRESS_UNK2;
			}
		} else if (rc != CARD_RESULT_BUSY) {
			if (unk10 < 3)
				unk10 = 3;
			if (waitForChoiceBM(PROGRESS_UNK0, PROGRESS_UNK0, 1) != -1)
				unk1C = changeMode(rc);
		}
	} break;

	case PROGRESS_UNK1F: {
		int rc = gpCardManager->getLastStatus();
		if (rc != CARD_RESULT_BUSY) {
			if (rc == CARD_RESULT_READY) {
				if (unk10 == 2)
					unk10 = 3;
				drawMessageBM(PROGRESS_UNK20);
				gpCardManager->probe();
				if (unk10 == 5)
					SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_DECIDE, 0,
					                                   nullptr, 0);
			} else {
				if (unk10 == 2)
					unk10 = 3;
				drawMessageBM(PROGRESS_UNK21);
			}
		} else {
			drawMessageBM(PROGRESS_UNK0);
		}
	} break;

#ifdef VERSION_GMSP01
	case PROGRESS_UNK20:
		if (waitForAnyKey(PROGRESS_UNK2) != -1)
			gpCardManager->getBookmarkInfos(unk40);
		break;

	case PROGRESS_UNK21:
		if (waitForAnyKeyBM(PROGRESS_UNK2) != -1)
			unk1C = changeMode(gpCardManager->getLastStatus());
		break;
#else
	case PROGRESS_UNK20:
	case PROGRESS_UNK21:
		if (waitForAnyKey(PROGRESS_UNK2) != -1)
			gpCardManager->getBookmarkInfos(unk40);
		break;
#endif

	case PROGRESS_UNK1D: {
		int rc = gpCardManager->getLastStatus();
		if (rc == CARD_RESULT_READY) {
			s8 choice = waitForChoiceBM(PROGRESS_UNK1F, PROGRESS_UNK2, 1);
			gpCardManager->probe();
			if (choice == 0) {
				clearBookmark(unkB0);
			} else if (choice == 1) {
				gpCardManager->getBookmarkInfos(unk40);
			}
		} else if (rc != CARD_RESULT_BUSY) {
			if (unk10 < 3)
				unk10 = 3;
			if (waitForChoiceBM(PROGRESS_UNK0, PROGRESS_UNK0, 0) != -1)
				unk1C = changeMode(rc);
		}
	} break;

	case PROGRESS_UNK22: {
		int rc = gpCardManager->getLastStatus();
		if (rc == CARD_RESULT_READY) {
			s8 choice = waitForChoiceBM(PROGRESS_UNK24, PROGRESS_UNK2, 1);
			gpCardManager->probe();
			if (choice == 0)
				gpCardManager->readBlock(unkB1);
			else if (choice == 1)
				gpCardManager->getBookmarkInfos(unk40);
		} else if (rc != CARD_RESULT_BUSY) {
			if (unk10 < 3)
				unk10 = 3;
			if (waitForChoiceBM(PROGRESS_UNK0, PROGRESS_UNK0, 1) != -1)
				unk1C = changeMode(rc);
		}
	} break;

	case PROGRESS_UNK24: {
		int rc = gpCardManager->getLastStatus();
		if (rc != CARD_RESULT_BUSY) {
			if (rc == CARD_RESULT_READY) {
				drawMessageBM(PROGRESS_UNK0);
				gpCardManager->probe();
				if (unk10 == 2) {
					gpCardManager->writeBlock(unkB2);
					unk1C     = PROGRESS_UNK25;
					prevUnk1C = PROGRESS_UNK25;
				}
			} else {
				if (unk10 == 2)
					unk10 = 3;
				drawMessageBM(PROGRESS_UNK26);
			}
		} else {
			drawMessageBM(PROGRESS_UNK0);
		}
	} break;

	case PROGRESS_UNK25: {
		int rc = gpCardManager->getLastStatus();
		if (rc != CARD_RESULT_BUSY) {
			if (rc == CARD_RESULT_READY) {
				if (unk10 == 2)
					unk10 = 3;
				drawMessageBM(PROGRESS_UNK27);
				gpCardManager->probe();
				if (unk10 == 5)
					SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_DECIDE, 0,
					                                   nullptr, 0);
			} else {
				if (unk10 == 2)
					unk10 = 3;
				drawMessageBM(PROGRESS_UNK26);
			}
		} else {
			drawMessageBM(PROGRESS_UNK0);
		}
	} break;

	case PROGRESS_UNK15: {
		int rc = gpCardManager->getLastStatus();
		if (rc == CARD_RESULT_READY) {
			s8 choice = waitForChoiceBM(PROGRESS_UNK24, PROGRESS_UNK13, 1);
			gpCardManager->probe();
			if (choice == 0)
				gpCardManager->readBlock(unkB1);
		} else if (rc != CARD_RESULT_BUSY) {
			if (unk10 < 3)
				unk10 = 3;
			if (waitForChoiceBM(PROGRESS_UNK0, PROGRESS_UNK0, 1) != -1)
				unk1C = changeMode(rc);
		}
	} break;

#ifndef VERSION_GMSP01
	case PROGRESS_UNK26:
#endif
	case PROGRESS_UNK27: {
		int rc = gpCardManager->probe();
		if (rc == CARD_RESULT_READY) {
			if (waitForAnyKeyBM(PROGRESS_UNK2) != -1)
				gpCardManager->getBookmarkInfos(unk40);
		} else if (rc != CARD_RESULT_BUSY) {
			if (waitForAnyKeyBM(PROGRESS_UNK0) != -1)
				unk1C = changeMode(rc);
		}
	} break;

#ifdef VERSION_GMSP01
	// US: a failed copy goes back through the error states.
	case PROGRESS_UNK26: {
		int rc = gpCardManager->getLastStatus();
		if (rc == CARD_RESULT_READY) {
			if (waitForAnyKeyBM(PROGRESS_UNK0) != -1)
				unk1C = changeMode(rc);
		} else if (rc != CARD_RESULT_BUSY) {
			if (waitForAnyKeyBM(PROGRESS_UNK0) != -1)
				unk1C = changeMode(rc);
		}
	} break;
#endif

	case PROGRESS_UNK28: {
		int rc = gpCardManager->getLastStatus();
		if (rc != CARD_RESULT_BUSY) {
			if (rc == CARD_RESULT_READY) {
				loadBookmark();
				unk1C = PROGRESS_UNK29;
			} else {
				unk1C = changeMode(rc);
			}
		}
	} break;

	case PROGRESS_UNK29: {
		if (TFlagManager::getInstance()->getFlag(MSF_SHINE_COUNT) < 1)
			gpMarDirector->setNextStage(0, nullptr);
		else
			gpMarDirector->setNextStage(1, nullptr);
		gpCardManager->unmount();
	} break;

	case PROGRESS_UNK32: {
		int rc = gpCardManager->getLastStatus();
		if (rc != CARD_RESULT_BUSY) {
			if (rc == CARD_RESULT_READY) {
				JSUMemoryOutputStream stream;
				gpCardManager->getOptionWriteStream(&stream);
				TFlagManager::getInstance()->saveOption(stream);
				gpCardManager->writeOptionBlock();
				unk1C = PROGRESS_UNK2F;
			} else {
				unk1C = changeMode(rc);
			}
		}
	} break;

	case PROGRESS_UNK2F: {
		int rc = gpCardManager->getLastStatus();
		if (rc != CARD_RESULT_BUSY) {
			if (rc == CARD_RESULT_READY) {
				if (unk10 == 2)
					unk10 = 3;
				drawMessage(PROGRESS_UNK2);
				if (unk10 == 5) {
					SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_DECIDE, 0,
					                                   nullptr, 0);
					gpCardManager->getBookmarkInfos(unk40);
				}
			} else {
				drawMessage(PROGRESS_UNK0);
				if (unk10 == 2)
					unk10 = 3;
				if (unk10 == 5)
					unk1C = changeMode(rc);
			}
		} else {
			drawMessage(PROGRESS_UNK0);
		}
	} break;
	}

	if (prevUnk1C != unk1C) {
		unk24 = 0;
		unk20 = unk1C;
		unk10 = 0;
#ifdef VERSION_GMSP01
		unkB4 = 0;
#endif
		switch (prevUnk1C) {
		case PROGRESS_UNK1C:
			unk284->offCollision();

		case PROGRESS_UNK13:
		case PROGRESS_UNK3:
		case PROGRESS_UNK4:
		case PROGRESS_UNK5:
		case PROGRESS_UNKC:
		case PROGRESS_UNKD:
		case PROGRESS_UNK10:
		case PROGRESS_UNK2D:
#ifdef VERSION_GMSP01
		case CARDLOAD_PROGRESS_US35:
#endif
			unk38->onFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
			unk275 = 0;
			break;
		}

		switch (unk1C) {
		case PROGRESS_UNK1C:
			unk284->onCollision();
			unk275 = 0;
			unk38->offFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
			break;

		case PROGRESS_UNK13:
		case PROGRESS_UNK3:
		case PROGRESS_UNK4:
		case PROGRESS_UNK5:
		case PROGRESS_UNKC:
		case PROGRESS_UNKD:
		case PROGRESS_UNK10:
		case PROGRESS_UNK2D:
#ifdef VERSION_GMSP01
		case CARDLOAD_PROGRESS_US35:
#endif
			unk275 = 1;
			unk38->offFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
			break;
		}

		if (unk1C == PROGRESS_UNK13) {
			for (int i = 0; i < 3; ++i)
				unk278[i]->makeBlockNormal();
		}
	}

	if (prevUnk0 != unk10) {
		if (prevUnk0 == 0)
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_2D_IN, 0, nullptr, 0);
		if (unk10 == 4)
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_2D_OUT, 0, nullptr, 0);
	}
}
