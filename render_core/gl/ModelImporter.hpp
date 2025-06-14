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
   public:
    ModelImporter(const fs::path& path);
    ~ModelImporter() = default;

    
    size_t getNodeCount() const;

    void processNodeBFS(ShaderRef shader);

    void addUbos(UBOs ubos);
};
}  // namespace io
}  // namespace RGL
