#include "platform/switch/SwitchToys.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>
#include <tuple>
#include <vector>

#include "Cafe/OS/libs/nsyshid/Dimensions.h"
#include "Cafe/OS/libs/nsyshid/Infinity.h"
#include "Cafe/OS/libs/nsyshid/Skylander.h"
#include "Common/FileStream.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"

namespace
{
struct FileEntry
{
	fs::path path;
	std::string name;
};

struct SlotState
{
	int coreSlot = -1;
	std::string fileName;
	std::string value;
};

std::mutex s_mutex;
fs::path s_root;
std::array<std::vector<FileEntry>, 3> s_files;
std::array<SlotState, nsyshid::MAX_SKYLANDERS> s_skylanders;
std::array<SlotState, nsyshid::MAX_FIGURES> s_infinity;
std::array<SlotState, 7> s_dimensions;
std::string s_status = "Ready";

constexpr std::array<uint8, 7> kDimensionPads{2, 1, 3, 2, 2, 3, 3};
constexpr std::array<const char*, nsyshid::MAX_FIGURES> kInfinitySlots{
	"Play Set / Power Disc", "Power Disc 2", "Power Disc 3", "Player 1",
	"Player 1 Ability 1", "Player 1 Ability 2", "Player 2", "Player 2 Ability 1",
	"Player 2 Ability 2"};
constexpr std::array<const char*, 7> kDimensionSlots{
	"Left 1", "Center", "Right 1", "Left 2", "Left 3", "Right 2", "Right 3"};

size_t TypeIndex(SwitchToyType type)
{
	return static_cast<size_t>(type);
}

const char* TypeFolder(SwitchToyType type)
{
	switch (type)
	{
	case SwitchToyType::Skylanders:
		return "skylanders";
	case SwitchToyType::Infinity:
		return "infinity";
	case SwitchToyType::Dimensions:
		return "dimensions";
	}
	return "unknown";
}

uintmax_t ExpectedSize(SwitchToyType type)
{
	switch (type)
	{
	case SwitchToyType::Skylanders:
		return nsyshid::SKY_FIGURE_SIZE;
	case SwitchToyType::Infinity:
		return nsyshid::INF_FIGURE_SIZE;
	case SwitchToyType::Dimensions:
		return 0x2D * 0x04;
	}
	return 0;
}

bool HasSupportedExtension(SwitchToyType type, const fs::path& path)
{
	std::string extension = _pathToUtf8(path.extension());
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	if (type == SwitchToyType::Skylanders)
		return extension == ".sky" || extension == ".bin" || extension == ".dump" || extension == ".dmp";
	return extension == ".bin";
}

std::vector<FileEntry>& Files(SwitchToyType type)
{
	return s_files[TypeIndex(type)];
}

SlotState* Slot(SwitchToyType type, int slot)
{
	if (slot < 0 || slot >= SwitchToys_GetSlotCount(type))
		return nullptr;
	switch (type)
	{
	case SwitchToyType::Skylanders:
		return &s_skylanders[slot];
	case SwitchToyType::Infinity:
		return &s_infinity[slot];
	case SwitchToyType::Dimensions:
		return &s_dimensions[slot];
	}
	return nullptr;
}

void RefreshFilesLocked(SwitchToyType type)
{
	auto& files = Files(type);
	files.clear();
	const fs::path directory = s_root / TypeFolder(type);
	std::error_code error;
	fs::create_directories(directory, error);
	if (error)
	{
		s_status = "Could not create " + _pathToUtf8(directory);
		return;
	}
	for (fs::directory_iterator iterator(directory, error), end; !error && iterator != end; iterator.increment(error))
	{
		std::error_code entryError;
		if (!iterator->is_regular_file(entryError) || entryError || !HasSupportedExtension(type, iterator->path()))
			continue;
		if (iterator->file_size(entryError) != ExpectedSize(type) || entryError)
			continue;
		files.push_back({iterator->path(), _pathToUtf8(iterator->path().filename())});
	}
	std::sort(files.begin(), files.end(), [](const FileEntry& left, const FileEntry& right) {
		return left.name < right.name;
	});
	if (error)
		s_status = "Could not scan " + _pathToUtf8(directory);
}

void ClearSlotLocked(SwitchToyType type, int slot)
{
	SlotState* state = Slot(type, slot);
	if (!state)
		return;
	switch (type)
	{
	case SwitchToyType::Skylanders:
		if (state->coreSlot >= 0)
			nsyshid::g_skyportal.RemoveSkylander(static_cast<uint8>(state->coreSlot));
		break;
	case SwitchToyType::Infinity:
		nsyshid::g_infinitybase.RemoveFigure(static_cast<uint8>(slot));
		break;
	case SwitchToyType::Dimensions:
		nsyshid::g_dimensionstoypad.RemoveFigure(kDimensionPads[slot], static_cast<uint8>(slot), true);
		break;
	}
	*state = {};
}

void SaveSlotsLocked()
{
	const fs::path path = s_root / "slots.ini";
	const fs::path temporary = s_root / "slots.ini.tmp";
	std::ofstream output(temporary, std::ios::trunc);
	if (!output)
		return;
	auto write = [&](std::string_view prefix, const auto& slots) {
		for (size_t index = 0; index < slots.size(); ++index)
			if (!slots[index].fileName.empty())
				output << prefix << index << '=' << slots[index].fileName << '\n';
	};
	write("sky_", s_skylanders);
	write("inf_", s_infinity);
	write("dim_", s_dimensions);
	output.close();
	if (!output)
		return;
	std::error_code error;
	fs::remove(path, error);
	error.clear();
	fs::rename(temporary, path, error);
}

bool LoadFileLocked(SwitchToyType type, int slot, int fileIndex, bool persist)
{
	auto& files = Files(type);
	SlotState* state = Slot(type, slot);
	if (!state || fileIndex < 0 || fileIndex >= static_cast<int>(files.size()))
		return false;
	const FileEntry fileEntry = files[fileIndex];

	for (int index = 0; index < SwitchToys_GetSlotCount(type); ++index)
	{
		SlotState* other = Slot(type, index);
		if (index != slot && other && other->fileName == fileEntry.name)
			ClearSlotLocked(type, index);
	}
	ClearSlotLocked(type, slot);

	std::unique_ptr<FileStream> stream(FileStream::openFile2(fileEntry.path, true));
	if (!stream)
	{
		s_status = "Could not open " + fileEntry.name;
		return false;
	}

	switch (type)
	{
	case SwitchToyType::Skylanders:
	{
		std::array<uint8, nsyshid::SKY_FIGURE_SIZE> data{};
		if (stream->readData(data.data(), data.size()) != data.size())
		{
			s_status = "Could not read " + fileEntry.name;
			return false;
		}
		const uint16 id = static_cast<uint16>(data[0x11]) << 8 | data[0x10];
		const uint16 variant = static_cast<uint16>(data[0x1D]) << 8 | data[0x1C];
		const uint8 portalSlot = nsyshid::g_skyportal.LoadSkylander(data.data(), std::move(stream));
		if (portalSlot == 0xFF)
		{
			s_status = "Skylanders portal is full";
			return false;
		}
		state->coreSlot = portalSlot;
		state->value = nsyshid::g_skyportal.FindSkylander(id, variant);
		break;
	}
	case SwitchToyType::Infinity:
	{
		std::array<uint8, nsyshid::INF_FIGURE_SIZE> data{};
		if (stream->readData(data.data(), data.size()) != data.size())
		{
			s_status = "Could not read " + fileEntry.name;
			return false;
		}
		const uint32 id = nsyshid::g_infinitybase.LoadFigure(data, std::move(stream), static_cast<uint8>(slot));
		state->value = nsyshid::g_infinitybase.FindFigure(id).second;
		break;
	}
	case SwitchToyType::Dimensions:
	{
		std::array<uint8, 0x2D * 0x04> data{};
		if (stream->readData(data.data(), data.size()) != data.size())
		{
			s_status = "Could not read " + fileEntry.name;
			return false;
		}
		const uint32 id = nsyshid::g_dimensionstoypad.LoadFigure(
			data, std::move(stream), kDimensionPads[slot], static_cast<uint8>(slot));
		state->value = nsyshid::g_dimensionstoypad.FindFigure(id);
		break;
	}
	}

	state->fileName = fileEntry.name;
	s_status = "Loaded " + state->value;
	if (persist)
		SaveSlotsLocked();
	return true;
}

std::optional<std::tuple<SwitchToyType, int, std::string>> ParseSlotLine(const std::string& line)
{
	const size_t equals = line.find('=');
	if (equals == std::string::npos || equals + 1 >= line.size())
		return std::nullopt;
	SwitchToyType type;
	size_t indexOffset;
	if (line.rfind("sky_", 0) == 0)
	{
		type = SwitchToyType::Skylanders;
		indexOffset = 4;
	}
	else if (line.rfind("inf_", 0) == 0)
	{
		type = SwitchToyType::Infinity;
		indexOffset = 4;
	}
	else if (line.rfind("dim_", 0) == 0)
	{
		type = SwitchToyType::Dimensions;
		indexOffset = 4;
	}
	else
		return std::nullopt;
	int slot = 0;
	for (size_t index = indexOffset; index < equals; ++index)
	{
		if (!std::isdigit(static_cast<unsigned char>(line[index])))
			return std::nullopt;
		slot = slot * 10 + line[index] - '0';
	}
	if (!Slot(type, slot))
		return std::nullopt;
	return std::tuple{type, slot, line.substr(equals + 1)};
}
} // namespace

void SwitchToys_Initialize()
{
	std::scoped_lock lock(s_mutex);
	s_root = ActiveSettings::GetUserDataPath("toys");
	std::error_code error;
	fs::create_directories(s_root, error);
	for (SwitchToyType type : {SwitchToyType::Skylanders, SwitchToyType::Infinity, SwitchToyType::Dimensions})
		RefreshFilesLocked(type);

	std::ifstream input(s_root / "slots.ini");
	std::string line;
	while (std::getline(input, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		auto parsed = ParseSlotLine(line);
		if (!parsed)
			continue;
		auto [type, slot, fileName] = *parsed;
		auto& files = Files(type);
		auto found = std::find_if(files.begin(), files.end(), [&](const FileEntry& entry) {
			return entry.name == fileName;
		});
		if (found != files.end())
			LoadFileLocked(type, slot, static_cast<int>(found - files.begin()), false);
	}
}

bool SwitchToys_IsEnabled(SwitchToyType type)
{
	const auto& devices = GetConfig().emulated_usb_devices;
	switch (type)
	{
	case SwitchToyType::Skylanders:
		return devices.emulate_skylander_portal.GetValue();
	case SwitchToyType::Infinity:
		return devices.emulate_infinity_base.GetValue();
	case SwitchToyType::Dimensions:
		return devices.emulate_dimensions_toypad.GetValue();
	}
	return false;
}

int SwitchToys_GetSlotCount(SwitchToyType type)
{
	switch (type)
	{
	case SwitchToyType::Skylanders:
		return nsyshid::MAX_SKYLANDERS;
	case SwitchToyType::Infinity:
		return nsyshid::MAX_FIGURES;
	case SwitchToyType::Dimensions:
		return 7;
	}
	return 0;
}

std::string SwitchToys_GetSlotTitle(SwitchToyType type, int slot)
{
	if (slot < 0 || slot >= SwitchToys_GetSlotCount(type))
		return {};
	switch (type)
	{
	case SwitchToyType::Skylanders:
		return "Portal " + std::to_string(slot + 1);
	case SwitchToyType::Infinity:
		return kInfinitySlots[slot];
	case SwitchToyType::Dimensions:
		return kDimensionSlots[slot];
	}
	return {};
}

std::string SwitchToys_GetSlotValue(SwitchToyType type, int slot)
{
	std::scoped_lock lock(s_mutex);
	SlotState* state = Slot(type, slot);
	return state && !state->value.empty() ? state->value : "Empty";
}

void SwitchToys_RefreshFiles(SwitchToyType type)
{
	std::scoped_lock lock(s_mutex);
	RefreshFilesLocked(type);
	s_status = std::to_string(Files(type).size()) + " compatible dumps found";
}

int SwitchToys_GetFileCount(SwitchToyType type)
{
	std::scoped_lock lock(s_mutex);
	return static_cast<int>(Files(type).size());
}

std::string SwitchToys_GetFileName(SwitchToyType type, int index)
{
	std::scoped_lock lock(s_mutex);
	const auto& files = Files(type);
	return index >= 0 && index < static_cast<int>(files.size()) ? files[index].name : std::string{};
}

bool SwitchToys_LoadFile(SwitchToyType type, int slot, int fileIndex)
{
	std::scoped_lock lock(s_mutex);
	const bool loaded = LoadFileLocked(type, slot, fileIndex, true);
	if (!loaded && s_status == "Ready")
		s_status = "Figure dump could not be loaded";
	return loaded;
}

bool SwitchToys_ClearSlot(SwitchToyType type, int slot)
{
	std::scoped_lock lock(s_mutex);
	SlotState* state = Slot(type, slot);
	if (!state || state->value.empty())
		return false;
	const std::string previous = state->value;
	ClearSlotLocked(type, slot);
	SaveSlotsLocked();
	s_status = "Removed " + previous;
	return true;
}

std::string SwitchToys_GetStatus()
{
	std::scoped_lock lock(s_mutex);
	return s_status;
}
