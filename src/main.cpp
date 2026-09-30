#include <Arduino.h>
#include <WiFi.h>
#include <esp_bt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "lgfx_config.hpp"
#include "ui.h"
#include "pokemon_client.h"
#include "secrets.h"

static LGFX gfx;
static PokemonClient pokemon(POKEMON_API_BASE_URL);

static const unsigned long WANTLIST_REFRESH_MS = 5UL * 60UL * 1000UL;
static const unsigned long WANTLIST_RETRY_MS = 30UL * 1000UL;  // after a failed refresh
static const unsigned long CARD_SWAP_MS = 10UL * 1000UL;

static std::vector<WantedCard> cachedCards;
static int currentCardIndex = -1;
static unsigned long lastWantlistAttemptAt = 0;
static unsigned long lastCardSwapAt = 0;
static bool haveWantlist = false;

// The next card to show is chosen and its art prefetched *while the current
// card is still on screen* (see maybeStartPrefetch), so that by the time
// CARD_SWAP_MS elapses the only work left is a fast local decode+draw
// instead of a full network fetch -- the network fetch is what makes a
// plain card swap visibly sit on the placeholder for a second or more.
static int nextCardIndex = -1;
static volatile bool nextArtReady = false;

// All gfx-touching work (drawing text/art) and all art network I/O happens
// on one persistent background task (see artWorkerTaskFn/setup), fed job
// structs over this queue, rather than a fresh xTaskCreatePinnedToCore()
// per card swap. Spinning up and tearing down an 8KB-stack task every
// ~10s turned out to fragment this board's already-tight heap on its own
// -- the PNG decoder's allocation degraded from succeeding several cards
// in a row to failing immediately, even after every other fragmentation
// source (sprites, a malloc'd-per-card download buffer) had already been
// fixed. A single long-lived task never touches the heap allocator for its
// own stack more than once. The main loop never touches `gfx` directly
// (other than the Wi-Fi status screens, gated the same way) since
// LovyanGFX doesn't support two tasks drawing to the panel concurrently.
enum class ArtJobType { kFetchAndDraw, kDrawBuffered, kPrefetch };

struct ArtJob {
  ArtJobType type;
  char *url;  // heap, freed by the worker; unused for kDrawBuffered
};

static QueueHandle_t artJobQueue = nullptr;
static volatile bool artWorkerBusy = false;

static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  ui::drawStatus(gfx, "Connecting", WIFI_SSID);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
    if (millis() - start > 15000) {
      // Keep retrying indefinitely, but let the user see it's stuck.
      ui::drawStatus(gfx, "Wi-Fi retrying", WIFI_SSID);
      start = millis();
    }
  }
  Serial.println();
  Serial.print("[wifi] connected, IP=");
  Serial.println(WiFi.localIP());
}

// A transient stall or early connection close mid-download is ordinary WiFi
// flakiness on this board, not a real failure, so the full fetch+draw path
// gets a couple of quick retries before settling for the placeholder. A
// plain prefetch doesn't retry -- it's opportunistic, and a miss just falls
// back to the slow path at swap time, which does.
static const int kArtFetchAttempts = 3;

static void artWorkerTaskFn(void *param) {
  ArtJob job;
  for (;;) {
    xQueueReceive(artJobQueue, &job, portMAX_DELAY);
    String url = job.url ? String(job.url) : String();
    if (job.url) free(job.url);

    switch (job.type) {
      case ArtJobType::kFetchAndDraw: {
        bool ok = false;
        for (int attempt = 1; attempt <= kArtFetchAttempts && !ok; attempt++) {
          ok = ui::drawArt(gfx, url);
          if (!ok && attempt < kArtFetchAttempts) {
            Serial.printf("[main] card art attempt %d failed, retrying\n", attempt);
            delay(300);
          }
        }
        if (!ok) {
          Serial.println("[main] card art fetch/decode failed, placeholder left up");
        }
        break;
      }
      case ArtJobType::kDrawBuffered:
        if (!ui::drawBufferedArt(gfx)) {
          Serial.println("[main] buffered card art decode failed, placeholder left up");
        }
        break;
      case ArtJobType::kPrefetch:
        nextArtReady = ui::prefetchArt(url);
        break;
    }
    artWorkerBusy = false;
  }
}

static void enqueueJob(ArtJobType type, const String &url = String()) {
  artWorkerBusy = true;
  ArtJob job;
  job.type = type;
  job.url = url.length() > 0 ? strdup(url.c_str()) : nullptr;
  xQueueSend(artJobQueue, &job, 0);
}

static int pickCardIndex(int excluding) {
  if (cachedCards.size() <= 1) return 0;
  int idx = excluding;
  while (idx == excluding) {
    idx = random(cachedCards.size());
  }
  return idx;
}

// Called whenever the worker is idle and no card is currently staged for
// the next swap: picks one and starts downloading its art in the
// background, while whatever's currently on screen stays put.
static void maybeStartPrefetch() {
  if (artWorkerBusy || cachedCards.empty() || nextCardIndex >= 0) return;

  nextCardIndex = pickCardIndex(currentCardIndex);
  nextArtReady = false;
  enqueueJob(ArtJobType::kPrefetch, cachedCards[nextCardIndex].imageUrl);
}

// Swaps to a new card. If one was already prefetched in time, this is just
// a fast local decode+draw of art that's already fully downloaded; only
// falls back to a full (slower, placeholder-first) fetch when it wasn't
// ready yet.
static void advanceCard() {
  if (cachedCards.empty()) return;

  if (nextArtReady && nextCardIndex >= 0) {
    currentCardIndex = nextCardIndex;
    nextCardIndex = -1;
    nextArtReady = false;
    ui::drawCard(gfx, cachedCards[currentCardIndex]);
    enqueueJob(ArtJobType::kDrawBuffered);
  } else {
    nextCardIndex = -1;
    nextArtReady = false;
    currentCardIndex = pickCardIndex(currentCardIndex);
    ui::drawCard(gfx, cachedCards[currentCardIndex]);
    enqueueJob(ArtJobType::kFetchAndDraw, cachedCards[currentCardIndex].imageUrl);
  }
}

static void refreshWantlist() {
  std::vector<WantedCard> fetched;
  if (!pokemon.fetchWantlist(fetched)) {
    Serial.println("[main] wantlist refresh failed, keeping last-known-good list");
    if (!haveWantlist) {
      ui::drawStatus(gfx, "Backend unreachable", "retrying...");
    }
    return;
  }

  haveWantlist = true;
  cachedCards = std::move(fetched);
  currentCardIndex = -1;  // force a fresh pick even if the same id ends up chosen
  nextCardIndex = -1;
  nextArtReady = false;

  if (cachedCards.empty()) {
    ui::drawStatus(gfx, "Wantlist empty", "Add cards in the app");
    return;
  }

  // If the worker is still busy with the previous card, don't draw here --
  // the regular gated check in loop() will pick a (now-fresh) card once it
  // finishes.
  if (!artWorkerBusy) {
    advanceCard();
    lastCardSwapAt = millis();
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  randomSeed(esp_random());

  // This device never uses Bluetooth, but the ESP32 Arduino core still
  // reserves a controller memory pool for it at boot by default -- tens of
  // KB that would otherwise sit permanently unavailable. Releasing it here
  // (before WiFi starts) was the difference between the PNG decoder's ~44KB
  // allocation reliably failing (largest free block stuck a hair under
  // that, even on the very first attempt after a fresh boot) and it working.
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);

  artJobQueue = xQueueCreate(1, sizeof(ArtJob));
  xTaskCreatePinnedToCore(artWorkerTaskFn, "artWorker", 8192, nullptr, 1, nullptr, 1);

  ui::begin(gfx);
  connectWifi();

  ui::drawStatus(gfx, "Loading", "Fetching wantlist...");
  refreshWantlist();
  lastWantlistAttemptAt = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }

  unsigned long refreshInterval = haveWantlist ? WANTLIST_REFRESH_MS : WANTLIST_RETRY_MS;
  if (millis() - lastWantlistAttemptAt >= refreshInterval) {
    lastWantlistAttemptAt = millis();
    refreshWantlist();
  }

  if (artWorkerBusy || cachedCards.empty()) return;

  if (millis() - lastCardSwapAt >= CARD_SWAP_MS) {
    lastCardSwapAt = millis();
    advanceCard();
  } else {
    maybeStartPrefetch();
  }
}
