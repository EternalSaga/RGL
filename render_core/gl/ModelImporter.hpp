#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "EnTTRelationship.hpp"
#include "EnttRelationship.hpp"
#include "GLObj.hpp"
#include <cstddef>
#include <filesystem>
#include <glm/fwd.hpp>
#include <memory>
#include <rllogger.hpp>
#include "Entity.hpp"
#include "VertexDescriptor.hpp"
#include "Mesh.hpp"
#include <map>
#include <utility>
#include "Material.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>
namespace RGL {
namespace io {
using namespace glcore;
namespace fs = std::filesystem;

Transform decomposeTransform(const aiMatrix4x4& transform);

class ModelImporter : public SingleReg {
    Assimp::Importer importer;	// importer管理了所有读取数据的生命周期

    const aiScene* scene;
    std::map<size_t, std::shared_ptr<MaterialData>> assimpid_materials;
    const aiScene* loadModel(const fs::path& path);
    void processMaterial(size_t assimpID, std::unique_ptr<Mesh>& mesh);
    TextureCache textureCache;
    fs::path modelRootPath;
    std::unique_ptr<Mesh> processMesh(aiMesh* importedMesh);
   public:
    ModelImporter(const fs::path& path);
    ~ModelImporter() = default;

    
    size_t getNodeCount() const;

    void processNodeBFS();
};
}  // namespace io
}  // namespace RGL
