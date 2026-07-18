#include "cemu_titles.h"
#include "cemu_container_title.h"
#include "tinyxml2.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <vector>

static std::string trimStr(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  size_t b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static bool parseHex(const char *text, size_t digits, uint64_t &value) {
  if (!text)
    return false;
  std::string input = trimStr(text);
  if (input.size() != digits)
    return false;
  uint64_t parsed = 0;
  for (char c : input) {
    unsigned digit;
    if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') digit = (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') digit = (unsigned)(c - 'A' + 10);
    else return false;
    parsed = (parsed << 4) | digit;
  }
  value = parsed;
  return true;
}

static std::string readLongName(tinyxml2::XMLElement *menu) {
  std::string fallback;
  for (auto *e = menu->FirstChildElement(); e; e = e->NextSiblingElement()) {
    const char *n = e->Name();
    if (!n || strncmp(n, "longname_", 9) != 0)
      continue;
    const char *txt = e->GetText();
    if (!txt || !*txt)
      continue;
    std::string v = txt;
    std::replace_if(v.begin(), v.end(), [](char c) { return c == '\r' || c == '\n'; }, ' ');
    v = trimStr(v);
    if (v.empty())
      continue;
    if (strcmp(n, "longname_en") == 0)
      return v;
    if (fallback.empty())
      fallback = v;
  }
  return fallback;
}

static bool readMetadata(const std::string &path, uint64_t &titleIdOut, std::string &name) {
  tinyxml2::XMLDocument doc;
  if (doc.LoadFile(path.c_str()) != tinyxml2::XML_SUCCESS)
    return false;
  auto *menu = doc.FirstChildElement("menu");
  auto *titleId = menu ? menu->FirstChildElement("title_id") : nullptr;
  if (!titleId || !parseHex(titleId->GetText(), 16, titleIdOut))
    return false;
  name = readLongName(menu);
  return true;
}

static uint64_t baseTitleId(uint64_t titleId) {
  if ((titleId >> 48) != 0x0005)
    return 0;
  const uint8_t type = (uint8_t)((titleId >> 32) & 0xFF);
  if (type == 0x00 || type == 0x02)
    return titleId;
  if (type != 0x0C && type != 0x0E)
    return 0;
  return 0x0005000000000000ULL | (titleId & 0xFFFFFFFFULL);
}

static uint64_t treeSize(const std::string &path, unsigned depth = 0) {
  if (depth > 64)
    return 0;
  struct stat st{};
  if (lstat(path.c_str(), &st) != 0)
    return 0;
  if (!S_ISDIR(st.st_mode))
    return S_ISREG(st.st_mode) && st.st_size > 0 ? (uint64_t)st.st_size : 0;
  DIR *dir = opendir(path.c_str());
  if (!dir)
    return 0;
  uint64_t total = 0;
  while (dirent *entry = readdir(dir)) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    const uint64_t size = treeSize(path + "/" + entry->d_name, depth + 1);
    total = size > UINT64_MAX - total ? UINT64_MAX : total + size;
  }
  closedir(dir);
  return total;
}

static std::string dirname_of(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

static bool equalsIgnoreCase(std::string left, const char *right) {
  std::transform(left.begin(), left.end(), left.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return left == right;
}

static uint64_t readExtractedTitleId(const std::string &gamePath) {
  struct stat st{};
  std::string root;
  if (stat(gamePath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    root = gamePath;
  } else {
    size_t dot = gamePath.find_last_of('.');
    std::string extension = dot == std::string::npos ? std::string() : gamePath.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (extension != ".rpx" && extension != ".elf")
      return 0;
    std::string parent = dirname_of(gamePath);
    size_t slash = parent.find_last_of("/\\");
    std::string leaf = slash == std::string::npos ? parent : parent.substr(slash + 1);
    root = equalsIgnoreCase(leaf, "code") ? dirname_of(parent) : parent;
  }
  if (root.empty())
    return 0;

  uint64_t titleId = 0;
  std::string name;
  return readMetadata(root + "/meta/meta.xml", titleId, name) ? baseTitleId(titleId) : 0;
}

std::string cemu_normalizeTitlePath(const std::string &path) {
  std::string input = trimStr(path);
  std::replace(input.begin(), input.end(), '\\', '/');
  if (input.size() >= 5) {
    std::string prefix = input.substr(0, 5);
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (prefix == "sdmc:") input.erase(0, 5);
  }

  std::vector<std::string> parts;
  size_t pos = 0;
  while (pos < input.size()) {
    while (pos < input.size() && input[pos] == '/') pos++;
    size_t end = input.find('/', pos);
    if (end == std::string::npos) end = input.size();
    std::string part = input.substr(pos, end - pos);
    if (part == "..") {
      if (!parts.empty()) parts.pop_back();
    } else if (!part.empty() && part != ".") {
      std::transform(part.begin(), part.end(), part.begin(), [](unsigned char c) { return (char)std::tolower(c); });
      parts.push_back(std::move(part));
    }
    pos = end + 1;
  }

  std::string normalized = "/";
  for (size_t i = 0; i < parts.size(); i++) {
    if (i) normalized += '/';
    normalized += parts[i];
  }
  return normalized;
}

std::vector<CemuTitle> cemu_scanMlcTitles(const std::string &mlcRoot) {
  std::vector<CemuTitle> out;
  std::string baseDir = mlcRoot + "/usr/title/00050000";
  DIR *d = opendir(baseDir.c_str());
  if (!d)
    return out;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;
    std::string low = e->d_name;
    uint64_t lowTitleId = 0;
    if (!parseHex(low.c_str(), 8, lowTitleId))
      continue;
    std::string dir = baseDir + "/" + low;
    std::string meta = dir + "/meta/meta.xml";
    struct stat st{};
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) ||
        stat(meta.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
      continue;
    CemuTitle t;
    t.titleId = (0x00050000ULL << 32) | lowTitleId;
    uint64_t metadataTitleId = 0;
    if (!readMetadata(meta, metadataTitleId, t.name) || metadataTitleId != t.titleId)
      continue;
    std::string icon = dir + "/meta/iconTex.tga";
    if (stat(icon.c_str(), &st) == 0 && S_ISREG(st.st_mode))
      t.iconPath = icon;
    out.push_back(std::move(t));
  }
  closedir(d);
  return out;
}

std::vector<CemuInstalledComponent> cemu_scanInstalledComponents(
    const std::string &mlcRoot, bool measureSizes) {
  struct TypeInfo {
    uint32_t high;
    CemuInstalledKind kind;
  };
  static const TypeInfo types[] = {
    {0x00050000, CemuInstalledKind::Base},
    {0x0005000E, CemuInstalledKind::Update},
    {0x0005000C, CemuInstalledKind::Dlc},
  };

  std::vector<CemuInstalledComponent> out;
  for (const auto &type : types) {
    char highName[9];
    snprintf(highName, sizeof(highName), "%08x", type.high);
    const std::string typeDir = mlcRoot + "/usr/title/" + highName;
    DIR *dir = opendir(typeDir.c_str());
    if (!dir)
      continue;
    while (dirent *entry = readdir(dir)) {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
        continue;
      uint64_t low = 0;
      if (!parseHex(entry->d_name, 8, low))
        continue;
      const std::string path = typeDir + "/" + entry->d_name;
      struct stat st{};
      if (lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
        continue;

      CemuInstalledComponent component;
      component.titleId = (uint64_t(type.high) << 32) | low;
      component.baseTitleId = 0x0005000000000000ULL | low;
      component.kind = type.kind;
      component.path = path;
      if (measureSizes)
        component.sizeBytes = treeSize(path);
      uint64_t metadataTitleId = 0;
      component.metadataValid = readMetadata(path + "/meta/meta.xml",
                                             metadataTitleId, component.name) &&
                                metadataTitleId == component.titleId;
      out.push_back(std::move(component));
    }
    closedir(dir);
  }

  for (auto &component : out) {
    if (!component.name.empty())
      continue;
    auto base = std::find_if(out.begin(), out.end(), [&](const auto &candidate) {
      return candidate.kind == CemuInstalledKind::Base &&
             candidate.baseTitleId == component.baseTitleId && !candidate.name.empty();
    });
    if (base != out.end())
      component.name = base->name;
  }
  std::sort(out.begin(), out.end(), [](const auto &left, const auto &right) {
    if (left.baseTitleId != right.baseTitleId)
      return left.baseTitleId < right.baseTitleId;
    return (int)left.kind < (int)right.kind;
  });
  return out;
}

static std::string basename_of(const std::string &p) {
  size_t s = p.find_last_of('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

std::vector<TitleCacheEntry> cemu_loadTitleCache(const std::string &cacheXmlPath) {
  std::vector<TitleCacheEntry> out;
  tinyxml2::XMLDocument doc;
  if (doc.LoadFile(cacheXmlPath.c_str()) != tinyxml2::XML_SUCCESS)
    return out;
  auto *root = doc.FirstChildElement("title_list");
  if (!root)
    return out;
  for (auto *t = root->FirstChildElement("title"); t; t = t->NextSiblingElement("title")) {
    const char *idAttr = t->Attribute("titleId");
    auto *pathEl = t->FirstChildElement("path");
    if (!idAttr || !pathEl || !pathEl->GetText())
      continue;
    uint64_t titleId = 0;
    std::string path = cemu_normalizeTitlePath(pathEl->GetText());
    if (!parseHex(idAttr, 16, titleId) || path == "/" || (titleId >> 48) != 0x0005)
      continue;
    out.push_back({std::move(path), titleId});
  }
  return out;
}

uint64_t cemu_resolveBaseTitleId(const std::string &gamePath,
                                 const std::vector<TitleCacheEntry> &cache,
                                 std::string *error) {
  if (error) error->clear();
  if (uint64_t titleId = readExtractedTitleId(gamePath))
    return titleId;

  std::string containerError;
  if (uint64_t titleId = cemu_readContainerBaseTitleId(
          gamePath, "sdmc:/switch/Cemu/keys.txt", &containerError))
    return titleId;

  std::string normalized = cemu_normalizeTitlePath(gamePath);
  std::string want = basename_of(normalized);
  uint64_t match = 0;
  for (const auto &e : cache) {
    if (e.path != normalized)
      continue;
    uint64_t candidate = baseTitleId(e.titleId);
    if (!candidate)
      continue;
    if (match && match != candidate)
      return 0;
    match = candidate;
  }
  if (!match) {
    std::string matchedPath;
    for (const auto &e : cache) {
      if (basename_of(e.path) != want)
        continue;
      uint64_t candidate = baseTitleId(e.titleId);
      if (!candidate)
        continue;
      if (!matchedPath.empty() && matchedPath != e.path)
        return 0;
      if (match && match != candidate)
        return 0;
      matchedPath = e.path;
      match = candidate;
    }
  }
  if (match) {
    if (error) error->clear();
    return match;
  }
  if (error) *error = std::move(containerError);
  return 0;
}
