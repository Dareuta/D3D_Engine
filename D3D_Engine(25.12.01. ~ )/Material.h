// Material.h
#pragma once

#include <string>
#include <memory>
#include <d3d11.h>
#include "MeshDataEx.h"

class Texture2DResource;

// GPU 머티리얼(텍스처 + 상수버퍼)
struct MaterialGPU
{
    MaterialGPU() = default;
    ~MaterialGPU();

    MaterialGPU(const MaterialGPU&) = delete;
    MaterialGPU& operator=(const MaterialGPU&) = delete;

    MaterialGPU(MaterialGPU&& other) noexcept { MoveFrom(other); }
    MaterialGPU& operator=(MaterialGPU&& other) noexcept
    {
        if (this != &other)
        {
            ReleaseAll();
            MoveFrom(other);
        }
        return *this;
    }

    // CPU 머티리얼(MaterialCPU) -> GPU 리소스 빌드
    void Build(ID3D11Device* dev, const MaterialCPU& cpu, const std::wstring& texRoot);

    // 바인딩/언바인딩
    void Bind(ID3D11DeviceContext* ctx) const;
    static void Unbind(ID3D11DeviceContext* ctx);

    // 텍스처 플래그
    bool hasDiffuse = false;
    bool hasNormal = false;
    bool hasSpecular = false;
    bool hasEmissive = false;
    bool hasOpacity = false;

    // ★ 이제 SRV를 직접 들고 있지 않고, Texture2DResource를 shared_ptr로 들고 있음
    std::shared_ptr<Texture2DResource> texDiffuse;
    std::shared_ptr<Texture2DResource> texNormal;
    std::shared_ptr<Texture2DResource> texSpecular;
    std::shared_ptr<Texture2DResource> texEmissive;
    std::shared_ptr<Texture2DResource> texOpacity;

    // FBX diffuseColor (r,g,b,1)
    float baseColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool  useBaseColor = false;          // 디퓨즈 텍스처 없으면 true

    // PS b5용 상수버퍼
    ID3D11Buffer* cbMat = nullptr;

private:
    void ReleaseAll()
    {
        if (cbMat)
        {
            cbMat->Release();
            cbMat = nullptr;
        }

        texDiffuse.reset();
        texNormal.reset();
        texSpecular.reset();
        texEmissive.reset();
        texOpacity.reset();

        hasDiffuse = false;
        hasNormal = false;
        hasSpecular = false;
        hasEmissive = false;
        hasOpacity = false;
        useBaseColor = false;
        // baseColor는 남아 있어도 상관 없음
    }

    void MoveFrom(MaterialGPU& o)
    {
        // 상수버퍼는 ownership 이동
        cbMat = o.cbMat;
        o.cbMat = nullptr;

        texDiffuse = std::move(o.texDiffuse);
        texNormal = std::move(o.texNormal);
        texSpecular = std::move(o.texSpecular);
        texEmissive = std::move(o.texEmissive);
        texOpacity = std::move(o.texOpacity);

        hasDiffuse = o.hasDiffuse;
        hasNormal = o.hasNormal;
        hasSpecular = o.hasSpecular;
        hasEmissive = o.hasEmissive;
        hasOpacity = o.hasOpacity;

        for (int i = 0; i < 4; ++i)
            baseColor[i] = o.baseColor[i];

        useBaseColor = o.useBaseColor;
    }
};
