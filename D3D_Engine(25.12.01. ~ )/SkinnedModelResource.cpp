#include "../D3D_Core/pch.h"

#include "SkinnedModelResource.h"

SkinnedModelResource::SkinnedModelResource(
    std::vector<SkinnedMeshPartResource>&& parts)
    : m_parts(std::move(parts))
{
    //아 무 것 도 없 지 롱
}