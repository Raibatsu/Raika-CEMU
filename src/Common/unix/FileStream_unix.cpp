#include "Common/unix/FileStream_unix.h"
#include <cstdarg>

#if defined(__SWITCH__)
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#endif

fs::path findPathCI(const fs::path& path)
{
#if defined(__SWITCH__)
	// fsdev is case-insensitive and may reject missing intermediate paths as I/O errors.
	return path;
#else
	std::error_code ec;
	if (fs::exists(path, ec)) return path;

	fs::path fName = path.filename();
	fs::path parentPath = path.parent_path();
	if (parentPath.empty())
		parentPath = ".";
	else if (!fs::exists(parentPath, ec))
		parentPath = findPathCI(parentPath);

	std::error_code listErr;
	for (auto&& dirEntry : fs::directory_iterator(parentPath, listErr))
		if (boost::iequals(dirEntry.path().filename().string(), fName.string()))
			return dirEntry;

	return parentPath / fName;
#endif
}

FileStream* FileStream::openFile(std::string_view path)
{
	return openFile2(path, false);
}

FileStream* FileStream::openFile(const wchar_t* path, bool allowWrite)
{
	return openFile2(path, allowWrite);
}

FileStream* FileStream::openFile2(const fs::path& path, bool allowWrite)
{
	FileStream* fs = new FileStream(path, true, allowWrite);
	if (fs->m_isValid)
		return fs;
	delete fs;
	return nullptr;
}

FileStream* FileStream::createFile(const wchar_t* path)
{
	return createFile2(path);
}

FileStream* FileStream::createFile(std::string_view path)
{
	return createFile2(path);
}

FileStream* FileStream::createFile2(const fs::path& path)
{
	FileStream* fs = new FileStream(path, false, false);
	if (fs->m_isValid)
		return fs;
	delete fs;
	return nullptr;
}

std::optional<std::vector<uint8>> FileStream::LoadIntoMemory(const fs::path& path)
{
	FileStream* fs = openFile2(path);
	if (!fs)
		return std::nullopt;
	uint64 fileSize = fs->GetSize();
	if (fileSize > 0xFFFFFFFFull)
	{
		delete fs;
		return std::nullopt;
	}
	std::optional<std::vector<uint8>> v(fileSize);
	if (fs->readData(v->data(), (uint32)fileSize) != (uint32)fileSize)
	{
		delete fs;
		return std::nullopt;
	}
	delete fs;
	return v;
}

void FileStream::SetPosition(uint64 pos)
{
#if defined(__SWITCH__)
	if (!m_isValid)
		return;
	if (pos > static_cast<uint64>(std::numeric_limits<off_t>::max()) ||
		::lseek(m_fileDescriptor, static_cast<off_t>(pos), SEEK_SET) < 0)
	{
		m_isValid = false;
	}
#else
	cemu_assert(m_isValid);
	if (m_prevOperationWasWrite)
		m_fileStream.seekp((std::streampos)pos);
	else
		m_fileStream.seekg((std::streampos)pos);
#endif
}

uint64 FileStream::GetSize()
{
#if defined(__SWITCH__)
	if (!m_isValid)
		return 0;
	struct stat fileStat{};
	if (::fstat(m_fileDescriptor, &fileStat) != 0 || fileStat.st_size < 0)
	{
		m_isValid = false;
		return 0;
	}
	return static_cast<uint64>(fileStat.st_size);
#else
	cemu_assert(m_isValid);
	auto currentPos = m_fileStream.tellg();
	m_fileStream.seekg(0, std::ios::end);
	auto fileSize = m_fileStream.tellg();
	m_fileStream.seekg(currentPos, std::ios::beg);
	uint64 fs = (uint64)fileSize;
	return fs;
#endif
}

bool FileStream::SetEndOfFile()
{
#if defined(__SWITCH__)
	if (!m_isValid)
		return false;
	const off_t position = ::lseek(m_fileDescriptor, 0, SEEK_CUR);
	if (position < 0 || ::ftruncate(m_fileDescriptor, position) != 0)
	{
		m_isValid = false;
		return false;
	}
	return true;
#else
	assert_dbg();
	return true;
	//return ::SetEndOfFile(m_hFile) != 0;
#endif
}

void FileStream::extract(std::vector<uint8>& data)
{
	uint64 fileSize = GetSize();
	SetPosition(0);
	data.resize(fileSize);
	readData(data.data(), fileSize);
}

void FileStream::Flush()
{
#if defined(__SWITCH__)
	if (m_isValid && ::fsync(m_fileDescriptor) != 0)
		m_isValid = false;
#else
    m_fileStream.flush();
#endif
}

uint32 FileStream::readData(void* data, uint32 length)
{
#if defined(__SWITCH__)
	if (!m_isValid)
		return 0;
	uint8* output = static_cast<uint8*>(data);
	uint32 totalRead = 0;
	while (totalRead < length)
	{
		const ssize_t bytesRead = ::read(m_fileDescriptor, output + totalRead, length - totalRead);
		if (bytesRead > 0)
		{
			totalRead += static_cast<uint32>(bytesRead);
			continue;
		}
		if (bytesRead < 0 && errno == EINTR)
			continue;
		if (bytesRead < 0)
			m_isValid = false;
		break;
	}
	return totalRead;
#else
	SyncReadWriteSeek(false);
	m_fileStream.read((char*)data, length);
	size_t bytesRead = m_fileStream.gcount();
	return (uint32)bytesRead;
#endif
}

bool FileStream::readU64(uint64& v)
{
	return readData(&v, sizeof(uint64)) == sizeof(uint64);
}

bool FileStream::readU32(uint32& v)
{
	return readData(&v, sizeof(uint32)) == sizeof(uint32);
}

bool FileStream::readU8(uint8& v)
{
	return readData(&v, sizeof(uint8)) == sizeof(uint8);
}

bool FileStream::readLine(std::string& line)
{
	line.clear();
	uint8 c;
	bool isEOF = true;
	while (readU8(c))
	{
		isEOF = false;
		if (c == '\r')
			continue;
		if (c == '\n')
			break;
		line.push_back((char)c);
	}
	return !isEOF;
}

sint32 FileStream::writeData(const void* data, sint32 length)
{
#if defined(__SWITCH__)
	if (!m_isValid || length < 0)
		return -1;
	const uint8* input = static_cast<const uint8*>(data);
	sint32 totalWritten = 0;
	while (totalWritten < length)
	{
		const ssize_t bytesWritten = ::write(m_fileDescriptor, input + totalWritten, length - totalWritten);
		if (bytesWritten > 0)
		{
			totalWritten += static_cast<sint32>(bytesWritten);
			continue;
		}
		if (bytesWritten < 0 && errno == EINTR)
			continue;
		m_isValid = false;
		return totalWritten > 0 ? totalWritten : -1;
	}
	return totalWritten;
#else
	SyncReadWriteSeek(true);
	m_fileStream.write((const char*)data, length);
	return length;
#endif
}

void FileStream::writeU64(uint64 v)
{
	writeData(&v, sizeof(uint64));
}

void FileStream::writeU32(uint32 v)
{
	writeData(&v, sizeof(uint32));
}

void FileStream::writeU8(uint8 v)
{
	writeData(&v, sizeof(uint8));
}

void FileStream::writeStringFmt(const char* format, ...)
{
	char buffer[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	writeData(buffer, (sint32)strlen(buffer));
}

void FileStream::writeString(const char* str)
{
	writeData(str, (sint32)strlen(str));
}

void FileStream::writeLine(const char* str)
{
	writeData(str, (sint32)strlen(str));
	writeData("\r\n", 2);
}

FileStream::~FileStream()
{
#if defined(__SWITCH__)
	if (m_fileDescriptor >= 0)
		::close(m_fileDescriptor);
#else
	if (m_isValid)
	{
		m_fileStream.close();
	}
	//	CloseHandle(m_hFile);
#endif
}

FileStream::FileStream(const fs::path& path, bool isOpen, bool isWriteable)
{
	fs::path CIPath = findPathCI(path);
#if defined(__SWITCH__)
	int flags = O_RDONLY;
	if (isOpen)
		flags = isWriteable ? O_RDWR : O_RDONLY;
	else
		flags = O_RDWR | O_CREAT | O_TRUNC;
	m_fileDescriptor = ::open(CIPath.c_str(), flags, 0666);
	m_isValid = m_fileDescriptor >= 0;
	if (m_isValid)
	{
		struct stat fileStat{};
		if (::fstat(m_fileDescriptor, &fileStat) != 0 || S_ISDIR(fileStat.st_mode))
		{
			::close(m_fileDescriptor);
			m_fileDescriptor = -1;
			m_isValid = false;
		}
	}
#else
	if (isOpen)
	{
		m_fileStream.open(CIPath, isWriteable ? (std::ios_base::in | std::ios_base::out | std::ios_base::binary) : (std::ios_base::in | std::ios_base::binary));
		m_isValid = m_fileStream.is_open();
	}
	else
	{
		m_fileStream.open(CIPath, std::ios_base::in | std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
		m_isValid = m_fileStream.is_open();
	}
	std::error_code isDirEc;
	if(m_isValid && fs::is_directory(path, isDirEc))
	{
		m_isValid = false;
		m_fileStream.close();
	}
#endif
}

void FileStream::SyncReadWriteSeek(bool nextOpIsWrite)
{
#if defined(__SWITCH__)
	(void)nextOpIsWrite;
#else
	// nextOpIsWrite == false -> read. Otherwise write
	if (nextOpIsWrite == m_prevOperationWasWrite)
		return;
	if (nextOpIsWrite)
		m_fileStream.seekp(m_fileStream.tellg(), std::ios::beg);
	else
		m_fileStream.seekg(m_fileStream.tellp(), std::ios::beg);

	m_prevOperationWasWrite = nextOpIsWrite;
#endif
}
