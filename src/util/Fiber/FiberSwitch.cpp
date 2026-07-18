#include "Fiber.h"
#include <cstdlib>
#include <cstdint>
#include <new>

thread_local Fiber* sCurrentFiber{};

namespace
{
	struct FiberCtx
	{
		uint64_t gpr[13];
		uint64_t simd[8];
	};
	static_assert(sizeof(FiberCtx) == 0xA8, "FiberCtx layout mismatch");
}

extern "C" void _fiberSwap(FiberCtx* from, FiberCtx* to);
extern "C" void _fiberTrampoline();

Fiber::Fiber(void (*FiberEntryPoint)(void* userParam), void* userParam, void* privateData) : m_privateData(privateData)
{
	FiberCtx* ctx = (FiberCtx*)calloc(1, sizeof(FiberCtx));
	if (!ctx)
		throw std::bad_alloc();

	const size_t stackSize = 2 * 1024 * 1024;
	m_stackPtr = malloc(stackSize);
	if (!m_stackPtr)
	{
		free(ctx);
		throw std::bad_alloc();
	}

	uintptr_t stackTop = ((uintptr_t)m_stackPtr + stackSize) & ~(uintptr_t)0xF;

	ctx->gpr[0] = (uint64_t)FiberEntryPoint;
	ctx->gpr[1] = (uint64_t)userParam;
	ctx->gpr[11] = (uint64_t)&_fiberTrampoline;
	ctx->gpr[12] = (uint64_t)stackTop;

	m_implData = (void*)ctx;
}

Fiber::Fiber(void* privateData) : m_privateData(privateData)
{
	FiberCtx* ctx = (FiberCtx*)calloc(1, sizeof(FiberCtx));
	if (!ctx)
		throw std::bad_alloc();
	m_implData = (void*)ctx;
	m_stackPtr = nullptr;
}

Fiber::~Fiber()
{
	if (m_stackPtr)
		free(m_stackPtr);
	free(m_implData);
}

Fiber* Fiber::PrepareCurrentThread(void* privateData)
{
	cemu_assert_debug(sCurrentFiber == nullptr);
	if (sCurrentFiber)
		return sCurrentFiber;
	sCurrentFiber = new Fiber(privateData);
	return sCurrentFiber;
}

void Fiber::Switch(Fiber& targetFiber)
{
	Fiber* leavingFiber = sCurrentFiber;
	sCurrentFiber = &targetFiber;
	_fiberSwap((FiberCtx*)leavingFiber->m_implData, (FiberCtx*)targetFiber.m_implData);
}

void* Fiber::GetFiberPrivateData()
{
	return sCurrentFiber->m_privateData;
}
