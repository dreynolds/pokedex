#pragma once
#include <Arduino.h>
#include <vector>

struct WantedCard {
  String id;
  String name;
  String number;
  String setName;
  // Card art at the backend's "small" image size (~245x342), the only
  // resolution used here -- the panel is 240x320, so anything larger would
  // just mean a bigger download and a slower on-device PNG decode for no
  // visible benefit.
  String imageUrl;
};

class PokemonClient {
public:
  explicit PokemonClient(const String &baseUrl);

  // Rebuilds the full wantlist: GET /api/collection/wanted for the card id
  // list, then /pokemon_tcg/cards.json and /pokemon_tcg/sets.json (both
  // id__in-filtered and chunked to keep URLs short) to fill in the name/
  // number/set/image fields needed for display. On any request failure,
  // leaves `out` untouched and returns false so the caller can keep
  // showing its last-known-good list rather than blanking the screen.
  bool fetchWantlist(std::vector<WantedCard> &out);

private:
  String _baseUrl;
};
