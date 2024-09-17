#pragma once
#include <cstddef>
#include <glm/ext/matrix_transform.hpp>
#include <glm/fwd.hpp>
#include <glm/glm.hpp>
#include <boost/uuid/uuid.hpp>
#include <map>
#include <memory>
#include <string>
#include "GLObj.hpp"
#include "Light.hpp"
#include "Mesh.hpp"
#include "Material.hpp"
#include "Shader.hpp"
namespace RGL {
namespace glcore {

class Entity {
    glm::vec3 position;

    float angleX;
    float angleY;
    float angleZ;
    glm::vec3 scale;
    boost::uuids::uuid uuid;
    std::string name;
    std::unique_ptr<Mesh> mesh;
    std::unique_ptr<Material> material;
    std::shared_ptr<Shader> shader;
    VAOCreater vaoCreator;

    std::unique_ptr<VAO> vao;
    size_t mvertCount;
    size_t midxOffset;

   protected:
    virtual void setShaderUniforms(Shader* shader) {
    }

   public:
    Entity(glm::vec3 position, float angleX, float angleY, float angleZ, glm::vec3 scale, std::shared_ptr<Shader> shader, std::string name);
    void rotateX(float angle);
    void rotateY(float angle);
    void rotateZ(float angle);

    void setPosition(const glm::vec3& position) {
	this->position = position;
    }
    void setScale(const glm::vec3& scale);
    boost::uuids::uuid getUUID() {
	return uuid;
    }

    void setMesh(std::unique_ptr<Mesh> mesh) {
	this->mesh = std::move(mesh);
    }
    void setMaterial(std::unique_ptr<Material> material) {
	this->material = std::move(material);
    }

    Mesh* getMesh() {
	return mesh.get();
    }
    // pitch yaw roll
    glm::mat4 getModelMatrix();

    void draw();
};

class SceneManager {
   private:
    std::map<boost::uuids::uuid, std::unique_ptr<Entity>> entities;
    std::map<boost::uuids::uuid, std::unique_ptr<Light>> lights;

    VAOCreater vaoCreator;
    std::shared_ptr<Shader> shader;

   public:
    SceneManager(std::shared_ptr<Shader> shader) : vaoCreator(shader), shader(shader) {
    }
    void addEntity(std::unique_ptr<Entity> entity) {
	entities[entity->getUUID()] = std::move(entity);
    }

    void removeEntity(const boost::uuids::uuid& uuid) {
	entities.erase(uuid);
    }
    void addLight(std::unique_ptr<Light> light) {
	lights[light->getUUID()] = std::move(light);
    }

    void drawALL() {
	for (auto& light : lights) {
	    light.second->setShaderUniforms(shader.get());
	}
	for (auto& entity : entities) {
	    entity.second->draw();
	}
    }
};

}  // namespace glcore
}  // namespace RGL
