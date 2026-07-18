#pragma once
#include <string>
#include <vector>

enum {
  GRIDDB_OK = 0,
  GRIDDB_NO_KEY,     // missing or rejected API key
  GRIDDB_NO_NET,     // network/DNS/connect failure
  GRIDDB_NOT_FOUND,  // no matching game or no 600x900 grid
  GRIDDB_ERROR,      // other (write failed, bad response)
};

struct GridDbGameResult {
  long id = 0;
  std::string name;
};

struct GridDbArtwork {
  std::string url;
  std::string thumbnailUrl;
  int width = 0;
  int height = 0;
};

int griddb_search_games(const std::string &apiKey, const std::string &title,
                        std::vector<GridDbGameResult> &results);
int griddb_fetch_artworks(const std::string &apiKey, long gameId,
                          std::vector<GridDbArtwork> &artworks);
int griddb_download_image(const std::string &url, const std::string &outPath);

int griddb_fetch_cover(const std::string &apiKey, const std::string &title,
                       const std::string &outPath);

int griddb_fetch_icons(const std::string &key, const std::string &title, const std::string &outDir, int maxCount);

bool griddb_global_init(void);
void griddb_global_exit(void);
