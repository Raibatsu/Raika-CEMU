#include "ExceptionHandler.h"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#define CEMU_STRINGIZE_IMPL(value) #value
#define CEMU_STRINGIZE(value) CEMU_STRINGIZE_IMPL(value)

namespace
{
	std::atomic_flag s_handlingException = ATOMIC_FLAG_INIT;

	void WriteAll(int fd, const char* data, size_t size)
	{
		while (size != 0)
		{
			const ssize_t written = ::write(fd, data, size);
			if (written > 0)
			{
				data += written;
				size -= static_cast<size_t>(written);
				continue;
			}
			if (written < 0 && errno == EINTR)
				continue;
			return;
		}
	}
}

extern "C"
{
	alignas(16) u8 __nx_exception_stack[0x10000];
	u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

	void __libnx_exception_handler(ThreadExceptionDump* context)
	{
		if (s_handlingException.test_and_set(std::memory_order_relaxed))
			return;

		int fd = ::open("sdmc:/switch/Cemu/crash.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0)
			fd = ::open("crash.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0)
			return;

		char report[4096]{};
		size_t used = 0;
		auto append = [&](const char* format, auto... args) {
			if (used >= sizeof(report))
				return;
			const int length = std::snprintf(report + used, sizeof(report) - used, format, args...);
			if (length <= 0)
				return;
			used += std::min(static_cast<size_t>(length), sizeof(report) - used - 1);
		};

		append("Cemu Switch native exception\n");
#if defined(EMULATOR_HASH)
		append("build: %s\n", CEMU_STRINGIZE(EMULATOR_HASH));
#endif
		append("error_desc: 0x%08x\n", context->error_desc);
		for (int index = 0; index < 29; ++index)
			append("x%-2d: 0x%016llx\n", index,
				static_cast<unsigned long long>(context->cpu_gprs[index].x));
		append("fp: 0x%016llx\n", static_cast<unsigned long long>(context->fp.x));
		append("lr: 0x%016llx\n", static_cast<unsigned long long>(context->lr.x));
		append("sp: 0x%016llx\n", static_cast<unsigned long long>(context->sp.x));
		append("pc: 0x%016llx\n", static_cast<unsigned long long>(context->pc.x));
		append("pstate: 0x%08x\n", context->pstate);
		append("afsr0: 0x%08x\n", context->afsr0);
		append("afsr1: 0x%08x\n", context->afsr1);
		append("esr: 0x%08x\n", context->esr);
		append("far: 0x%016llx\n", static_cast<unsigned long long>(context->far.x));
		WriteAll(fd, report, used);
		::close(fd);
	}
}

void ExceptionHandler_Init()
{
}

#undef CEMU_STRINGIZE
#undef CEMU_STRINGIZE_IMPL
