
module;
#include <memory>
export module apiAbstractor;
namespace RGL {

	export enum class API_TYPE
	{
		OPENGL46,
		CPU,
		VULKAN13
	};
	export class Renderer {
	public:
		Renderer() = default;
		virtual void operator()() = 0;

		virtual ~Renderer() = default;
	};

	export class RendererContext {
	protected:
		API_TYPE api_type;
		
	public:
		RendererContext():api_type(API_TYPE::OPENGL46){}
		virtual void render() = 0;
		//为了保证renderer和context的生命周期，所以把renderer放到子类里
		virtual void setRenderer(std::unique_ptr<Renderer> renderer) = 0;
		inline API_TYPE getAPI() {
			return api_type;
		}
		virtual ~RendererContext() = default;
	};

}
