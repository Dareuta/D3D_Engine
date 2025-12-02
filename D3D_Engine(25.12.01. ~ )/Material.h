// Material.h
#pragma once
#include <string>
#include <d3d11.h>
#include "MeshDataEx.h"

struct MaterialGPU {
   
    void Build(ID3D11Device* dev, const MaterialCPU& cpu, const std::wstring& texRoot);
    void Bind(ID3D11DeviceContext* ctx) const;
    static void Unbind(ID3D11DeviceContext* ctx);


    bool hasDiffuse = false, hasNormal = false, hasSpecular = false, hasEmissive = false, hasOpacity = false;

    ID3D11ShaderResourceView* srvDiffuse = nullptr;
    ID3D11ShaderResourceView* srvNormal = nullptr;
    ID3D11ShaderResourceView* srvSpecular = nullptr;
    ID3D11ShaderResourceView* srvEmissive = nullptr;
    ID3D11ShaderResourceView* srvOpacity = nullptr;

    float baseColor[4] = { 1,1,1,1 }; // FBX diffuseColor (r,g,b,1)
    bool useBaseColor = false;               // diffuse 텍스처 없을 때 true
    ID3D11Buffer* cbMat = nullptr;           // PS b5


    ~MaterialGPU();
};
