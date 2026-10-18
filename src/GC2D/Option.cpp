#include <GC2D/Option.hpp>
#include <macros.h>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/J2D/J2DScreen.hpp>
#include <JSystem/J2D/J2DTextBox.hpp>
#include <JSystem/J2D/J2DOrthoGraph.hpp>
#include <JSystem/JAudio/JAInterface/JAIGlobalParameter.hpp>
#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>
#include <System/MarioGamePad.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MarioUtil/RumbleMgr.hpp>
#include <MSound/MSound.hpp>
#include <GC2D/ExPane.hpp>
#include <Camera/CameraOption.hpp>
#include <Player/MarioAccess.hpp>
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
#include <JSystem/J2D/J2DPicture.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <GC2D/MessageUtil.hpp>
#include <stdio.h>
#include <string.h>
#endif

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>

// TODO: get rid of this
static const char* dummyMactorStringValue1 = "\0\0\0\0\0\0\0\0\0\0\0";
static const char* SMS_NO_MEMORY_MESSAGE   = "メモリが足りません\n";

namespace {

void tag_to_string(char*, u32) { }

void print_pane_tree(J2DPane*, int) { }

const TPatternAnmControl::TAnmChunk cRumbleAnm[] = {
	{ 'cnt0', 0.03 }, { 'cnt1', 0.03 }, { 'cnt2', 0.03 }, { 'cnt1', 0.03 },
	{ 'cnt2', 0.03 }, { 'cnt1', 0.03 }, { 'cnt2', 0.03 }, { 'cnt1', 0.03 },
	{ 'cnt2', 0.03 }, { 'cnt0', 2.0 },
};

const TPatternAnmControl::TAnmChunk cStopRumbleAnm[] = {
	{ 'cnt0', 1.0f },
};

const TPatternAnmControl::TAnmChunk cSurMonteAnm[] = {
	{ 'fa_8', 0.3f }, { 'fa_9', 0.3f }, { 'fa_8', 0.3f }, { 'fa_9', 0.3f },
	{ 'fa_6', 0.3f }, { 'fa_7', 0.3f }, { 'fa_6', 0.3f }, { 'fa_7', 0.3f },
};

const TPatternAnmControl::TAnmChunk cSurLTSpeakerAnm[] = {
	{ 'sp2b', 0.3f }, { 'sp2a', 0.3f }, { 'sp2b', 0.3f },
	{ 'sp2a', 0.3f }, { 'sp2a', 1.2f },
};

const TPatternAnmControl::TAnmChunk cSurRTSpeakerAnm[] = {
	{ 'sp2c', 0.3f }, { 'sp2d', 0.3f }, { 'sp2c', 0.3f },
	{ 'sp2d', 0.3f }, { 'sp2c', 1.2f },
};

const TPatternAnmControl::TAnmChunk cSurLBSpeakerAnm[] = {
	{ 'sp2e', 1.2f }, { 'sp2f', 0.3f }, { 'sp2e', 0.3f },
	{ 'sp2f', 0.3f }, { 'sp2e', 0.3f },
};

const TPatternAnmControl::TAnmChunk cSurRBSpeakerAnm[] = {
	{ 'sp2g', 1.2f }, { 'sp2g', 0.3f }, { 'sp2h', 0.3f },
	{ 'sp2g', 0.3f }, { 'sp2h', 0.3f },
};

const TPatternAnmControl::TAnmChunk cSteMonteAnm[] = {
	{ 'fa_4', 0.3f }, { 'fa_5', 0.3f }, { 'fa_4', 0.3f }, { 'fa_5', 0.3f },
	{ 'fa_2', 0.3f }, { 'fa_3', 0.3f }, { 'fa_2', 0.3f }, { 'fa_3', 0.3f },
};

const TPatternAnmControl::TAnmChunk cSteLSpeakerAnm[] = {
	{ 'sp0b', 0.3f }, { 'sp0a', 0.3f }, { 'sp0b', 0.3f },
	{ 'sp0a', 0.3f }, { 'sp0a', 1.2f },
};

const TPatternAnmControl::TAnmChunk cSteRSpeakerAnm[] = {
	{ 'sp1a', 1.2f }, { 'sp1b', 0.3f }, { 'sp1a', 0.3f },
	{ 'sp1b', 0.3f }, { 'sp1a', 0.3f },
};

const TPatternAnmControl::TAnmChunk cMonoMonteAnm[] = {
	{ 'fa_0', 0.3f },
	{ 'fa_1', 0.3f },
};

const TPatternAnmControl::TAnmChunk cMonoSpeakerAnm[] = {
	{ 'sp0a', 0.3f },
	{ 'sp0b', 0.3f },
};

const u32 cRumbleToggleItems[] = {
	'sel1',
	'sel0',
};

const u32 cSoundToggleItems[] = {
	'sel3',
	'sel2',
	'sel4',
};

#ifdef VERSION_GMSP01
const u32 cSubtitleToggleItems[] = {
	'sel6',
	'sel5',
};
#endif

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
const u32 cLanguageToggleItems[] = {
	'lan0', 'lan1', 'lan2', 'lan3', 'lan4',
};

// PAL: one texture per language: /option/timg/<language>/<name>.
void loadTexture(JUTTexture** textures, char* name)
{
	static const char* langArray[] = { "", "ge/", "fr/", "sp/", "it/" };
	for (int i = 0; i < 5; ++i) {
		char path[256];
		snprintf(path, 0xFF, "/option/timg/%s%s", langArray[i], name);
		textures[i] = new JUTTexture((const ResTIMG*)JKRGetResource(path));
	}
}

// PAL: shows a language's picture in a pane, keeping the pane centred on the
// same point; returns the pane's new width.
inline int changePaneTexture(J2DScreen* screen, u32 tag, JUTTexture* texture)
{
	J2DPicture* picture = (J2DPicture*)screen->search(tag);
	int d = (texture->getWidth() - picture->getWidth()) / 2;
	picture->changeTexture(texture->getTexInfo(), 0);
	JUTRect bounds = picture->getBounds();
	bounds.reform(-d, 0, d, 0);
	picture->mBounds = bounds;
	return picture->getWidth();
}

// PAL: the pulsing bubble around an option fits its widest picture.
inline void fitBubble(TPaneScalingControl* bubble, int width)
{
	int d = (width + 30 - bubble->mInitialBounds.getWidth()) / 2;
	bubble->mInitialBounds.reform(-d, 0, d, 0);
	bubble->update();
}
#endif

} // namespace

const TOptionSoundUnit::FabricatedSoundSettings
    TOptionSoundUnit::cSoundSettings[]
    = {
	      { MSD_SE_SY_SOUT_MONO, 0 },
	      { MSD_SE_SY_SOUT_STEREO, 1 },
	      { MSD_SE_SY_SOUT_SURROUND, 2 },
      };

const TOptionSoundUnit::FabricatedFlagInfo TOptionSoundUnit::cFlagInfos[] = {
	{ TOptionSoundUnit::SOUND_TYPE_MONO, 0 },
	{ TOptionSoundUnit::SOUND_TYPE_STEREO, 1 },
	{ TOptionSoundUnit::SOUND_TYPE_SURROUND, 2 },
};

void TArrowControl::update()
{
	updateAlpha();
	if (mPane->getAlpha() != 0) {
		updateScale();
	}
}

void TArrowControl::updateAlpha()
{
	int iVar3 = unk14 != 0 ? 1 : -1;
	mPane->setAlpha(
	    JGeometry::TUtil<s32>::clamp(iVar3 * 8 + mPane->getAlpha(), 0, 255));
}

static inline JUTRect ArrowRectA(int a, int b, int c, int d) { return JUTRect(a, b, c, d); }
static inline JUTRect ArrowRect(int a, int b, int c, int d) { return ArrowRectA(a, b, c, d); }
// incorrect
void TArrowControl::updateScale()
{
	int move = calcMoveX(mPhase);
	mPane->setBounds(
	    ArrowRect(mBounds.x1 - move, mBounds.y1, mBounds.x2, mBounds.y2));

	mPhase = JGeometry::TUtil<int>::mod(mPhase + 101, 100);
}

int TArrowControl::calcMoveX(int phase) const
{
	int iVar3 = phase < 50 ? phase : 100 - phase;
	f32 fVar1 = iVar3 / 50.0f;
	f32 fVar2 = 1.0f - fVar1;
	return fVar1 * 8.0f * fVar1 + fVar2 * -8.0f * fVar2 + fVar1 * fVar2;
}

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
TArrowControl::TArrowControl(J2DScreen* screen, J2DPane* pane)
    : mScreen(screen)
    , mPane(pane)
    , unk14(true)
    , mPhase(0)
{
	mBounds = mPane->getBounds();
	loadTexture(mTextures, (char*)"select_quit.bti");
}

// The label grows to the right (the arrow's tip stays where it is).
int TArrowControl::replaceTexture(u32 tag, JUTTexture* texture)
{
	J2DPicture* picture = (J2DPicture*)mScreen->search(tag);
	int d               = texture->getWidth() - picture->getWidth();
	picture->changeTexture(texture->getTexInfo(), 0);
	JUTRect bounds = picture->getBounds();
	bounds.reform(0, 0, d, 0);
	picture->mBounds = bounds;
	return picture->getWidth();
}
#endif

TBalloonControl::TBalloonControl(int size)
    : unk4(size)
    , unk8(0)
{
	unk0 = new UnknownBalloonControlStruct[size];
}

void TBalloonControl::add(TExPane* pane)
{
	if (unk4 > unk8) {
		unk0[unk8].unk0 = pane;
		unk0[unk8].unk4 = pane->getPane()->getAlpha();
		++unk8;
	}
}

void TBalloonControl::setupAnm()
{
	mFrameCtrl.init((unk8 + 1) * 120.0f);
	mFrameCtrl.setAttribute(J3DFrameCtrl::ATTR_LOOP);
	unk20 = 0;
	for (int i = 0; i < unk8; ++i)
		unk0[i].unk0->getPane()->setAlpha(0);
	mFrameCtrl.setRate(1.0f);
}

void TBalloonControl::startAnm() { mFrameCtrl.setRate(1.0f); }

void TBalloonControl::stopAnm()
{
	mFrameCtrl.setRate(0.0f);
	mFrameCtrl.reset();
	for (int i = 0; i < unk8; ++i)
		unk0[i].unk0->getPane()->setAlpha(0);
}

void TBalloonControl::update() { }

TPaneScalingControl::TPaneScalingControl(J2DPane* pane)
    : mPane(pane)
{
	mInitialBounds = pane->getBounds();
}

void TPaneScalingControl::setupAnm(f32 amplitude, f32 speed)
{
	mAmplitude = amplitude;
	mFrameCtrl.init(0x78);
	mFrameCtrl.setAttribute(J3DFrameCtrl::ATTR_LOOP);
	mFrameCtrl.setRate(speed);
}

void TPaneScalingControl::startAnm() { mFrameCtrl.setRate(1.0f); }

void TPaneScalingControl::stopAnm()
{
	mFrameCtrl.setRate(0.0f);
	mFrameCtrl.reset();
}

void TPaneScalingControl::update()
{
	int iVar10 = mInitialBounds.getWidth();
	int iVar5  = mInitialBounds.getHeight();

	f32 progress = (f32)mFrameCtrl.getFrame() / (f32)mFrameCtrl.getEnd();
	f32 fVar2    = mAmplitude * MsSin(RAD_TO_DEG(progress * (2 * M_PI)));

	int uVar6 = fVar2 * iVar10;
	int uVar1 = fVar2 * iVar5;

	JUTRect local_5c = mInitialBounds;
	local_5c.move(mInitialBounds.x1 - uVar6 / 2, mInitialBounds.y1 - uVar1 / 2);
	local_5c.resize(uVar6 + mInitialBounds.getWidth(),
	                uVar1 + mInitialBounds.getHeight());
	mPane->mBounds = local_5c;
	mFrameCtrl.update();
}

TPatternAnmControl::TPatternAnmControl(J2DScreen* screen)
    : mScreen(screen)
{
}

void TPatternAnmControl::set(const TPatternAnmControl::TAnmChunk* chunks,
                             int num_chunks)
{
	mChunks.set(chunks, num_chunks);
	hide();
}

void TPatternAnmControl::setupAnm()
{
	mCurrentChunk = mChunks.begin();

	// Kahan compensated summation via Fast2Sum
	f32 sum   = 0.0f;
	f32 error = 0.0f;
	for (const TAnmChunk *it = mChunks.begin(), *e = mChunks.end(); it != e;
	     ++it) {
		f32 lastSum = sum;
		f32 next    = error + it->mDuration;
		sum += next;
		error = next - (sum - lastSum);
	}

	mFrameCtrl.init(120.0f * sum + 1.0f);
	mFrameCtrl.setAttribute(J3DFrameCtrl::ATTR_LOOP);
	mFrameCtrl.setRate(1.0f);
	show();
	mNextTriggerFrame = mCurrentChunk->mDuration * 120.0f;
}

void TPatternAnmControl::update()
{
	if (mFrameCtrl.checkPass(mNextTriggerFrame)
	    || mFrameCtrl.checkState(J3DFrameCtrl::STATE_LOOPED_ONCE)) {
		mScreen->search(mCurrentChunk->mTag)->hide();
		if (++mCurrentChunk == mChunks.end()
		    || mFrameCtrl.checkState(J3DFrameCtrl::STATE_LOOPED_ONCE)) {
			mCurrentChunk     = mChunks.begin();
			mNextTriggerFrame = 0.0f;
		}
		mScreen->search(mCurrentChunk->mTag)->show();
		mNextTriggerFrame += mCurrentChunk->mDuration * 120.0f;
	}
	mFrameCtrl.update();
}

void TPatternAnmControl::show()
{
	hide();
	mScreen->search(mCurrentChunk->mTag)->show();
}

void TPatternAnmControl::hide()
{
	for (const TAnmChunk* it = mChunks.begin(); it != mChunks.end(); ++it)
		mScreen->search(it->mTag)->hide();
}

TToggleControl::TToggleControl(J2DScreen* screen)
    : mScreen(screen)
{
}

void TToggleControl::setupToggle(const u32* tags, int num_tags)
{
	// TODO: the const_cast is a guess, the retail code instantiates
	// ArrayWrapper<unsigned long> (not <const unsigned long>).
	mItems.set(const_cast<u32*>(tags), num_tags);
	for (const u32* it = mItems.begin(); it != mItems.end(); ++it)
		mScreen->search(*it)->hide();
	mCurItem = mItems.begin();
	mScreen->search(*mCurItem)->show();
}

void TToggleControl::toggle()
{
	mScreen->search(*mCurItem)->hide();
	if (++mCurItem == mItems.end())
		mCurItem = mItems.begin();
	mScreen->search(*mCurItem)->show();
}

s32 TToggleControl::getNumber() const { return mCurItem - mItems.begin(); }

void TToggleControl::setNumber(int num)
{
	mScreen->search(*mCurItem)->hide();
	mCurItem = mItems.begin() + num;
	mScreen->search(*mCurItem)->show();
}

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
TOptionRumbleUnit::TOptionRumbleUnit(J2DScreen* screen)
    : mScreen(screen)
    , mShouldRumble(false)
{
	mParentPane   = new TExPane(mScreen, 'oya1');
	mInitialAlpha = mParentPane->getPane()->getAlpha();

	mSelectionBubble = new TPaneScalingControl(mScreen->search('me_0'));
	mSelectionBubble->setupAnm(0.05f, 1.0f);
	mSelectionBubble->stopAnm();

	mSelectionText = new TToggleControl(mScreen);
	mSelectionText->setupToggle(cRumbleToggleItems,
	                            ARRAY_COUNT(cRumbleToggleItems));

	loadTexture(mTitleTex, (char*)"select_rumble.bti");
	const char* sel[] = { "select_on.bti", "select_off.bti" };
	for (int i = 0; i < 2; ++i)
		loadTexture(mSelTex[i], (char*)sel[i]);

	setState(STATE_INACTIVE);
}

void TOptionRumbleUnit::changeTexture(int language)
{
	int width = changePaneTexture(mScreen, 'm_1', mTitleTex[language]);
	for (int i = 0; i < 2; ++i) {
		int w = changePaneTexture(mScreen, 'sel0' + i, mSelTex[i][language]);
		if (w > width)
			width = w;
	}
	fitBubble(mSelectionBubble, width);
}
#else
TOptionRumbleUnit::TOptionRumbleUnit(J2DScreen* screen)
    : mScreen(screen)
    , mShouldRumble(false)
{
	mParentPane   = new TExPane(mScreen, 'oya1');
	mInitialAlpha = mParentPane->getPane()->getAlpha();

#ifndef VERSION_GMSP01
	unkC = new TBalloonControl(3);
	unkC->add(new TExPane(mScreen, 'bub0'));
	unkC->add(new TExPane(mScreen, 'bub1'));
	unkC->add(new TExPane(mScreen, 'bub2'));
	unkC->setupAnm();
#endif

	// The speech bubble around the on/off text that pulsates
	// when this setting is selected.
	mSelectionBubble = new TPaneScalingControl(mScreen->search('me_0'));
	mSelectionBubble->setupAnm(0.05f, 1.0f);
	mSelectionBubble->stopAnm();

	// The image of a gamepad that either shakes occasionally or not based on
	// whether rumble is enabled in the options.
	mGamepadIcon[1] = new TPatternAnmControl(mScreen);
	mGamepadIcon[1]->set(cRumbleAnm, ARRAY_COUNT(cRumbleAnm));
	mGamepadIcon[1]->setupAnm();
	mGamepadIcon[1]->hide();

	mGamepadIcon[0] = new TPatternAnmControl(mScreen);
	mGamepadIcon[0]->set(cStopRumbleAnm, ARRAY_COUNT(cStopRumbleAnm));
	mGamepadIcon[0]->setupAnm();
	mGamepadIcon[0]->hide();

	// The text that says on/off for rumble in the options menu.
	mSelectionText = new TToggleControl(mScreen);
	mSelectionText->setupToggle(cRumbleToggleItems,
	                            ARRAY_COUNT(cRumbleToggleItems));

	setState(STATE_INACTIVE);
}
#endif

#pragma dont_inline on
void TOptionRumbleUnit::update()
{
	switch (mState) {
	case STATE_DEACTIVATING:
		mParentPane->update();
		// fade-out animation is done
		if (mParentPane->getPane()->getAlpha() == 150)
			setState(STATE_INACTIVE);
		break;

	case STATE_ACTIVE:
		mParentPane->update();
		mSelectionBubble->update();
#if !(defined(VERSION_GMSP01) && !defined(VERSION_GMSE01))
		mGamepadIcon[getValue()]->update();
		checkRumble();
#endif
		break;

	case STATE_INACTIVE:
		break;
	}
}
#pragma dont_inline off

void TOptionRumbleUnit::checkRumble()
{
	if (mShouldRumble) {
		if (mGamepadIcon[mSelectionText->getNumber()]->checkCompletedOnce()) {
			mShouldRumble = false;
			SMSRumbleMgr->stop();
		} else {
			switch (mGamepadIcon[mSelectionText->getNumber()]
			            ->getCurrentPaneTag()) {
			case 'cnt0':
				SMSRumbleMgr->stop();
				break;

			case 'cnt1':
			case 'cnt2':
				SMSRumbleMgr->start(8, (float*)nullptr);
				break;
			}
		}
	}
}

void TOptionRumbleUnit::toggle()
{
	mShouldRumble = true;
	mSelectionText->toggle();
	adjust();
	SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_SELECT_COMMON, 0, nullptr, 0);
	SMSRumbleMgr->start(8, (float*)nullptr);
}

void TOptionRumbleUnit::adjust()
{
	bool b = mSelectionText->getNumber() == 1 ? true : false;
	SMSRumbleMgr->setActive(b);
#if !(defined(VERSION_GMSP01) && !defined(VERSION_GMSE01))
	adjustView();
#endif
}

void TOptionRumbleUnit::adjustView()
{
	switch (mSelectionText->getNumber()) {
	case 0:
		mGamepadIcon[1]->hide();
		mGamepadIcon[0]->setupAnm();
		break;
	case 1:
		mGamepadIcon[0]->hide();
		mGamepadIcon[1]->setupAnm();
		break;
	}
}

void TOptionRumbleUnit::show() { }

void TOptionRumbleUnit::hide() { }

void TOptionRumbleUnit::deactivate(bool force)
{
	SMSRumbleMgr->stop();
	if (force)
		setState(TOptionRumbleUnit::STATE_INACTIVE);
	else
		setState(TOptionRumbleUnit::STATE_DEACTIVATING);
}

void TOptionRumbleUnit::activate()
{
	setState(TOptionRumbleUnit::STATE_ACTIVE);
}

void TOptionRumbleUnit::setValue(TOptionRumbleUnit::RumbleType type)
{
	mSelectionText->setNumber(type);
	adjust();
}

void TOptionRumbleUnit::setState(TOptionRumbleUnit::State state)
{
	mState = state;
	switch (state) {
	case STATE_INACTIVE:
		mParentPane->getPane()->setAlpha(150);
		setInfluencedAlphaRecursive(mParentPane->getPane(), true);
		mSelectionBubble->stopAnm();
#ifndef VERSION_GMSP01
		unkC->stopAnm();
#endif
		mShouldRumble = false;
		break;

	case STATE_DEACTIVATING:
		mParentPane->setPaneAlpha(30, 150, mInitialAlpha);
		setInfluencedAlphaRecursive(mParentPane->getPane(), true);
#ifndef VERSION_GMSP01
		unkC->stopAnm();
#endif
		mShouldRumble = false;
		break;

	case STATE_ACTIVE:
		mParentPane->getPane()->setAlpha(mInitialAlpha);
		setInfluencedAlphaRecursive(mParentPane->getPane(), false);
		mSelectionBubble->startAnm();
#ifndef VERSION_GMSP01
		unkC->setupAnm();
		unkC->startAnm();
#endif
#if !(defined(VERSION_GMSP01) && !defined(VERSION_GMSE01))
		adjustView();
#endif
		break;
	}
}

void TOptionRumbleUnit::setInfluencedAlphaRecursive(J2DPane* pane, bool flag)
{
	for (JSUTreeIterator<J2DPane> it = pane->getPaneTree()->getFirstChild();
	     it != pane->getPaneTree()->getEndChild(); ++it) {
		it->setInfluenceAlpha(flag);
		setInfluencedAlphaRecursive(it.getObject(), flag);
	}
}

#ifdef VERSION_GMSP01
TOptionSubtitleUnit::TOptionSubtitleUnit(J2DScreen* screen)
    : mScreen(screen)
{
	mParentPane   = new TExPane(mScreen, 'txp2');
	mInitialAlpha = mParentPane->getPane()->getAlpha();

	// The speech bubble around the on/off text that pulsates
	// when this setting is selected.
	mSelectionBubble = new TPaneScalingControl(mScreen->search('me_2'));
	mSelectionBubble->setupAnm(0.05f, 1.0f);
	mSelectionBubble->stopAnm();

	// The text that says on/off for subtitles in the options menu.
	mSelectionText = new TToggleControl(mScreen);
	mSelectionText->setupToggle(cSubtitleToggleItems,
	                            ARRAY_COUNT(cSubtitleToggleItems));

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
	loadTexture(mTitleTex, (char*)"select_subtitles.bti");
	const char* sel[] = { "select_on.bti", "select_off.bti" };
	for (int i = 0; i < 2; ++i)
		loadTexture(mSelTex[i], (char*)sel[i]);
#endif

	setState(STATE_INACTIVE);
}

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
void TOptionSubtitleUnit::changeTexture(int language)
{
	int width = changePaneTexture(mScreen, 'm_3', mTitleTex[language]);
	for (int i = 0; i < 2; ++i) {
		int w = changePaneTexture(mScreen, 'sel5' + i, mSelTex[i][language]);
		if (w > width)
			width = w;
	}
	fitBubble(mSelectionBubble, width);
}
#endif

#pragma dont_inline on
void TOptionSubtitleUnit::update()
{
	switch (mState) {
	case STATE_DEACTIVATING:
		mParentPane->update();
		// fade-out animation is done
		if (mParentPane->getPane()->getAlpha() == 150)
			setState(STATE_INACTIVE);
		break;

	case STATE_ACTIVE:
		mParentPane->update();
		mSelectionBubble->update();
		break;

	case STATE_INACTIVE:
		break;
	}
}
#pragma dont_inline off

void TOptionSubtitleUnit::toggle()
{
	mSelectionText->toggle();
	// TODO: the result is unused, but the call to ArrayWrapper::begin()
	// survives in the retail code. Probably an adjust() that was stubbed out.
	mSelectionText->getNumber();
	SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_SELECT_COMMON, 0, nullptr, 0);
}

void TOptionSubtitleUnit::deactivate(bool force)
{
	if (force)
		setState(TOptionSubtitleUnit::STATE_INACTIVE);
	else
		setState(TOptionSubtitleUnit::STATE_DEACTIVATING);
}

void TOptionSubtitleUnit::activate()
{
	setState(TOptionSubtitleUnit::STATE_ACTIVE);
}

void TOptionSubtitleUnit::setValue(int value)
{
	mSelectionText->setNumber(value);
}

int TOptionSubtitleUnit::getValue() const
{
	return mSelectionText->getNumber();
}

void TOptionSubtitleUnit::setState(TOptionSubtitleUnit::State state)
{
	mState = state;
	switch (state) {
	case STATE_INACTIVE:
		mParentPane->getPane()->setAlpha(150);
		setInfluencedAlphaRecursive(mParentPane->getPane(), true);
		mSelectionBubble->stopAnm();
		break;

	case STATE_DEACTIVATING:
		mParentPane->setPaneAlpha(30, 150, mInitialAlpha);
		setInfluencedAlphaRecursive(mParentPane->getPane(), true);
		break;

	case STATE_ACTIVE:
		mParentPane->getPane()->setAlpha(mInitialAlpha);
		setInfluencedAlphaRecursive(mParentPane->getPane(), false);
		mSelectionBubble->startAnm();
		break;
	}
}

void TOptionSubtitleUnit::setInfluencedAlphaRecursive(J2DPane* pane, bool flag)
{
	for (JSUTreeIterator<J2DPane> it = pane->getPaneTree()->getFirstChild();
	     it != pane->getPaneTree()->getEndChild(); ++it) {
		it->setInfluenceAlpha(flag);
		setInfluencedAlphaRecursive(it.getObject(), flag);
	}
}
#endif

TOptionSoundUnit::TOptionSoundUnit(J2DScreen* screen)
    : mScreen(screen)
{
	mParentPane   = new TExPane(mScreen, 'oya2');
	mInitialAlpha = mParentPane->getPane()->getAlpha();

#ifndef VERSION_GMSP01
	unkC = new TBalloonControl(2);
	unkC->add(new TExPane(mScreen, 'bub3'));
	unkC->add(new TExPane(mScreen, 'bub4'));
	unkC->setupAnm();
#endif

	// The speech bubble around the mono/stereo/surround text that pulsates
	// when this setting is selected.
	mSelectionBubble = new TPaneScalingControl(mScreen->search('me_1'));
	mSelectionBubble->setupAnm(0.05f, 1.0f);
	mSelectionBubble->stopAnm();

	// These 3 are for the animation of a pianta (monte) vibing to the speakers
	// (the mono one is inlined at one level less depth than initMonoAnm
	// would give, so it is spelled out here)
	{
		TPatternAnmControl** ary = mMonoAnimations;

		ary[0] = new TPatternAnmControl(mScreen);
		ary[0]->set(cMonoMonteAnm, ARRAY_COUNT(cMonoMonteAnm));
		ary[0]->setupAnm();

		ary[1] = new TPatternAnmControl(mScreen);
		ary[1]->set(cMonoSpeakerAnm, ARRAY_COUNT(cMonoSpeakerAnm));
		ary[1]->setupAnm();

		mMonteIcons[0].set(mMonoAnimations, ARRAY_COUNT(mMonoAnimations));
	}
	initSteleoAnm();
	initSurroundAnm();

	// The toggle for switching between mono/stereo/surround.
	mSelectionText = new TToggleControl(mScreen);
	mSelectionText->setupToggle(cSoundToggleItems,
	                            ARRAY_COUNT(cSoundToggleItems));
	mMusicFrameCtrl.init(289);
	mMusicFrameCtrl.setAttribute(J3DFrameCtrl::ATTR_LOOP);
	mMusicFrameCtrl.setRate(1.0f);
	mMusic = nullptr;
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
	loadTexture(mTitleTex, (char*)"select_sound.bti");
	const char* sel[] = { "select_stereo.bti", "select_mono.bti",
		                  "select_surround.bti" };
	for (int i = 0; i < 3; ++i)
		loadTexture(mSelTex[i], (char*)sel[i]);
#endif
	setState(STATE_INACTIVE);
	adjustView();
}

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
void TOptionSoundUnit::changeTexture(int language)
{
	int width = changePaneTexture(mScreen, 'm_2', mTitleTex[language]);
	for (int i = 0; i < 3; ++i) {
		int w = changePaneTexture(mScreen, 'sel2' + i, mSelTex[i][language]);
		if (w > width)
			width = w;
	}
	fitBubble(mSelectionBubble, width);
}

TOptionLanguageUnit::TOptionLanguageUnit(J2DScreen* screen)
    : mScreen(screen)
{
	mParentPane   = new TExPane(mScreen, 'txp3');
	mInitialAlpha = mParentPane->getPane()->getAlpha();

	mSelectionBubble = new TPaneScalingControl(mScreen->search('me_3'));
	mSelectionBubble->setupAnm(0.05f, 1.0f);
	mSelectionBubble->stopAnm();

	// The language names ('lan0'..'lan4') are in the layout itself.
	mSelectionText = new TToggleControl(mScreen);
	mSelectionText->setupToggle(cLanguageToggleItems,
	                            ARRAY_COUNT(cLanguageToggleItems));

	loadTexture(mTitleTex, (char*)"select_language.bti");

	setState(STATE_INACTIVE);
}

#pragma dont_inline on
void TOptionLanguageUnit::update()
{
	switch (mState) {
	case STATE_DEACTIVATING:
		mParentPane->update();
		if (mParentPane->getPane()->getAlpha() == 150)
			setState(STATE_INACTIVE);
		break;

	case STATE_ACTIVE:
		mParentPane->update();
		mSelectionBubble->update();
		break;

	case STATE_INACTIVE:
		break;
	}
}
#pragma dont_inline off

void TOptionLanguageUnit::toggle()
{
	mSelectionText->toggle();
	mSelectionText->getNumber();
	SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_SELECT_COMMON, 0, nullptr, 0);
}

void TOptionLanguageUnit::deactivate(bool force)
{
	if (force)
		setState(TOptionLanguageUnit::STATE_INACTIVE);
	else
		setState(TOptionLanguageUnit::STATE_DEACTIVATING);
}

void TOptionLanguageUnit::activate()
{
	setState(TOptionLanguageUnit::STATE_ACTIVE);
}

void TOptionLanguageUnit::setValue(int value)
{
	mSelectionText->setNumber(value);
}

int TOptionLanguageUnit::getValue() const
{
	return mSelectionText->getNumber();
}

void TOptionLanguageUnit::setState(TOptionLanguageUnit::State state)
{
	mState = state;
	switch (state) {
	case STATE_INACTIVE:
		mParentPane->getPane()->setAlpha(150);
		setInfluencedAlphaRecursive(mParentPane->getPane(), true);
		mSelectionBubble->stopAnm();
		break;

	case STATE_DEACTIVATING:
		mParentPane->setPaneAlpha(30, 150, mInitialAlpha);
		setInfluencedAlphaRecursive(mParentPane->getPane(), true);
		break;

	case STATE_ACTIVE:
		mParentPane->getPane()->setAlpha(mInitialAlpha);
		setInfluencedAlphaRecursive(mParentPane->getPane(), false);
		mSelectionBubble->startAnm();
		break;
	}
}

void TOptionLanguageUnit::setInfluencedAlphaRecursive(J2DPane* pane, bool flag)
{
	for (JSUTreeIterator<J2DPane> it = pane->getPaneTree()->getFirstChild();
	     it != pane->getPaneTree()->getEndChild(); ++it) {
		it->setInfluenceAlpha(flag);
		setInfluencedAlphaRecursive(it.getObject(), flag);
	}
}

void TOptionLanguageUnit::changeTexture(int language)
{
	int width = replaceTexture('m_4', mTitleTex[language]);
	int w     = mScreen->search('lan0' + mSelectionText->getNumber())->getWidth();
	if (w > width)
		width = w;
	fitBubble(mSelectionBubble, width);
}

int TOptionLanguageUnit::replaceTexture(u32 tag, JUTTexture* texture)
{
	return changePaneTexture(mScreen, tag, texture);
}
#endif

void TOptionSoundUnit::initMonoAnm()
{
	TPatternAnmControl** ary = mMonoAnimations;

	ary[0] = new TPatternAnmControl(mScreen);
	ary[0]->set(cMonoMonteAnm, ARRAY_COUNT(cMonoMonteAnm));
	ary[0]->setupAnm();

	ary[1] = new TPatternAnmControl(mScreen);
	ary[1]->set(cMonoSpeakerAnm, ARRAY_COUNT(cMonoSpeakerAnm));
	ary[1]->setupAnm();

	mMonteIcons[0].set(mMonoAnimations, ARRAY_COUNT(mMonoAnimations));
}

void TOptionSoundUnit::initSteleoAnm()
{
	TPatternAnmControl** ary = mStereoAnimations;

	ary[0] = new TPatternAnmControl(mScreen);
	ary[0]->set(cSteMonteAnm, ARRAY_COUNT(cSteMonteAnm));
	ary[0]->setupAnm();

	ary[1] = new TPatternAnmControl(mScreen);
	ary[1]->set(cSteRSpeakerAnm, ARRAY_COUNT(cSteRSpeakerAnm));
	ary[1]->setupAnm();

	ary[2] = new TPatternAnmControl(mScreen);
	ary[2]->set(cSteLSpeakerAnm, ARRAY_COUNT(cSteLSpeakerAnm));
	ary[2]->setupAnm();

	mMonteIcons[1].set(mStereoAnimations, ARRAY_COUNT(mStereoAnimations));
}

void TOptionSoundUnit::initSurroundAnm()
{
	TPatternAnmControl** ary = mSurroundAnimations;

	ary[0] = new TPatternAnmControl(mScreen);
	ary[0]->set(cSurMonteAnm, ARRAY_COUNT(cSurMonteAnm));
	ary[0]->setupAnm();

	ary[1] = new TPatternAnmControl(mScreen);
	ary[1]->set(cSurRTSpeakerAnm, ARRAY_COUNT(cSurRTSpeakerAnm));
	ary[1]->setupAnm();

	ary[2] = new TPatternAnmControl(mScreen);
	ary[2]->set(cSurLTSpeakerAnm, ARRAY_COUNT(cSurLTSpeakerAnm));
	ary[2]->setupAnm();

	ary[3] = new TPatternAnmControl(mScreen);
	ary[3]->set(cSurRBSpeakerAnm, ARRAY_COUNT(cSurRBSpeakerAnm));
	ary[3]->setupAnm();

	ary[4] = new TPatternAnmControl(mScreen);
	ary[4]->set(cSurLBSpeakerAnm, ARRAY_COUNT(cSurLBSpeakerAnm));
	ary[4]->setupAnm();

	mMonteIcons[2].set(mSurroundAnimations, ARRAY_COUNT(mSurroundAnimations));
}

#pragma dont_inline on
void TOptionSoundUnit::update()
{
	switch (mState) {
	case STATE_DEACTIVATING:
		mParentPane->update();
		// fade-out animation is done
		if (mParentPane->getPane()->getAlpha() == 150)
			setState(STATE_INACTIVE);
		break;

	case STATE_ACTIVE:
		mParentPane->update();
		mSelectionBubble->update();
		updatePatternAnm();
		break;

	case STATE_INACTIVE:
		break;
	}
}
#pragma dont_inline off

void TOptionSoundUnit::updatePatternAnm()
{
	ArrayWrapper<TPatternAnmControl*>& ary
	    = mMonteIcons[mSelectionText->getNumber()];

	for (TPatternAnmControl** it = ary.begin(); it != ary.end(); ++it)
		(*it)->update();

	mMusicFrameCtrl.update();
	if (mMusicFrameCtrl.checkState(J3DFrameCtrl::STATE_LOOPED_ONCE))
		adjustSound();
}

void TOptionSoundUnit::foreachPatternAnm(ArrayWrapper<TPatternAnmControl*>& ary,
                                         void (TPatternAnmControl::*ptmf)())
{
	for (TPatternAnmControl** it = ary.mData; it != ary.mData + ary.mSize; ++it)
		((*it)->*ptmf)();
}

void TOptionSoundUnit::toggle()
{
	mSelectionText->toggle();
	adjust();
	adjustSound();
}

void TOptionSoundUnit::adjust()
{
	adjustView();
	const FabricatedSoundSettings& setting
	    = cSoundSettings[mSelectionText->getNumber()];
	JAIGlobalParameter::setParamSoundOutputMode(setting.mOutputMode);
}

void TOptionSoundUnit::show() { }

void TOptionSoundUnit::hide() { }

void TOptionSoundUnit::deactivate(bool force)
{
	if (force)
		setState(TOptionSoundUnit::STATE_INACTIVE);
	else
		setState(TOptionSoundUnit::STATE_DEACTIVATING);
}

void TOptionSoundUnit::activate() { setState(TOptionSoundUnit::STATE_ACTIVE); }

void TOptionSoundUnit::setValue(int value)
{
	mSelectionText->setNumber(flagToType(value));
	adjust();
}

int TOptionSoundUnit::getValue() const
{
	return typeToFlag((SoundType)mSelectionText->getNumber());
}

void TOptionSoundUnit::stopSound()
{
	if (mMusic)
		mMusic->stop(1);
}

TOptionSoundUnit::SoundType TOptionSoundUnit::flagToType(int flag)
{
	for (const FabricatedFlagInfo* it = cFlagInfos;
	     it != cFlagInfos + ARRAY_COUNT(cFlagInfos); ++it) {
		if (it->mFlag == flag)
			return it->mSoundType;
	}

	return SOUND_TYPE_MONO;
}

int TOptionSoundUnit::typeToFlag(TOptionSoundUnit::SoundType type)
{
	for (const FabricatedFlagInfo* it = cFlagInfos;
	     it != cFlagInfos + ARRAY_COUNT(cFlagInfos); ++it) {
		if (it->mSoundType == type)
			return it->mFlag;
	}

	return 0;
}

void TOptionSoundUnit::setState(TOptionSoundUnit::State state)
{
	mState = state;
	switch (state) {
	case STATE_INACTIVE:
		mParentPane->getPane()->setAlpha(150);
		setInfluencedAlphaRecursive(mParentPane->getPane(), true);
		mSelectionBubble->stopAnm();
#ifndef VERSION_GMSP01
		unkC->stopAnm();
#endif
		stopSound();
		break;

	case STATE_DEACTIVATING:
		mParentPane->setPaneAlpha(30, 150, mInitialAlpha);
		setInfluencedAlphaRecursive(mParentPane->getPane(), true);
#ifndef VERSION_GMSP01
		unkC->stopAnm();
#endif
		break;

	case STATE_ACTIVE:
		mParentPane->getPane()->setAlpha(mInitialAlpha);
		setInfluencedAlphaRecursive(mParentPane->getPane(), false);
		mSelectionBubble->startAnm();
#ifndef VERSION_GMSP01
		unkC->setupAnm();
		unkC->startAnm();
#endif
		adjustView();
		adjustSound();
		break;
	}
}

void TOptionSoundUnit::adjustView()
{
	switch (mSelectionText->getNumber()) {
	case SOUND_TYPE_MONO:
		foreachPatternAnm(mMonteIcons[1], &TPatternAnmControl::hide);
		foreachPatternAnm(mMonteIcons[2], &TPatternAnmControl::hide);
		foreachPatternAnm(mMonteIcons[0], &TPatternAnmControl::show);
		foreachPatternAnm(mMonteIcons[0], &TPatternAnmControl::setupAnm);
		break;
	case SOUND_TYPE_STEREO:
		foreachPatternAnm(mMonteIcons[0], &TPatternAnmControl::hide);
		foreachPatternAnm(mMonteIcons[2], &TPatternAnmControl::hide);
		foreachPatternAnm(mMonteIcons[1], &TPatternAnmControl::show);
		foreachPatternAnm(mMonteIcons[1], &TPatternAnmControl::setupAnm);
		break;
	case SOUND_TYPE_SURROUND:
		foreachPatternAnm(mMonteIcons[0], &TPatternAnmControl::hide);
		foreachPatternAnm(mMonteIcons[1], &TPatternAnmControl::hide);
		foreachPatternAnm(mMonteIcons[2], &TPatternAnmControl::show);
		foreachPatternAnm(mMonteIcons[2], &TPatternAnmControl::setupAnm);
		break;
	}
}

void TOptionSoundUnit::adjustSound()
{
	stopSound();

	const FabricatedSoundSettings& setting
	    = cSoundSettings[mSelectionText->getNumber()];
	SMSGetMSound()->startSoundSystemSE(setting.mSoundSystemSE, 0, &mMusic, 0);

	mMusicFrameCtrl.setFrame(0.0f);
}

void TOptionSoundUnit::setInfluencedAlphaRecursive(J2DPane* pane, bool flag)
{
	for (JSUTreeIterator<J2DPane> it = pane->getPaneTree()->getFirstChild();
	     it != pane->getPaneTree()->getEndChild(); ++it) {
		it->setInfluenceAlpha(flag);
		setInfluencedAlphaRecursive(it.getObject(), flag);
	}
}

#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
void TOptionControl::load()
{
	JKRArchive* optionArch = (JKRArchive*)JKRFileLoader::getVolume("option");

	mScreen = new J2DSetScreen("option.blo", optionArch);
	mScreen->setCullBack(GX_CULL_BACK);
	mTopMessage[0] = (J2DTextBox*)mScreen->search('m_0a');
	mTopMessage[1] = (J2DTextBox*)mScreen->search('m_0b');
	mTopMessage[0]->setFont((JUTFont*)gpSystemFont);
	mTopMessage[1]->setFont((JUTFont*)gpSystemFont);
	SMSMakeTextBuffer(mTopMessage[0], 0x200);
	SMSMakeTextBuffer(mTopMessage[1], 0x200);
	mLoadMessage[0] = JKRGetResource("/option/loadmessage_en.bmg");
	mLoadMessage[1] = JKRGetResource("/option/loadmessage_ge.bmg");
	mLoadMessage[2] = JKRGetResource("/option/loadmessage_fr.bmg");
	mLoadMessage[3] = JKRGetResource("/option/loadmessage_sp.bmg");
	mLoadMessage[4] = JKRGetResource("/option/loadmessage_it.bmg");
	mBackArrow       = new TArrowControl(mScreen, mScreen->search('yaji'));
	mRumbleOption    = new TOptionRumbleUnit(mScreen);
	mSoundOption     = new TOptionSoundUnit(mScreen);
	mSubtitleOption  = new TOptionSubtitleUnit(mScreen);
	mLanguageOption  = new TOptionLanguageUnit(mScreen);
	setType(SELECT_TYPE_RUMBLE_OPTION, true);
	loadSetting();
	mWasJumping      = false;
	mStickWasNeutral = true;
}

#pragma dont_inline on
void TOptionControl::loadSetting()
{
	switch (TFlagManager::getInstance()->getFlag(MSF_SOUND_MODE)) {
	case 0:
		mSoundOption->setValue(0);
		break;
	case 1:
		mSoundOption->setValue(1);
		break;
	case 2:
		mSoundOption->setValue(2);
		break;
	}

	switch (TFlagManager::getInstance()->getFlag(MSF_RUMBLE)) {
	case 0:
		mRumbleOption->setValue(TOptionRumbleUnit::RUMBLE_TYPE_UNK0);
		break;
	case 1:
		mRumbleOption->setValue(TOptionRumbleUnit::RUMBLE_TYPE_UNK1);
		break;
	}

	switch (TFlagManager::getInstance()->getFlag(MSF_SUBTITLES)) {
	case 0:
		mSubtitleOption->setValue(0);
		break;
	case 1:
		mSubtitleOption->setValue(1);
		break;
	}

	switch (TFlagManager::getInstance()->getFlag(MSF_LANGUAGE)) {
	case 0:
		mLanguageOption->setValue(0);
		break;
	case 1:
		mLanguageOption->setValue(1);
		break;
	case 2:
		mLanguageOption->setValue(2);
		break;
	case 3:
		mLanguageOption->setValue(3);
		break;
	case 4:
		mLanguageOption->setValue(4);
		break;
	}

	resetChangedSetting();
}
#pragma dont_inline off

void TOptionControl::movementCommon() { }

void TOptionControl::draw(J2DOrthoGraph* graph) { mScreen->draw(0, 0, graph); }

// mario walks from the card select screen to the options screen
bool TOptionControl::movementCard2Option()
{
	if (gpCameraOption->unk12 == 0) {
		mRumbleOption->mShouldRumble = false;
		mScreen->search('txp2')->show();
		mScreen->search('txp3')->show();
		mScreen->search('oya0')->show();
		mScreen->search('oya1')->show();
		mScreen->search('oya2')->show();
		mWasJumping = false;
		setType(mSelectedOption, true);

		int language = mLanguageOption->getValue();
		mBackArrow->replaceTexture('s_1', mBackArrow->mTextures[language]);
		mRumbleOption->changeTexture(language);
		mSoundOption->changeTexture(language);
		mSubtitleOption->changeTexture(language);
		mLanguageOption->changeTexture(language);
		changeTopMessage(language);
		return true;
	}

	return false;
}

bool TOptionControl::movementOption()
{
	mBackArrow->update();
	mRumbleOption->update();
	mSoundOption->update();
	mSubtitleOption->update();
	mLanguageOption->update();

	checkInput();
	writeValue();

	if (gpCameraOption->unk0 & 1) {
		mSoundOption->stopSound();
		return true;
	}

	return false;
}

// mario walks back from the options screen to the card select screen
bool TOptionControl::movementOption2Card()
{
	if (gpCameraOption->unk12 == 0) {
		mScreen->search('oya0')->hide();
		mScreen->search('oya1')->hide();
		mScreen->search('oya2')->hide();

		isChangedSetting();

		return true;
	}

	return false;
}

void TOptionControl::setType(TOptionControl::SelectType type,
                             bool initial_options_entry)
{
	if (mSelectedOption != type || initial_options_entry) {
		mSelectedOption = type;
		switch (type) {
		case SELECT_TYPE_RUMBLE_OPTION:
			mRumbleOption->activate();
			mSoundOption->deactivate(initial_options_entry);
			mSubtitleOption->deactivate(initial_options_entry);
			mLanguageOption->deactivate(initial_options_entry);
			break;
		case SELECT_TYPE_SOUND_OPTION:
			mRumbleOption->deactivate(initial_options_entry);
			mSoundOption->activate();
			mSubtitleOption->deactivate(initial_options_entry);
			mLanguageOption->deactivate(initial_options_entry);
			break;
		case SELECT_TYPE_SUBTITLE_OPTION:
			mSubtitleOption->activate();
			mRumbleOption->deactivate(initial_options_entry);
			mSoundOption->deactivate(initial_options_entry);
			mLanguageOption->deactivate(initial_options_entry);
			break;
		case SELECT_TYPE_LANGUAGE_OPTION:
			mLanguageOption->activate();
			mSubtitleOption->deactivate(initial_options_entry);
			mRumbleOption->deactivate(initial_options_entry);
			mSoundOption->deactivate(initial_options_entry);
			break;
		}

		if (!initial_options_entry)
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_CURSOR_COMMON, 0,
			                                   nullptr, 0);
	}
}

void TOptionControl::toggleCurType()
{
	switch (mSelectedOption) {
	case SELECT_TYPE_RUMBLE_OPTION:
		mRumbleOption->toggle();
		break;
	case SELECT_TYPE_SOUND_OPTION:
		mSoundOption->toggle();
		break;
	case SELECT_TYPE_SUBTITLE_OPTION:
		mSubtitleOption->toggle();
		break;
	case SELECT_TYPE_LANGUAGE_OPTION: {
		// The new language applies to the whole option screen at once.
		mLanguageOption->toggle();
		int language = mLanguageOption->getValue();
		mBackArrow->replaceTexture('s_1', mBackArrow->mTextures[language]);
		mRumbleOption->changeTexture(language);
		mSoundOption->changeTexture(language);
		mSubtitleOption->changeTexture(language);
		mLanguageOption->changeTexture(language);
		changeTopMessage(language);
		break;
	}
	}
}

void TOptionControl::checkInput()
{
	f32 fVar1 = gpMarDirector->unk18[0]->getMainStickInDir(0.0f, 1.0f);
	// The options are laid out top to bottom: rumble, sound, subtitles,
	// language. The stick has to return to neutral before it moves again.
	if (fVar1 >= 0.75f) {
		if (mStickWasNeutral) {
			mStickWasNeutral = false;
			switch (mSelectedOption) {
			case SELECT_TYPE_RUMBLE_OPTION:
			case SELECT_TYPE_SOUND_OPTION:
			default:
				setType(SELECT_TYPE_RUMBLE_OPTION, false);
				break;
			case SELECT_TYPE_SUBTITLE_OPTION:
				setType(SELECT_TYPE_SOUND_OPTION, false);
				break;
			case SELECT_TYPE_LANGUAGE_OPTION:
				setType(SELECT_TYPE_SUBTITLE_OPTION, false);
				break;
			}
		}
	} else if (fVar1 <= -0.75f) {
		if (mStickWasNeutral) {
			mStickWasNeutral = false;
			switch (mSelectedOption) {
			case SELECT_TYPE_RUMBLE_OPTION:
				setType(SELECT_TYPE_SOUND_OPTION, false);
				break;
			case SELECT_TYPE_SOUND_OPTION:
				setType(SELECT_TYPE_SUBTITLE_OPTION, false);
				break;
			default:
				setType(SELECT_TYPE_LANGUAGE_OPTION, false);
				break;
			}
		}
	} else {
		mStickWasNeutral = true;
	}

	bool jumping = SMS_IsMarioStatusTypeJumping();
	if (!mWasJumping && jumping)
		toggleCurType();
	mWasJumping = jumping;
}

void TOptionControl::writeValue()
{
	TFlagManager::getInstance()->setFlag(MSF_RUMBLE, mRumbleOption->getValue());
	TFlagManager::getInstance()->setFlag(MSF_SOUND_MODE,
	                                     mSoundOption->getValue());
	TFlagManager::getInstance()->setFlag(MSF_SUBTITLES,
	                                     mSubtitleOption->getValue());
	TFlagManager::getInstance()->setFlag(MSF_LANGUAGE,
	                                     mLanguageOption->getValue());
}

bool TOptionControl::isChangedSetting() const
{
	bool result       = true;
	bool notSubtitles = true;
	bool notSound     = true;

	if (mInitialRumbleValue == mRumbleOption->getValue()
	    && mInitialSoundValue == mSoundOption->getValue())
		notSound = false;

	if (!notSound && mInitialSubtitleValue == mSubtitleOption->getValue())
		notSubtitles = false;

	if (!notSubtitles && mInitialLanguageValue == mLanguageOption->getValue())
		result = false;

	return result;
}

void TOptionControl::resetChangedSetting()
{
	mInitialRumbleValue   = mRumbleOption->getValue();
	mInitialSoundValue    = mSoundOption->getValue();
	mInitialSubtitleValue = mSubtitleOption->getValue();
	mInitialLanguageValue = mLanguageOption->getValue();
}

// The line at the top of the option screen, in the chosen language: the A
// button ('@') and the control stick ('*') marks are drawn in their colours.
void TOptionControl::changeTopMessage(int language)
{
	bool invalid = true;
	if (language >= 0 && (u32)language < 5)
		invalid = false;
	if (invalid)
		language = 0;

	const char* src = (const char*)SMSGetMessageData(mLoadMessage[language], 0x1C);
	char buffer[0x200];
	char* dst = buffer;
	for (; *src != '\0'; src++) {
		char c = *src;
		if (c == '@') {
			strncpy(dst,
			        "\x1bGM[0]\x1b"
			        "CC[64ff64]\x1b"
			        "FX[26]\x1b"
			        "FY[26]\x1bSH[3]\x1b"
			        "CD[4]@\x1bGM[0]\x1b"
			        "CC\x1b"
			        "FX\x1b"
			        "FY\x1bSH\x1b"
			        "CU[4]",
			        0x44);
			dst += 0x44;
		} else if (c == '*') {
			strncpy(dst,
			        "\x1bGM[0]\x1b"
			        "CC[dcdcdc]\x1b"
			        "FX[26]\x1b"
			        "FY[26]\x1bSH[3]\x1b"
			        "CD[4]*\x1bGM[0]\x1b"
			        "CC\x1b"
			        "FX\x1b"
			        "FY\x1bSH\x1b"
			        "CU[4]",
			        0x44);
			dst += 0x44;
		} else {
			*dst++ = c;
		}
	}
	*dst = '\0';
	strncpy(mTopMessage[0]->getStringPtr(), buffer, 0x200);
	strncpy(mTopMessage[1]->getStringPtr(), buffer, 0x200);
}
#else
void TOptionControl::load()
{
	JKRArchive* optionArch = (JKRArchive*)JKRFileLoader::getVolume("option");

	mScreen = new J2DSetScreen("option.blo", optionArch);
	mScreen->setCullBack(GX_CULL_BACK);
	J2DTextBox* textBoxA = (J2DTextBox*)mScreen->search('m_0a');
	J2DTextBox* textBoxB = (J2DTextBox*)mScreen->search('m_0b');
	textBoxA->setFont((JUTFont*)gpSystemFont);
	textBoxB->setFont((JUTFont*)gpSystemFont);
	mBackArrow    = new TArrowControl(mScreen->search('yaji'));
	mRumbleOption = new TOptionRumbleUnit(mScreen);
	mSoundOption  = new TOptionSoundUnit(mScreen);
#ifdef VERSION_GMSP01
	mSubtitleOption = new TOptionSubtitleUnit(mScreen);
#endif
	setType(SELECT_TYPE_RUMBLE_OPTION, true);
	loadSetting();
	mWasJumping = false;
#ifdef VERSION_GMSP01
	mStickWasNeutral = true;
#endif
}

#pragma dont_inline on
void TOptionControl::loadSetting()
{
	switch (TFlagManager::getInstance()->getFlag(MSF_SOUND_MODE)) {
	case 0:
		mSoundOption->setValue(0);
		break;
	case 1:
		mSoundOption->setValue(1);
		break;
	case 2:
		mSoundOption->setValue(2);
		break;
	}

	switch (TFlagManager::getInstance()->getFlag(MSF_RUMBLE)) {
	case 0:
		mRumbleOption->setValue(TOptionRumbleUnit::RUMBLE_TYPE_UNK0);
		break;
	case 1:
		mRumbleOption->setValue(TOptionRumbleUnit::RUMBLE_TYPE_UNK1);
		break;
	}

#ifdef VERSION_GMSP01
	switch (TFlagManager::getInstance()->getFlag(MSF_SUBTITLES)) {
	case 0:
		mSubtitleOption->setValue(0);
		break;
	case 1:
		mSubtitleOption->setValue(1);
		break;
	}
#endif

	resetChangedSetting();
}
#pragma dont_inline off

void TOptionControl::movementCommon() { }

void TOptionControl::draw(J2DOrthoGraph* graph) { mScreen->draw(0, 0, graph); }

// mario walks from the card select screen to the options screen
bool TOptionControl::movementCard2Option()
{
	if (gpCameraOption->unk12 == 0) {
		mRumbleOption->mShouldRumble = false;
		mScreen->search('txp2')->show();
		mScreen->search('oya0')->show();
		mScreen->search('oya1')->show();
		mScreen->search('oya2')->show();
		mWasJumping = false;
		setType(mSelectedOption, true);
		return true;
	}

	return false;
}

bool TOptionControl::movementOption()
{
	mBackArrow->update();
	mRumbleOption->update();
	mSoundOption->update();
#ifdef VERSION_GMSP01
	mSubtitleOption->update();
#endif

	checkInput();
	writeValue();

	if (gpCameraOption->unk0 & 1) {
		mSoundOption->stopSound();
		return true;
	}

	return false;
}

// mario walks back from the options screen to the card select screen
bool TOptionControl::movementOption2Card()
{
	if (gpCameraOption->unk12 == 0) {
		mScreen->search('oya0')->hide();
		mScreen->search('oya1')->hide();
		mScreen->search('oya2')->hide();

		isChangedSetting();

		return true;
	}

	return false;
}

void TOptionControl::setType(TOptionControl::SelectType type,
                             bool initial_options_entry)
{
	if (mSelectedOption != type || initial_options_entry) {
		mSelectedOption = type;
		switch (type) {
		case SELECT_TYPE_RUMBLE_OPTION:
			mRumbleOption->activate();
			mSoundOption->deactivate(initial_options_entry);
#ifdef VERSION_GMSP01
			mSubtitleOption->deactivate(initial_options_entry);
#endif
			break;
		case SELECT_TYPE_SOUND_OPTION:
			mRumbleOption->deactivate(initial_options_entry);
			mSoundOption->activate();
#ifdef VERSION_GMSP01
			mSubtitleOption->deactivate(initial_options_entry);
#endif
			break;
#ifdef VERSION_GMSP01
		case SELECT_TYPE_SUBTITLE_OPTION:
			mSubtitleOption->activate();
			mRumbleOption->deactivate(initial_options_entry);
			mSoundOption->deactivate(initial_options_entry);
			break;
#endif
		}

		if (!initial_options_entry)
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_CURSOR_COMMON, 0,
			                                   nullptr, 0);
	}
}

void TOptionControl::toggleCurType()
{
	switch (mSelectedOption) {
	case SELECT_TYPE_RUMBLE_OPTION:
		mRumbleOption->toggle();
		break;
	case SELECT_TYPE_SOUND_OPTION:
		mSoundOption->toggle();
		break;
#ifdef VERSION_GMSP01
	case SELECT_TYPE_SUBTITLE_OPTION:
		mSubtitleOption->toggle();
		break;
#endif
	}
}

void TOptionControl::checkInput()
{
	f32 fVar1 = gpMarDirector->unk18[0]->getMainStickInDir(0.0f, 1.0f);
#ifdef VERSION_GMSP01
	// The options are laid out top to bottom: rumble, sound, subtitles.
	// The stick has to return to neutral before it moves the cursor again.
	int sel = mSelectedOption;
	if (0.75f <= fVar1) {
		if (mStickWasNeutral) {
			mStickWasNeutral = false;
			switch (sel) {
			case SELECT_TYPE_RUMBLE_OPTION:
			case SELECT_TYPE_SOUND_OPTION:
				setType(SELECT_TYPE_RUMBLE_OPTION, false);
				break;
			case SELECT_TYPE_SUBTITLE_OPTION:
				setType(SELECT_TYPE_SOUND_OPTION, false);
				break;
			}
		}
	} else if (fVar1 <= -0.75f) {
		if (mStickWasNeutral) {
			mStickWasNeutral = false;
			switch (sel) {
			case SELECT_TYPE_RUMBLE_OPTION:
				setType(SELECT_TYPE_SOUND_OPTION, false);
				break;
			case SELECT_TYPE_SOUND_OPTION:
			case SELECT_TYPE_SUBTITLE_OPTION:
				setType(SELECT_TYPE_SUBTITLE_OPTION, false);
				break;
			}
		}
	} else {
		mStickWasNeutral = true;
	}
#else
	if (fVar1 >= 0.75f)
		setType(SELECT_TYPE_RUMBLE_OPTION, false);
	else if (fVar1 <= -0.75f)
		setType(SELECT_TYPE_SOUND_OPTION, false);
#endif

	bool jumping = SMS_IsMarioStatusTypeJumping();
	if (!mWasJumping && jumping)
		toggleCurType();
	mWasJumping = jumping;
}

void TOptionControl::writeValue()
{
	TFlagManager::getInstance()->setFlag(MSF_RUMBLE, mRumbleOption->getValue());
	TFlagManager::getInstance()->setFlag(MSF_SOUND_MODE,
	                                     mSoundOption->getValue());
#ifdef VERSION_GMSP01
	TFlagManager::getInstance()->setFlag(MSF_SUBTITLES,
	                                     mSubtitleOption->getValue());
#endif
}

bool TOptionControl::isChangedSetting() const
{
	bool result = true;

#ifdef VERSION_GMSP01
	bool rumbleOrSoundChanged = true;
	if (mInitialRumbleValue == mRumbleOption->getValue()
	    && mInitialSoundValue == mSoundOption->getValue())
		rumbleOrSoundChanged = false;

	if (!rumbleOrSoundChanged
	    && mInitialSubtitleValue == mSubtitleOption->getValue())
		result = false;
#else
	if (mInitialRumbleValue == mRumbleOption->getValue()
	    && mInitialSoundValue == mSoundOption->getValue())
		result = false;
#endif

	return result;
}

void TOptionControl::resetChangedSetting()
{
	mInitialRumbleValue = mRumbleOption->getValue();
	mInitialSoundValue  = mSoundOption->getValue();
#ifdef VERSION_GMSP01
	mInitialSubtitleValue = mSubtitleOption->getValue();
#endif
}
#endif
