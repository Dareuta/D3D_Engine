#include "../D3D_Core/pch.h"
#include "StaticMeshResource.h"
#include "AssimpImporterEX.h"
#include "MeshDataEx.h"

StaticMeshResource::StaticMeshResource(
    ID3D11Device* dev,
    const std::wstring& fbxPath,
    const std::wstring& texDir)
{
    MeshData_PNTT cpu;
    if (!AssimpImporterEx::LoadFBX_PNTT_AndMaterials(
        fbxPath, cpu, /*flipUV*/true, /*leftHanded*/true))
    {
        throw std::runtime_error("StaticMeshResource: FBX load failed.");
    }

    if (!m_mesh.Build(dev, cpu))
    {
        throw std::runtime_error("StaticMeshResource: Mesh build failed.");
    }

    m_materials.resize(cpu.materials.size());
    for (size_t i = 0; i < cpu.materials.size(); ++i)
    {
        m_materials[i].Build(dev, cpu.materials[i], texDir);
    }
}