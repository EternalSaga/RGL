#include <iostream>
#include <string_view>
#include <boost/hana/tuple.hpp>
#include <boost/hana/for_each.hpp>
#include <boost/hana/all_of.hpp>
#include <array>
template<typename T>
struct VertexElement
{
	std::string_view name;
	consteval size_t getSize() { return sizeof(T); };
	consteval size_t getLength() {
		T t;
		return std::size(t);
	};
	VertexElement(std::string_view n) :name(n) {}
};
namespace hana = boost::hana;
using namespace hana::literals;




template <typename T>
concept IsVertexElementTuple = requires(T t) {
	hana::for_each(t, [](auto t) {
		t.name->std::convertible_to<std::string_view>;
		t.getSize()->std::convertible_to<std::size_t>;
		t.getLength()->std::convertible_to<std::size_t>;
		});
};

template<IsVertexElementTuple T>
void printOffset(const T& vertexDescription) {

	size_t size = 0;
	hana::for_each(vertexDescription, [&size](auto vert) {
		size += vert.getSize();
		});
	std::cout << "Total size:" <<size<< std::endl;

	// 遍历offsets元组，输出每个元素的偏移量
	size_t current_offset = 0;
	hana::for_each(vertexDescription, [&current_offset](auto vert) {
		std::cout << vert.name << " offset: " << current_offset <<". Length:  "<< vert.getLength() << std::endl;
		current_offset += vert.getSize();
		});
}

int main() {

	auto vertexDescription = hana::make_tuple(
		VertexElement<float[3]>("pos"),
		VertexElement<float[2]>("uv"),
		VertexElement<float[3]>("norm"),
		VertexElement<float[4]>("color")
	);

	printOffset(vertexDescription);

	return 0;
}
