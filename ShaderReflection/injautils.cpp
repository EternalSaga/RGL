#include "injautils.hpp"
#include <chrono>
namespace RGL::reflection{

std::string getCurrentTimestamp() {
    auto now = std::chrono::system_clock::now();
    return std::format("{:%Y-%m-%d %H:%M:%S}",
	std::chrono::zoned_time{std::chrono::current_zone(), now});
}
}  // namespace RGL::reflection