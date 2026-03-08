#pragma once

#include <type_traits>

namespace tachyon::core {
	template <typename T>
	concept TachyonPayload = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T> && !std::is_pointer_v<T>;
} // namespace tachyon::core
