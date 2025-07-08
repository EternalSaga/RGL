#include "shaderReflection.hpp"
#include <boost/program_options.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <iostream>
#include <fstream>

int main(int argc, char* argv[]) {

    namespace po = boost::program_options;
    using json = nlohmann::json;
    using namespace RGL::reflection;
    try {
        po::options_description desc("SPIR-V Reflector Options");
        desc.add_options()
            ("help,h", "produce help message")
            ("input,i", po::value<std::string>(), "input SPIR-V file")
            ("output,o", po::value<std::string>(), "output JSON file, default stdout")
            ("pretty,p",po::bool_switch()->default_value(false), "pretty print JSON output")
        ;

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);

        // 如果用户请求帮助信息，则打印并退出
        if (vm.count("help")) {
            std::cout << "A command-line tool to reflect SPIR-V shaders into a JSON contract.\n\n";
            std::cout << desc << std::endl;
            return 0;
        }

        // 检查所有必须的选项是否都已提供
        // po::required() 会在 vm.notify() 时抛出异常
        po::notify(vm);

        // 3. 执行核心逻辑
        std::string input_path = vm["input"].as<std::string>();
        
        std::cout << "Reflecting shader: " << input_path << "..." << std::endl;

        // 创建 ShaderReflection 实例，这会触发反射过程
        ShaderReflection reflection(input_path);
        
        // 通过类型转换操作符获取JSON对象
        json result_json = static_cast<json>(reflection);

        // 获取JSON字符串，根据--pretty选项决定是否格式化
        int indent = vm["pretty"].as<bool>() ? 4 : -1;
        std::string output_string = result_json.dump(indent);

        // 4. 输出结果
        if (vm.count("output")) {
            // 输出到文件
            std::string output_path = vm["output"].as<std::string>();
            std::ofstream out_file(output_path);
            if (!out_file.is_open()) {
                throw std::runtime_error("Failed to open output file: " + output_path);
            }
            out_file << output_string << std::endl; // endl 会刷新缓冲区并添加换行符
            out_file.close();
            std::cout << "Successfully wrote reflection data to " << output_path << std::endl;
        } else {
            // 输出到标准输出
            std::cout << "\n--- Reflection JSON Output ---\n" << std::endl;
            std::cout << output_string << std::endl;
        }

    } catch (const po::error& e) {
         std::cerr << "Error: " << e.what() << std::endl;
        std::cerr << "Use --help for a list of options." << std::endl;
        return 1;
    }catch (const std::exception& e) {
        // 其他所有异常 (文件未找到, spirv-cross解析失败等)
        std::cerr << "An error occurred: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        // 捕获所有未知异常
        std::cerr << "An unknown error occurred." << std::endl;
        return 1;
    }



    return 0;
}