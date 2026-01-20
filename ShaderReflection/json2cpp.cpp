#include "json2cpp.hpp"
#include "rllogger.hpp"
#include <fstream>
#include "injautils.hpp"

namespace RGL {
namespace reflection {


std::string title_case(const std::string& input) {
    if (input.empty()) return "";
    std::string result = input;
    result[0] = std::toupper(result[0]);
    // 如果需要更复杂的逻辑（如单词首字母），可以在此处扩展
    return result;
}

Template2Struct::Template2Struct(const nlohmann::json& j, const std::filesystem::path& outPutFolder,const std::filesystem::path& shaderName,const std::filesystem::path& templatePath):shaderJson(j),outPutFolder(outPutFolder),shaderName(shaderName){
    tempalteHpp = templatePath/"shader_template.hpp.jinja";
    templateCpp = templatePath/"shader_template.cpp.jinja";

    env.add_callback("glsl_type_to_cpp", [](std::vector<const nlohmann::json*>& args) -> nlohmann::json {
        if (args.empty()) {
            return nlohmann::json("");
        }
        
        std::string glslType = args[0]->get<std::string>();
        auto it = GLSL_TYPE_TO_CPP.find(glslType);
        if (it != GLSL_TYPE_TO_CPP.end()) {
            return nlohmann::json(it->second);
        }
        // If type not found, return the original type
        return nlohmann::json(glslType);
    });

    env.add_callback("title", 1, [](inja::Arguments& args) {
    std::string s = args.at(0)->get<std::string>();
    return title_case(s);
});
}

void Template2Struct::generate(){
    try {
        const auto hppPath = outPutFolder/(shaderName.filename().string() + ".hpp");
        const auto cppPath = outPutFolder/(shaderName.filename().string() + ".cpp");
        
        auto logger = RGL::RLLogger::getInstance();

        // Add current timestamp to the JSON data
        nlohmann::json dataWithTimestamp = shaderJson;
        dataWithTimestamp["generation_time"] = getCurrentTimestamp();

        // Render HPP file
        try {
            auto hppresult = env.render_file(tempalteHpp.generic_string(), dataWithTimestamp);
            logger->debug("hpp result:\n{}",hppresult);

            std::ofstream hppFile(hppPath);
            if (!hppFile.is_open()) {
                logger->error("Failed to open hpp file for writing: {}", hppPath.generic_string());
                throw std::runtime_error("Cannot open hpp file: " + hppPath.generic_string());
            }
            hppFile << hppresult;
            if (!hppFile.good()) {
                logger->error("Error writing to hpp file: {}", hppPath.generic_string());
                throw std::runtime_error("Error writing to hpp file: " + hppPath.generic_string());
            }
            logger->info("Successfully generated hpp file: {}", hppPath.generic_string());
        } catch (const std::exception& e) {
            logger->error("Exception while rendering/writing hpp file: {}", e.what());
            throw;
        }

        // Render CPP file
        try {
            auto cppresult = env.render_file(templateCpp.generic_string(), dataWithTimestamp);
            logger->debug("cpp result:\n{}",cppresult);

            std::ofstream cppFile(cppPath);
            if (!cppFile.is_open()) {
                logger->error("Failed to open cpp file for writing: {}", cppPath.generic_string());
                throw std::runtime_error("Cannot open cpp file: " + cppPath.generic_string());
            }
            cppFile << cppresult;
            if (!cppFile.good()) {
                logger->error("Error writing to cpp file: {}", cppPath.generic_string());
                throw std::runtime_error("Error writing to cpp file: " + cppPath.generic_string());
            }
            logger->info("Successfully generated cpp file: {}", cppPath.generic_string());
        } catch (const std::exception& e) {
            logger->error("Exception while rendering/writing cpp file: {}", e.what());
            throw;
        }

        logger->info("Code generation completed successfully for shader: {}", shaderName.generic_string());
    } catch (const std::exception& e) {
        auto logger = RGL::RLLogger::getInstance();
        logger->error("Code generation failed: {}", e.what());
        throw;
    } catch (...) {
        auto logger = RGL::RLLogger::getInstance();
        logger->error("Unknown error occurred during code generation");
        throw;
    }
}


}  // namespace reflection
}  // namespace RGL