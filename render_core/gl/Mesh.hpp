#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
#include "GLCheckError.hpp"
#include "GLTextures.hpp"
#include "Helpers.hpp"
#include "Shader.hpp"
#include "VertexDescriptor.hpp"

#include "GLObj.hpp"
#include "Material.hpp"
#include "rllogger.hpp"
namespace RGL {
namespace glcore {

class Mesh {
   protected:
    std::vector<int> indices;
    size_t indicesCount;
    std::vector<GLfloat> channeledVertices;
    FloatDescs descs;
    size_t vertLength = 0;
    size_t indicesOffset = 0;

    size_t getVertexLength();


    std::shared_ptr<MaterialData> material;
    PBRComponent pbrComponent;

    bool materialHasSet = false;
    

   public:
    explicit Mesh();
    explicit Mesh(FloatDescs descs,size_t numOfVertcies);
    std::tuple<size_t, size_t> getIdicesCountAndOffset();

    std::vector<int> getIndices() const;
    std::vector<GLfloat> getChanneledVertices() const;
    size_t getIndicesCount() const;
    virtual ~Mesh() = default;
    FloatDescs getDesc() const;
    void pushVertex(const std::vector<GLfloat>& vertex);    
    void pushIndex(const int index);

    void setMaterial(std::shared_ptr<MaterialData> material);

    void setPBRComponent(const PBRComponent& pbrComponent);
    PBRComponent getPBRComponent() const;

    std::shared_ptr<MaterialData> getMaterial() const;
};

namespace VAOCreater {

// 需要使用shader确定vao上的input attributes位置
std::unique_ptr<VAO> createMeshVAO(const Mesh& mesh, const Shader& shader);
std::unique_ptr<VAO> createMeshVAO(std::vector<Mesh> meshes, const Shader& shader);
}  // namespace VAOCreater

namespace SamplerCreater{

    enum class TextureStorageType{
        TEXTURE2D,TEXTURE3D,TEXTURE_CUBE
    };

    struct Sampler{

        std::string samplerName;
        GLint textureUnit;
        std::shared_ptr<Texture> texture;
    };

    using Samplers = std::vector<Sampler>;

    Samplers createSamplers(const Mesh& mesh, const Shader& shader);

    class SamplersScope{
        Samplers& samplers;
        public:
	 SamplersScope(Samplers& samplers);

	 ~SamplersScope();
    };
}

}  // namespace glcore
}  // namespace RGL
