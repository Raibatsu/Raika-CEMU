#pragma once

#include <cstdint>
#include <string>

bool cemu_hasConfiguredDiscKey(const std::string &keysPath);

uint64_t cemu_readContainerBaseTitleId(const std::string &path,
                                       const std::string &keysPath,
                                       std::string *error);
