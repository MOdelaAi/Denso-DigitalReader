// The combined POST body for /api/brazing/update: {"zone<n>": value, ...}, keys
// in ascending zone order (std::map). Pure std::string so it unit-tests without
// Qt; the BrazingClient wraps it in the HTTP request.
#pragma once

#include "brazing/zone_reading.h"   // ZoneValue

#include <map>
#include <string>

namespace denso::ui {

std::string build_brazing_payload(const std::map<int, ZoneValue>& zones);

} // namespace denso::ui
