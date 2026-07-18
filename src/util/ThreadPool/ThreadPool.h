#include "util/helpers/ThreadHelpers.h"

class ThreadPool
{
public:
	template<class TFunction, class... TArgs>
	static void FireAndForget(TFunction&& f, TArgs&&... args)
	{
		// todo - find a way to use std::async here so we can utilize thread pooling?
		cemuCreateDetachedThread(std::forward<TFunction>(f), std::forward<TArgs>(args)...);
	}
};
