#pragma once
#include <memory>
#include "Camera.hpp"

namespace RGL {

	enum class API_TYPE
	{
		OPENGL46,
		CPU,
		VULKAN13
	};
	class Renderer {
	public:
		Renderer() = default;
		virtual void operator()() = 0;

		virtual ~Renderer() = default;
	};

	class RendererContext {
	protected:
		API_TYPE api_type;
		std::unique_ptr<Renderer> renderer;

	public:
		RendererContext() :renderer(nullptr),api_type(API_TYPE::OPENGL46){}
		virtual void render() = 0;
		virtual void setRenderer(std::unique_ptr<Renderer> renderer) {
			this->renderer = std::move(renderer);
		}
		inline API_TYPE getAPI() {
			return api_type;
		}

		virtual ~RendererContext() = default;
	};

}
