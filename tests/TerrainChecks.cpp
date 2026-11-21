#include "../TerrainMesh.h"
#define STB_IMAGE_IMPLEMENTATION
#include "../stb_image.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace DirectX;
static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void CheckCoverage(const TerrainMesh& terrain)
{
    std::array<int, 256> coverage = {};
    for (int index : terrain.Selected)
    {
        const auto& node = terrain.Nodes[index];
        const int x0 = node.SampleX / 64, z0 = node.SampleZ / 64, size = node.SampleSize / 64;
        for (int z = z0; z < z0 + size; ++z)
            for (int x = x0; x < x0 + size; ++x) ++coverage[z * 16 + x];
    }
    for (int count : coverage) Require(count == 1, "Missing or overlapping LOD coverage");
}

// Independent reference: transform all eight AABB corners to clip space.
static bool ReferenceVisible(const TerrainNode& node, const XMMATRIX& vp)
{
    std::array<int, 6> outside = {};
    for (int i = 0; i < 8; ++i)
    {
        XMFLOAT4 p;
        XMStoreFloat4(&p, XMVector4Transform(XMVectorSet(
            i & 1 ? node.Maximum.x : node.Minimum.x,
            i & 2 ? node.Maximum.y : node.Minimum.y,
            i & 4 ? node.Maximum.z : node.Minimum.z, 1.f), vp));
        const float distance[] = { p.w + p.x, p.w - p.x, p.w + p.y, p.w - p.y, p.z, p.w - p.z };
        for (int j = 0; j < 6; ++j) if (distance[j] < -0.03f) ++outside[j];
    }
    return std::none_of(outside.begin(), outside.end(), [](int count) { return count == 8; });
}

int main(int argc, char** argv)
{
    try
    {
        TerrainMesh terrain;
        terrain.Load(argc > 1 ? argv[1] : "terrain/heightmap.png");
        Require(terrain.Nodes.size() == 341, "Unexpected quadtree node count");
        Require(terrain.Resolution() == 1025, "Heightmap resolution mismatch");
        for (uint32_t index : terrain.Indices) Require(index < terrain.Vertices.size(), "Invalid mesh index");
        for (const auto& node : terrain.Nodes)
        {
            Require(node.SurfaceIndexCount == 32 * 32 * 6, "Invalid tile topology");
            for (uint32_t i = node.IndexStart; i < node.IndexStart + node.SurfaceIndexCount + node.SkirtIndexCount; ++i)
            {
                const auto& v = terrain.Vertices[terrain.Indices[i]];
                Require(v.Position.x >= node.Minimum.x && v.Position.x <= node.Maximum.x
                    && v.Position.y >= node.Minimum.y && v.Position.y <= node.Maximum.y
                    && v.Position.z >= node.Minimum.z && v.Position.z <= node.Maximum.z, "Bounds exclude tile geometry");
                const float norm = v.Normal.x*v.Normal.x + v.Normal.y*v.Normal.y + v.Normal.z*v.Normal.z;
                Require(std::isfinite(norm) && std::abs(norm - 1.f) < 0.001f, "Invalid normal");
            }
        }
        const XMFLOAT3 nearEye = { 5896.f, 300.f, 0.f }, farEye = { 5896.f, 16000.f, -16000.f };
        terrain.Select(nearEye, XMMatrixIdentity(), false, false, true);
        CheckCoverage(terrain);
        Require(terrain.Stats.Tiles == 256, "Max LOD must draw 256 leaf tiles");
        const auto maximum = terrain.Stats.Triangles;
        terrain.Select(nearEye, XMMatrixIdentity(), false, true, true);
        CheckCoverage(terrain);
        const auto nearCount = terrain.Stats.Triangles;
        terrain.Select(farEye, XMMatrixIdentity(), false, true, true);
        CheckCoverage(terrain);
        const auto farCount = terrain.Stats.Triangles;
        Require(farCount < nearCount && nearCount <= maximum, "LOD does not reduce far geometry");

        const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 16.f / 9.f, 1.f, 30000.f);
        int reductions = 0;
        for (int i = 0; i < 16; ++i)
        {
            const float angle = XM_2PI * i / 16.f;
            const XMFLOAT3 eye = { 5896.f + sinf(angle) * 3000.f, 900.f, cosf(angle) * 3000.f };
            const XMVECTOR position = XMLoadFloat3(&eye);
            const XMMATRIX view = XMMatrixLookToLH(position,
                XMVectorSet(cosf(angle), -0.15f, sinf(angle), 0.f), XMVectorSet(0,1,0,0));
            const XMMATRIX vp = view * proj;
            terrain.Select(eye, vp, false, true, true);
            CheckCoverage(terrain);
            std::set<int> expected;
            for (int index : terrain.Selected)
                if (ReferenceVisible(terrain.Nodes[index], vp)) expected.insert(index);
            const auto withoutCulling = terrain.Stats.Triangles;
            terrain.Select(eye, vp, true, true, true);
            const std::set<int> actual(terrain.Selected.begin(), terrain.Selected.end());
            Require(expected == actual, "Culling differs from independent clip-space test");
            if (terrain.Stats.Triangles < withoutCulling) ++reductions;
        }
        Require(reductions > 0, "Culling never reduces submitted geometry");
        terrain.Select(farEye, XMMatrixIdentity(), false, true, false);
        const auto surfaceOnly = terrain.Stats.Triangles;
        terrain.Select(farEye, XMMatrixIdentity(), false, true, true);
        Require(terrain.Stats.Triangles > surfaceOnly, "Skirt switch has no effect");
        if (argc > 2)
        {
            terrain.ExportHeightTiles(argv[2]);
            size_t count = 0;
            for (const auto& entry : std::filesystem::directory_iterator(argv[2]))
                if (entry.path().extension() == ".r16")
                {
                    Require(entry.file_size() == 65 * 65 * 2, "Invalid exported tile size");
                    ++count;
                }
            Require(count == 256, "Wrong exported tile count");
            auto readTile = [&](int x, int z)
            {
                const auto path = std::filesystem::path(argv[2]) /
                    ("height_" + std::to_string(x) + "_" + std::to_string(z) + ".r16");
                std::ifstream file(path, std::ios::binary);
                std::vector<unsigned char> bytes(65 * 65 * 2);
                file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
                Require(static_cast<bool>(file), "Cannot read exported tile");
                return bytes;
            };
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x)
                {
                    const auto tile = readTile(x, z);
                    if (x < 15)
                    {
                        const auto right = readTile(x + 1, z);
                        for (int row = 0; row < 65; ++row)
                            for (int byte = 0; byte < 2; ++byte)
                                Require(tile[(row * 65 + 64) * 2 + byte] == right[row * 65 * 2 + byte],
                                    "Horizontal tile seam differs");
                    }
                    if (z < 15)
                    {
                        const auto below = readTile(x, z + 1);
                        for (int col = 0; col < 65 * 2; ++col)
                            Require(tile[64 * 65 * 2 + col] == below[col], "Vertical tile seam differs");
                    }
                }
        }
        std::cout << "PASS: 16-bit heights, 341 nodes, valid indices/bounds/normals, full LOD coverage, "
            << "16 culling views, skirts. Triangles max/near/far: "
            << maximum << '/' << nearCount << '/' << farCount << '\n';
        return 0;
    }
    catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
