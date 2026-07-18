#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SwitchStorage
{
struct SmbShare
{
	std::string id;
	std::string name;
	std::string server;
	std::string share;
	std::string path;
	std::string user;
	std::string password;
	std::string domain;
	bool autoMount = true;
};

struct Location
{
	std::string path;
	std::string label;
};

bool InitializeUsb(std::string* error = nullptr);
uint64_t UsbStatusGeneration();
bool MountSmb(const SmbShare& share, std::string* error = nullptr);
bool UnmountSmb(const std::string& id);
bool IsSmbMounted(const std::string& id);

std::string SmbRootPath(const std::string& id);
std::string SmbBrowsePath(const SmbShare& share);
std::vector<Location> ListUsbLocations();
std::vector<SmbShare> LoadSmbShares(const std::string& iniPath);
void InitializeFromConfig(const std::string& iniPath, bool initializeUsb = true,
                          std::vector<std::string>* errors = nullptr);
void Shutdown();
} // namespace SwitchStorage
