module;
#include <glad/glad.h>
#include "Helpers.hpp"
#include <cassert>
#include <vector>
#include <stdexcept>
#include <spdlog/spdlog.h>
#include <map>
#include <vector>
#include <utility>
export module GLObjWrapper;

import GLCheckError;

namespace RGL {
	namespace glcore {





		//VBO,vertex buffer object��CPU�洢���ڴ�������Դ�һ�����������
		//class VBO{ID,GPU_ADDRESS,SIZE,etc.}��
		//����VBO�Ĳ�������Ҫ�Ȱ󶨣��ٲ���
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
		//����������Ϣ�����ߣ�uv(����λ��)�ȵ�
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

		export enum class BUFF_ATTRIBUTION
		{
			VERT_POSITION,
			COLOR,
			UV,
		};

		using PositionBuffType = std::vector<BUFF_ATTRIBUTION>;
		using VAOs_Pos_Types = std::map<GLuint, PositionBuffType>;

		export class VAO {
			GLuint* vao;
			//����vao��position����ʼ��Ϊ0��ÿ�ε���set����1
			//����Ҫ�ĺ�shader������ϵ�����ԣ�layout(location=n)��n�����n��position
			GLuint* positions;
			GLuint mNumOfVao;
			VAOs_Pos_Types vao_pos_types;
			VAO(size_t numOfVao) :mNumOfVao(numOfVao), vao_pos_types() {
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

			bool checkAttributionDuplicated(GLuint vaoIdx, BUFF_ATTRIBUTION attrib) {
				const auto& currentAttributes = vao_pos_types[vaoIdx];
				if (std::find(currentAttributes.begin(), currentAttributes.end(), attrib) != currentAttributes.end()) {
					return true;
				}
				else
				{
					return false;
				}
			}

		public:

			VAO() :VAO(1) {}
			~VAO() {
				delete[] vao;
				delete[] positions;
			}


			/// <summary>
			/// ��Ӧsingle buffer״̬��һ��VBO��Ӧһ�����ԣ���VAO�������÷�װ
			/// </summary>
			/// <param name="vaoIdx">����vaoIdxλ���ϵ����ԣ�ͬʱ����󶨵�����vbo����Ϊ����һ���Դ����˶��vao��������Ҫȷ��vao���±�</param>
			/// <param name="vbo">��Ҫ�������Ե�vbo��������vbo��ţ�Ҳ������VBO����</param>
			/// <param name="numOfFloat">�����Ե�float�������������������3����RGBA��ɫ������4����UV�������������2��</param>
			/// <param name="buffAttr">VAO��ӦVBO��������ö�٣�����ͨ�����Բ鿴VAO��Position������ʹ����һ��map������position�����Եı�</param>
			void set(GLuint vaoIdx, GLuint vbo, GLuint numOfFloat, BUFF_ATTRIBUTION buffAttr) {
				assert(vaoIdx < mNumOfVao);
				if (!glIsBuffer(vbo)) {
					logger->error("Index {} is not a valid vbo object.", vbo);
				}

				//Ҫ���õ���vao[vaoIdx]�����԰���
				glCall(glBindVertexArray, vao[vaoIdx]);
				//��ΪҪvao��vbo�����ԣ������Ȱ�vbo��������vao
				glCall(glBindBuffer, GL_ARRAY_BUFFER, vbo);
				glCall(glEnableVertexAttribArray, positions[vaoIdx]);

				glCall(glVertexAttribPointer, positions[vaoIdx],numOfFloat,	GL_FLOAT,GL_TRUE,numOfFloat * sizeof(float),(void*)NULL);
				if (checkAttributionDuplicated(vaoIdx, buffAttr))
				{
					this->logger->error("Input duplated attribution");
				}
				vao_pos_types[vaoIdx].push_back(buffAttr);

				positions[vaoIdx]++;//ÿ�ε��ú�position+1
			}

			/// <summary>
			/// ��vaoֻ��һ����ʱ���single buffer������
			/// </summary>
			/// <param name="vbo"></param>
			/// <param name="numOfFloat"></param>
			/// <param name="buffAttr"></param>
			void set(GLuint vbo, GLuint numOfFloat, BUFF_ATTRIBUTION buffAttr) {
				assert(mNumOfVao == 1);
				set(0, vbo, numOfFloat, buffAttr);
			}



			/// <summary>
			/// ������interleaved buffer��
			/// </summary>
			/// <param name="vaoIdx">����vaoIdxλ���ϵ����ԣ�ͬʱ����󶨵�����vbo����Ϊ����һ���Դ����˶��vao��������Ҫȷ��vao���±�</param>
			/// <param name="vbo">��Ҫ�������Ե�vbo��������vbo��ţ�Ҳ������VBO����</param>
			/// <param name="numOfFloat">�����Ե�float�������������������3����RGBA��ɫ������4����UV�������������2��</param>
			/// <param name="stride">ÿ��vbo���ж��ٸ�float������ÿ��vbo��һ�������һ��rgba��ɫ����ôÿ��������3��float��zyz��+4����ɫfloat��rgba������ô����д7</param>
			/// <param name="offset">���set������vbo�׵�ַ��ƫ����������vbo������float*3�����㣩+float*4����ɫ������ô��������ɫ��ʱ��offset����3</param>
			/// <param name="buffAttr">VAO��ӦVBO��������ö�٣�����ͨ�����Բ鿴VAO��Position������ʹ����һ��map������position�����Եı�</param>
			void set(GLuint vaoIdx, GLuint vbo, GLuint numOfFloat, GLuint stride, GLuint offset, BUFF_ATTRIBUTION buffAttr) {
				assert(vaoIdx < mNumOfVao);
				glCall(glBindVertexArray, vao[vaoIdx]);
				glCall(glBindBuffer, GL_ARRAY_BUFFER, vbo);
				glCall(glEnableVertexAttribArray, positions[vaoIdx]);

				glCall(glVertexAttribPointer, positions[vaoIdx],	//����vaoPosλ���ϵ�����
					numOfFloat,					//�����ɼ���float��ɣ�һ�����������3��float��һ����ɫ���������������ĸ�float��RGB��RGBA��
					GL_FLOAT,					//�����ɼ���float���
					GL_TRUE,					//���Թ�һ����
					stride * sizeof(float),	//����vboԪ��֮��Ŀ�ȣ����ǵ���vbo�Ĵ�С��stride��
					(void*)(offset * sizeof(float)));				//vbo�ڲ���ȣ��������ǵ���vbo��ʼ��ַ��ƫ������offset)

				if (checkAttributionDuplicated(vaoIdx,buffAttr))
				{
					this->logger->error("Input duplated attribution");
				}

				vao_pos_types[vaoIdx].push_back(buffAttr);

				positions[vaoIdx]++;
			}

			void set(GLuint vbo, GLuint numOfFloat, GLuint stride, GLuint offset, BUFF_ATTRIBUTION buffAttr) {
				assert(mNumOfVao == 1);
				set(0, vbo, numOfFloat, stride, offset, buffAttr);
			}
			/// <summary>
			/// ����shader��layout(location = N)��λ��ͬattribution��location
			/// </summary>
			/// <param name="vaoIdx">vaoIdx</param>
			/// <param name="attr">��Ҫ����location�Ķ�������</param>
			/// <returns></returns>
			GLuint getPositionByAttribution(GLuint vaoIdx, BUFF_ATTRIBUTION attr) {
				const auto it = std::find(vao_pos_types[vaoIdx].begin(), vao_pos_types[vaoIdx].end(), attr);
				if (it != vao_pos_types[vaoIdx].end())
				{
					return std::distance(vao_pos_types[vaoIdx].begin(), it);
				}
				else {
					this->logger->error("Cannot find attr");
				}
			}
			/// <summary>
			/// ����shader��layout(location = N)��λ��ͬattribution��location
			/// </summary>
			/// <param name="attr">��Ҫ����location�Ķ�������</param>
			/// <returns></returns>
			GLuint getPositionByAttribution(BUFF_ATTRIBUTION attr) {
				return getPositionByAttribution(0, attr);
			}
		};
	}
}