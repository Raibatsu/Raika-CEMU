#pragma once

#include <cstdint>
#include <string>

uint64_t cemu_readContainerBaseTitleId(const std::string &path,
                                       const std::string &keysPath,
                                       std::string *error);
