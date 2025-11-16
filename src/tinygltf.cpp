// tinygltf: https://github.com/syoyo/tinygltf (header-only)
// Compile with /std:c++20 and link your Direct3D 12 libs separately.

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "tiny_gltf.h"

#include <vector>
#include <string>
#include <unordered_map>
#include <cassert>
#include <cmath>

struct Bone {
    int node;                  // index in glTF nodes
    int parent;                // parent bone index, or -1
    std::string name;
    // bind-space data
    float inverseBind[16];
    // runtime data (updated per-frame)
    float globalMatrix[16];
};

struct Skin {
    int skinIndex;
    std::vector<Bone> bones;
    int skeletonRootNode = -1; // optional: from skin.skeleton (if author provides)
};

// Helpers for 4x4 operations omitted for brevity; use a math lib in production.

static void ReadAccessorMat4(const tinygltf::Model& model, int accessorIndex,
                             std::vector<float>& out) {
    const auto& acc = model.accessors[accessorIndex];
    const auto& view = model.bufferViews[acc.bufferView];
    const auto& buf = model.buffers[view.buffer];

    assert(acc.type == TINYGLTF_TYPE_MAT4);
    size_t stride = view.byteStride ? view.byteStride : sizeof(float) * 16;
    out.resize(acc.count * 16);

    const uint8_t* base = buf.data.data() + view.byteOffset + acc.byteOffset;
    for (size_t i = 0; i < acc.count; ++i) {
        memcpy(&out[i*16], base + stride * i, sizeof(float) * 16);
    }
}

Skin BuildSkin(const tinygltf::Model& model, int skinIndex) {
    const auto& gltfSkin = model.skins[skinIndex];
    Skin s; s.skinIndex = skinIndex;
    const auto& joints = gltfSkin.joints;

    // 1) read inverseBindMatrices (optional but common)
    std::vector<float> ibm; // N * 16
    if (gltfSkin.inverseBindMatrices >= 0) {
        ReadAccessorMat4(model, gltfSkin.inverseBindMatrices, ibm);
        assert(ibm.size() == joints.size() * 16 &&
               "inverseBindMatrices count must match joints count");
    } else {
        ibm.resize(joints.size() * 16, 0.0f);
        // Fill with identity if absent
        for (size_t i = 0; i < joints.size(); ++i) {
            float* M = &ibm[i*16]; memset(M, 0, 16*sizeof(float));
            M[0]=M[5]=M[10]=M[15]=1.0f;
        }
    }

    // 2) build bones array
    s.bones.resize(joints.size());
    std::unordered_map<int,int> nodeToBone; nodeToBone.reserve(joints.size());
    for (size_t i = 0; i < joints.size(); ++i) {
        const int nodeIdx = joints[i];
        nodeToBone[nodeIdx] = int(i);
        s.bones[i].node = nodeIdx;
        s.bones[i].parent = -1;
        if (nodeIdx >= 0 && nodeIdx < (int)model.nodes.size())
            s.bones[i].name = model.nodes[nodeIdx].name;
        memcpy(s.bones[i].inverseBind, &ibm[i*16], sizeof(float)*16);
    }

    // 3) compute parent links from node hierarchy
    for (size_t i = 0; i < joints.size(); ++i) {
        int nodeIdx = joints[i];
        // find any parent among joints (linear scan is fine for small rigs)
        for (size_t j = 0; j < joints.size(); ++j) {
            const auto& parentNode = model.nodes[joints[j]];
            for (int child : parentNode.children) {
                if (child == nodeIdx) { s.bones[i].parent = int(j); break; }
            }
            if (s.bones[i].parent >= 0) break;
        }
    }

    // optional: skin.skeleton is the common root node of the skeleton
    if (gltfSkin.skeleton >= 0) s.skeletonRootNode = gltfSkin.skeleton;

    return s;
}

bool LoadGLTF(const std::string& path, tinygltf::Model& model) {
    tinygltf::TinyGLTF loader; std::string err; std::string warn;
    bool ok = false;
    if (path.size() >= 4 && (path.substr(path.size()-4) == ".glb"))
        ok = loader.LoadBinaryFromFile(&model, &err, &warn, path);
    else
        ok = loader.LoadASCIIFromFile(&model, &err, &warn, path);
    if (!warn.empty()) fprintf(stderr, "glTF warn: %s\n", warn.c_str());
    if (!err.empty())  fprintf(stderr, "glTF err: %s\n", err.c_str());
    return ok;
}

struct VertexSkinView {
    const uint16_t* joints16 = nullptr; // or uint8_t*
    const float*    weights   = nullptr; // 4 floats per vertex
    size_t count = 0; size_t strideJ = 0; size_t strideW = 0;
    bool valid = false;
};

VertexSkinView GetSkinStreams(const tinygltf::Model& m,
                              const tinygltf::Primitive& prim) {
    VertexSkinView v{};
    auto getView = [&](const char* sem, bool weights) ->std::pair<const uint8_t*, size_t> {
        auto it = prim.attributes.find(sem);
        if (it == prim.attributes.end()) return {nullptr,0};
        const auto& acc = m.accessors[it->second];
        const auto& view = m.bufferViews[acc.bufferView];
        const auto& buf = m.buffers[view.buffer];
        size_t stride = view.byteStride ? view.byteStride :
                        (weights ? sizeof(float)*4
                                 : (acc.componentType==TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT ? sizeof(uint16_t)*4 : sizeof(uint8_t)*4));
        const uint8_t* base = buf.data.data() + view.byteOffset + acc.byteOffset;
        return { base, stride };
    };
    auto [jptr, jstride] = getView("JOINTS_0", false);
    auto [wptr, wstride] = getView("WEIGHTS_0", true);
    if (!jptr || !wptr) return v;

    v.joints16 = reinterpret_cast<const uint16_t*>(jptr); // adjust if u8
    v.weights  = reinterpret_cast<const float*>(wptr);
    v.count    = m.accessors[prim.attributes.at("WEIGHTS_0")].count;
    v.strideJ  = jstride; v.strideW = wstride;
    v.valid    = true; return v;
}

void ValidatePrimitiveSkin(const tinygltf::Model& model,
                           const tinygltf::Skin& skin,
                           const tinygltf::Primitive& prim)
{
    auto v = GetSkinStreams(model, prim);
    if (!v.valid) throw std::runtime_error("Missing JOINTS_0/WEIGHTS_0");

    for (size_t i = 0; i < v.count; ++i) {
        const uint16_t* J = reinterpret_cast<const uint16_t*>(
            reinterpret_cast<const uint8_t*>(v.joints16) + v.strideJ * i);
        const float* W = reinterpret_cast<const float*>(
            reinterpret_cast<const uint8_t*>(v.weights) + v.strideW * i);

        float sum = W[0]+W[1]+W[2]+W[3];
        if (!(sum > 0.0f)) throw std::runtime_error("All zero weights");
        if (std::abs(sum - 1.0f) > 1e-3f) {
            // Renormalize or warn; many exporters leave small drift
        }
        for (int k=0;k<4;k++) {
            if (W[k] < 0.0f) throw std::runtime_error("Negative weight");
            if (J[k] >= skin.joints.size())
                throw std::runtime_error("Joint index out of range");
        }
    }
}
