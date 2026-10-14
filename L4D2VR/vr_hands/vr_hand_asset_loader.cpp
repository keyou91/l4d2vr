#include "vr_hand_asset_loader.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "vr_hand_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace
{
    cgltf_result ReadUtf8CgltfFile(const cgltf_memory_options* memory,
        const cgltf_file_options*, const char* path, cgltf_size* size, void** data)
    {
        if (!path || !data)
            return cgltf_result_invalid_options;
        *data = nullptr;
        try
        {
            std::ifstream stream(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
            if (!stream.is_open())
                return cgltf_result_file_not_found;
            const std::streamoff available = stream.tellg();
            if (available < 0)
                return cgltf_result_io_error;
            const auto requested = size && *size ? static_cast<unsigned long long>(*size) :
                static_cast<unsigned long long>(available);
            if (requested > static_cast<unsigned long long>(available) ||
                requested > static_cast<unsigned long long>((std::numeric_limits<cgltf_size>::max)()) ||
                requested > static_cast<unsigned long long>((std::numeric_limits<std::streamsize>::max)()))
                return cgltf_result_io_error;
            const cgltf_size length = static_cast<cgltf_size>(requested);
            const auto allocate = memory->alloc_func ? memory->alloc_func : cgltf_default_alloc;
            const auto release = memory->free_func ? memory->free_func : cgltf_default_free;
            void* bytes = allocate(memory->user_data, length);
            if (!bytes)
                return cgltf_result_out_of_memory;
            stream.seekg(0, std::ios::beg);
            stream.read(static_cast<char*>(bytes), static_cast<std::streamsize>(length));
            if (!stream.good())
            {
                release(memory->user_data, bytes);
                return cgltf_result_io_error;
            }
            if (size)
                *size = length;
            *data = bytes;
            return cgltf_result_success;
        }
        catch (const std::bad_alloc&)
        {
            return cgltf_result_out_of_memory;
        }
        catch (const std::filesystem::filesystem_error&)
        {
            return cgltf_result_io_error;
        }
    }

    const cgltf_accessor* FindAttribute(const cgltf_primitive& primitive, cgltf_attribute_type type, int index = 0)
    {
        for (cgltf_size i = 0; i < primitive.attributes_count; ++i)
        {
            const cgltf_attribute& attribute = primitive.attributes[i];
            if (attribute.type == type && attribute.index == index)
                return attribute.data;
        }
        return nullptr;
    }

    const cgltf_skin* FindSkinForMesh(const cgltf_data& data, const cgltf_mesh* mesh)
    {
        for (cgltf_size i = 0; i < data.nodes_count; ++i)
        {
            const cgltf_node& node = data.nodes[i];
            if (node.mesh == mesh && node.skin)
                return node.skin;
        }
        return nullptr;
    }

    bool ReadBinaryFile(const std::filesystem::path& path, std::vector<std::uint8_t>& out)
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open())
            return false;

        stream.seekg(0, std::ios::end);
        const std::streamoff size = stream.tellg();
        stream.seekg(0, std::ios::beg);
        if (size <= 0 || static_cast<unsigned long long>(size) > static_cast<unsigned long long>((std::numeric_limits<size_t>::max)()))
            return false;

        out.resize(static_cast<size_t>(size));
        stream.read(reinterpret_cast<char*>(out.data()), size);
        return stream.good();
    }

    void CopyImageBytes(const cgltf_image* image, const std::filesystem::path& glbPath, std::vector<std::uint8_t>& out)
    {
        out.clear();
        if (!image)
            return;

        if (image->buffer_view && image->buffer_view->buffer && image->buffer_view->buffer->data)
        {
            const auto* begin = static_cast<const std::uint8_t*>(image->buffer_view->buffer->data) + image->buffer_view->offset;
            out.assign(begin, begin + image->buffer_view->size);
            return;
        }

        if (image->uri && *image->uri && std::strncmp(image->uri, "data:", 5) != 0)
            ReadBinaryFile(glbPath.parent_path() / std::filesystem::u8path(image->uri), out);
    }

    void CopyMaterialTexture(const cgltf_material* material, const std::filesystem::path& glbPath, std::vector<std::uint8_t>& out)
    {
        out.clear();
        if (!material || !material->has_pbr_metallic_roughness)
            return;

        const cgltf_texture* texture = material->pbr_metallic_roughness.base_color_texture.texture;
        if (!texture)
            return;
        CopyImageBytes(texture->image, glbPath, out);
    }

    bool ReadFloatAccessor(const cgltf_accessor* accessor, cgltf_size index, float* values, cgltf_size components)
    {
        if (!accessor || index >= accessor->count)
            return false;
        return cgltf_accessor_read_float(accessor, index, values, components) != 0;
    }

    bool ReadUintAccessor(const cgltf_accessor* accessor, cgltf_size index, cgltf_uint* values, cgltf_size components)
    {
        if (!accessor || index >= accessor->count)
            return false;
        return cgltf_accessor_read_uint(accessor, index, values, components) != 0;
    }

    bool CopyInverseBindMatrices(const cgltf_skin& skin, std::vector<VrHandMatrix4>& out, std::string& outError)
    {
        out.assign(static_cast<size_t>(skin.joints_count), VrHandMath::Identity());
        if (!skin.inverse_bind_matrices)
            return true;
        if (skin.inverse_bind_matrices->count != skin.joints_count)
        {
            outError = "inverseBindMatrices count does not match skin joint count";
            return false;
        }

        for (cgltf_size joint = 0; joint < skin.joints_count; ++joint)
        {
            float values[16]{};
            if (!ReadFloatAccessor(skin.inverse_bind_matrices, joint, values, 16))
            {
                outError = "cannot read inverseBindMatrices accessor";
                return false;
            }
            std::copy(std::begin(values), std::end(values), out[static_cast<size_t>(joint)].m.begin());
        }
        return true;
    }

    bool CopyBindMatrices(
        const std::vector<VrHandMatrix4>& inverseBindMatrices,
        std::vector<VrHandMatrix4>& out,
        std::string& outError)
    {
        out.resize(inverseBindMatrices.size());
        for (size_t i = 0; i < inverseBindMatrices.size(); ++i)
        {
            if (!VrHandMath::Invert4x4(inverseBindMatrices[i], out[i]))
            {
                outError = "cannot invert hand GLB inverseBindMatrices accessor";
                out.clear();
                return false;
            }
        }
        return true;
    }

    void CopyJointParents(const cgltf_skin& skin, std::vector<int>& out)
    {
        out.assign(static_cast<size_t>(skin.joints_count), -1);
        for (cgltf_size i = 0; i < skin.joints_count; ++i)
        {
            const cgltf_node* parent = skin.joints[i] ? skin.joints[i]->parent : nullptr;
            if (!parent)
                continue;

            for (cgltf_size candidate = 0; candidate < skin.joints_count; ++candidate)
            {
                if (skin.joints[candidate] == parent)
                {
                    out[static_cast<size_t>(i)] = static_cast<int>(candidate);
                    break;
                }
            }
        }
    }


    void TransformStaticVertexByNode(const cgltf_float* matrix, VrHandVertex& vertex)
    {
        if (!matrix)
            return;

        const float px = vertex.position[0];
        const float py = vertex.position[1];
        const float pz = vertex.position[2];
        vertex.position[0] = matrix[0] * px + matrix[4] * py + matrix[8] * pz + matrix[12];
        vertex.position[1] = matrix[1] * px + matrix[5] * py + matrix[9] * pz + matrix[13];
        vertex.position[2] = matrix[2] * px + matrix[6] * py + matrix[10] * pz + matrix[14];

        // Exported magazine assets are expected to use ordinary Blender object
        // transforms. Apply the node's linear part to normals and normalize it.
        // This is exact for the usual rotation + uniform-scale case and keeps the
        // static test asset visible when Blender stores its transform on the node.
        const float nx = vertex.normal[0];
        const float ny = vertex.normal[1];
        const float nz = vertex.normal[2];
        float tx = matrix[0] * nx + matrix[4] * ny + matrix[8] * nz;
        float ty = matrix[1] * nx + matrix[5] * ny + matrix[9] * nz;
        float tz = matrix[2] * nx + matrix[6] * ny + matrix[10] * nz;
        const float lengthSquared = tx * tx + ty * ty + tz * tz;
        if (lengthSquared > 0.0000001f)
        {
            const float inverseLength = 1.0f / std::sqrt(lengthSquared);
            tx *= inverseLength;
            ty *= inverseLength;
            tz *= inverseLength;
        }
        vertex.normal[0] = tx;
        vertex.normal[1] = ty;
        vertex.normal[2] = tz;
    }

    bool LoadStaticPrimitive(const std::filesystem::path& path,
        const cgltf_primitive& primitive,
        const cgltf_float* nodeWorldTransform,
        VrHandMeshAsset& outAsset,
        std::string& outError)
    {
        const cgltf_accessor* positions = FindAttribute(primitive, cgltf_attribute_type_position);
        const cgltf_accessor* normals = FindAttribute(primitive, cgltf_attribute_type_normal);
        const cgltf_accessor* texcoords = FindAttribute(primitive, cgltf_attribute_type_texcoord, 0);
        if (!positions || !normals || !texcoords)
        {
            outError = "static GLB primitive is missing POSITION, NORMAL or TEXCOORD_0";
            return false;
        }

        const cgltf_size vertexCount = positions->count;
        if (vertexCount == 0 || vertexCount > 65535 ||
            normals->count != vertexCount || texcoords->count != vertexCount)
        {
            outError = "static GLB vertex accessor counts are invalid";
            return false;
        }

        outAsset.vertices.assign(static_cast<size_t>(vertexCount), VrHandVertex{});
        for (cgltf_size i = 0; i < vertexCount; ++i)
        {
            VrHandVertex& vertex = outAsset.vertices[static_cast<size_t>(i)];
            if (!ReadFloatAccessor(positions, i, vertex.position, 3) ||
                !ReadFloatAccessor(normals, i, vertex.normal, 3) ||
                !ReadFloatAccessor(texcoords, i, vertex.uv, 2))
            {
                outError = "cannot read static GLB vertex accessor";
                return false;
            }

            TransformStaticVertexByNode(nodeWorldTransform, vertex);
            vertex.joints[0] = 0;
            vertex.weights[0] = 1.0f;
        }

        if (primitive.indices)
        {
            const cgltf_size indexCount = primitive.indices->count;
            if (indexCount == 0 || (indexCount % 3) != 0)
            {
                outError = "static GLB triangle index count is invalid";
                return false;
            }
            outAsset.indices.resize(static_cast<size_t>(indexCount));
            for (cgltf_size i = 0; i < indexCount; ++i)
            {
                const cgltf_size value = cgltf_accessor_read_index(primitive.indices, i);
                if (value >= vertexCount || value > 65535u)
                {
                    outError = "static GLB contains an invalid triangle index";
                    return false;
                }
                outAsset.indices[static_cast<size_t>(i)] = static_cast<std::uint16_t>(value);
            }
        }
        else
        {
            if ((vertexCount % 3) != 0)
            {
                outError = "unindexed static GLB primitive is not a triangle list";
                return false;
            }
            outAsset.indices.resize(static_cast<size_t>(vertexCount));
            for (cgltf_size i = 0; i < vertexCount; ++i)
                outAsset.indices[static_cast<size_t>(i)] = static_cast<std::uint16_t>(i);
        }

        outAsset.jointNames = { "root" };
        outAsset.jointParents = { -1 };
        outAsset.inverseBindMatrices = { VrHandMath::Identity() };
        outAsset.bindMatrices = { VrHandMath::Identity() };
        CopyMaterialTexture(primitive.material, path, outAsset.baseColorTextureBytes);
        return outAsset.IsValid();
    }

    bool LoadPrimitive(const std::filesystem::path& path,
        const cgltf_primitive& primitive,
        const cgltf_skin& skin,
        VrHandMeshAsset& outAsset,
        std::string& outError)
    {
        const cgltf_accessor* positions = FindAttribute(primitive, cgltf_attribute_type_position);
        const cgltf_accessor* normals = FindAttribute(primitive, cgltf_attribute_type_normal);
        const cgltf_accessor* texcoords = FindAttribute(primitive, cgltf_attribute_type_texcoord, 0);
        const cgltf_accessor* joints = FindAttribute(primitive, cgltf_attribute_type_joints, 0);
        const cgltf_accessor* weights = FindAttribute(primitive, cgltf_attribute_type_weights, 0);
        if (!positions || !normals || !texcoords || !joints || !weights)
        {
            outError = "hand GLB primitive is missing POSITION, NORMAL, TEXCOORD_0, JOINTS_0 or WEIGHTS_0";
            return false;
        }

        const cgltf_size vertexCount = positions->count;
        if (vertexCount == 0 || vertexCount > 65535 ||
            normals->count != vertexCount || texcoords->count != vertexCount ||
            joints->count != vertexCount || weights->count != vertexCount)
        {
            outError = "hand GLB vertex accessor counts are invalid";
            return false;
        }

        outAsset.vertices.assign(static_cast<size_t>(vertexCount), VrHandVertex{});
        for (cgltf_size i = 0; i < vertexCount; ++i)
        {
            VrHandVertex& vertex = outAsset.vertices[static_cast<size_t>(i)];
            cgltf_uint jointValues[4]{};
            if (!ReadFloatAccessor(positions, i, vertex.position, 3) ||
                !ReadFloatAccessor(normals, i, vertex.normal, 3) ||
                !ReadFloatAccessor(texcoords, i, vertex.uv, 2) ||
                !ReadFloatAccessor(weights, i, vertex.weights, 4) ||
                !ReadUintAccessor(joints, i, jointValues, 4))
            {
                outError = "cannot read hand GLB vertex accessor";
                return false;
            }

            float weightSum = 0.0f;
            for (int influence = 0; influence < 4; ++influence)
            {
                if (jointValues[influence] >= skin.joints_count || jointValues[influence] > 255u)
                {
                    outError = "hand GLB contains an invalid skin joint index";
                    return false;
                }
                vertex.joints[influence] = static_cast<std::uint8_t>(jointValues[influence]);
                weightSum += vertex.weights[influence];
            }
            if (weightSum > 0.000001f)
            {
                const float invWeight = 1.0f / weightSum;
                for (float& weight : vertex.weights)
                    weight *= invWeight;
            }
        }

        if (primitive.indices)
        {
            const cgltf_size indexCount = primitive.indices->count;
            if (indexCount == 0 || (indexCount % 3) != 0)
            {
                outError = "hand GLB triangle index count is invalid";
                return false;
            }
            outAsset.indices.resize(static_cast<size_t>(indexCount));
            for (cgltf_size i = 0; i < indexCount; ++i)
            {
                const cgltf_size value = cgltf_accessor_read_index(primitive.indices, i);
                if (value >= vertexCount || value > 65535u)
                {
                    outError = "hand GLB contains an invalid triangle index";
                    return false;
                }
                outAsset.indices[static_cast<size_t>(i)] = static_cast<std::uint16_t>(value);
            }
        }
        else
        {
            if ((vertexCount % 3) != 0)
            {
                outError = "unindexed hand GLB primitive is not a triangle list";
                return false;
            }
            outAsset.indices.resize(static_cast<size_t>(vertexCount));
            for (cgltf_size i = 0; i < vertexCount; ++i)
                outAsset.indices[static_cast<size_t>(i)] = static_cast<std::uint16_t>(i);
        }

        outAsset.jointNames.clear();
        outAsset.jointNames.reserve(static_cast<size_t>(skin.joints_count));
        for (cgltf_size i = 0; i < skin.joints_count; ++i)
        {
            const cgltf_node* joint = skin.joints[i];
            outAsset.jointNames.emplace_back((joint && joint->name) ? joint->name : "");
        }
        CopyJointParents(skin, outAsset.jointParents);

        if (!CopyInverseBindMatrices(skin, outAsset.inverseBindMatrices, outError))
            return false;
        if (!CopyBindMatrices(outAsset.inverseBindMatrices, outAsset.bindMatrices, outError))
            return false;
        CopyMaterialTexture(primitive.material, path, outAsset.baseColorTextureBytes);
        return outAsset.IsValid();
    }
}

bool VrHandAssetLoader::LoadGlb(const std::string& path, VrHandMeshAsset& outAsset, std::string& outError)
{
    outAsset = {};
    outError.clear();

    cgltf_options options{};
    options.file.read = ReadUtf8CgltfFile;
    options.file.release = cgltf_default_file_release;
    cgltf_data* data = nullptr;
    const cgltf_result parseResult = cgltf_parse_file(&options, path.c_str(), &data);
    if (parseResult != cgltf_result_success || !data)
    {
        outError = "cgltf cannot parse hand GLB";
        return false;
    }

    const auto freeData = [&]() { cgltf_free(data); };
    const cgltf_result bufferResult = cgltf_load_buffers(&options, data, path.c_str());
    if (bufferResult != cgltf_result_success)
    {
        outError = "cgltf cannot load hand GLB buffers";
        freeData();
        return false;
    }

    if (cgltf_validate(data) != cgltf_result_success)
    {
        outError = "cgltf reports an invalid hand GLB";
        freeData();
        return false;
    }

    bool loaded = false;
    for (cgltf_size meshIndex = 0; meshIndex < data->meshes_count && !loaded; ++meshIndex)
    {
        const cgltf_mesh& mesh = data->meshes[meshIndex];
        const cgltf_skin* skin = FindSkinForMesh(*data, &mesh);
        if (!skin || skin->joints_count == 0 || skin->joints_count > 64)
            continue;

        for (cgltf_size primitiveIndex = 0; primitiveIndex < mesh.primitives_count && !loaded; ++primitiveIndex)
        {
            const cgltf_primitive& primitive = mesh.primitives[primitiveIndex];
            if (primitive.type != cgltf_primitive_type_triangles)
                continue;
            loaded = LoadPrimitive(std::filesystem::u8path(path), primitive, *skin, outAsset, outError);
        }
    }

    freeData();
    if (!loaded)
    {
        if (outError.empty())
            outError = "hand GLB has no supported skinned triangle primitive";
        outAsset = {};
        return false;
    }

    outAsset.sourcePath = path;
    return true;
}


bool VrHandAssetLoader::LoadStaticGlb(const std::string& path, VrHandMeshAsset& outAsset, std::string& outError)
{
    outAsset = {};
    outError.clear();

    cgltf_options options{};
    options.file.read = ReadUtf8CgltfFile;
    options.file.release = cgltf_default_file_release;
    cgltf_data* data = nullptr;
    const cgltf_result parseResult = cgltf_parse_file(&options, path.c_str(), &data);
    if (parseResult != cgltf_result_success || !data)
    {
        outError = "cgltf cannot parse static GLB";
        return false;
    }

    const auto freeData = [&]() { cgltf_free(data); };
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success)
    {
        outError = "cgltf cannot load static GLB buffers";
        freeData();
        return false;
    }

    if (cgltf_validate(data) != cgltf_result_success)
    {
        outError = "cgltf reports an invalid static GLB";
        freeData();
        return false;
    }

    bool loaded = false;
    // Iterate mesh nodes rather than raw mesh definitions so object transforms
    // written by Blender are baked into the standalone magazine vertices.
    for (cgltf_size nodeIndex = 0; nodeIndex < data->nodes_count && !loaded; ++nodeIndex)
    {
        const cgltf_node& node = data->nodes[nodeIndex];
        if (!node.mesh)
            continue;

        cgltf_float nodeWorldTransform[16]{};
        cgltf_node_transform_world(&node, nodeWorldTransform);
        const cgltf_mesh& mesh = *node.mesh;
        for (cgltf_size primitiveIndex = 0; primitiveIndex < mesh.primitives_count && !loaded; ++primitiveIndex)
        {
            const cgltf_primitive& primitive = mesh.primitives[primitiveIndex];
            if (primitive.type != cgltf_primitive_type_triangles)
                continue;
            loaded = LoadStaticPrimitive(
                std::filesystem::u8path(path),
                primitive,
                nodeWorldTransform,
                outAsset,
                outError);
        }
    }

    freeData();
    if (!loaded)
    {
        if (outError.empty())
            outError = "static GLB has no supported triangle primitive";
        outAsset = {};
        return false;
    }

    outAsset.sourcePath = path;
    return true;
}
