/*
 * port_widescreen.h - game side of true widescreen (Hor+) for the PC port (w11).
 *
 * Contract with the render side (w6, port_window.cpp / Aurora patches):
 *  - The game keeps drawing into its 640x448 (x480 for 2D) logical frame. In a widescreen
 *    mode the render target and the display get the wide shape, i.e. the logical frame is
 *    shown anamorphically, every GX viewport/scissor x stretched by PORTWideScale().
 *  - So the game must (a) widen its 3D view horizontally by the same factor, keeping the
 *    vertical FOV of the GameCube (camera aspect * scale), and (b) squeeze its 4:3 2D/HUD
 *    by 1/scale around the screen centre so menus keep their shape. Full-screen 2D drawn
 *    with a plain 0..640 projection (fader rects, wipes, screen-texture quads) needs no
 *    change: it already spans the whole target.
 *  - PORTWideScale() = display aspect / (4/3); exactly 1.0f in the 4:3, fit and stretch
 *    modes. Every hook below is an exact no-op at 1.0f (GameCube picture, same state).
 *
 * The scale comes from port_get_aspect_scale() (w6's setting); SMS_PORT_WIDESCREEN
 * ("16:9", "21:9", "1.7778", ...) overrides it for tests.
 * Implementation: port/src/port_widescreen.cpp.
 */
#ifndef PORT_WIDESCREEN_H
#define PORT_WIDESCREEN_H

#ifdef SMS_PORT

#ifdef __cplusplus
extern "C" {
#endif

/* Horizontal widening of the picture: display aspect / (4/3), 1.0f at 4:3. */
float PORTWideScale(void);

/* 3D: widen a camera's projection aspect in place (call before building its projection).
 * Remembers the game's own value per aspect field, so it can run every frame and follows
 * the game's (and the setting's) later changes; a no-op at scale 1. All consumers of the
 * camera aspect (frustum culling, projected textures, particles) then see the wide view. */
void PORTWideCameraAspect(float* aspect);

/* 2D: squeeze a logical-frame x range (viewport/scissor, pixels of the 640-wide frame)
 * around the frame centre by 1/scale (the int variant, for scissors, rounds to pixels). */
void PORTWideSqueezeRectX(float* x, float* width);
void PORTWideSqueezeRectXi(int* x, int* width);

/* 2D: widen an orthographic x range around its centre by scale (squeezes what is drawn in
 * it by 1/scale on the anamorphic target). */
void PORTWideOrthoX(float* left, float* right);

/* Gameplay view tests with a fixed horizontal sight angle (degrees): the extra angle the
 * wide view adds for a camera with vertical FOV `fovy` and (already widened) `aspect`.
 * 0 at scale 1. */
float PORTWideSightExtraDeg(float fovy, float aspect);

/* Widescreen HUD position (setting [window] hud_position = centered / 16:9 / match, or for
 * tests SMS_PORT_HUD_EDGES = 0 / 1 (16:9) / 2 (match)): horizontal shift for a HUD element
 * on the left (side < 0) or right (side > 0), in units of its 2D frame, `frameWidth` units
 * wide across the 4:3 picture (the HUD layout's 600-unit ortho is widened by the scale, so
 * its 4:3 edges sit at 0 / frameWidth). 0 when centred (original), at 4:3, or side == 0.
 * PORTWideHudSide classifies an element by its centre x in that frame (-1 left, 0 the middle
 * band, +1 right). */
float PORTWideHudShift(int side, float frameWidth);
int PORTWideHudSide(float centreX, float frameWidth);

/* 2D drawn in a full-frame orthographic projection that is not widened (the movie
 * director's 0..640 camera): PORTWideOrthoProjBegin squeezes the current orthographic
 * projection by 1/scale around its centre (saving it in `saved`, 7 floats as
 * GXGetProjectionv), PORTWideOrthoProjEnd restores it. No-ops at scale 1 or for a
 * perspective projection; Begin returns whether End must be called. */
int PORTWideOrthoProjBegin(float saved[7]);
void PORTWideOrthoProjEnd(const float saved[7]);

#ifdef __cplusplus
}

#include <type_traits>

/* RAII form of PORTWideOrthoProjBegin/End. */
class PORTWideOrthoProjScope {
public:
	PORTWideOrthoProjScope()
	    : mActive(PORTWideOrthoProjBegin(mSaved))
	{
	}
	~PORTWideOrthoProjScope()
	{
		if (mActive)
			PORTWideOrthoProjEnd(mSaved);
	}

private:
	float mSaved[7];
	int mActive;
};

/* 2D: while in scope, widens a full-screen J2D pane (a dimming mask, a menu background)
 * around the centre of its bounds by scale, so it covers the whole wide picture inside
 * the squeezed J2D viewport. Restores the pane's bounds afterwards; nothing at scale 1. */
template <class Pane>
class PORTWidePaneScope {
public:
	explicit PORTWidePaneScope(Pane* pane)
	    : mPane(nullptr)
	{
		const float scale = PORTWideScale();
		if (pane == nullptr || scale == 1.0f)
			return;
		mPane   = pane;
		mSaved  = pane->getBounds();
		auto r  = mSaved;
		float c = 0.5f * (float)(r.x1 + r.x2), half = 0.5f * (float)(r.x2 - r.x1) * scale;
		r.x1    = (int)(c - half - 1.0f);
		r.x2    = (int)(c + half + 1.0f);
		pane->setBounds(r);
	}
	~PORTWidePaneScope()
	{
		if (mPane != nullptr)
			mPane->setBounds(mSaved);
	}

private:
	Pane* mPane;
	typename std::decay<decltype(((Pane*)0)->getBounds())>::type mSaved;
};

/* HUD "screen edges": while in scope, moves each child group of a HUD root pane (its width
 * is the frame width) to the left or right screen edge by PORTWideHudShift, according to
 * its side; middle groups stay. Restores the positions afterwards. Nothing when off.
 * shiftOf(pane) gives the shift applied to a pane's group (0 if none), for HUD parts the
 * game draws itself from pane bounds. */
template <class Pane>
class PORTWideHudEdgesScope {
public:
	explicit PORTWideHudEdgesScope(Pane* root)
	    : mCount(0)
	{
		if (root == nullptr)
			return;
		const int frameW = root->getBounds().x2 - root->getBounds().x1;
		if (PORTWideHudShift(1, (float)frameW) == 0.0f)
			return;
		for (auto* t = root->getFirstChild(); t != root->getEndChild() && mCount < kMax;
		     t = t->getNextChild()) {
			Pane* p       = t->getObject();
			const auto& b = p->getBounds();
			// a group is an anchor pane with its parts at offsets: centre of the parts.
			// Parts wider than the whole frame (the news ticker's scrolling text boxes)
			// say nothing about where the group sits.
			float lo = (float)b.x1, hi = (float)b.x2;
			bool any = false;
			for (auto* c = p->getFirstChild(); c != p->getEndChild(); c = c->getNextChild()) {
				const auto& cb = c->getObject()->getBounds();
				if (cb.x2 - cb.x1 > frameW)
					continue;
				const float x1 = (float)(b.x1 + cb.x1), x2 = (float)(b.x1 + cb.x2);
				lo  = any && lo < x1 ? lo : x1;
				hi  = any && hi > x2 ? hi : x2;
				any = true;
			}
			const float dx = PORTWideHudShift(PORTWideHudSide(0.5f * (lo + hi), (float)frameW),
			                                  (float)frameW);
			if (dx == 0.0f)
				continue;
			mPanes[mCount] = p;
			mDx[mCount]    = (int)dx;
			p->add(mDx[mCount], 0);
			++mCount;
		}
	}
	~PORTWideHudEdgesScope()
	{
		for (int i = 0; i < mCount; ++i)
			mPanes[i]->add(-mDx[i], 0);
	}
	int shiftOf(const Pane* pane) const
	{
		for (int i = 0; i < mCount; ++i)
			if (mPanes[i] == pane)
				return mDx[i];
		return 0;
	}

private:
	enum { kMax = 48 };
	Pane* mPanes[kMax];
	int mDx[kMax];
	int mCount;
};
#endif

#endif /* SMS_PORT */

#endif /* PORT_WIDESCREEN_H */
