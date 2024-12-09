#include <unordered_map>
#include <entt/entt.hpp>
#include <mutex>
#include <string>
namespace RGL {

	class SharingData {
		std::unordered_map<std::string, entt::any> globalSharingData;
	    static std::once_flag initOnce;
		static SharingData* instance;
		SharingData() {

		}
	   public:
		void setData(const std::string& name, const entt::any& data);
		   entt::any getData(const std::string& name);
		static SharingData* getInstance();
	};

}
