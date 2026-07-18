#pragma once
#include <functional>
#include <memory>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

#if defined(__SWITCH__)
#include <pthread.h>
#endif

template<typename Function, typename... Args>
void cemuCreateDetachedThread(Function&& function, Args&&... args)
{
#if defined(__SWITCH__)
	auto bound = [callable = std::decay_t<Function>(std::forward<Function>(function)),
		arguments = std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...)]() mutable {
		std::apply(
			[&callable](auto&&... unpacked) {
				std::invoke(std::move(callable), std::forward<decltype(unpacked)>(unpacked)...);
			},
			std::move(arguments));
	};
	using Task = decltype(bound);
	auto task = std::make_unique<Task>(std::move(bound));

	pthread_attr_t attributes{};
	int error = pthread_attr_init(&attributes);
	if (error != 0)
		throw std::system_error(error, std::generic_category(), "failed to configure detached thread");

	error = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	if (error != 0)
	{
		pthread_attr_destroy(&attributes);
		throw std::system_error(error, std::generic_category(), "failed to configure detached thread");
	}

	pthread_t thread{};
	error = pthread_create(&thread, &attributes,
		[](void* opaque) -> void* {
			auto ownedTask = std::unique_ptr<Task>(static_cast<Task*>(opaque));
			try
			{
				std::invoke(std::move(*ownedTask));
			}
			catch (...)
			{
				std::terminate();
			}
			return nullptr;
		}, task.get());
	pthread_attr_destroy(&attributes);
	if (error != 0)
		throw std::system_error(error, std::generic_category(), "failed to create detached thread");
	task.release();
#else
	std::thread(std::forward<Function>(function), std::forward<Args>(args)...).detach();
#endif
}
