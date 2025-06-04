#include "ModelImporter.hpp"
#include <spdlog/common.h>
#include "EnTTRelationship.hpp"
#include "GLTextures.hpp"
#include "Material.hpp"
#include "Mesh.hpp"
#include "ShaderManager.hpp"
#include "rllogger.hpp"
#include <entt/entity/entity.hpp>
#include <filesystem>
#include <memory>
#include <queue>
#include <utility>
#include <vector>
#include "RenderQueue.hpp"
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

	processMaterial(material, meshObj);
    }

    return std::move(meshObj);
}

void ModelImporter::processNodeBFS(ShaderRef shader) {
    std::queue<aiNode*> nodeQueue;

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

	// 处理当前节点的子节点，所有的子节点都创建一个entity，并设置成relationship的child
	for (size_t i = 0; i < currentNode->mNumChildren; i++) {
	    auto childNode = currentNode->mChildren[i];
	    entt::entity childEntity = singleReg->create();
	    nodeMap[childNode] = childEntity;
	    nodeQueue.push(childNode);
	    currentRel.children.push_back(childEntity);
	}

	singleReg->emplace<Relationship>(currentEntity, currentRel);
	singleReg->emplace<ShaderRef>(currentEntity, shader);

	singleReg->emplace<DiscreteUniforms>(currentEntity);

	// 处理节点自身mesh
	for (size_t i = 0; i < currentNode->mNumMeshes; i++) {
	    auto meshIndex = currentNode->mMeshes[i];
	    auto mesh = scene->mMeshes[meshIndex];
	    auto meshObj = processMesh(mesh);

	    // 检查mesh的material透明度
	    if (meshObj->getMaterial()->getTransparent()) {
		singleReg->emplace<RenderTags::Transparent>(currentEntity);
	    }else {
		singleReg->emplace<RenderTags::Opaque>(currentEntity);
	    }
		singleReg->emplace<RenderTags::Renderable>(currentEntity, true);
		auto vertArrayComp = VAOCreater::createMeshVAO(*meshObj, *shader);
	    auto [vertCount, idxOffset] = meshObj->getIdicesCountAndOffset();
	    singleReg->emplace<VertArrayComponent>(currentEntity, std::move(vertArrayComp), vertCount, idxOffset);

	    const auto sampler = SamplerCreater::createSamplers(*meshObj, *shader);

	    singleReg->emplace<SamplerCreater::Samplers>(currentEntity, sampler);
	}
    }

    logger->debug("Count of nodes processed: {}", nodeMap.size());
    logger->debug("Count of meshes in root node: {}", rootNode->mNumMeshes);
    logger->debug("Model path: {}", this->modelRootPath.string());

    for (auto& [node, entity] : nodeMap) {
	singleReg->emplace<Transform>(entity, decomposeTransform(node->mTransformation));
    }
#ifndef NDEBUG
    for (auto& [node, entity] : nodeMap) {
	logger->log_if(spdlog::level::debug, nullptr == singleReg->try_get<VertArrayComponent>(entity), "Entity {} not have VertArrayComponent", entt::to_integral(entity));
	logger->log_if(spdlog::level::debug, nullptr == singleReg->try_get<DiscreteUniforms>(entity), "Entity {} not have DiscreteUniforms,perhaps it is has no Textures", entt::to_integral(entity));
	logger->log_if(spdlog::level::debug, nullptr == singleReg->try_get<UBOs>(entity), "Entity {} not have UBOs", entt::to_integral(entity));
	logger->log_if(spdlog::level::debug, nullptr == singleReg->try_get<ShaderRef>(entity), "Entity {} not have ShaderRef", entt::to_integral(entity));
	logger->log_if(spdlog::level::debug, nullptr == singleReg->try_get<Relationship>(entity), "Entity {} not have Relationship", entt::to_integral(entity));
	logger->log_if(spdlog::level::debug, nullptr == singleReg->try_get<Transform>(entity), "Entity {} not have Transform", entt::to_integral(entity));
    }
#endif
}
const aiScene* ModelImporter::loadModel(const fs::path& path) {
    const auto scene = importer.ReadFile(path.string(), aiProcess_Triangulate | aiProcess_GenNormals);
    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
	logger->error("Failed to load model: {}, current work path is {}", path.string(), fs::current_path().string());
	throw std::runtime_error("Failed to load model: " + path.string());
    }
    return scene;
}
void ModelImporter::processMaterial(size_t assimpID, std::unique_ptr<Mesh>& mesh) {
    aiMaterial* material = scene->mMaterials[assimpID];

    // 确定assimpid_materials里没有这个id,
    if (assimpid_materials.find(assimpID) != assimpid_materials.end()) {
	mesh->setMaterial(assimpid_materials[assimpID]);
	return;
    } else {
	std::shared_ptr<MaterialData> materialData = std::make_shared<MaterialData>();
	{
	    bool isTransparent = false;
	    if (material->Get(AI_MATKEY_OPACITY, isTransparent) != aiReturn_SUCCESS) [[unlikely]] {
		logger->error("Failed to get opacity from material with ID: {}", assimpID);
	    }
	    materialData->setTransparent(isTransparent);
	}

	auto appendTexture = [this, &mesh, &materialData](TextureUsageType type, aiMaterial* material) {
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
		fs::path texturePathStr = this->modelRootPath / fs::path((texturePath.C_Str()));  // 来自于assimp的texture路径很可能是相对路径。而相对路径和进程的当前路径可能往往不一样,所以需要拼接模型根路径和纹理路径。这里假设纹理文件和模型文件在同一目录下，或者在子目录中。如果不在同一目录下，可能需要额外的配置来指定纹理文件的位置。
		auto texture = textureCache.getTexture(texturePathStr, type);
		if (texture) {
		    materialData->appendTexture(texture);
		}
	    }
	};
	appendTexture(TextureUsageType::DIFFUSE, material);
	appendTexture(TextureUsageType::NORMAL, material);
	appendTexture(TextureUsageType::SPECULAR, material);
	appendTexture(TextureUsageType::AMBIENT, material);
	mesh->setMaterial(materialData);
    }
}
size_t ModelImporter::getNodeCount() const {
    return scene->mRootNode->mNumChildren;
}
ModelImporter::ModelImporter(const fs::path& path) : importer{}, scene(loadModel(path)), modelRootPath(path.parent_path()) {
    this->logger = RLLogger::getInstance();
}

glcore::Transform decomposeTransform(const aiMatrix4x4& transform) {
    glm::quat quaternion;
    glm::vec3 position;
    glm::vec3 skew;
    glm::vec4 perspective;
    glm::vec3 scale;

    glm::decompose(glm::make_mat4(&transform.a1), scale, quaternion, position, skew, perspective);

    glm::mat4 rotation = glm::toMat4(quaternion);
    glm::vec3 eulerAngle;
    glm::extractEulerAngleXYZ(rotation, eulerAngle.x, eulerAngle.y, eulerAngle.z);

    return Transform(position, eulerAngle, scale);
}
void ModelImporter::addUbos(UBOs ubos) {
    if (this->nodeMap.size() == 0) {
	logger->error("entity map is empty");
	throw std::logic_error("entity map is empty");
    }

    for (auto [node, entity] : nodeMap) {
	singleReg->emplace_or_replace<UBOs>(entity, ubos);
    }
}
}  // namespace io
}  // namespace RGL
