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
#include <stdexcept>
#include <utility>
#include <vector>
#include "RenderQueue.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>
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
	logger->trace("Model node local transform for entity {}, mat4 \na1:{},a2:{},a3:{},a4:{}\nb1:{},b2:{},b3:{}b4:{}\nc1:{},c2:{},c3:{},c4:{}\nd1:{},d2:{},d3:{},d4:{}",
		(entt::to_integral(entity)),
node->mTransformation.a1,
	node->mTransformation.a2,
	node->mTransformation.a3,
	node->mTransformation.a4,
	node->mTransformation.b1,
	node->mTransformation.b2,
	node->mTransformation.b3,
	node->mTransformation.b4,
		node->mTransformation.c1,
	node->mTransformation.c2,
	node->mTransformation.c3,
	node->mTransformation.c4,
		node->mTransformation.d1,
	node->mTransformation.d2,
	node->mTransformation.d3,
	node->mTransformation.d4
	);
    }
#ifndef NDEBUG
    for (auto& [node, entity] : nodeMap) {
	logger->log_if(spdlog::level::debug, nullptr == singleReg->try_get<VertArrayComponent>(entity), "Entity {} not have VertArrayComponent", entt::to_integral(entity));
	logger->log_if(spdlog::level::debug, nullptr == singleReg->try_get<DiscreteUniforms>(entity), "Entity {} not have DiscreteUniforms,perhaps it is has no Textures", entt::to_integral(entity));
	// 一般来说，在这里没有UBOs是符合预期的，因为addUbos需要手动之后调用
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
		float opacity = 1.0f;
	    if (material->Get(AI_MATKEY_OPACITY, opacity) != aiReturn_SUCCESS) [[unlikely]] {
		logger->error("Failed to get opacity from material with ID: {}", assimpID);
	    }

		if( opacity < 0.999f) {
		    isTransparent = true;
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

    glm::mat4 glmTransform(
        transform.a1, transform.b1, transform.c1, transform.d1, // Col 1
        transform.a2, transform.b2, transform.c2, transform.d2, // Col 2
        transform.a3, transform.b3, transform.c3, transform.d3, // Col 3
        transform.a4, transform.b4, transform.c4, transform.d4  // Col 4
    );

	 if (!glm::decompose(glmTransform, scale, quaternion, position, skew, perspective)) {
        // 如果分解失败 (例如，矩阵是奇异的或包含非仿射变换，如纯投影)
        auto logger = RLLogger::getInstance();
        logger->error("glm::decompose failed for matrix!");
        throw std::invalid_argument("glm::decompose failed for matrix!");
    }

    glm::vec3 eulerAngleRad = glm::eulerAngles(quaternion);

    auto logger = RLLogger::getInstance();
    logger->trace("Input aiMatrix4x4: a4={}, b4={}, c4={}", transform.a4, transform.b4, transform.c4); // 打印原始位移部分
    logger->trace("Decomposed: Position: {}, Euler Angle (Rad): {}, Scale: {}", glm::to_string(position), glm::to_string(eulerAngleRad), glm::to_string(scale));

    // Transform 类期望角度
    return Transform(position, glm::degrees(eulerAngleRad), scale);

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
