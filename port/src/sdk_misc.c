/* Small interim definitions for the headless boot.
 * - sinf/cosf: the x86 UCRT only has them as header inlines, but game code
 *   declares and calls them as functions. (No <math.h> here on purpose.)
 * - __GDCurrentDL: a data symbol (GD is still stubbed); an auto-generated
 *   code stub in its place would be written to as data. */
#include <dolphin/gd.h>

double sin(double);
double cos(double);

#ifndef _WIN64 /* x64 UCRT exports sinf/cosf itself */
float sinf(float x)
{
	volatile double d = x; /* keep clang from folding this back into sinf() */
	return (float)sin(d);
}

float cosf(float x)
{
	volatile double d = x;
	return (float)cos(d);
}

#endif

GDLObj* __GDCurrentDL;
