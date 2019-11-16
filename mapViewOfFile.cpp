#include <iostream>
#include <Windows.h>
#include <cstdint>
#include <thread>
#include <sstream>
#include <vector>


namespace
{
constexpr bool LogCollisions		= false;
constexpr size_t ConcurrencyCount	= 50;
constexpr uint_fast8_t RetryCount	= 10;
constexpr bool UseTopAddresses		= true;
constexpr size_t IterationCount		= 1000;


/*
	=> ConcurrencyCount = 1
	- UseTopAddresses:false = 279339 ticks
	- UseTopAddresses:true  = 329099 ticks // +17%

	=> ConcurrencyCount = 50
	- UseTopAddresses:false = 6305722 ticks
	- UseTopAddresses:true  = 6746238 ticks // +7%
*/
}


namespace
{
struct CloseHandleFunctor
{
	inline auto operator()(HANDLE handle)
	{
		return CloseHandle(handle);
	}
};

using ScopedHanle = std::unique_ptr<std::remove_pointer_t<HANDLE>, CloseHandleFunctor>;
}

namespace
{
struct UnmapFileViewFunctor
{
	inline auto operator()(void *ptr)
	{
		return UnmapViewOfFile(ptr);
	}
};

using ScopedViewOfFile = std::unique_ptr<void, UnmapFileViewFunctor>;
}

namespace
{
std::string GetFileName()
{
	std::vector<char> buf(static_cast<size_t>(MAX_PATH), '\0');

	for (; INFINITE;)
	{
		GetModuleFileNameA(nullptr, data(buf), size(buf));
		const auto gle = GetLastError();
		switch (gle)
		{
		case ERROR_SUCCESS:
			return buf.data();
		case ERROR_INSUFFICIENT_BUFFER:
			buf.resize(0);
			buf.resize(buf.capacity() * 2, '\0');
			break;
		default:
			throw std::runtime_error("can't receive file name");
		}
	}
}
}

namespace
{
void* GetTopFreeAddress(size_t size)
{
	if (!UseTopAddresses)
		return nullptr;

	auto *ptr = VirtualAlloc(nullptr, size, MEM_TOP_DOWN | MEM_RESERVE, PAGE_NOACCESS);
	VirtualFree(ptr, 0, MEM_RELEASE);

	return ptr;
}

ScopedHanle OpenFile(const char *file)
{
	auto h = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		throw std::runtime_error("can't open file");
	return ScopedHanle{h};
}

LARGE_INTEGER GetSizeOfFile(const ScopedHanle &file)
{
	LARGE_INTEGER fileSz;
	if (!GetFileSizeEx(file.get(), &fileSz))
		throw std::runtime_error("Can't receive file size");
	return fileSz;
}

ScopedHanle CreateFileMap(const ScopedHanle &file, const LARGE_INTEGER &fileSize)
{
	auto h = CreateFileMapping(file.get(), nullptr, PAGE_READONLY, fileSize.HighPart, fileSize.LowPart, nullptr);
	if (!h)
		throw std::runtime_error("can't create file mapping");

	return ScopedHanle{h};
}

ScopedViewOfFile MapFileAtTopAddresses(const ScopedHanle &fileMap, size_t mapSize)
{
	for (uint_fast8_t it = 0; it < RetryCount; it++)
	{
		void *addr = GetTopFreeAddress(mapSize);
		void *data = MapViewOfFileEx(fileMap.get(), FILE_MAP_READ, 0, 0, mapSize, addr);
		if (!data)
		{
			if (LogCollisions)
			{
				const auto  gle = GetLastError();
				std::stringstream buf;
				buf << "[" << GetCurrentThreadId() << "] GLE=" << gle << " | addr = " << addr << " | retry count = " << static_cast<uint32_t>(it) << "\n";
				std::cout << buf.str();
			}

			continue;
		}

		if (addr && (addr != data))
			throw std::logic_error("addr != data");

		return ScopedViewOfFile{ data };
	}

	std::cout << "FAILED to map\n";
	return nullptr;
}
}

void Map(const char *fileName)
{
	ScopedHanle file = OpenFile(fileName);
	const auto fileSize = GetSizeOfFile(file);
	ScopedHanle fileMap = CreateFileMap(file, fileSize);
	file.reset();

	const auto mapSize = 1024 * 1024 * 22; // 1Mb

	MapFileAtTopAddresses(fileMap, min(mapSize, static_cast<size_t>(fileSize.QuadPart)));
	fileMap.reset();
}


HANDLE startEvent{};


void Work(const char *file)
try
{
	WaitForSingleObject(startEvent, INFINITE);
	Map(file);
}
catch (const std::exception &ex)
{
	std::stringstream buf;
	buf << "[" << GetCurrentThread() << "] Exception occured:\n" << ex.what() << "\n";
	std::cout << buf.str();
}

class Chronometer final
{
public:
	inline Chronometer() : reset_time{ clock.now() }
	{
	}

	inline auto ElapsedTicks() const
	{
		return (clock.now() - reset_time).count();
	}

private:
	const std::chrono::high_resolution_clock::time_point reset_time;
	std::chrono::high_resolution_clock clock;
};


int main()
try
{
	const auto fileName = GetFileName();
	std::cout << "FileName to map: " << fileName.c_str() << "\n";

	uint64_t elapsedSum{};
	for (size_t it = 0; it < IterationCount; it++)
	{
		startEvent = CreateEventA(nullptr, true, false, "");
		ScopedHanle _ev{ startEvent };

		std::thread th[ConcurrencyCount];
		for (auto &t : th)
			t = std::thread{ Work, fileName.c_str() };
		///////////

		Chronometer chronometer;
		{
			SetEvent(startEvent);

			for (auto &t : th)
				t.join();
		}
		const auto elapsed = chronometer.ElapsedTicks();
		elapsedSum += elapsed;
	}

	std::cout << "-----------\n" << (elapsedSum / IterationCount) << " ticks \n";

	getchar();
	return 0;
}
catch (const std::exception &ex)
{
	std::cout << "Exception occurred:\n" << ex.what() << "\n";
	return -1;
}