#pragma once
#include "lgfx_config.hpp"
#include "pokemon_client.h"

namespace ui {

void begin(LGFX &gfx);

void drawStatus(LGFX &gfx, const String &line1, const String &line2);

// Draws the full card screen: name/set/number text, and a placeholder box
// in the art area. Call prefetchArt()+drawBufferedArt() (or drawArt()) to
// fill the art area in without redrawing the text.
void drawCard(LGFX &gfx, const WantedCard &card);

// Downloads imageUrl into the internal art buffer only -- no decode/draw --
// so it can be called ahead of time (e.g. while a different card is still
// on screen) without touching the panel. Returns whether the download
// completed. Does blocking network I/O -- call from the background art
// task, never the main loop.
bool prefetchArt(const String &imageUrl);

// Decodes and draws whatever prefetchArt() last downloaded, or the
// placeholder on failure (including if nothing was prefetched). Returns
// whether art was drawn. CPU-only, no network -- fast enough to be safe to
// call right after prefetchArt() succeeds, but still belongs on the
// background task alongside it, matching every other gfx call outside the
// main loop.
bool drawBufferedArt(LGFX &gfx);

// prefetchArt() + drawBufferedArt() in one call, for when there's nothing
// usable already buffered (e.g. a prefetch didn't finish in time). Returns
// whether art was drawn.
bool drawArt(LGFX &gfx, const String &imageUrl);

}  // namespace ui
