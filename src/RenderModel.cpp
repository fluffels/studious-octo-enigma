#pragma warning(disable: 4018)
#pragma warning(disable: 4267)

#include <cmath>

#include "RenderModel.h"

#include "FileSystem.h"
#include "Model.h"
#include "Palette.h"

#include "glm/vec2.hpp"
#include "glm/vec3.hpp"

using glm::vec2;

typedef float scalar_t;
typedef glm::vec3 vec3_t;

// http://www.gamers.org/dEngine/quake/spec/quake-spec34/qkspec_5.htm
struct MDLHeader {
    char id[4];         // 0x4F504449 = "IDPO" for IDPOLYGON
    int32_t version;    // Version = 6
    vec3_t scale;    // Model scale factors.
    vec3_t origin;   // Model origin.
    scalar_t radius; // Model bounding radius.
    vec3_t offsets;  // Eye position (useless?)
    int32_t numskins;   // the number of skin textures
    int32_t skinwidth;  // Width of skin texture, multiple of 8
    int32_t skinheight; // Height of skin texture multiple of 8
    int32_t numverts;   // Number of vertices
    int32_t numtris;    // Number of triangles surfaces
    int32_t numframes;  // Number of frames
    int32_t synctype;   // 0= synchron, 1= random
    int32_t flags;      // 0 (see Alias models)
    scalar_t size;   // average size of triangles
};

struct TexCoord {
    int32_t onseam; // 0 or 0x20
    int32_t s;      // position, horizontally in range [0, skinwidth]
    int32_t t;      // position, vertically in range [0,skinheight]
};

struct Triangle {
    int32_t facesfront;  // boolean
    int32_t vertices[3]; // vertex indices [0, numverts]
};

struct FrameVertex {
    uint8_t packedPosition[3];
    uint8_t lightNormalIdx;
};

struct Frame {
    FrameVertex min;
    FrameVertex max;
    char name[16];
    vector<FrameVertex> vertices;
};

struct FrameGroup {
    FrameVertex min;
    FrameVertex max;
    vector<float> times;
    vector<Frame> frames;
};

struct ModelVertex {
    vec3 position;
    vec2 texCoord;
};

static vector<VulkanMesh> frames;
static vector<FrameGroup> groups;
static vector<vec3> origins;
static VulkanPipeline pipeline = {};

void readFrame(FILE* file, int32_t numverts, Frame& frame) {
    readStruct(file, frame.min);
    readStruct(file, frame.max);
    readStruct(file, frame.name);
    frame.vertices.resize(numverts);
    fread(frame.vertices.data(), sizeof(FrameVertex), numverts, file);
}

void readFrameGroup(FILE* file, int32_t numverts, FrameGroup& group) {
    int32_t groupType = -1;
    fread(&groupType, sizeof(groupType), 1, file);

    int32_t frameCount = 0;
    fread(&frameCount, sizeof(frameCount), 1, file);
    if (frameCount < 1) throw std::runtime_error("invalid frame count");

    if (groupType == 0) {
        Frame frame = {};
        readFrame(file, numverts, frame);
    } else if (groupType > 0) {
        readStruct(file, group.min);
        readStruct(file, group.max);
        group.times.resize(frameCount);
        fread(group.times.data(), sizeof(float), frameCount, file);
        group.frames.resize(frameCount);
        for (int i = 0; i < frameCount; i++) {
            readFrame(file, numverts, group.frames[i]);
        }
    } else {
        throw std::runtime_error("invalid frame group type");
    }
}

void initModels(
    Vulkan& vk,
    PAKParser& pak,
    vector<Entity>& entities
) {
    initVKPipeline(vk, "alias_model", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, pipeline);
    updateUniformBuffer(
        vk.device,
        pipeline.descriptorSet,
        0,
        vk.mvp.handle
    );

    for (auto& entity: entities) {
        auto name = entity.className;
        if (strcmp(name, "light_flame_large_yellow") == 0) {
            origins.push_back(entity.origin);
        }
    }

    auto palette = pak.loadPalette();
    auto file = pak.file;
    auto entry = pak.findEntry("progs/flame2.mdl");
    MDLHeader header;
    seek(file, entry.offset);
    readStruct(file, header);

    uint32_t group;
    readStruct(file, group);

    if (group != 0) {
        LOG(ERROR) << "group skins not supported";
        exit(-1);
    }

    uint32_t skinIdxsSize = header.skinheight * header.skinwidth;
    vector<uint8_t> skinIdxs(skinIdxsSize);
    fread(skinIdxs.data(), skinIdxsSize, 1, file);

    uint32_t skinColorsSize = skinIdxsSize * 4;
    vector<uint8_t> skinColors(skinColorsSize);

    for (uint32_t i = 0; i < skinIdxsSize; i++) {
        auto colorIdx = skinIdxs[i];
        auto paletteColor = palette->colors[colorIdx];
        skinColors[i*4] = paletteColor.r;
        skinColors[i*4+1] = paletteColor.g;
        skinColors[i*4+2] = paletteColor.b;
        if (colorIdx == 208) {
            skinColors[i*4+3] = 0;
        } else {
            skinColors[i*4+3] = 255;
        }
    }
    delete palette;

    VulkanSampler sampler = {};
    uploadTexture(
        vk.device,
        vk.memories,
        vk.queue,
        vk.queueFamily,
        vk.cmdPoolTransient,
        header.skinwidth,
        header.skinheight,
        skinColors.data(),
        skinColorsSize,
        sampler
    );

    updateCombinedImageSampler(
        vk.device,
        pipeline.descriptorSet,
        1,
        &sampler,
        1
    );

    vector<TexCoord> texCoords(header.numverts);
    fread(texCoords.data(), sizeof(TexCoord), header.numverts, file);

    vector<Triangle> triangles(header.numtris);
    fread(triangles.data(), sizeof(Triangle), header.numtris, file);

    groups.resize(header.numframes);
    for (auto& group: groups) {
        readFrameGroup(file, header.numverts, group);
    }

    for (auto& group: groups) {
        for (auto& frame: group.frames) {
            vector<ModelVertex> vertices;

            for (auto& triangle: triangles) {
                for (int i = 0; i < 3; i ++) {
                    auto vertIdx = triangle.vertices[i];
                    auto& vertex = vertices.emplace_back();

                    auto& packedVertex = frame.vertices[vertIdx];
                    vertex.position.x = packedVertex.packedPosition[0]
                        * header.scale.x + header.origin.x;
                    vertex.position.y = -packedVertex.packedPosition[2]
                        * header.scale.z - header.origin.z;
                    vertex.position.z = packedVertex.packedPosition[1]
                        * header.scale.y + header.origin.y;
                    
                    auto& texCoord = texCoords[vertIdx];
                    vertex.texCoord.s = (float)texCoord.s / header.skinwidth;
                    vertex.texCoord.t = (float)texCoord.t / header.skinheight;

                    if ((!triangle.facesfront) && texCoord.onseam) {
                        vertex.texCoord.s += .5f;
                    }
                }
            }
            
            auto& mesh = frames.emplace_back();
            uploadMesh(
                vk.device,
                vk.memories,
                vk.queueFamily,
                vertices.data(),
                vertices.size() * sizeof(ModelVertex),
                mesh
            );
            mesh.vCount = vertices.size();
        }
    }
}

void recordModelCommandBuffers(
    Vulkan& vk,
    float epoch,
    vector<VkCommandBuffer>& cmds
) {
    auto framebufferCount = vk.swap.images.size();
    cmds.resize(framebufferCount);
    createCommandBuffers(
        vk.device,
        vk.cmdPoolTransient,
        framebufferCount,
        cmds
    );
    for (int idx = 0; idx < framebufferCount; idx++) {
        auto& cmd = cmds[idx];
        auto& fb = vk.swap.framebuffers[idx];

        beginFrameCommandBuffer(cmd);

        VkRenderPassBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.clearValueCount = 0;
        beginInfo.framebuffer = fb;
        beginInfo.renderArea.extent = vk.swap.extent;
        beginInfo.renderArea.offset = {0, 0};
        beginInfo.renderPass = vk.renderPassNoClear;

        vkCmdBeginRenderPass(cmd, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
        VkDeviceSize offsets[] = {0};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle);
        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline.layout,
            0, 1,
            &pipeline.descriptorSet,
            0, nullptr
        );
        uint32_t frameIdx = 0;
        auto& frameGroup = groups[1];
        float maxTime = frameGroup.times[frameGroup.times.size()-1];
        float animationTime = std::fmod(epoch, maxTime);
        for (frameIdx = 0; frameIdx < frameGroup.times.size(); frameIdx++) {
            float frameTime = frameGroup.times[frameIdx];
            if (animationTime < frameTime) {
                break;
            }
        }
        auto& mesh = frames[frameIdx+6];
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vBuff.handle, offsets);
        for (auto& origin: origins) {
            vkCmdPushConstants(
                cmd,
                pipeline.layout,
                VK_SHADER_STAGE_VERTEX_BIT,
                0,
                sizeof(origin),
                &origin
            );
            vkCmdDraw(cmd, mesh.vCount, 1, 0, 0);
        }
        vkCmdEndRenderPass(cmd);

        endCommandBuffer(cmd);
    }
}
