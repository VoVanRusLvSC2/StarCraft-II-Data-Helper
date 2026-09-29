#pragma once
#include <pugixml.hpp>

namespace sc2dh {
// Preserve semantic processing instructions and comments in every data mutation.
// pugixml does not resolve external entities or fetch DTDs.
inline constexpr unsigned int xmlParseFlags = pugi::parse_default | pugi::parse_pi | pugi::parse_comments;
}
