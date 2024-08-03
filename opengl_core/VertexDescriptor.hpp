#pragma once
#include <string_view>
#include <boost/hana/tuple.hpp>
#include <boost/hana/for_each.hpp>


namespace RGL {
	namespace glcore {
		template<typename T>
		struct VertexElement
		{
			std::string name;
			consteval size_t getSize() { return sizeof(T); };
			consteval size_t getLength() {
				T t;
				return std::size(t);
			};
			VertexElement(std::string n) :name(n) {}
		};
		namespace hana = boost::hana;
		using namespace hana::literals;

		template <typename T>
		concept IsVertexElementTuple = requires(T t) {
			hana::for_each(t, [](auto t) {
				t.name->std::convertible_to<std::string>;
				t.getSize()->std::convertible_to<std::size_t>;
				t.getLength()->std::convertible_to<std::size_t>;
				});
		};

		//template<IsVertexElementTuple T>
		//void printOffset(const T& vertexDescription) {
		//	// 使用hana::transform计算每个元素的偏移量
		//	auto offsets = hana::transform(vertexDescription, [](auto x) {
		//		static constexpr size_t offset = x.getSize();
		//		return hana::make_tuple(x.name, offset);
		//		});

		//	// 遍历offsets元组，输出每个元素的偏移量
		//	size_t current_offset = 0;
		//	hana::for_each(offsets, [&current_offset](auto offset) {
		//		std::cout << offset[0_c] << " offset: " << current_offset << std::endl;
		//		current_offset += offset[1_c];
		//		});
		//}
	}
}
