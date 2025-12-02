// Material.cpp
#include "../D3D_Core/pch.h"
#include "Material.h"

#include <algorithm>
#include <DirectXTK/WICTextureLoader.h>
#include <DirectXTK/DDSTextureLoader.h>

static HRESULT LoadTex2D(ID3D11Device* dev, const std::wstring& path, ID3D11ShaderResourceView** outSRV)
{
    if (path.empty()) return E_INVALIDARG;

    auto ends_with = [](const std::wstring& s, const std::wstring& suf) {
        if (s.size() < suf.size()) return false;
        return std::equal(suf.rbegin(), suf.rend(), s.rbegin(),
            [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); });
        };

    if (ends_with(path, L".dds"))
        return DirectX::CreateDDSTextureFromFile(dev, path.c_str(), nullptr, outSRV);
    else
        return DirectX::CreateWICTextureFromFile(dev, path.c_str(), nullptr, outSRV);
}

void MaterialGPU::Build(ID3D11Device* dev, const MaterialCPU& cpu, const std::wstring& texRoot)
{
    auto join = [&](const std::wstring& f)->std::wstring { return texRoot + f; };

    if (!cpu.diffuse.empty()) hasDiffuse = SUCCEEDED(LoadTex2D(dev, join(cpu.diffuse), &srvDiffuse));
    if (!cpu.normal.empty()) hasNormal = SUCCEEDED(LoadTex2D(dev, join(cpu.normal), &srvNormal));
    if (!cpu.specular.empty()) hasSpecular = SUCCEEDED(LoadTex2D(dev, join(cpu.specular), &srvSpecular));
    if (!cpu.emissive.empty()) hasEmissive = SUCCEEDED(LoadTex2D(dev, join(cpu.emissive), &srvEmissive));
    if (!cpu.opacity.empty()) hasOpacity = SUCCEEDED(LoadTex2D(dev, join(cpu.opacity), &srvOpacity));

    // FBX diffuseColor -> baseColor
    baseColor[0] = cpu.diffuseColor[0];
    baseColor[1] = cpu.diffuseColor[1];
    baseColor[2] = cpu.diffuseColor[2];
    baseColor[3] = 1.0f;

    // 정책: 디퓨즈 텍스처 없으면 FBX 색을 기본색으로 사용
    useBaseColor = !hasDiffuse;

    // PS b5용 상수버퍼
    struct CBMat { float baseColor[4]; UINT useBaseColor; UINT pad[3]; };
    D3D11_BUFFER_DESC bd{};
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(CBMat);
    dev->CreateBuffer(&bd, nullptr, &cbMat);
}

void MaterialGPU::Bind(ID3D11DeviceContext* ctx) const
{
    // t0~t4 바인딩
    ID3D11ShaderResourceView* srvs[5] =
    { srvDiffuse, srvNormal, srvSpecular, srvEmissive, srvOpacity };
    ctx->PSSetShaderResources(0, 5, srvs);

    // b5 업데이트
    struct CBMat { float baseColor[4]; UINT useBaseColor; UINT pad[3]; } cb{};
    cb.baseColor[0] = baseColor[0];
    cb.baseColor[1] = baseColor[1];
    cb.baseColor[2] = baseColor[2];
    cb.baseColor[3] = baseColor[3];
    cb.useBaseColor = useBaseColor ? 1u : 0u;

    ctx->UpdateSubresource(cbMat, 0, nullptr, &cb, 0, 0);
    ctx->PSSetConstantBuffers(5, 1, &cbMat);
}

void MaterialGPU::Unbind(ID3D11DeviceContext* ctx)
{
    ID3D11ShaderResourceView* nulls[5] = { nullptr,nullptr,nullptr,nullptr,nullptr };
    ctx->PSSetShaderResources(0, 5, nulls);
}

MaterialGPU::~MaterialGPU()
{
    if (cbMat)       cbMat->Release();
    if (srvDiffuse)  srvDiffuse->Release();
    if (srvNormal)   srvNormal->Release();
    if (srvSpecular) srvSpecular->Release();
    if (srvEmissive) srvEmissive->Release();
    if (srvOpacity)  srvOpacity->Release();
}
