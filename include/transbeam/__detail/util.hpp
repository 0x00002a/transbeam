#pragma once

namespace transbeam::__detail::util {

constexpr auto diff(auto a, auto b) { return a > b ? a - b : b - a; }

} // namespace transbeam::__detail::util