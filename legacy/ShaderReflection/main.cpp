#include <fmt/format.h>
#include "samplerException.hpp"
#include "shaderReflection.hpp"
#include <boost/program_options.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/value_semantic.hpp>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include "json2cpp.hpp"
#include "rllogger.hpp"
#include "assimpMaterialBinding.hpp"
int main(int argc, char* argv[]) {
    namespace po = boost::program_options;
    using json = nlohmann::json;
    using namespace RGL::reflection;
    try {
	po::options_description desc("SPIR-V Reflector Options");
	desc.add_options()("help,h", "produce help message")("input,i", po::value<std::string>(), "input SPIR-V file")("type,t", po::value<std::string>()->required(), "output type, cpp or material_binding")("template-dir,d", po::value<std::string>(), "template directory which contains jinja templates for cpp output, required for cpp output type, ignored for json output")("output,o", po::value<std::string>(), "output file name, doesn't include any kind of file extension")("config-file,c", po::value<std::string>()->required(), "config file path for reflection config")("sampler_rule,s", po::value<std::string>(), "sampler rules file path, which used to check shader samplers if they are valid for the given rules.")("vertex_rule,v", po::value<std::string>(), "vertex rules file path, which used to check shader vertex inputs if they are valid for the given rules.")("material_binding_output_dir,m", po::value<std::string>(), "material binding output dir, which is used to generate shader sampler index to assimp material binding mapping")("material_binding_template,mbt", po::value<std::string>(), "material binding template jinja path");
	// 一些碎碎念，在unix命令行解析规范中，-h 是短选项，--help 是长选项，注意不要写-help，会把elp当成参数传给-h
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
	

	

	// 创建 ShaderReflection 实例，这会触发反射过程

	// 通过类型转换操作符获取JSON对象

	std::string outputType = vm["type"].as<std::string>();

	// 在这里对各种output type所需命令行参数进行检查
	if (outputType == "cpp") {
	    if (!vm.count("output") || !vm.count("template-dir") || !vm.count("input") || !vm.count("sampler_rule") || !vm.count("vertex_rule")) {
		throw std::runtime_error("Missing required arguments for json/cpp output type. Please check command line options.");
	    }
	} else if (outputType == "material_binding") {
	    if (!vm.count("material_binding_output_dir") || !vm.count("material_binding_template") || !vm.count("sampler_rule")) {
		throw std::runtime_error("Missing required arguments for material_binding output type. Please check command line options.");
	    }
	} else {
	    throw std::runtime_error("Invalid output type. Please use 'cpp', or 'material_binding'.");
	}

	if (outputType == "cpp") {
		std::string input_path = vm["input"].as<std::string>();
		RGL::RLLogger::getInstance()->info("Reflecting shader: {}...", input_path);
	    ShaderReflection reflection(input_path, vm["sampler_rule"].as<std::string>(), vm["vertex_rule"].as<std::string>());
	    json result_json = static_cast<json>(reflection);
	    const std::filesystem::path outputfile{vm["output"].as<std::string>()};
	    const std::filesystem::path outputFolder = outputfile.parent_path();
	    const std::filesystem::path spirvPath{vm["input"].as<std::string>()};
	    const std::filesystem::path templateDir{vm["template-dir"].as<std::string>()};
	    const auto shaderName = spirvPath.filename().stem();

	    RGL::RLLogger::getInstance()->info("Reflecting spir-v to cpp, folder is {}.", outputFolder.generic_string());

	    RGL::reflection::Template2Struct tempalte2Struct(result_json, outputFolder, shaderName, templateDir);

	    tempalte2Struct.generate();

	} else if (outputType == "material_binding") {
	    // Generate Assimp Material Binding
	    std::filesystem::path bindOutputPath = vm["material_binding_output_dir"].as<std::string>();
	    std::filesystem::path bindTemplatePath = vm["material_binding_template"].as<std::string>();
	    std::filesystem::path samplerRulePath = vm["sampler_rule"].as<std::string>();

	    RGL::RLLogger::getInstance()->info("Generating Assimp material binding to {}...", bindOutputPath.generic_string());
	    AssimpMaterialBinding bindingGenerator(bindOutputPath, samplerRulePath, bindTemplatePath);
	}

    } catch (const po::error& e) {
	RGL::RLLogger::getInstance()->error("Error: {}\nUse --help for a list of options.", e.what());

	return 1;
    } catch (const SamplerException& e) {
	RGL::RLLogger::getInstance()->error("Invalid Sampler: {}", e.what());
	return 1;
    } catch (const VertexException& e) {
	RGL::RLLogger::getInstance()->error("Invalid Vertex Config: {}", e.what());
	return 1;
    } catch (const std::exception& e) {
	RGL::RLLogger::getInstance()->error("An error occurred: {}", e.what());
	return 1;
    } catch (...) {
	// 捕获所有未知异常
	RGL::RLLogger::getInstance()->error("An unknown error occurred.");

	return 1;
    }

    return 0;
}
