#include "json2cpp.hpp"
#include "rllogger.hpp"
#include <fstream>

namespace RGL {
namespace reflection {


Template2Struct::Template2Struct(const nlohmann::json& j, const std::filesystem::path& outPutFolder,const std::filesystem::path& shaderName):shaderJson(j),outPutFolder(outPutFolder),shaderName(shaderName){

}

void Template2Struct::generate(){
    const auto hppPath = outPutFolder/(shaderName.filename().string() + ".hpp");
    const auto cppPath = outPutFolder/(shaderName.filename().string() + ".cpp");
    
    auto logger = RGL::RLLogger::getInstance();

    auto hppresult = env.render_file(tempalteHpp.generic_string(), shaderJson);

    logger->debug("hpp result:\n{}",hppresult);
    std::ofstream hppFile(hppPath);
    hppFile << hppresult;

    auto cppresult = env.render_file(templateCpp.generic_string(), shaderJson);
    logger->debug("cpp result:\n{}",cppresult);

    std::ofstream cppFile(cppPath);
    cppFile << cppresult;

}


}  // namespace reflection
}  // namespace RGL