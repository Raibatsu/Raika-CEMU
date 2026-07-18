#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct CemuTitle {
  uint64_t titleId = 0;
  std::string name;
  std::string iconPath;
};

std::vector<CemuTitle> cemu_scanMlcTitles(const std::string &mlcRoot);

enum class CemuInstalledKind {
  Base,
  Update,
  Dlc,
};

struct CemuInstalledComponent {
  uint64_t titleId = 0;
  uint64_t baseTitleId = 0;
  CemuInstalledKind kind = CemuInstalledKind::Base;
  std::string name;
  std::string path;
  uint64_t sizeBytes = 0;
  bool metadataValid = false;
};

std::vector<CemuInstalledComponent> cemu_scanInstalledComponents(
    const std::string &mlcRoot, bool measureSizes = false);

struct TitleCacheEntry { std::string path; uint64_t titleId; };
std::vector<TitleCacheEntry> cemu_loadTitleCache(const std::string &cacheXmlPath);
std::string cemu_normalizeTitlePath(const std::string &path);
uint64_t cemu_resolveBaseTitleId(const std::string &gamePath,
                                 const std::vector<TitleCacheEntry> &cache,
                                 std::string *error = nullptr);
