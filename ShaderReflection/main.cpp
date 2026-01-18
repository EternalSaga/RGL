#include <fmt/format.h>
#include "shaderReflection.hpp"
#include <boost/program_options.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <string>
#include "json2cpp.hpp"
#include "rllogger.hpp"

int main(int argc, char* argv[]) {
    namespace po = boost::program_options;
    using json = nlohmann::json;
    using namespace RGL::reflection;
    try {
	po::options_description desc("SPIR-V Reflector Options");
	desc.add_options()
	("help,h", "produce help message")
	("input,i", po::value<std::string>()->required(), "input SPIR-V file")
	("type,t", po::value<std::string>()->required(), "output type, json or cpp")
	("template-dir,d",po::value<std::string>(),"template directory which contains jinja templates for cpp output, required for cpp output type, ignored for json output")
	("output,o", po::value<std::string>()->required(), "output file name, doesn't include any kind of file extension")
	("config-file,c", po::value<std::string>()->required(), "config file path for reflection config")
	("sampler_rule,s", po::value<std::string>()->required(), "sampler rules file path, which used to check shader samplers if they are valid for the given rules.");

	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);

	// 如果用户请求帮助信息，则打印并退出
	if (vm.count("help")) {
	    std::cout << "A command-line tool to reflect SPIR-V shaders into a JSON contract.\n\n";
	    std::cout << desc << std::endl;
	    return 0;
	}

	po::notify(vm);
	std::string loggerPath = vm["config-file"].as<std::string>();

	RGL::RLLogger::initialize(loggerPath);
	std::string input_path = vm["input"].as<std::string>();
	
	RGL::RLLogger::getInstance()->info("Reflecting shader: {}...", input_path);

	// 创建 ShaderReflection 实例，这会触发反射过程
	ShaderReflection reflection(input_path,vm["sampler_rule"].as<std::string>());

	// 通过类型转换操作符获取JSON对象
	json result_json = static_cast<json>(reflection);

	std::string outputType = vm["type"].as<std::string>();

	if (outputType == "json") {
	    std::string output_string = result_json.dump(4);
	    std::string output_path = fmt::format("{}.json", vm["output"].as<std::string>());
		std::ofstream out_file(output_path);
		RGL::RLLogger::getInstance()->info("Reflecting spir-v to json file {}.", output_path);
		if (!out_file.is_open()) {
			RGL::RLLogger::getInstance()->error("Failed to open output file: {}.",output_path);

		}
		out_file << output_string << std::endl;	
		out_file.close();
		RGL::RLLogger::getInstance()->info("Successfully wrote reflection data to {}", output_path);

	}else if (outputType == "cpp") {
        // 生成C++代码
		const std::filesystem::path outputfile{vm["output"].as<std::string>()};
		const std::filesystem::path outputFolder = outputfile.parent_path();
		const std::filesystem::path spirvPath {vm["input"].as<std::string>()};
		const std::filesystem::path templateDir{vm["template-dir"].as<std::string>()};
		const auto shaderName = spirvPath.filename().stem();

		RGL::RLLogger::getInstance()->info("Reflecting spir-v to cpp, folder is {}.",outputFolder.generic_string());

        RGL::reflection::Template2Struct tempalte2Struct(result_json,outputFolder,shaderName,templateDir);

		tempalte2Struct.generate();

    }else {
		throw std::runtime_error("Unknown output type");
	}

	// 4. 输出结果

    } catch (const po::error& e) {
	RGL::RLLogger::getInstance()->error("Error: {}\nUse --help for a list of options.",e.what());

	return 1;
    } catch (const std::exception& e) {
	RGL::RLLogger::getInstance()->error("An error occurred: {}",e.what());
	return 1;
    } catch (...) {
	// 捕获所有未知异常
	RGL::RLLogger::getInstance()->error("An unknown error occurred.");

	return 1;
    }

    return 0;
}
