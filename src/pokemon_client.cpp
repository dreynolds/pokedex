#include "pokemon_client.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <map>

namespace {

// Keeps URLs (and the JSON responses behind them) a sane size. Datasette's
// id__in filter takes a plain comma-separated list in the query string.
constexpr size_t kChunkSize = 50;

std::vector<String> chunksOf(const std::vector<String> &ids) {
  std::vector<String> chunks;
  for (size_t i = 0; i < ids.size(); i += kChunkSize) {
    String joined;
    size_t end = min(i + kChunkSize, ids.size());
    for (size_t j = i; j < end; j++) {
      if (j > i) joined += ",";
      joined += ids[j];
    }
    chunks.push_back(joined);
  }
  return chunks;
}

// GETs a URL and parses the body as JSON, applying `filter` (see ArduinoJson
// filtering docs) to keep memory use bounded regardless of how many columns
// the server sends back. An https:// backend is treated the same way the
// Jellyfin screen treats its own server: certificate validation is skipped
// rather than embedding a root CA bundle on the device, a reasonable
// trade-off for a personal box only this device talks to. Picking the
// client by scheme matters, not just cosmetic: handing a WiFiClientSecure
// to a plain http:// server made it attempt a TLS handshake against a
// plaintext response, failing immediately with an "invalid SSL record"
// error.
bool httpGetJson(const String &url, JsonDocument &filter, JsonDocument &doc) {
  WiFiClientSecure secureClient;
  WiFiClient plainClient;

  HTTPClient http;
  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    http.begin(secureClient, url);
  } else {
    http.begin(plainClient, url);
  }
  http.setTimeout(10000);
  http.setConnectTimeout(10000);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[pokemon] GET %s failed, code=%d\n", url.c_str(), code);
    http.end();
    return false;
  }

  // getStream() reads the raw connection, which this backend's responses
  // don't play nicely with (no Content-Length -- getSize() reports -1,
  // likely chunked or connection-close-delimited through a reverse proxy):
  // deserializeJson against the raw stream silently produced an empty
  // result instead of erroring. getString() buffers the body first and
  // handles chunked/unknown-length responses correctly, at the cost of
  // holding the whole (modest, a few KB) response in RAM.
  String body = http.getString();
  http.end();

  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));

  if (err) {
    Serial.printf("[pokemon] JSON parse failed for %s: %s\n", url.c_str(), err.c_str());
    return false;
  }
  return true;
}

}  // namespace

PokemonClient::PokemonClient(const String &baseUrl) : _baseUrl(baseUrl) {
  while (_baseUrl.endsWith("/")) {
    _baseUrl.remove(_baseUrl.length() - 1);
  }
}

bool PokemonClient::fetchWantlist(std::vector<WantedCard> &out) {
  // 1. Card ids on the wantlist.
  JsonDocument wantedFilter;
  JsonObject wantedItemFilter = wantedFilter.add<JsonObject>();
  wantedItemFilter["card_id"] = true;

  JsonDocument wantedDoc;
  if (!httpGetJson(_baseUrl + "/api/collection/wanted", wantedFilter, wantedDoc)) {
    return false;
  }

  std::vector<String> ids;
  for (JsonObject row : wantedDoc.as<JsonArray>()) {
    const char *id = row["card_id"] | "";
    if (id[0] != '\0') ids.push_back(String(id));
  }

  if (ids.empty()) {
    out.clear();
    return true;
  }

  // 2. Card details (name/number/set_id/image) for those ids, chunked.
  JsonDocument cardFilter;
  JsonObject cardItemFilter = cardFilter.add<JsonObject>();
  cardItemFilter["id"] = true;
  cardItemFilter["name"] = true;
  cardItemFilter["number"] = true;
  cardItemFilter["set_id"] = true;
  cardItemFilter["image_small"] = true;

  std::vector<WantedCard> cards;
  std::map<String, String> setIdByCardId;
  for (const String &chunk : chunksOf(ids)) {
    JsonDocument cardsDoc;
    String url = _baseUrl + "/pokemon_tcg/cards.json?id__in=" + chunk + "&_shape=array&_size=1000";
    if (!httpGetJson(url, cardFilter, cardsDoc)) return false;

    for (JsonObject row : cardsDoc.as<JsonArray>()) {
      WantedCard card;
      card.id = row["id"] | "";
      card.name = row["name"] | "";
      card.number = row["number"] | "";
      String imageSmall = row["image_small"] | "";
      // The backend now proxies card art itself (a "/images/..." path) so
      // it can be served over plain HTTP alongside the rest of the API,
      // rather than the ESP32 negotiating its own HTTPS connection to the
      // external image CDN -- that connection was the source of most of
      // the SSL flakiness this device used to hit fetching art. A leading
      // "/" means it's one of those relative paths; anything else (still
      // seen for cards the backend hasn't mirrored yet) is already a full
      // URL and is used as-is.
      card.imageUrl = imageSmall.startsWith("/") ? _baseUrl + imageSmall : imageSmall;
      setIdByCardId[card.id] = row["set_id"] | "";
      cards.push_back(card);
    }
  }

  // 3. Set names for the set ids actually in use, chunked.
  std::vector<String> uniqueSetIds;
  for (auto &pair : setIdByCardId) {
    if (pair.second.length() == 0) continue;
    bool alreadyPresent = false;
    for (const String &s : uniqueSetIds) {
      if (s == pair.second) { alreadyPresent = true; break; }
    }
    if (!alreadyPresent) uniqueSetIds.push_back(pair.second);
  }

  JsonDocument setFilter;
  JsonObject setItemFilter = setFilter.add<JsonObject>();
  setItemFilter["id"] = true;
  setItemFilter["name"] = true;

  std::map<String, String> setNameById;
  for (const String &chunk : chunksOf(uniqueSetIds)) {
    JsonDocument setsDoc;
    String url = _baseUrl + "/pokemon_tcg/sets.json?id__in=" + chunk + "&_shape=array&_size=1000";
    if (!httpGetJson(url, setFilter, setsDoc)) return false;

    for (JsonObject row : setsDoc.as<JsonArray>()) {
      String setId = row["id"] | "";
      setNameById[setId] = row["name"] | "";
    }
  }

  for (WantedCard &card : cards) {
    String setId = setIdByCardId[card.id];
    auto it = setNameById.find(setId);
    card.setName = (it != setNameById.end()) ? it->second : setId;
  }

  out = std::move(cards);
  return true;
}
