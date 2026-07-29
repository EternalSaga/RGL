#include "DataPipeline.hpp"
namespace RGL {
std::once_flag SharingData::initOnce;
SharingData* SharingData::instance;
void SharingData::setData(const std::string& name, const entt::any& data) {
    globalSharingData[name] = data;
}
entt::any SharingData::getData(const std::string& name) {
    return globalSharingData[name];
}
SharingData* SharingData::getInstance() {
    std::call_once(initOnce, []() {
	instance = new SharingData();
    });
    return instance;
}
}  // namespace RGL
