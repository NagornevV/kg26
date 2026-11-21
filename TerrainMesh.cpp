#include "TerrainMesh.h"
#include "stb_image.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

using namespace DirectX;

float TerrainMesh::Height(int x, int z) const
{
    x = std::clamp(x, 0, mResolution - 1);
    z = std::clamp(z, 0, mResolution - 1);
    // Numerical height data, without sRGB/gamma conversion. Global scale avoids seams.
    return -70.f + (mHeights[z * mResolution + x] - mMinimumHeight) / 65535.f * 1800.f;
}

void TerrainMesh::Load(const char* heightmapPath)
{
    int w = 0, h = 0, channels = 0;
    if (!stbi_is_16_bit(heightmapPath))
        throw std::runtime_error("Terrain: expected a 16-bit heightmap");
    std::unique_ptr<stbi_us, decltype(&stbi_image_free)> image(
        stbi_load_16(heightmapPath, &w, &h, &channels, 1), stbi_image_free);
    if (!image || w != 1025 || h != 1025)
        throw std::runtime_error("Terrain: expected a 1025 x 1025 heightmap");
    mResolution = w;
    mHeights.assign(image.get(), image.get() + w * h);
    mMinimumHeight = *std::min_element(mHeights.begin(), mHeights.end());
    Vertices.clear(); Indices.clear(); Nodes.clear(); Selected.clear();
    BuildNode(0, 0, w - 1, 0);
}

int TerrainMesh::BuildNode(int x, int z, int sampleSize, int depth)
{
    const int index = static_cast<int>(Nodes.size());
    Nodes.emplace_back();
    TerrainNode node;
    node.SampleX = x; node.SampleZ = z; node.SampleSize = sampleSize; node.Depth = depth;
    const float spacing = WorldSize / (mResolution - 1);
    float minY = Height(x, z), maxY = minY;
    // Include ALL height samples, not only the coarsely sampled mesh vertices.
    for (int sz = z; sz <= z + sampleSize; ++sz)
        for (int sx = x; sx <= x + sampleSize; ++sx)
        {
            minY = (std::min)(minY, Height(sx, sz));
            maxY = (std::max)(maxY, Height(sx, sz));
        }
    constexpr float skirtBottom = -170.f;
    node.Minimum = { OriginX + x * spacing, (std::min)(minY, skirtBottom), OriginZ + z * spacing };
    node.Maximum = { OriginX + (x + sampleSize) * spacing, maxY, OriginZ + (z + sampleSize) * spacing };
    const uint32_t base = static_cast<uint32_t>(Vertices.size());
    const int step = sampleSize / GridCells;
    for (int row = 0; row <= GridCells; ++row)
        for (int col = 0; col <= GridCells; ++col)
        {
            const int sx = x + col * step, sz = z + row * step;
            XMFLOAT3 normal;
            XMStoreFloat3(&normal, XMVector3Normalize(XMVectorSet(
                Height(sx - 1, sz) - Height(sx + 1, sz), 2.f * spacing,
                Height(sx, sz - 1) - Height(sx, sz + 1), 0.f)));
            Vertices.push_back({ { OriginX + sx * spacing, Height(sx, sz), OriginZ + sz * spacing },
                normal, { (sx + 0.5f) / mResolution, (sz + 0.5f) / mResolution } });
        }
    node.IndexStart = static_cast<uint32_t>(Indices.size());
    for (int row = 0; row < GridCells; ++row)
        for (int col = 0; col < GridCells; ++col)
        {
            const uint32_t a = base + row * (GridCells + 1) + col;
            const uint32_t b = a + 1, c = a + GridCells + 1, d = c + 1;
            Indices.insert(Indices.end(), { a, c, b, b, c, d });
        }
    node.SurfaceIndexCount = static_cast<uint32_t>(Indices.size()) - node.IndexStart;

    // Vertical skirts reach below the global minimum, closing any LOD height gap.
    std::vector<uint32_t> perimeter;
    for (int i = 0; i < GridCells; ++i) perimeter.push_back(base + i);
    for (int i = 0; i < GridCells; ++i) perimeter.push_back(base + i * (GridCells + 1) + GridCells);
    for (int i = GridCells; i > 0; --i) perimeter.push_back(base + GridCells * (GridCells + 1) + i);
    for (int i = GridCells; i > 0; --i) perimeter.push_back(base + i * (GridCells + 1));
    const uint32_t skirtBase = static_cast<uint32_t>(Vertices.size());
    for (uint32_t top : perimeter)
    {
        auto vertex = Vertices[top];
        vertex.Position.y = skirtBottom;
        Vertices.push_back(vertex);
    }
    for (uint32_t i = 0; i < perimeter.size(); ++i)
    {
        const uint32_t j = (i + 1) % static_cast<uint32_t>(perimeter.size());
        const uint32_t a = perimeter[i], b = perimeter[j], c = skirtBase + i, d = skirtBase + j;
        Indices.insert(Indices.end(), { a, b, c, b, d, c });
    }
    node.SkirtIndexCount = static_cast<uint32_t>(Indices.size()) - node.IndexStart - node.SurfaceIndexCount;
    // Never keep a reference into Nodes while recursion grows the vector.
    if (depth < MaxDepth)
    {
        const int half = sampleSize / 2;
        node.Children[0] = BuildNode(x, z, half, depth + 1);
        node.Children[1] = BuildNode(x + half, z, half, depth + 1);
        node.Children[2] = BuildNode(x, z + half, half, depth + 1);
        node.Children[3] = BuildNode(x + half, z + half, half, depth + 1);
    }
    Nodes[index] = node;
    return index;
}

void TerrainMesh::Select(const XMFLOAT3& eye, const XMMATRIX& viewProj,
    bool culling, bool adaptiveLod, bool skirts)
{
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, viewProj);
    // Row-vector DirectX convention: clip.x/y within +/-w, clip.z within [0,w].
    const std::array<XMFLOAT4, 6> planes = {{
        { m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41 },
        { m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41 },
        { m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42 },
        { m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42 },
        { m._13, m._23, m._33, m._43 },
        { m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43 }
    }};
    Stats = {}; Selected.clear();
    if (!Nodes.empty()) SelectNode(0, eye, planes, culling, adaptiveLod, skirts);
}

void TerrainMesh::SelectNode(int index, const XMFLOAT3& eye,
    const std::array<XMFLOAT4, 6>& planes, bool culling, bool adaptiveLod, bool skirts)
{
    const auto& node = Nodes[index];
    if (culling)
        for (const auto& p : planes)
        {
            // The corner farthest along the plane normal. Only reject a fully outside AABB.
            const float x = p.x >= 0 ? node.Maximum.x : node.Minimum.x;
            const float y = p.y >= 0 ? node.Maximum.y : node.Minimum.y;
            const float z = p.z >= 0 ? node.Maximum.z : node.Minimum.z;
            if (p.x * x + p.y * y + p.z * z + p.w < -0.01f)
            {
                ++Stats.RejectedNodes;
                return; // Skip this entire subtree.
            }
        }
    const float dx = eye.x - std::clamp(eye.x, node.Minimum.x, node.Maximum.x);
    const float dy = eye.y - std::clamp(eye.y, node.Minimum.y, node.Maximum.y);
    const float dz = eye.z - std::clamp(eye.z, node.Minimum.z, node.Maximum.z);
    const float splitDistance = (node.Maximum.x - node.Minimum.x) * 1.25f;
    const bool split = node.Children[0] >= 0 &&
        (!adaptiveLod || dx * dx + dy * dy + dz * dz < splitDistance * splitDistance);
    if (split)
    {
        for (int child : node.Children) SelectNode(child, eye, planes, culling, adaptiveLod, skirts);
        return; // Children replace the parent, never overlap it.
    }
    Selected.push_back(index);
    ++Stats.Tiles; ++Stats.Levels[node.Depth];
    Stats.Triangles += (node.SurfaceIndexCount + (skirts ? node.SkirtIndexCount : 0)) / 3;
}

void TerrainMesh::ExportHeightTiles(const std::string& directory) const
{
    std::filesystem::create_directories(directory);
    const int size = (mResolution - 1) / (1 << MaxDepth);
    for (const auto& node : Nodes)
    {
        if (node.Depth != MaxDepth) continue;
        const auto path = std::filesystem::path(directory) /
            ("height_" + std::to_string(node.SampleX / size) + "_" + std::to_string(node.SampleZ / size) + ".r16");
        std::ofstream file(path, std::ios::binary);
        if (!file) throw std::runtime_error("Cannot export terrain tiles");
        for (int z = node.SampleZ; z <= node.SampleZ + size; ++z)
            for (int x = node.SampleX; x <= node.SampleX + size; ++x)
            {
                const uint16_t value = mHeights[z * mResolution + x];
                const char bytes[] = { static_cast<char>(value & 255), static_cast<char>(value >> 8) };
                file.write(bytes, 2); // Explicit little-endian R16, 65 x 65 including shared edges.
            }
        if (!file) throw std::runtime_error("Failed to write terrain tile");
    }
}
