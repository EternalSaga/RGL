#include "ModelImporter.hpp"
#include "EnTTRelationship.hpp"
#include "GLTextures.hpp"
#include "Material.hpp"
#include <memory>
#include <queue>
#include <utility>
#include <vector>

namespace RGL {
namespace io {

std::unique_ptr<Mesh> ModelImporter::processMesh(aiMesh* importedMesh) {
    const auto& numofVertices = importedMesh->mNumVertices;
    auto meshObj = std::make_unique<Mesh>(
	FloatDescs{FloatDesc{"inPos", 3}, FloatDesc{"inUV", 2}, FloatDesc{"inNormal", 3}}, numofVertices);
    // 处理顶点
    for (unsigned int j = 0; j < numofVertices; j++) {
	// 顶点位置
	glm::vec3 pos{importedMesh->mVertices[j].x, importedMesh->mVertices[j].y, importedMesh->mVertices[j].z};
	// 顶点法线
	if (!importedMesh->HasNormals()) {
	    auto logger = RLLogger::getInstance();
	    logger->error("This mesh has no normals");
	    throw std::invalid_argument("This mesh has no normals");
	}
	glm::vec3 normal{importedMesh->mNormals[j].x, importedMesh->mNormals[j].y, importedMesh->mNormals[j].z};
	// 顶点纹理坐标(UV)
	glm::vec2 texCoord{0.0f, 0.0f};
	if (importedMesh->mTextureCoords[0]) {	// 如果模型的第一个纹理坐标不为空，把它当成纹理坐标，有的时候，模型有多套纹理坐标，也可能是空的
	    texCoord.x = importedMesh->mTextureCoords[0][j].x;
	    texCoord.y = importedMesh->mTextureCoords[0][j].y;
	}
	std::vector<GLfloat> vertex{pos.x, pos.y, pos.z, texCoord.x, texCoord.y, normal.x, normal.y, normal.z};
	meshObj->pushVertex(vertex);
    }
    // 处理索引
    for (unsigned int j = 0; j < importedMesh->mNumFaces; j++) {
	auto face = importedMesh->mFaces[j];
	assert(face.mNumIndices == 3);
	for (size_t k = 0; k < face.mNumIndices; k++) {
	    meshObj->pushIndex(face.mIndices[k]);
	}
    }

	// 处理材质
    if (importedMesh->mMaterialIndex >= 0) {
		auto material = importedMesh->mMaterialIndex;

		processMaterial(material);
    }

    return std::move(meshObj);
}
void ModelImporter::processNodeBFS(const aiScene* scene) {
    std::queue<aiNode*> nodeQueue;
    std::map<aiNode*, entt::entity> nodeMap;

    auto rootNode = scene->mRootNode;

    // 先处理下rootNode
    Relationship rootRel;
    rootRel.parent = entt::null;
    rootRel.children = std::vector<entt::entity>{};

    entt::entity rootEntity = singleReg->create();
    nodeMap[rootNode] = rootEntity;
    nodeQueue.push(rootNode);

    while (!nodeQueue.empty()) {
	auto currentNode = nodeQueue.front();
	nodeQueue.pop();

	auto currentEntity = nodeMap[currentNode];
	Relationship currentRel;

	if (currentNode->mParent != nullptr) [[likely]] {
	    currentRel.parent = nodeMap[currentNode->mParent];
	} else [[unlikely]] {
	    currentRel.parent = entt::null;
	}

	// 处理子节点，关联entity和node
	for (size_t i = 0; i < currentNode->mNumChildren; i++) {
	    auto childNode = currentNode->mChildren[i];
	    entt::entity childEntity = singleReg->create();
	    nodeMap[childNode] = childEntity;
	    nodeQueue.push(childNode);
	    currentRel.children.push_back(childEntity);
	}

	singleReg->emplace<Relationship>(currentEntity, currentRel);

	// 处理节点自身mesh
	for (size_t i = 0; i < currentNode->mNumMeshes; i++) {
	    auto meshIndex = currentNode->mMeshes[i];
	    auto mesh = scene->mMeshes[meshIndex];
	    auto meshObj = processMesh(mesh);
	    singleReg->emplace<std::unique_ptr<Mesh>>(currentEntity, std::move(meshObj));
	}
    }
}
const aiScene* ModelImporter::loadModel(const fs::path& path) {
    const auto scene = importer.ReadFile(path.string(), aiProcess_Triangulate | aiProcess_GenNormals);
    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
	auto logger = RLLogger::getInstance();
	logger->error("Failed to load model: {}", path.string());
	throw std::runtime_error("Failed to load model: " + path.string());
    }
    return scene;
}
void ModelImporter::processMaterial(size_t assimpID,std::unique_ptr<Mesh>& mesh) {

	aiMaterial *material = scene->mMaterials[assimpID];

    // 确定assimpid_materials里没有这个id,
    if (assimpid_materials.find(assimpID) != assimpid_materials.end()) {
		mesh->setMaterial(assimpid_materials[assimpID]);
		return;
    }else {
	 	std::shared_ptr<MaterialData> materialData = std::make_shared<MaterialData>();
		auto appendTexture = [this,&mesh,&materialData](TextureUsageType type,aiMaterial *material) {
		  aiTextureType textureType = aiTextureType_NONE;
		  switch (type) {
		    case TextureUsageType::DIFFUSE:
		      textureType = aiTextureType_DIFFUSE;
		      break;
		    case TextureUsageType::NORMAL:
		      textureType = aiTextureType_NORMALS;
		      break;
			case TextureUsageType::SPECULAR:
		      textureType = aiTextureType_SPECULAR;
		      break;
			case TextureUsageType::AMBIENT:
		      textureType = aiTextureType_AMBIENT;
		      break;
		  }
			const int textureCount = material->GetTextureCount(textureType);
			for (int i = 0; i < textureCount; i++) {
				aiString texturePath;
				material->GetTexture(textureType, i, &texturePath);
				fs::path texturePathStr(texturePath.C_Str());
				auto texture = textureCache.getTexture(texturePathStr, type);
				if (texture) {
					materialData->appendTexture(texture);
				}
			}
		};

		appendTexture(TextureUsageType::DIFFUSE,material);
		appendTexture(TextureUsageType::NORMAL,material);
		appendTexture(TextureUsageType::SPECULAR,material);
		appendTexture(TextureUsageType::AMBIENT,material);
		mesh->setMaterial(materialData);
	}
}
}  // namespace io
}  // namespace RGL
