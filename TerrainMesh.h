#pragma once
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct TerrainVertex
{
    DirectX::XMFLOAT3 Position;
    DirectX::XMFLOAT3 Normal;
    DirectX::XMFLOAT2 UV;
};

struct TerrainNode
{
    DirectX::XMFLOAT3 Minimum, Maximum;
    std::array<int, 4> Children = { -1, -1, -1, -1 };
    uint32_t IndexStart = 0;
    uint32_t SurfaceIndexCount = 0;
    uint32_t SkirtIndexCount = 0;
    int SampleX = 0, SampleZ = 0, SampleSize = 0, Depth = 0;
};

struct TerrainStats
{
    uint32_t Tiles = 0, Triangles = 0, RejectedNodes = 0;
    std::array<uint32_t, 5> Levels = {};
};

// CPU owns heights, meshes and the quadtree. GPU draws the selected index ranges.
class TerrainMesh
{
public:
    static constexpr int GridCells = 32;
    static constexpr int MaxDepth = 4;
    static constexpr float WorldSize = 8192.f;
    static constexpr float OriginX = 1800.f;
    static constexpr float OriginZ = -4096.f;

    void Load(const char* heightmapPath);
    void Select(const DirectX::XMFLOAT3& eye, const DirectX::XMMATRIX& viewProj,
        bool culling, bool adaptiveLod, bool skirts);
    void ExportHeightTiles(const std::string& directory) const;
    float Height(int x, int z) const;
    int Resolution() const { return mResolution; }
    std::vector<TerrainVertex> Vertices;
    std::vector<uint32_t> Indices;
    std::vector<TerrainNode> Nodes;
    std::vector<int> Selected;
    TerrainStats Stats;

private:
    int BuildNode(int x, int z, int sampleSize, int depth);
    void SelectNode(int index, const DirectX::XMFLOAT3& eye,
        const std::array<DirectX::XMFLOAT4, 6>& planes,
        bool culling, bool adaptiveLod, bool skirts);
    int mResolution = 0;
    uint16_t mMinimumHeight = 0;
    std::vector<uint16_t> mHeights;
};
