module;
#include <glad/glad.h>
#include "Helpers.hpp"
#include <cassert>
#include <vector>
#include <stdexcept>
#include <spdlog/spdlog.h>
export module GLObjWrapper;

import GLCheckError;

namespace RGL {
	namespace glcore {


		//VBO,vertex buffer object��CPU�洢���ڴ�������Դ�һ�����������
		//class VBO{ID,GPU_ADDRESS,SIZE,etc.}��
		//����VBO�Ĳ�������Ҫ�Ȱ󶨣��ٲ������ٽ����
		export class VBO {
			GLuint* vbo;
			GLuint mNumOfVbo;
			spdlog::logger* logger;
			VBO(GLuint numOfVbo) :mNumOfVbo(numOfVbo) {
				//genBufferû�з����Դ�,�����Ǵ���vbo
				vbo = new GLuint[mNumOfVbo];
				logger = Logger::getInstance();
				glCall(glGenBuffers, mNumOfVbo, vbo);

				for (size_t i = 0; i < mNumOfVbo; i++)
				{
					glCall(glBindBuffer, GL_ARRAY_BUFFER, vbo[i]);
					if (!glIsBuffer(vbo[i])) {
						logger->error("Index {} is not a valid vbo object.", vbo[i]);
					}
				}
			}
		public:

			//����Ϊ1�ķ�װ
			VBO() :VBO(1) {
			}
			~VBO()
			{
				glCall(glDeleteBuffers, mNumOfVbo, vbo);
				delete[] vbo;
			}
			//����index��ȡvbo
			GLuint operator[](GLuint i) {
				assert(i < mNumOfVbo);
				return vbo[i];
			}
			//������Ϊ1ʱֱ�ӷ���vbo
			GLuint operator()() {
				assert(mNumOfVbo == 1);
				return vbo[0];
			}

			//������Ϊ1ʱҲ����ֱ�Ӻ�vbo GLuint��ʽת��
			operator GLuint() const {
				assert(mNumOfVbo == 1);
				return vbo[0];
			}

			//ʹ��glBufferData����GPU�������ݣ����ٿռ䡣
			//ÿ�ε��ö��Ὺ���Դ棬��ҪƵ������
			//����ǰ�Ȱ�
			void setData(GLuint vboIdx, const std::vector<float>& data) {
				assert(vboIdx < mNumOfVbo);
				glCall(glBindBuffer, GL_ARRAY_BUFFER, vbo[vboIdx]);

				if (!glIsBuffer(vbo[vboIdx])) {
					logger->error("Index {} is not a valid vbo object.", vboIdx);
				}

				//���һ�����������������ûʲô�仯���ṩ�����Ż����ԣ�����һ����DYNAMIC_DRAW
				glCall(glBufferData, GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_STATIC_DRAW);
			}
			//����Ϊ1ʱ����һ�����ݸ�GPU
			void setData(const std::vector<float>& data) {
				assert(mNumOfVbo == 1);
				setData(0, data);
			}
		};


		//VBO�����Ͼ���һ������+������������������Щ������Ӷ�������������㣬��ɫ���ȵȣ�����GPU������ݵ����ã���ҪVAO
		// ���壺���ڴ���һ��Mesh�������еĶ�������������Ϣ
		//�Զ���Ϊ��
		//size->ÿ������3������
		//type->������������float
		//stride->ƫ������ÿ��������ռ��С3*sizeof(float)�������interleaved�����㣬��ɫ�����㣬��ɫ�����Ͻ��棩���ݣ�������ݱȽ���Ҫ
		//����������Ϣ�����ߣ�uv�ȵ�
		//VBO��������Щ���ݣ�VAOҪ����Щ���ݽ�������
		/*
			struct VAO_ELEMENT {
				float position[3];
				float color[3];
				float normal[3];
				float uv[3];
			};
		*/

		//VAO = vbo descriptor array object
		export class VAO {
			GLuint* vao;
			//����vao��position����ʼ��Ϊ0��ÿ�ε���set����1
			GLuint* positions;
			GLuint mNumOfVao;
			VAO(size_t numOfVao) :mNumOfVao(numOfVao) {
				vao = new GLuint[mNumOfVao];
				glGenVertexArrays(mNumOfVao, vao);
				logger = Logger::getInstance();
				positions = new GLuint[mNumOfVao];
				for (size_t i = 0; i < mNumOfVao; i++)
				{
					positions[i] = 0;
				}
			}
			spdlog::logger* logger;
		public:

			VAO() :VAO(1) {}
			~VAO() {
				delete[] vao;
				delete[] positions;
			}

			//����vaoIdxλ���ϵ����ԣ�ͬʱ����󶨵�����vbo
			//��Ϊһ��vao�ж������λ��������Ҫ�趨����λ��index��vaoPos��
			//��������Ӧsingle buffer״̬��һ��VBO��Ӧһ�����ԣ�
			void set(GLuint vaoIdx, GLuint vbo, GLuint numOfFloat) {
				assert(vaoIdx < mNumOfVao);
				if (!glIsBuffer(vbo)) {
					logger->error("Index {} is not a valid vbo object.", vbo);
				}

				//Ҫ���õ���vao[vaoIdx]�����԰���
				glCall(glBindVertexArray, vao[vaoIdx]);
				//��ΪҪvao��vbo�����ԣ������Ȱ�vbo��������vao
				glCall(glBindBuffer, GL_ARRAY_BUFFER, vbo);
				glCall(glEnableVertexAttribArray, positions[vaoIdx]);

				glCall(glVertexAttribPointer, positions[vaoIdx],	//����vaoPosλ���ϵ�����
					numOfFloat,					//�����ɼ���float��ɣ�һ�����������3��float��һ����ɫ���������������ĸ�float��RGB��RGBA��
					GL_FLOAT,					//�����ɼ���float���
					GL_TRUE,					//���Թ�һ����
					numOfFloat * sizeof(float),	//����vboԪ��֮��Ŀ�ȣ����ǵ���vbo�Ĵ�С��stride��
					(void*)NULL);				//vbo�ڲ���ȣ��������ǵ���vbo��ʼ��ַ��ƫ������offset��

				positions[vaoIdx]++;//ÿ�ε��ú�position+1
			}
			//��������Ӧsingle buffer״̬��һ��VBO��Ӧһ�����ԣ�
			void set(GLuint vbo, GLuint numOfFloat) {
				assert(mNumOfVao == 1);
				set(0, vbo, numOfFloat);
			}

			//stride:ÿ��vbo���ж��ٸ�float������ÿ��vbo��һ�������һ��rgba��ɫ����ôÿ��������3��float��zyz��+4����ɫfloat��rgba������ô����д7
			//offset�����set������vbo�׵�ַ��ƫ����������vbo������float*3�����㣩+float*4����ɫ������ô��������ɫ��ʱ��offset����3
			void set(GLuint vaoIdx, GLuint vbo, GLuint numOfFloat, GLuint stride, GLuint offset) {
				assert(vaoIdx < mNumOfVao);
				glCall(glBindVertexArray, vao[vaoIdx]);
				glCall(glBindBuffer, GL_ARRAY_BUFFER, vbo);
				glCall(glEnableVertexAttribArray, positions[vaoIdx]);

				glCall(glVertexAttribPointer, positions[vaoIdx],	//����vaoPosλ���ϵ�����
					numOfFloat,					//�����ɼ���float��ɣ�һ�����������3��float��һ����ɫ���������������ĸ�float��RGB��RGBA��
					GL_FLOAT,					//�����ɼ���float���
					GL_TRUE,					//���Թ�һ����
					stride * sizeof(float),	//����vboԪ��֮��Ŀ�ȣ����ǵ���vbo�Ĵ�С��stride��
					(void*)(offset * sizeof(float)));				//vbo�ڲ���ȣ��������ǵ���vbo��ʼ��ַ��ƫ������offset��
				positions[vaoIdx]++;
			}

			void set(GLuint vbo, GLuint numOfFloat, GLuint stride, GLuint offset) {
				assert(mNumOfVao == 1);
				set(0, vbo, numOfFloat, stride, offset);
			}
		};
	}
}