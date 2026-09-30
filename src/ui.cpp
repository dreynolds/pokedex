// Must precede "ui.h" (which pulls in LovyanGFX.hpp): LovyanGFX only compiles
// in its Stream-based drawPng() overload on LGFX if HTTPClient.h was already
// included by the time it's processed, per its own conditional-compilation
// convention (see lgfx_filesystem_support.hpp).
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "ui.h"
#include <vector>

namespace {

// Portrait 240x320 layout: card art on top, name/set/number stacked below
// it. images.pokemontcg.io serves the "small" art variant at a consistent
// ~245x342 (unlike the Jellyfin screen's Jellyfin-side-resized album art,
// there's no server-side resize API here, so the source size is assumed
// rather than guaranteed -- a card that came back a different size would
// just be scaled slightly off, not broken).
constexpr float kSourceCardW = 245.0f, kSourceCardH = 342.0f;

constexpr int ART_W = 240, ART_H = 260;
constexpr int ART_X = 0, ART_Y = 0;

constexpr int TEXT_X = 10, TEXT_Y = ART_Y + ART_H + 6, TEXT_W = 220, TEXT_H = 54;

constexpr uint32_t COLOR_BG = TFT_BLACK;
constexpr uint32_t COLOR_NAME = TFT_WHITE;
constexpr uint32_t COLOR_ACCENT = 0xFFE0;  // Pokeball-yellow
constexpr uint32_t COLOR_SUBTLE = 0xA514;  // light gray-blue

// Both art and text are drawn straight to the panel rather than into
// sprites. A 240x260x16bpp sprite for the art alone would be ~122KB, one
// single allocation that -- kept alive for the program's whole life --
// permanently splits this board's ~320KB heap in two, capping the largest
// free block below the ~44KB the PNG decoder needs for its own allocation
// (see drawArt). Even the much smaller ~23KB text sprite this used to be
// turned out to matter: this board's PNG-decode-capable headroom sits only
// a few KB short of 44KB as it is, so every permanently-resident sprite
// counts against it. Losing the double-buffering costs a redraw flicker on
// the roughly-once-per-10s text update, an acceptable trade here.

// Card art downloads go into this fixed-size static buffer (.bss, not the
// heap) instead of a fresh malloc() per card. The PNG decoder itself needs
// a ~44KB heap allocation every draw (see drawArt's releasePngMemory
// comment), and a repeated malloc()/free() of the ~40-58KB compressed file
// alongside that -- even though both individually succeeded -- left the
// heap fragmented enough that the decoder's allocation started failing
// after only one or two card swaps. A static buffer never touches the heap
// allocator at all, so it can't contribute to that fragmentation.
constexpr size_t kArtBufSize = 73728;  // largest observed card PNG so far is ~66KB
static uint8_t artBuf[kArtBufSize];
static size_t artBufLen = 0;  // valid bytes in artBuf, set by prefetchArt()

String truncateToWidth(LGFX &s, const String &text, int maxWidth) {
  if (s.textWidth(text) <= maxWidth) return text;
  String t = text;
  while (t.length() > 1 && s.textWidth(t + "...") > maxWidth) {
    t.remove(t.length() - 1);
  }
  return t + "...";
}

// Wraps text to at most maxLines, breaking on spaces where possible. The
// final line is ellipsis-truncated if content still doesn't fit.
std::vector<String> wrapText(LGFX &s, const String &text, int maxWidth, int maxLines) {
  std::vector<String> lines;
  String remaining = text;
  remaining.trim();

  while ((int)lines.size() < maxLines && remaining.length() > 0) {
    bool isLastAllowedLine = (int)lines.size() == maxLines - 1;

    if (s.textWidth(remaining) <= maxWidth) {
      lines.push_back(remaining);
      remaining = "";
      break;
    }

    if (isLastAllowedLine) {
      lines.push_back(truncateToWidth(s, remaining, maxWidth));
      remaining = "";
      break;
    }

    int fitLen = remaining.length();
    while (fitLen > 0 && s.textWidth(remaining.substring(0, fitLen)) > maxWidth) {
      fitLen--;
    }
    int breakAt = fitLen;
    while (breakAt > 0 && remaining[breakAt] != ' ') breakAt--;
    if (breakAt == 0) breakAt = fitLen;  // no space to break on -- hard break

    lines.push_back(remaining.substring(0, breakAt));
    remaining = remaining.substring(breakAt);
    remaining.trim();
  }
  return lines;
}

}  // namespace

namespace ui {

void begin(LGFX &gfx) {
  gfx.init();
  gfx.setRotation(0);  // native 240x320 portrait -- matches card aspect ratio
  gfx.setBrightness(200);
  gfx.fillScreen(COLOR_BG);
}

void drawStatus(LGFX &gfx, const String &line1, const String &line2) {
  gfx.fillScreen(COLOR_BG);
  gfx.setTextDatum(middle_center);

  gfx.setTextColor(COLOR_NAME);
  gfx.setTextSize(2);
  gfx.drawString(line1, gfx.width() / 2, gfx.height() / 2 - 12);

  gfx.setTextColor(COLOR_SUBTLE);
  gfx.setTextSize(1);
  gfx.drawString(line2, gfx.width() / 2, gfx.height() / 2 + 14);

  gfx.setTextDatum(top_left);
}

namespace {

void drawArtPlaceholder(LGFX &gfx) {
  gfx.fillRect(ART_X, ART_Y, ART_W, ART_H, COLOR_BG);
  gfx.drawRect(ART_X, ART_Y, ART_W, ART_H, COLOR_SUBTLE);
  gfx.setTextDatum(middle_center);
  gfx.setTextColor(COLOR_SUBTLE);
  gfx.setTextSize(3);
  gfx.drawString("?", ART_X + ART_W / 2, ART_Y + ART_H / 2);
  gfx.setTextDatum(top_left);
}

}  // namespace

bool prefetchArt(const String &imageUrl) {
  artBufLen = 0;
  if (imageUrl.length() == 0) return false;

  // Downloads the whole file into a buffer and closes the connection
  // *before* decoding, rather than decoding straight off the HTTP stream.
  // Streaming decode needs the TLS connection to stay healthy for the
  // entire multi-second decode, and on this board that repeatedly stalled
  // or dropped mid-transfer (this chip's mbedTLS stack over external HTTPS
  // has the same flakiness noted for the WebSocket connection in the
  // Jellyfin screen this was adapted from). Decoding from a complete
  // in-memory buffer has no such dependency.
  //
  // The backend now proxies most card art itself over plain HTTP (see
  // PokemonClient::fetchWantlist), so this is usually a plain WiFiClient;
  // WiFiClientSecure (with cert validation skipped, as for the API calls)
  // is only needed for cards the backend hasn't mirrored yet, still served
  // straight from the external image CDN over HTTPS. Handing a
  // WiFiClientSecure to a plain-HTTP server fails immediately with an
  // "invalid SSL record" error -- the scheme has to pick the client.
  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  HTTPClient http;
  if (imageUrl.startsWith("https://")) {
    secureClient.setInsecure();
    http.begin(secureClient, imageUrl);
  } else {
    http.begin(plainClient, imageUrl);
  }
  http.setTimeout(10000);
  http.setConnectTimeout(10000);

  bool ok = false;
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    int len = http.getSize();
    if (len > 0 && (size_t)len <= kArtBufSize) {
      WiFiClient *stream = http.getStreamPtr();
      size_t received = 0;
      unsigned long lastByteAt = millis();
      while (received < (size_t)len) {
        size_t avail = stream->available();
        if (avail > 0) {
          int n = stream->read(artBuf + received, min(avail, (size_t)len - received));
          if (n > 0) {
            received += n;
            lastByteAt = millis();
          }
        } else if (millis() - lastByteAt > 10000) {
          Serial.println("[ui] card art download stalled");
          break;
        } else {
          delay(1);
        }
      }

      if (received == (size_t)len) {
        artBufLen = len;
        ok = true;
      }
    } else if (len > 0) {
      Serial.printf("[ui] card art too large for buffer: %d bytes\n", len);
    } else {
      Serial.println("[ui] card art has no Content-Length");
    }
  } else {
    Serial.printf("[ui] GET card art failed, code=%d\n", code);
  }
  http.end();
  return ok;
}

bool drawBufferedArt(LGFX &gfx) {
  bool ok = false;
  if (artBufLen > 0) {
    // Scale-to-fit within the art area, preserving aspect ratio, centered.
    // drawPng's (x, y) is the top-left of the drawn image, not a datum
    // anchor point despite the datum parameter existing -- middle_center
    // there controls alignment *within* an explicit maxWidth/maxHeight box,
    // not centering around (x, y), so the box's top-left has to be computed
    // by hand here.
    float scale = min((float)ART_W / kSourceCardW, (float)ART_H / kSourceCardH);
    int drawW = (int)(kSourceCardW * scale);
    int drawH = (int)(kSourceCardH * scale);
    int drawX = ART_X + (ART_W - drawW) / 2;
    int drawY = ART_Y + (ART_H - drawH) / 2;

    gfx.fillRect(ART_X, ART_Y, ART_W, ART_H, COLOR_BG);
    ok = gfx.drawPng(artBuf, artBufLen, drawX, drawY, ART_W, ART_H, 0, 0, scale, scale, top_left);
  }
  artBufLen = 0;

  // LovyanGFX caches its ~44KB PNG decoder state after first use rather
  // than freeing it, so a card draw doesn't repeatedly pay to allocate it --
  // see its own comment on draw_png(). But on this board's ~320KB, no-PSRAM
  // heap, leaving that block permanently resident fragments everything else
  // down to a single-digit-KB ceiling within a couple of draws, which then
  // starves the decoder's own smaller per-call buffers. Releasing it after
  // every draw costs a fresh 44KB allocation next time (cheap, and this
  // only redraws once per card swap) in exchange for not permanently
  // splitting the heap.
  gfx.releasePngMemory();

  if (!ok) {
    drawArtPlaceholder(gfx);
  }
  return ok;
}

bool drawArt(LGFX &gfx, const String &imageUrl) {
  bool fetched = prefetchArt(imageUrl);
  bool ok = drawBufferedArt(gfx);
  return fetched && ok;
}

void drawCard(LGFX &gfx, const WantedCard &card) {
  gfx.fillScreen(COLOR_BG);
  drawArtPlaceholder(gfx);

  const int centerX = ART_X + ART_W / 2;  // art already spans the full screen width, centered

  gfx.setTextDatum(top_center);

  gfx.setTextColor(COLOR_NAME, COLOR_BG);
  gfx.setTextSize(1.6f);
  int lineHeight = gfx.fontHeight() + 2;
  std::vector<String> nameLines = wrapText(gfx, card.name, TEXT_W, 2);
  int y = TEXT_Y;
  for (const String &line : nameLines) {
    gfx.drawString(line, centerX, y);
    y += lineHeight;
  }

  gfx.setTextColor(COLOR_ACCENT, COLOR_BG);
  gfx.setTextSize(1);
  String setLine = card.setName + " #" + card.number;
  gfx.drawString(truncateToWidth(gfx, setLine, TEXT_W), centerX, y + 4);

  gfx.setTextDatum(top_left);
}

}  // namespace ui
