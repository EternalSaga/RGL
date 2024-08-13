
module;
#include <memory>
export module apiAbstractor;
namespace RGL {

	export enum class API_TYPE
	{
		OPENGL46,
		CPU,
		VULKAN13,
		DX12
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
		virtual void render() = 0;
		virtual void setRenderer(std::unique_ptr<Renderer>) = 0;
		inline API_TYPE getAPI() {
			return api_type;
		}
		virtual ~RendererContext() = default;
	};

}
