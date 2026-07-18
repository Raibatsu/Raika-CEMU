#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct GfxPreset {
  std::string category;
  std::string name;
  bool isDefault = false;
};

struct GfxPack {
  std::string name;
  std::string path;
  std::string description;
  std::string rulesRel;
  std::vector<uint64_t> titleIds;
  bool universal = false;
  std::vector<GfxPreset> presets;
};

enum { GFX_OK = 0, GFX_UPTODATE, GFX_NET_FAIL, GFX_EXTRACT_FAIL };

// Downloads replace the managed downloadedGraphicPacks directory.
int gfxpacks_downloadLatest(const std::string &graphicPacksDir);

std::vector<GfxPack> gfxpacks_enumerate(const std::string &graphicPacksDir);
