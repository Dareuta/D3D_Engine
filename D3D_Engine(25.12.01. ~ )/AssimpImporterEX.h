#pragma once
#include <string>
#include "MeshDataEx.h"

// Assimp 전방 선언(헤더 의존 최소화)
struct aiScene;
struct aiMesh;

class AssimpImporterEx {
public:
    static bool LoadFBX_PNTT_AndMaterials(
        const std::wstring& path,
        MeshData_PNTT& out,
        bool filpUV = false,   // <- 오타지만 프로젝트 전반과 일치만 하면 OK
        bool leftHanded = true);

    static void ConvertAiMeshToPNTT(const aiMesh* am, MeshData_PNTT& out);

    static void ExtractMaterials(const aiScene* sc, std::vector<MaterialCPU>& out);
};