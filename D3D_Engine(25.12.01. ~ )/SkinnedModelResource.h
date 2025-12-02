#pragma once

#include <vector>

#include "Material.h"
#include "SkinnedMesh.h"

//TODO: 언젠가 애니메이션 없는 스키닝 모델 리소스를 만들려면, 수정이 필요함(지금당장 필요 없어서 안만듬)

// 스키닝된 모델 한 개(FBX 한 개)를 표현하는 리소스.
//  - 여러 파트(메쉬 + 머티리얼 세트)를 가질 수 있게 해 둔다.
struct SkinnedMeshPartResource
{
    SkinnedMesh                 mesh;
    std::vector<MaterialGPU>    materials;
    // 나중에 필요하면 ownerNode 같은 정보 추가 가능
};

class SkinnedModelResource
{
public:
    SkinnedModelResource() = default;

    // FBX 로딩 후 한 번에 채워넣는 용도
    explicit SkinnedModelResource(std::vector<SkinnedMeshPartResource>&& parts);

    // 복사는 막고, 이동만 허용
    SkinnedModelResource(const SkinnedModelResource&) = delete;
    SkinnedModelResource& operator=(const SkinnedModelResource&) = delete;

    SkinnedModelResource(SkinnedModelResource&&) noexcept = default;
    SkinnedModelResource& operator=(SkinnedModelResource&&) noexcept = default;

    const std::vector<SkinnedMeshPartResource>& GetParts() const { return m_parts; }
    std::vector<SkinnedMeshPartResource>& GetParts() { return m_parts; }

    bool Empty() const { return m_parts.empty(); }

private:
    std::vector<SkinnedMeshPartResource> m_parts;
};
