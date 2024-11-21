#include <iostream>
#include <boost/pfr.hpp>
#include <vector>
#include <array>
struct UniformData1 {
	float x;
	float y;
	float z;
	std::array<float,3> campos;
	std::array<float, 3> entityPos;
};


struct UniformData2 {
    float x;
 
    std::array<float, 3> campos;
    std::array<float, 3> entityPos;
};

struct Uniform2Struct {
    UniformData1 u1;
    UniformData2 u2;
};

 
int main() {
	UniformData1 u1;
    UniformData2 u2;
	Uniform2Struct u3;
    boost::pfr::for_each_field(u3, [](auto& f, size_t idx) {
	std::cout << sizeof(f) << std::endl;
    });
    return 0;
}
