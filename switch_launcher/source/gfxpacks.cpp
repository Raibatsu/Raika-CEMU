#include "gfxpacks.h"

#include <curl/curl.h>
#include <zlib.h>

#include <dirent.h>
#include <sys/stat.h>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <limits>
#include <string_view>

static constexpr size_t GFX_MAX_BODY = 128u * 1024 * 1024;
static constexpr size_t GFX_MAX_ENTRY = 128u * 1024 * 1024;
static constexpr size_t GFX_MAX_EXTRACTED = 1024u * 1024 * 1024;
static constexpr size_t GFX_MAX_ENTRIES = 20000;
struct DlBuf { std::string data; };

static size_t gfx_write_cb(void *p, size_t sz, size_t n, void *u) {
  DlBuf *b = (DlBuf *)u;
  if (n != 0 && sz > std::numeric_limits<size_t>::max() / n) return 0;
  size_t add = sz * n;
  if (add > GFX_MAX_BODY - b->data.size()) return 0;
  b->data.append((char *)p, add);
  return add;
}
// CURL success does not imply HTTP success.
static bool gfx_http_get(const std::string &url, std::string &out, long *code) {
  if (code) *code = 0;
  CURL *c = curl_easy_init();
  if (!c) return false;
  DlBuf b;
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, gfx_write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
  curl_easy_setopt(c, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
  curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)GFX_MAX_BODY);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "Cemu-Launcher/" CEMU_SWITCH_VERSION);
  CURLcode rc = curl_easy_perform(c);
  long hc = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &hc);
  if (code) *code = hc;
  curl_easy_cleanup(c);
  out.swap(b.data);
  return rc == CURLE_OK;
}

static std::string json_str(const std::string &j, const char *key) {
  size_t p = j.find(key);
  if (p == std::string::npos) return "";
  p += strlen(key);
  std::string s;
  while (p < j.size() && j[p] != '"') {
    if (j[p] == '\\' && p + 1 < j.size()) { p++; s += (j[p] == '/' ? '/' : j[p]); }
    else s += j[p];
    p++;
  }
  return s;
}

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool makeDir(const std::string &path) {
  if (mkdir(path.c_str(), 0777) == 0) return true;
  if (errno != EEXIST) return false;
  struct stat st{};
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool mkdirs(const std::string &path, bool includeLeaf) {
  size_t end = includeLeaf ? path.size() : path.find_last_of('/');
  if (end == std::string::npos) return true;
  for (size_t i = 1; i <= end; i++) {
    if (i != end && path[i] != '/') continue;
    std::string sub = path.substr(0, i);
    if (!sub.empty() && !makeDir(sub)) return false;
  }
  return true;
}

static bool inflateToFile(const uint8_t *src, uint32_t csize, uint32_t usize, uint32_t expectedCrc,
                          uint16_t method, const std::string &outPath) {
  if (!mkdirs(outPath, false)) return false;
  FILE *f = fopen(outPath.c_str(), "wb");
  if (!f) return false;
  bool ok = true;
  size_t written = 0;
  uLong crc = crc32(0L, Z_NULL, 0);
  if (method == 0) {
    ok = csize == usize && fwrite(src, 1, csize, f) == csize;
    if (ok) {
      written = csize;
      crc = crc32(crc, src, csize);
    }
  } else if (method == 8) {
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
      fclose(f);
      remove(outPath.c_str());
      return false;
    }
    zs.next_in = (Bytef *)src;
    zs.avail_in = csize;
    static thread_local uint8_t buf[1 << 16];
    int ret = Z_OK;
    do {
      zs.next_out = buf;
      zs.avail_out = sizeof(buf);
      ret = inflate(&zs, Z_NO_FLUSH);
      if (ret != Z_OK && ret != Z_STREAM_END) { ok = false; break; }
      size_t have = sizeof(buf) - zs.avail_out;
      if (have > usize - written || (have && fwrite(buf, 1, have, f) != have)) {
        ok = false;
        break;
      }
      if (have) {
        written += have;
        crc = crc32(crc, buf, have);
      }
      if (ret == Z_OK && zs.avail_in == 0 && have == 0) {
        ok = false;
        break;
      }
    } while (ret != Z_STREAM_END);
    ok = ok && ret == Z_STREAM_END && zs.avail_in == 0;
    inflateEnd(&zs);
  } else {
    ok = false;
  }
  int closeResult = fclose(f);
  ok = ok && written == usize && crc == expectedCrc && closeResult == 0;
  if (!ok) remove(outPath.c_str());
  return ok;
}

static bool isSafeZipPath(const std::string &name) {
  if (name.empty() || name.size() > 4096 || name.front() == '/' || name.front() == '\\' ||
      name.find('\\') != std::string::npos || name.find(':') != std::string::npos ||
      name.find('\0') != std::string::npos)
    return false;

  size_t end = name.back() == '/' ? name.size() - 1 : name.size();
  if (end == 0) return false;
  size_t begin = 0;
  while (begin < end) {
    size_t slash = name.find('/', begin);
    if (slash == std::string::npos || slash > end) slash = end;
    std::string_view part(name.data() + begin, slash - begin);
    if (part.empty() || part == "." || part == "..") return false;
    begin = slash + 1;
  }
  return true;
}

static bool unzipMem(const uint8_t *zip, size_t zsize, const std::string &destDir) {
  if (zsize < 22) return false;
  size_t eocd = std::string::npos;
  for (size_t i = zsize - 22 + 1; i-- > 0;) {
    if (zip[i] == 'P' && zip[i + 1] == 'K' && zip[i + 2] == 5 && zip[i + 3] == 6) { eocd = i; break; }
    if (zsize - i > 22 + 65535) break;
  }
  if (eocd == std::string::npos) return false;
  if (rd16(zip + eocd + 4) != 0 || rd16(zip + eocd + 6) != 0) return false;
  uint16_t diskEntries = rd16(zip + eocd + 8);
  uint16_t total = rd16(zip + eocd + 10);
  uint32_t cdSize = rd32(zip + eocd + 12);
  uint32_t cdOff = rd32(zip + eocd + 16);
  uint16_t commentLen = rd16(zip + eocd + 20);
  if (diskEntries != total || total == 0 || total > GFX_MAX_ENTRIES ||
      commentLen != zsize - (eocd + 22) || cdOff > eocd || cdSize > eocd - cdOff)
    return false;

  size_t p = cdOff;
  const size_t cdEnd = (size_t)cdOff + cdSize;
  size_t extracted = 0;
  size_t fileCount = 0;
  for (size_t n = 0; n < total; n++) {
    if (p > cdEnd || 46 > cdEnd - p || rd32(zip + p) != 0x02014b50) return false;
    uint16_t flags = rd16(zip + p + 8);
    uint16_t method = rd16(zip + p + 10);
    uint32_t expectedCrc = rd32(zip + p + 16);
    uint32_t csize = rd32(zip + p + 20);
    uint32_t usize = rd32(zip + p + 24);
    uint16_t nlen = rd16(zip + p + 28);
    uint16_t elen = rd16(zip + p + 30);
    uint16_t clen = rd16(zip + p + 32);
    uint32_t lho = rd32(zip + p + 42);
    size_t recordSize = (size_t)46 + nlen + elen + clen;
    if (recordSize > cdEnd - p) return false;
    std::string name((const char *)(zip + p + 46), nlen);
    p += recordSize;
    if (!isSafeZipPath(name) || (flags & 1) != 0 || (method != 0 && method != 8)) return false;
    std::string outPath = destDir + "/" + name;
    if (name.back() == '/') {
      if (!mkdirs(outPath, true)) return false;
      continue;
    }
    if (usize > GFX_MAX_ENTRY || usize > GFX_MAX_EXTRACTED - extracted) return false;
    if (lho > zsize || 30 > zsize - lho || rd32(zip + lho) != 0x04034b50) return false;
    if ((rd16(zip + lho + 6) & 1) != 0 || rd16(zip + lho + 8) != method) return false;
    uint16_t lnlen = rd16(zip + lho + 26);
    uint16_t lelen = rd16(zip + lho + 28);
    size_t headerSize = (size_t)30 + lnlen + lelen;
    if (headerSize > zsize - lho) return false;
    size_t dataOff = (size_t)lho + headerSize;
    if (csize > zsize - dataOff ||
        !inflateToFile(zip + dataOff, csize, usize, expectedCrc, method, outPath))
      return false;
    extracted += usize;
    fileCount++;
  }
  return p == cdEnd && fileCount != 0;
}

static void rmrf(const std::string &path) {
  DIR *d = opendir(path.c_str());
  if (d) {
    struct dirent *e;
    while ((e = readdir(d))) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      std::string c = path + "/" + e->d_name;
      struct stat st;
      if (stat(c.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) rmrf(c);
      else remove(c.c_str());
    }
    closedir(d);
  }
  rmdir(path.c_str());
}

int gfxpacks_downloadLatest(const std::string &graphicPacksDir) {
  std::string resp;
  long code = 0;
  if (!gfx_http_get("https://api.github.com/repos/cemu-project/cemu_graphic_packs/releases/latest", resp, &code) || code < 200 || code >= 300)
    return GFX_NET_FAIL;
  std::string version = json_str(resp, "\"tag_name\":\"");
  std::string url = json_str(resp, "\"browser_download_url\":\"");
  if (url.empty()) return GFX_NET_FAIL;

  std::string dlDir = graphicPacksDir + "/downloadedGraphicPacks";
  std::string tmpDir = dlDir + ".tmp";
  std::string oldDir = dlDir + ".old";
  std::string verFile = dlDir + "/version.txt";
  struct stat st{};
  if (stat(dlDir.c_str(), &st) != 0 && stat(oldDir.c_str(), &st) == 0)
    rename(oldDir.c_str(), dlDir.c_str());
  if (!version.empty()) {
    FILE *vf = fopen(verFile.c_str(), "r");
    if (vf) { char cur[128] = {0}; if (fgets(cur, sizeof(cur), vf)) { std::string c = cur; while (!c.empty() && (c.back() == '\n' || c.back() == '\r')) c.pop_back(); if (c == version) { fclose(vf); return GFX_UPTODATE; } } fclose(vf); }
  }

  std::string zip;
  if (!gfx_http_get(url, zip, &code) || code < 200 || code >= 300 || zip.size() < 64)
    return GFX_NET_FAIL;

  rmrf(tmpDir);
  if (!mkdirs(graphicPacksDir, true) || !makeDir(tmpDir) ||
      !unzipMem((const uint8_t *)zip.data(), zip.size(), tmpDir)) {
    rmrf(tmpDir);
    return GFX_EXTRACT_FAIL;
  }

  if (!version.empty()) {
    FILE *vf = fopen((tmpDir + "/version.txt").c_str(), "w");
    bool wroteVersion = vf && fprintf(vf, "%s\n", version.c_str()) >= 0;
    if (vf && fclose(vf) != 0) wroteVersion = false;
    if (!wroteVersion) {
      rmrf(tmpDir);
      return GFX_EXTRACT_FAIL;
    }
  }

  rmrf(oldDir);
  bool hadOld = stat(dlDir.c_str(), &st) == 0;
  if (hadOld && rename(dlDir.c_str(), oldDir.c_str()) != 0) {
    rmrf(tmpDir);
    return GFX_EXTRACT_FAIL;
  }
  if (rename(tmpDir.c_str(), dlDir.c_str()) != 0) {
    if (hadOld) rename(oldDir.c_str(), dlDir.c_str());
    rmrf(tmpDir);
    return GFX_EXTRACT_FAIL;
  }
  rmrf(oldDir);
  return GFX_OK;
}

static void parseRules(const std::string &content, GfxPack &p) {
  auto lc = [](std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; };
  auto trimq = [](std::string s) {
    size_t x = s.find_first_not_of(" \t\r"), y = s.find_last_not_of(" \t\r");
    if (x == std::string::npos) return std::string();
    s = s.substr(x, y - x + 1);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
  };
  std::string section;
  GfxPreset cur; bool inPreset = false;
  auto flush = [&] { if (inPreset && !cur.name.empty()) p.presets.push_back(cur); cur = GfxPreset{}; inPreset = false; };

  size_t pos = 0;
  while (pos < content.size()) {
    size_t eol = content.find('\n', pos);
    std::string line = content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    pos = eol == std::string::npos ? content.size() : eol + 1;
    size_t a = line.find_first_not_of(" \t\r"); if (a == std::string::npos) continue;
    size_t b = line.find_last_not_of(" \t\r"); line = line.substr(a, b - a + 1);
    if (line.empty() || line[0] == ';' || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      flush(); section = lc(line.substr(1, line.size() - 2));
      if (section == "preset") inPreset = true;
      continue;
    }
    size_t eq = line.find('='); if (eq == std::string::npos) continue;
    std::string key = lc(trimq(line.substr(0, eq)));
    std::string val = trimq(line.substr(eq + 1));
    if (section == "definition") {
      if (key == "name") p.name = val;
      else if (key == "path") p.path = val;
      else if (key == "description") { p.description = val; for (auto &c : p.description) if (c == '|') c = '\n'; }
      else if (key == "titleids") {
        if (val.empty() || val.find('*') != std::string::npos) p.universal = true;
        else {
          size_t s = 0;
          while (s < val.size()) {
            size_t comma = val.find(',', s);
            std::string one = val.substr(s, comma == std::string::npos ? std::string::npos : comma - s);
            size_t x = one.find_first_not_of(" \t"), y = one.find_last_not_of(" \t");
            if (x != std::string::npos) { uint64_t id = strtoull(one.substr(x, y - x + 1).c_str(), nullptr, 16); if (id) p.titleIds.push_back(id); }
            if (comma == std::string::npos) break;
            s = comma + 1;
          }
        }
      }
    } else if (section == "preset") {
      if (key == "name") cur.name = val;
      else if (key == "category") cur.category = val;
      else if (key == "default") cur.isDefault = (val == "1" || val == "true" || val == "TRUE");
    }
  }
  flush();
}

static void scanDir(const std::string &dir, std::vector<GfxPack> &out) {
  std::string rules = dir + "/rules.txt";
  struct stat st;
  if (stat(rules.c_str(), &st) == 0) {
    FILE *f = fopen(rules.c_str(), "rb");
    if (f) {
      std::string content;
      char buf[4096]; size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
      fclose(f);
      GfxPack p;
      parseRules(content, p);
      if (p.name.empty()) p.name = dir.substr(dir.find_last_of('/') + 1);
      if (p.path.empty()) p.path = p.name; // Keep ungrouped packs visible.
      p.rulesRel = rules;
      out.push_back(std::move(p));
      return;
    }
  }
  DIR *d = opendir(dir.c_str());
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    std::string c = dir + "/" + e->d_name;
    if (stat(c.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) scanDir(c, out);
  }
  closedir(d);
}

std::vector<GfxPack> gfxpacks_enumerate(const std::string &graphicPacksDir) {
  std::vector<GfxPack> out;
  scanDir(graphicPacksDir, out);
  std::sort(out.begin(), out.end(), [](const GfxPack &a, const GfxPack &b) { return a.path < b.path; });
  return out;
}
