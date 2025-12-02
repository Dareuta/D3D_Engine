// ResourceManager.h
#pragma once

#include <memory>
#include <string>
#include <unordered_map>

struct ID3D11Device;
struct ID3D11ShaderResourceView;

class Texture2DResource;
class StaticMeshResource;
class SkinnedModelResource;

class ResourceManager final
{
public:
    // 싱글톤 접근
    static ResourceManager& Instance();

    // TutorialApp::InitD3D 이후, m_pDevice 만들어진 다음에 한 번 호출
    void Initialize(ID3D11Device* device);

    // 프로그램 끝날 때 호출 (OnUninitialize / UninitScene 직전이나 그 즈음)
    void Shutdown();

    bool IsInitialized() const noexcept { return m_device != nullptr; }
    ID3D11Device* GetDevice() const noexcept { return m_device; }

    // ---------------------------------------------------------
    // 1) 텍스처 2D
    //    path: 전체 경로 (예: L"./Resource/Textures/xxx.dds")
    // ---------------------------------------------------------
    std::shared_ptr<Texture2DResource>
        LoadTexture2D(const std::wstring& path);

    // ---------------------------------------------------------
    // 2) Static Mesh + Materials (PNTT)
    //
    //    fbxPath : FBX 파일 경로
    //    texDir  : FBX가 사용하는 텍스처 root (예: L"./Resource/Textures/BoxHuman/")
    //
    //    같은 (fbxPath, texDir)로 두 번 이상 호출해도
    //    GPU 리소스는 한 번만 만들어지고 공유됨.
    // ---------------------------------------------------------
    std::shared_ptr<StaticMeshResource>
        LoadStaticMesh(const std::wstring& fbxPath,
            const std::wstring& texDir);

    // ---------------------------------------------------------
    // 3) Skinned Mesh + Materials
    //    (지금은 틀만 잡아두는 용도. 나중에 스키닝 쪽 연결)
    // ---------------------------------------------------------
    std::shared_ptr<SkinnedModelResource>
        LoadSkinnedModel(const std::wstring& fbxPath,
            const std::wstring& texDir);

private:
    ResourceManager() = default;
    ~ResourceManager() = default;

    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    // (fbxPath, texDir) 같이 두 개를 key로 쓰고 싶을 때
    static std::wstring MakeKey(const std::wstring& a,
        const std::wstring& b);

    ID3D11Device* m_device = nullptr; // 우리가 AddRef 안 함. TutorialApp이 소유.

    using TexCache = std::unordered_map<std::wstring, std::weak_ptr<Texture2DResource>>;
    using StaticMeshCache = std::unordered_map<std::wstring, std::weak_ptr<StaticMeshResource>>;
    using SkinnedMeshCache = std::unordered_map<std::wstring, std::weak_ptr<SkinnedModelResource>>;

    TexCache        m_texCache;
    StaticMeshCache m_staticCache;
    SkinnedMeshCache m_skinnedCache;
};
