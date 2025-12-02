// Material.h
#pragma once
#include <string>
#include <d3d11.h>
#include "MeshDataEx.h"

struct MaterialGPU {

	MaterialGPU() = default;
	~MaterialGPU();

	MaterialGPU(const MaterialGPU&) = delete;
	MaterialGPU& operator=(const MaterialGPU&) = delete;

	MaterialGPU(MaterialGPU&& other) noexcept { MoveFrom(other); }
	MaterialGPU& operator=(MaterialGPU&& other) noexcept {
		if (this != &other) {
			ReleaseAll();
			MoveFrom(other);
		}
		return *this;
	}
   
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

private:
	void ReleaseAll() {
		if (cbMat)       cbMat->Release();
		if (srvDiffuse)  srvDiffuse->Release();
		if (srvNormal)   srvNormal->Release();
		if (srvSpecular) srvSpecular->Release();
		if (srvEmissive) srvEmissive->Release();
		if (srvOpacity)  srvOpacity->Release();
		cbMat = nullptr;
		srvDiffuse = srvNormal = srvSpecular = srvEmissive = srvOpacity = nullptr;
	}
	void MoveFrom(MaterialGPU& o) {
		cbMat = o.cbMat;           o.cbMat = nullptr;
		srvDiffuse = o.srvDiffuse; o.srvDiffuse = nullptr;
		srvNormal = o.srvNormal;  o.srvNormal = nullptr;
		srvSpecular = o.srvSpecular; o.srvSpecular = nullptr;
		srvEmissive = o.srvEmissive; o.srvEmissive = nullptr;
		srvOpacity = o.srvOpacity; o.srvOpacity = nullptr;

		hasDiffuse = o.hasDiffuse;
		hasNormal = o.hasNormal;
		hasSpecular = o.hasSpecular;
		hasEmissive = o.hasEmissive;
		hasOpacity = o.hasOpacity;
		for (int i = 0; i < 4; ++i) baseColor[i] = o.baseColor[i];
		useBaseColor = o.useBaseColor;
	}
};
