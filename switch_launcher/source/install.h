#pragma once
#include <string>

// Title metadata determines the destination; progress may be null.
int cemu_installTitle(const std::string &srcFolder, const std::string &mlcRoot,
                      void (*progress)(int), std::string &errMsg);

bool cemu_removeInstalledComponent(const std::string &mlcRoot, uint64_t titleId,
                                   std::string &errMsg);
