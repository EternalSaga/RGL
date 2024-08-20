#pragma once
#include <string_view>
#include <boost/hana/tuple.hpp>
#include <boost/hana/for_each.hpp>

namespace RGL
{
namespace glcore
{
template <typename T>
struct VertexElement {
    std::string_view name;
    consteval size_t getSize() { return sizeof(T); };
    consteval size_t getLength()
    {
	T t;
	return std::size(t);
    };
    VertexElement(std::string_view n) : name(n) {}
};
namespace hana = boost::hana;
using namespace hana::literals;

template <typename T>
concept IsVertexElementTuple = requires(T t) {
    hana::for_each(t, [](auto t) {
	t.name->std::template convertible_to<std::string_view>;
	t.getSize()->std::template convertible_to<std::size_t>;
	t.getLength()->std::template convertible_to<std::size_t>;
    });
};

template <IsVertexElementTuple VertexDescType>
size_t
getVertexSize(const VertexDescType &vertexDescription)
{
    size_t size = 0;
    // 累加总size
    hana::for_each(vertexDescription,
	[&size](auto vert) { size += vert.getSize(); });
	return size;
}

} // namespace glcore

} // namespace RGL
