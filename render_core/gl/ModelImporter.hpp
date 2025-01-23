#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "EnttRelationship.hpp"
#include "GLObj.hpp"
#include <cstddef>
#include <filesystem>
#include <memory>
#include <rllogger.hpp>
#include "Entity.hpp"
#include "VertexDescriptor.hpp"
#include "Mesh.hpp"
#include <map>
#include <utility>
#include "Material.hpp"

namespace RGL {
namespace io {
using namespace glcore;
namespace fs = std::filesystem;
class ModelImporter : public SingleReg {
    Assimp::Importer importer;	// importer管理了所有读取数据的生命周期

	const aiScene* scene;
	std::map<size_t, std::shared_ptr<MaterialData>> assimpid_materials;
	const aiScene* loadModel(const fs::path& path);
	void processMaterial(size_t assimpID,std::unique_ptr<Mesh>& mesh);
    TextureCache textureCache;
       public:
	ModelImporter(const fs::path &path) : importer{} ,scene(loadModel(path)){
    }
    ~ModelImporter() = default;
    

    std::unique_ptr<Mesh> processMesh(aiMesh* importedMesh);

    void processNodeBFS(const aiScene* scene);
};
}  // namespace io
}  // namespace RGL
