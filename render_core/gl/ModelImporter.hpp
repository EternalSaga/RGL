#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>
#include "EnTTRelationship.hpp"
#include "EnttRelationship.hpp"
#include "GLObj.hpp"
#include <cstddef>
#include <filesystem>
#include <glm/fwd.hpp>
#include <memory>
#include <rllogger.hpp>
#include "Entity.hpp"
#include "RenderQueue.hpp"
#include "ShaderManager.hpp"
#include "UBO.hpp"
#include "VertexDescriptor.hpp"
#include "Mesh.hpp"
#include <map>
#include <stdexcept>
#include <utility>
#include "Material.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>
#include "Shader.hpp"
namespace RGL {
namespace io {
using namespace glcore;
namespace fs = std::filesystem;

Transform decomposeTransform(const aiMatrix4x4& transform);

class ModelImporter : public SingleReg {
    Assimp::Importer importer;	// importer管理了所有读取数据的生命周期

    const aiScene* scene;

    const aiScene* loadModel(const fs::path& path);
    std::tuple<std::shared_ptr<MaterialData>,PBRComponent> processMaterial(size_t assimpID);
    TextureCache textureCache;
    fs::path modelRootPath;
    std::unique_ptr<Mesh> processMesh(aiMesh* importedMesh);

    RLLogger* logger;
    std::map<aiNode*, entt::entity> nodeMap;  // 一个node对应一个entity

    // 辅助函数，将aiMatrix4x4转换为glm::mat4
    glm::mat4 aiMatrix4x4ToGlm(const aiMatrix4x4& from);
    
    // 递归函数，用于遍历节点树并合并网格
    void mergeNodeDFS(Mesh& outMesh);

   public:
    ModelImporter(const fs::path& path);
    ~ModelImporter() = default;

    
    size_t getNodeCount() const;

    void processNodeBFS(ShaderRef shader);

    void addUbos(UBOs ubos);

    std::unique_ptr<Mesh> importAsSingleMesh();
    
    //默认添加的RenderTag是Opaque，在主循环中会被加入到opaqueQueue中，使用本方法可以添加其他Tag并加入到对应的Queue中，
    // 并移除默认的Opaque Tag

    template<typename T>
    void attachComponentAndRemoveDefaultTag(const T& component){
        if (nodeMap.empty()){
            this->logger->error("No nodes to attach components");
            throw std::runtime_error("No nodes to attach components");
        }
        for(auto [key,value]: nodeMap) {
            singleReg->remove<RenderTags::Opaque>(value);
            singleReg->emplace<T>(value,component);
        }
    }

};
}  // namespace io
}  // namespace RGL
