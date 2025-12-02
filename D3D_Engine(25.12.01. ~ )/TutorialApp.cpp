//================================================================================================
// TutorialApp.cpp
//================================================================================================
#include "../D3D_Core/pch.h"
#include "../D3D_Core/Helper.h"
#include "TutorialApp.h"
#include "AssimpImporterEx.h"

#include <d3dcompiler.h>
#include <Directxtk/DDSTextureLoader.h>  // CreateDDSTextureFromFile
#include <DirectXTK/WICTextureLoader.h>

#pragma comment (lib, "d3d11.lib")
#pragma comment(lib,"d3dcompiler.lib")

//================================================================================================

// GPU가 기대하고 있는 메모리 레이아웃과 1대1로 대응해야한다
struct ConstantBuffer // 상수버퍼
{
	Matrix mWorld;
	Matrix mView;
	Matrix mProjection;
	Matrix mWorldInvTranspose;

	Vector4 vLightDir;
	Vector4 vLightColor;
};

struct BlinnPhongCB
{
	Vector4 EyePosW;   // (ex,ey,ez,1)
	Vector4 kA;        // (ka.r,ka.g,ka.b,0)
	Vector4 kSAlpha;   // (ks, alpha, 0, 0)
	Vector4 I_ambient; // (Ia.r,Ia.g,Ia.b,0)
};


struct UseCB
{
	UINT  useDiffuse, useNormal, useSpecular, useEmissive;
	UINT  useOpacity;
	float alphaCut;
	float pad[2];
};

bool TutorialApp::OnInitialize()
{
	if (!InitD3D())
		return false;

#ifdef _DEBUG
	if (!InitImGUI())
		return false;
#endif

	if (!InitScene())
		return false;

	return true;
}

void TutorialApp::OnUninitialize()
{
	UninitScene();

#ifdef _DEBUG
	UninitImGUI();
#endif

	UninitD3D();
}

// 공용 애니메이션 컨트롤 UI
static void AnimUI(const char* label,
	bool& play, bool& loop, float& speed, double& t,
	double durationSec,
	const std::function<void(double)>& evalPose)
{
	if (ImGui::TreeNode(label)) {
		ImGui::Checkbox("Play", &play); ImGui::SameLine();
		ImGui::Checkbox("Loop", &loop);

		ImGui::DragFloat("Speed (x)", &speed, 0.01f, -4.0f, 4.0f, "%.2f");

		const float maxT = (float)((durationSec > 0.0) ? durationSec : 1.0);
		float tUI = (float)t;
		if (ImGui::SliderFloat("Time (sec)", &tUI, 0.0f, maxT, "%.3f")) {
			t = (double)tUI;
			if (evalPose) evalPose(t);
		}

		if (ImGui::Button("Rewind")) { t = 0.0; if (evalPose) evalPose(t); }
		ImGui::SameLine();
		if (ImGui::Button("Go End")) { t = durationSec; if (evalPose) evalPose(t); }

		ImGui::TreePop();
	}
}

bool TutorialApp::CreateShadowResources(ID3D11Device* dev)
{
	// 1) Shadow map: R32 typeless + DSV + SRV
	D3D11_TEXTURE2D_DESC td{};
	td.Width = mShadowW; td.Height = mShadowH;
	td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R32_TYPELESS;
	td.SampleDesc.Count = 1;
	td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	HR_T(dev->CreateTexture2D(&td, nullptr, mShadowTex.GetAddressOf()));

	D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};
	dsvd.Format = DXGI_FORMAT_D32_FLOAT;
	dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	HR_T(dev->CreateDepthStencilView(mShadowTex.Get(), &dsvd, mShadowDSV.GetAddressOf()));

	D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
	srvd.Format = DXGI_FORMAT_R32_FLOAT;
	srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvd.Texture2D.MipLevels = 1;
	HR_T(dev->CreateShaderResourceView(mShadowTex.Get(), &srvd, mShadowSRV.GetAddressOf()));

	// 2) Comparison sampler (PS s1)
	D3D11_SAMPLER_DESC sd{};
	sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
	sd.MinLOD = 0; sd.MaxLOD = D3D11_FLOAT32_MAX;
	HR_T(dev->CreateSamplerState(&sd, mSamShadowCmp.GetAddressOf()));

	// 3) Depth-bias Rasterizer (그림자 패스 전용)
	D3D11_RASTERIZER_DESC rs{};
	rs.FillMode = D3D11_FILL_SOLID;
	rs.CullMode = D3D11_CULL_BACK;
	rs.DepthClipEnable = TRUE;
	rs.DepthBias = (INT)mShadowDepthBias;      // 예: 100~2000 구간에서 튜닝
	rs.SlopeScaledDepthBias = mShadowSlopeBias; // 예: 1.0~2.0
	rs.DepthBiasClamp = 0.0f;
	HR_T(dev->CreateRasterizerState(&rs, mRS_ShadowBias.GetAddressOf()));

	// 4) Viewport
	mShadowVP = { 0, 0, (float)mShadowW, (float)mShadowH, 0.0f, 1.0f };

	// 5) ShadowCB (b6) : LVP + Params
	D3D11_BUFFER_DESC cbd{};
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.Usage = D3D11_USAGE_DEFAULT;
	cbd.ByteWidth = sizeof(DirectX::XMFLOAT4X4) + sizeof(DirectX::XMFLOAT4);
	HR_T(dev->CreateBuffer(&cbd, nullptr, mCB_Shadow.GetAddressOf()));

	return true;
}

void TutorialApp::UpdateLightCameraAndShadowCB(ID3D11DeviceContext* ctx)
{
	using namespace DirectX::SimpleMath;

	// 1) 타겟 지점
	const Vector3 camPos = m_Camera.m_World.Translation();
	const Vector3 camDir = m_Camera.GetForward(); // normalized
	const Vector3 lookAt = mShUI.followCamera
		? (camPos + camDir * mShUI.focusDist)
		: mShUI.manualTarget;

	// 2) 라이트 방향 (광선 진행 방향 = 아래로 향함)
	Vector3 lightDir = Vector3::TransformNormal(
		Vector3::UnitZ,
		Matrix::CreateFromYawPitchRoll(m_LightYaw, m_LightPitch, 0.0f)
	);
	lightDir.Normalize();

	// 3) 라이트 "계산용 위치"
	const Vector3 lightPos = mShUI.useManualPos
		? mShUI.manualPos
		: (lookAt - lightDir * mShUI.lightDist);

	// 4) up 특이점 회피
	const Vector3 up = (fabsf(lightDir.y) > 0.97f) ? Vector3::UnitZ : Vector3::UnitY;

	// 5) 자동 커버(원근 프러스텀) 또는 직교(옵션)
	if (mShUI.autoCover) {
		const float fovY = XMConvertToRadians(m_FovDegree);
		const float aspect = float(m_ClientWidth) / float(m_ClientHeight);
		const float halfH = tanf(0.5f * fovY) * mShUI.focusDist;
		const float halfW = halfH * aspect;
		const float r = sqrtf(halfW * halfW + halfH * halfH) * mShUI.coverMargin;
		const float d = mShUI.lightDist;

		mShadowNear = max(0.01f, d - r);
		mShadowFar = d + r;
		//mShadowNear = 0.01f;
		//mShadowFar = 500.0f;
		mShadowFovY = 2.0f * atanf(r / max(1e-4f, d));
	}

	const float aspectSh = float(mShadowW) / float(mShadowH);

	const Matrix V = Matrix::CreateLookAt(lightPos, lookAt, up);

	Matrix P;
	if (mShUI.useOrtho) {
		// 직교 투영(안정적): autoCover 계산 r을 그대로 씀
		const float fovY = XMConvertToRadians(m_FovDegree);
		const float aspect = float(m_ClientWidth) / float(m_ClientHeight);
		const float halfH = tanf(0.5f * fovY) * mShUI.focusDist * mShUI.coverMargin;
		const float halfW = halfH * aspect;
		const float l = -halfW, r = +halfW, b = -halfH, t = +halfH;
		P = Matrix::CreateOrthographicOffCenter(l, r, b, t, mShadowNear, mShadowFar);
	}
	else {
		// 기존: 원근 투영
		P = Matrix::CreatePerspectiveFieldOfView(mShadowFovY, aspectSh, mShadowNear, mShadowFar);
	}

	mLightView = V;
	mLightProj = P;

	// 6) b6 업로드
	struct ShadowCB_ { Matrix LVP; Vector4 Params; } scb;
	scb.LVP = XMMatrixTranspose(V * P);
	scb.Params = Vector4(/*compareBias*/ 0.0f, 1.0f / mShadowW, 1.0f / mShadowH, 0.0f); // 비교바이어스 0으로 운용
	ctx->UpdateSubresource(mCB_Shadow.Get(), 0, nullptr, &scb, 0, 0);

	ID3D11Buffer* b6 = mCB_Shadow.Get();
	ctx->VSSetConstantBuffers(6, 1, &b6);
	ctx->PSSetConstantBuffers(6, 1, &b6);

	// 7) 메인 조명 CB(vLightDir)도 동일 정의로 유지
	//    셰이딩에서 NdotL = dot(N, -vLightDir)를 사용하도록 HLSL 확인
}

bool TutorialApp::CreateDepthOnlyShaders(ID3D11Device* dev)
{
	using Microsoft::WRL::ComPtr;
	ComPtr<ID3DBlob> vsPntt, vsSkin, psDepth;

	HR_T(CompileShaderFromFile(L"Shader/DepthOnly_VS.hlsl", "main", "vs_5_0", vsPntt.GetAddressOf()));
	HR_T(CompileShaderFromFile(L"Shader/DepthOnly_SkinnedVS.hlsl", "main", "vs_5_0", vsSkin.GetAddressOf()));
	HR_T(CompileShaderFromFile(L"Shader/DepthOnly_PS.hlsl", "main", "ps_5_0", psDepth.GetAddressOf()));

	HR_T(dev->CreateVertexShader(vsPntt->GetBufferPointer(), vsPntt->GetBufferSize(), nullptr, mVS_Depth.GetAddressOf()));
	HR_T(dev->CreateVertexShader(vsSkin->GetBufferPointer(), vsSkin->GetBufferSize(), nullptr, mVS_DepthSkinned.GetAddressOf()));
	HR_T(dev->CreatePixelShader(psDepth->GetBufferPointer(), psDepth->GetBufferSize(), nullptr, mPS_Depth.GetAddressOf()));

	// IL: PNTT
	static const D3D11_INPUT_ELEMENT_DESC IL_PNTT[] = {
		{"POSITION",0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"NORMAL",  0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD",0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
	};
	HR_T(dev->CreateInputLayout(IL_PNTT, _countof(IL_PNTT),
		vsPntt->GetBufferPointer(), vsPntt->GetBufferSize(), mIL_PNTT.GetAddressOf()));

	// IL: PNTT + Bone
	static const D3D11_INPUT_ELEMENT_DESC IL_SKIN[] = {
		{"POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"TANGENT",      0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT,      0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
		{"BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0},
	};
	HR_T(dev->CreateInputLayout(IL_SKIN, _countof(IL_SKIN),
		vsSkin->GetBufferPointer(), vsSkin->GetBufferSize(), mIL_PNTT_BW.GetAddressOf()));

	return true;
}

void TutorialApp::BuildLightCameraAndUpload(ID3D11DeviceContext* ctx,
	const DirectX::SimpleMath::Vector3& camPos,
	const DirectX::SimpleMath::Vector3& camForward,
	const DirectX::SimpleMath::Vector3& lightDir_unit,
	float focusDist, float lightDist)
{
	// === 라이트 카메라 구성 ===
	using namespace DirectX;
	const Vector3 eyePos = m_Camera.m_World.Translation();
	const Vector3 camFwd = m_Camera.GetForward();

	// 1) lookAt 결정
	Vector3 lookAt;
	if (mShUI.followCamera) {
		lookAt = eyePos + camFwd * mShUI.focusDist;
	}
	else {
		lookAt = XMLoadFloat3(&mShUI.manualTarget);
	}

	// 2) 라이트 방향 (기존 yaw/pitch → vLightDir)
	Vector3 Ldir(vLightDir.x, vLightDir.y, vLightDir.z);
	Ldir.Normalize();

	// 3) lightPos 결정 (directional을 '계산용 위치'로)
	Vector3 lightPos;
	if (mShUI.useManualPos) {
		lightPos = XMLoadFloat3(&mShUI.manualPos);
	}
	else {
		lightPos = lookAt - Ldir * mShUI.lightDist;
	}

	// 4) View 행렬
	const Matrix lightView = Matrix::CreateLookAt(lightPos, lookAt, Vector3::UnitY);

	// 5) Proj 행렬
	float aspectSh = float(mShadowW) / float(mShadowH);

	// “카메라 화면의 focusDist 평면 직사각형”을 라이트 프러스텀이 덮게(자동 커버)
	if (mShUI.autoCover) {
		// 카메라 파라미터에서 반높이/반너비
		const float camFovY = XMConvertToRadians(m_FovDegree);
		const float halfH = tanf(camFovY * 0.5f) * mShUI.focusDist;
		const float halfW = halfH * (m_ClientWidth / float(m_ClientHeight));
		const float r = sqrtf(halfW * halfW + halfH * halfH) * mShUI.coverMargin;

		// lightDist 기준으로 FOV / 클립 자동 산출
		const float d = mShUI.lightDist;
		const float nz = max(d - r, 0.01f);
		const float fz = d + r;
		mShadowNear = nz;
		mShadowFar = fz;
		mShadowFovY = 2.0f * atanf(r / d); // 라이트 FOVY 자동화
	}

	// 최종 투영
	const Matrix lightProj = Matrix::CreatePerspectiveFieldOfView(mShadowFovY, aspectSh, mShadowNear, mShadowFar);
	const Matrix lightVP = lightView * lightProj;

	// (디버그용 보관)
	mLightView = lightView;
	mLightProj = lightProj;

	// 4) CB(b6) 업로드 (Shared.hlsli: LightViewProj, ShadowParams)
	struct ShadowCB_ {
		DirectX::XMFLOAT4X4 LVP;
		DirectX::XMFLOAT4   Params; // x=bias(미사용), y=1/w, z=1/h, w=0
	} scb{};

	auto LVP = (mLightView * mLightProj).Transpose();
	XMStoreFloat4x4(&scb.LVP, LVP);
	scb.Params = { 0.0f, 1.0f / mShadowW, 1.0f / mShadowH, 0.0f };

	ctx->UpdateSubresource(mCB_Shadow.Get(), 0, nullptr, &scb, 0, 0);
	ID3D11Buffer* b6 = mCB_Shadow.Get();
	ctx->VSSetConstantBuffers(6, 1, &b6); // b6: ShadowCB
	ctx->PSSetConstantBuffers(6, 1, &b6);
}

void TutorialApp::RenderShadowPass(ID3D11DeviceContext* ctx,
	const DirectX::SimpleMath::Vector3& camPos,
	const DirectX::SimpleMath::Vector3& camForward,
	const DirectX::SimpleMath::Vector3& lightDir_unit,
	float focusDist, float lightDist)
{
	// 0) LightViewProj 계산 + CB(b6) 업로드
	BuildLightCameraAndUpload(ctx, camPos, camForward, lightDir_unit, focusDist, lightDist);

	// 1) 타깃 설정 (컬러 RTV 없음, DSV만)
	ctx->OMSetRenderTargets(0, nullptr, mShadowDSV.Get());
	ctx->ClearDepthStencilView(mShadowDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
	ctx->RSSetViewports(1, &mShadowVP);
	ctx->RSSetState(mRS_ShadowBias.Get());

	// 2) 공용 상태
	float blend[4] = { 0,0,0,0 };
	ctx->OMSetBlendState(nullptr, blend, 0xFFFFFFFF);
	ID3D11SamplerState* cmp = mSamShadowCmp.Get();
	ctx->PSSetSamplers(1, 1, &cmp); // s1

	ID3D11SamplerState* lin = m_pSamplerLinear;
	ctx->PSSetSamplers(0, 1, &lin); // s0 (samLinear) - opacity 샘플링용	
}

//================================================================================================

void TutorialApp::OnUpdate()
{
	static float tHold = 0.0f;
	if (!mDbg.freezeTime) tHold = GameTimer::m_Instance->TotalTime();
	float t = tHold;

	XMMATRIX mSpin = XMMatrixRotationY(t * spinSpeed);

	XMMATRIX mScaleA = XMMatrixScaling(cubeScale.x, cubeScale.y, cubeScale.z);
	XMMATRIX mTranslateA = XMMatrixTranslation(cubeTransformA.x, cubeTransformA.y, cubeTransformA.z);
	m_World = mScaleA * mSpin * mTranslateA;

	// TutorialApp.cpp::OnUpdate()

	const double dt = (double)GameTimer::m_Instance->DeltaTime();

	// --- BoxHuman ---
	if (mBoxRig) {
		if (!mDbg.freezeTime && mBoxAC.play) mBoxAC.t += dt * mBoxAC.speed;
		const double durSec = mBoxRig->GetClipDurationSec();
		if (durSec > 0.0) {
			if (mBoxAC.loop) {
				mBoxAC.t = fmod(mBoxAC.t, durSec); if (mBoxAC.t < 0.0) mBoxAC.t += durSec;
			}
			else {
				if (mBoxAC.t >= durSec) { mBoxAC.t = durSec; mBoxAC.play = false; } // 끝에서 정지
				if (mBoxAC.t < 0.0) { mBoxAC.t = 0.0;   mBoxAC.play = false; } // 앞에서 정지
			}
		}
		mBoxRig->EvaluatePose(mBoxAC.t, mBoxAC.loop);  // ← loop 전달
	}

	// --- Skinned ---
	if (mSkinRig) {
		if (!mDbg.freezeTime && mSkinAC.play) mSkinAC.t += dt * mSkinAC.speed;
		const double durSec = mSkinRig->DurationSec();
		if (durSec > 0.0) {
			if (mSkinAC.loop) {
				mSkinAC.t = fmod(mSkinAC.t, durSec); if (mSkinAC.t < 0.0) mSkinAC.t += durSec;
			}
			else {
				if (mSkinAC.t >= durSec) { mSkinAC.t = durSec; mSkinAC.play = false; }
				if (mSkinAC.t < 0.0) { mSkinAC.t = 0.0;   mSkinAC.play = false; }
			}
		}
		mSkinRig->EvaluatePose(mSkinAC.t, mSkinAC.loop);
	}


}

//================================================================================================
//////////////////////////////////////////////////////////////////////////////////////////////////
//================================================================================================

void TutorialApp::OnRender()
{
	auto* ctx = m_pDeviceContext;

	// ───────────────────────────────────────────────────────────────
	// 0) 라이트 카메라/섀도우 CB 업데이트 
	// ───────────────────────────────────────────────────────────────
	UpdateLightCameraAndShadowCB(ctx); // mLightView, mLightProj, mShadowVP, mCB_Shadow

	// ───────────────────────────────────────────────────────────────
	// 1) 기본 파라미터 클램프 + 메인 RT 클리어
	// ───────────────────────────────────────────────────────────────
	if (m_FovDegree < 10.0f)       m_FovDegree = 10.0f;
	else if (m_FovDegree > 120.0f) m_FovDegree = 120.0f;
	if (m_Near < 0.0001f)          m_Near = 0.0001f;
	float minFar = m_Near + 0.001f;
	if (m_Far < minFar)            m_Far = minFar;

	const float aspect = m_ClientWidth / (float)m_ClientHeight;
	m_Projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(m_FovDegree), aspect, m_Near, m_Far);

	// RS 선택
	if (mDbg.wireframe && m_pWireRS)         ctx->RSSetState(m_pWireRS);
	else if (mDbg.cullNone && m_pDbgRS)      ctx->RSSetState(m_pDbgRS);
	else                                     ctx->RSSetState(m_pCullBackRS);

	const float clearColor[4] = { color[0], color[1], color[2], color[3] };
	ctx->ClearRenderTargetView(m_pRenderTargetView, clearColor);
	ctx->ClearDepthStencilView(m_pDepthStencilView, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

	// ───────────────────────────────────────────────────────────────
	// 2) 공통 CB0(b0) / Blinn(b1) 업로드 (메인 카메라 기준)
	// ───────────────────────────────────────────────────────────────
	Matrix view; m_Camera.GetViewMatrix(view);
	Matrix viewNoTrans = view; viewNoTrans._41 = viewNoTrans._42 = viewNoTrans._43 = 0.0f;

	ConstantBuffer cb{};
	cb.mWorld = XMMatrixTranspose(Matrix::Identity);
	cb.mWorldInvTranspose = XMMatrixInverse(nullptr, Matrix::Identity);
	cb.mView = XMMatrixTranspose(view);
	cb.mProjection = XMMatrixTranspose(m_Projection);

	// 디렉셔널 라이트(dir from yaw/pitch)
	XMMATRIX R = XMMatrixRotationRollPitchYaw(m_LightPitch, m_LightYaw, 0.0f);
	XMVECTOR base = XMVector3Normalize(XMVectorSet(0, 0, 1, 0));
	XMVECTOR L = XMVector3Normalize(XMVector3TransformNormal(base, R));
	Vector3  dirV = { XMVectorGetX(L), XMVectorGetY(L), XMVectorGetZ(L) };
	cb.vLightDir = Vector4(dirV.x, dirV.y, dirV.z, 0.0f);
	cb.vLightColor = Vector4(m_LightColor.x * m_LightIntensity,
		m_LightColor.y * m_LightIntensity,
		m_LightColor.z * m_LightIntensity, 1.0f);

	ctx->UpdateSubresource(m_pConstantBuffer, 0, nullptr, &cb, 0, 0);
	ctx->VSSetConstantBuffers(0, 1, &m_pConstantBuffer);
	ctx->PSSetConstantBuffers(0, 1, &m_pConstantBuffer);

	// b1
	BlinnPhongCB bp{};
	const Vector3 eye = m_Camera.m_World.Translation();
	bp.EyePosW = Vector4(eye.x, eye.y, eye.z, 1);
	bp.kA = Vector4(m_Ka.x, m_Ka.y, m_Ka.z, 0);
	bp.kSAlpha = Vector4(m_Ks, m_Shininess, 0, 0);
	bp.I_ambient = Vector4(m_Ia.x, m_Ia.y, m_Ia.z, 0);
	ctx->UpdateSubresource(m_pBlinnCB, 0, nullptr, &bp, 0, 0);
	ctx->PSSetConstantBuffers(1, 1, &m_pBlinnCB);

	// 공통 셰이더(정적 메쉬) 기본 바인드
	ctx->IASetInputLayout(m_pMeshIL);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->VSSetShader(m_pMeshVS, nullptr, 0);
	ctx->PSSetShader(m_pMeshPS, nullptr, 0);
	if (m_pSamplerLinear) ctx->PSSetSamplers(0, 1, &m_pSamplerLinear);

	// 섀도우용 파라미터(한 번만)
	struct ShadowCB_ { Matrix LVP; Vector4 Params; } scb;
	scb.LVP = XMMatrixTranspose(mLightView * mLightProj);
	scb.Params = Vector4(mShadowCmpBias, 1.0f / mShadowW, 1.0f / mShadowH, 0.0f);
	ctx->UpdateSubresource(mCB_Shadow.Get(), 0, nullptr, &scb, 0, 0);
	ID3D11Buffer* b6 = mCB_Shadow.Get();
	ctx->VSSetConstantBuffers(6, 1, &b6);
	ctx->PSSetConstantBuffers(6, 1, &b6);

	// ───────────────────────────────────────────────────────────────
	// 3) SHADOW PASS (DepthOnly)  
	// ───────────────────────────────────────────────────────────────
	ID3D11RasterizerState* rsBeforeShadow = nullptr;
	ctx->RSGetState(&rsBeforeShadow); // AddRef

	{
		// t5 언바인드, DSV only
		ID3D11ShaderResourceView* nullSRV[1] = { nullptr };
		ctx->PSSetShaderResources(5, 1, nullSRV);
		ctx->OMSetRenderTargets(0, nullptr, mShadowDSV.Get());
		ctx->ClearDepthStencilView(mShadowDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

		// 라이트용 VP/RS
		ctx->RSSetViewports(1, &mShadowVP);
		if (mRS_ShadowBias) ctx->RSSetState(mRS_ShadowBias.Get());

		// Depth 전용 셰이더 바인드
		// 정적: m_pMeshVS + mPS_Depth / 스키닝: mVS_DepthSkinned + mPS_Depth
		// (정적 먼저 쓰도록 정리)
		auto DrawDepth_Static = [&](StaticMesh& mesh, const std::vector<MaterialGPU>& mtls, const Matrix& world, bool alphaCut)
			{
				// b0: 라이트 View/Proj 로 교체
				ConstantBuffer cbd = cb;
				cbd.mWorld = XMMatrixTranspose(world);
				cbd.mWorldInvTranspose = world.Invert();
				cbd.mView = XMMatrixTranspose(mLightView);
				cbd.mProjection = XMMatrixTranspose(mLightProj);
				ctx->UpdateSubresource(m_pConstantBuffer, 0, nullptr, &cbd, 0, 0);
				ctx->VSSetConstantBuffers(0, 1, &m_pConstantBuffer);

				ctx->IASetInputLayout(m_pMeshIL);
				ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				ctx->VSSetShader(mVS_Depth.Get(), nullptr, 0);    // 깊이 전용 VS를 따로 두지 않는 경우
				ctx->PSSetShader(mPS_Depth.Get(), nullptr, 0);

				for (size_t i = 0; i < mesh.Ranges().size(); ++i) {
					const auto& r = mesh.Ranges()[i];
					const auto& mat = mtls[r.materialIndex];
					const bool isCut = mat.hasOpacity;

					if (alphaCut != isCut) continue;

					UseCB use{};
					use.useOpacity = isCut ? 1u : 0u;
					use.alphaCut = isCut ? mShadowAlphaCut : -1.0f; // 컷아웃이면 clip() 활성
					ctx->UpdateSubresource(m_pUseCB, 0, nullptr, &use, 0, 0);
					ctx->PSSetConstantBuffers(2, 1, &m_pUseCB);

					mat.Bind(ctx);            // opacity 텍스처를 PS에서 clip()에 이용
					mesh.DrawSubmesh(ctx, (UINT)i);
					MaterialGPU::Unbind(ctx);
				}
			};

		if (mTreeX.enabled) { Matrix W = ComposeSRT(mTreeX);  DrawDepth_Static(gTree, gTreeMtls, W, false); DrawDepth_Static(gTree, gTreeMtls, W, true); }
		if (mCharX.enabled) { Matrix W = ComposeSRT(mCharX);  DrawDepth_Static(gChar, gCharMtls, W, false); DrawDepth_Static(gChar, gCharMtls, W, true); }
		if (mZeldaX.enabled) { Matrix W = ComposeSRT(mZeldaX); DrawDepth_Static(gZelda, gZeldaMtls, W, false); DrawDepth_Static(gZelda, gZeldaMtls, W, true); }

		if (mBoxRig && mBoxX.enabled)
		{
			// b0: 라이트 뷰/프로젝션으로 업데이트
			ConstantBuffer cbd = cb;
			const Matrix W = ComposeSRT(mBoxX);
			cbd.mWorld = XMMatrixTranspose(W);
			cbd.mWorldInvTranspose = Matrix::Identity;       // Rigid는 VS에서 필요 없으면 Identity로
			cbd.mView = XMMatrixTranspose(mLightView);
			cbd.mProjection = XMMatrixTranspose(mLightProj);
			ctx->UpdateSubresource(m_pConstantBuffer, 0, nullptr, &cbd, 0, 0);
			ctx->VSSetConstantBuffers(0, 1, &m_pConstantBuffer);

			// IL/VS/PS를 depth 전용으로
			ctx->IASetInputLayout(mIL_PNTT.Get());
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ctx->VSSetShader(mVS_Depth.Get(), nullptr, 0);
			ctx->PSSetShader(mPS_Depth.Get(), nullptr, 0);

			// 알파 컷아웃 재질 대응(있다면 clip)
			UseCB use{};
			use.useOpacity = 1u;                  // Rigid 내부에서 재질 분기한다면 그대로 둬도 OK
			use.alphaCut = mShadowAlphaCut;     // ImGui에서 쓰는 값
			ctx->UpdateSubresource(m_pUseCB, 0, nullptr, &use, 0, 0);
			ctx->PSSetConstantBuffers(2, 1, &m_pUseCB);

			// RigidSkeletal 깊이 드로우 (시그니처는 네 프로젝트에 맞춰)
			mBoxRig->DrawDepthOnly(
				ctx, W,
				mLightView, mLightProj,
				m_pConstantBuffer,    // b0
				m_pUseCB,             // b2
				mVS_Depth.Get(),
				mPS_Depth.Get(),
				mIL_PNTT.Get(),
				mShadowAlphaCut
			);
		}

		// 스키닝 깊이
		if (mSkinRig && mSkinX.enabled)
		{
			ctx->IASetInputLayout(mIL_PNTT_BW.Get());
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ctx->VSSetShader(mVS_DepthSkinned.Get(), nullptr, 0);
			ctx->PSSetShader(mPS_Depth.Get(), nullptr, 0);

			// b0를 라이트 VP로 세팅
			ConstantBuffer cbd = cb;
			cbd.mWorld = XMMatrixTranspose(ComposeSRT(mSkinX));
			cbd.mWorldInvTranspose = Matrix::Identity; // 스키닝에서는 VS에서 처리할 수 있음
			cbd.mView = XMMatrixTranspose(mLightView);
			cbd.mProjection = XMMatrixTranspose(mLightProj);
			ctx->UpdateSubresource(m_pConstantBuffer, 0, nullptr, &cbd, 0, 0);
			ctx->VSSetConstantBuffers(0, 1, &m_pConstantBuffer);

			mSkinRig->DrawDepthOnly(
				ctx, ComposeSRT(mSkinX),
				mLightView, mLightProj,
				m_pConstantBuffer,  // b0
				m_pUseCB,           // b2 (alphaCut 제어)
				m_pBoneCB,          // b4
				mVS_DepthSkinned.Get(),
				mPS_Depth.Get(),
				mIL_PNTT_BW.Get(),
				mShadowAlphaCut
			);
		}

		// 메인 RT 복구
		ID3D11RenderTargetView* rtv = m_pRenderTargetView;
		ctx->OMSetRenderTargets(1, &rtv, m_pDepthStencilView);

		D3D11_VIEWPORT vp{};
		vp.TopLeftX = 0; vp.TopLeftY = 0;
		vp.Width = (float)m_ClientWidth; vp.Height = (float)m_ClientHeight;
		vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
		ctx->RSSetViewports(1, &vp);
	}

	ctx->RSSetState(rsBeforeShadow);
	SAFE_RELEASE(rsBeforeShadow);

	// ───────────────────────────────────────────────────────────────
	// 4) SKYBOX (선택)
	// ───────────────────────────────────────────────────────────────
	if (mDbg.showSky)
	{
		ID3D11RasterizerState* oldRS = nullptr; ctx->RSGetState(&oldRS);
		ID3D11DepthStencilState* oldDSS = nullptr; UINT oldRef = 0; ctx->OMGetDepthStencilState(&oldDSS, &oldRef);

		ctx->RSSetState(m_pSkyRS);
		ctx->OMSetDepthStencilState(m_pSkyDSS, 0);

		ctx->IASetInputLayout(m_pSkyIL);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(m_pSkyVS, nullptr, 0);
		ctx->PSSetShader(m_pSkyPS, nullptr, 0);

		ConstantBuffer skyCB{};
		skyCB.mWorld = XMMatrixTranspose(Matrix::Identity);
		skyCB.mView = XMMatrixTranspose(viewNoTrans);
		skyCB.mProjection = XMMatrixTranspose(m_Projection);
		skyCB.mWorldInvTranspose = Matrix::Identity;
		ctx->UpdateSubresource(m_pConstantBuffer, 0, nullptr, &skyCB, 0, 0);
		ctx->VSSetConstantBuffers(0, 1, &m_pConstantBuffer);

		ctx->PSSetShaderResources(0, 1, &m_pSkySRV);
		ctx->PSSetSamplers(0, 1, &m_pSkySampler);

		UINT stride = sizeof(DirectX::XMFLOAT3), offset = 0;
		ctx->IASetVertexBuffers(0, 1, &m_pSkyVB, &stride, &offset);
		ctx->IASetIndexBuffer(m_pSkyIB, DXGI_FORMAT_R16_UINT, 0);
		ctx->DrawIndexed(36, 0, 0);

		ID3D11ShaderResourceView* null0[1] = { nullptr };
		ctx->PSSetShaderResources(0, 1, null0);
		ctx->RSSetState(oldRS);
		ctx->OMSetDepthStencilState(oldDSS, oldRef);
		SAFE_RELEASE(oldRS); SAFE_RELEASE(oldDSS);

		// 메쉬 셋업 복구
		ctx->IASetInputLayout(m_pMeshIL);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(m_pMeshVS, nullptr, 0);
		ctx->PSSetShader(m_pMeshPS, nullptr, 0);
		if (m_pSamplerLinear) ctx->PSSetSamplers(0, 1, &m_pSamplerLinear);
	}

	// ───────────────────────────────────────────────────────────────
	// 5) 본 패스에서 섀도우 샘플 바인드 (PS: t5/s1/b6)
	// ───────────────────────────────────────────────────────────────
	{
		// 안전하게 재바인드
		ctx->UpdateSubresource(mCB_Shadow.Get(), 0, nullptr, &scb, 0, 0);
		ID3D11Buffer* b6r = mCB_Shadow.Get();
		ID3D11SamplerState* cmp = mSamShadowCmp.Get();
		ID3D11ShaderResourceView* shSRV = mShadowSRV.Get();
		ctx->PSSetConstantBuffers(6, 1, &b6r);
		ctx->PSSetSamplers(1, 1, &cmp);
		ctx->PSSetShaderResources(5, 1, &shSRV);
	}

	// === Toon ramp bind (PS: t6/b7) ===
	{
		//툰 셰이딩 바인드
		ToonCB_ t{};
		t.useToon = mDbg.useToon ? 1u : 0u;
		t.halfLambert = mDbg.toonHalfLambert ? 1u : 0u;
		t.specStep = mDbg.toonSpecStep;
		t.specBoost = mDbg.toonSpecBoost;
		t.shadowMin = mDbg.toonShadowMin;

		if (m_pToonCB) {
			ctx->UpdateSubresource(m_pToonCB, 0, nullptr, &t, 0, 0);
			ctx->PSSetConstantBuffers(7, 1, &m_pToonCB);      // PS b7
		}
		if (m_pRampSRV && mDbg.useToon) {
			ctx->PSSetShaderResources(6, 1, &m_pRampSRV);     // PS t6
		}
	}


	// 바인더
	auto BindStatic = [&]() {
		ctx->IASetInputLayout(m_pMeshIL);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(m_pMeshVS, nullptr, 0);
		ctx->PSSetShader(m_pMeshPS, nullptr, 0);
		};
	auto BindSkinned = [&]() {
		ctx->IASetInputLayout(m_pSkinnedIL);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(m_pSkinnedVS, nullptr, 0);
		ctx->PSSetShader(m_pMeshPS, nullptr, 0);
		};

	// 드로우 헬퍼
	auto DrawOpaqueOnly = [&](StaticMesh& mesh, const std::vector<MaterialGPU>& mtls, const Matrix& world)
		{
			ConstantBuffer local = cb;
			local.mWorld = XMMatrixTranspose(world);
			local.mWorldInvTranspose = world.Invert();
			ctx->UpdateSubresource(m_pConstantBuffer, 0, nullptr, &local, 0, 0);

			for (size_t i = 0; i < mesh.Ranges().size(); ++i) {
				const auto& r = mesh.Ranges()[i];
				const auto& mat = mtls[r.materialIndex];
				if (mat.hasOpacity) continue;

				mat.Bind(ctx);
				UseCB use{};
				use.useDiffuse = mat.hasDiffuse ? 1u : 0u;
				use.useNormal = (mat.hasNormal && !mDbg.disableNormal) ? 1u : 0u;
				use.useSpecular = (!mDbg.disableSpecular) ? (mat.hasSpecular ? 1u : 2u) : 0u;
				use.useEmissive = (mat.hasEmissive && !mDbg.disableEmissive) ? 1u : 0u;
				use.useOpacity = 0u;
				use.alphaCut = mDbg.forceAlphaClip ? mDbg.alphaCut : -1.0f;
				ctx->UpdateSubresource(m_pUseCB, 0, nullptr, &use, 0, 0);
				ctx->PSSetConstantBuffers(2, 1, &m_pUseCB);

				mesh.DrawSubmesh(ctx, (UINT)i);
				MaterialGPU::Unbind(ctx);
			}
		};
	auto DrawAlphaCutOnly = [&](StaticMesh& mesh, const std::vector<MaterialGPU>& mtls, const Matrix& world)
		{
			ConstantBuffer local = cb;
			local.mWorld = XMMatrixTranspose(world);
			local.mWorldInvTranspose = world.Invert();
			ctx->UpdateSubresource(m_pConstantBuffer, 0, nullptr, &local, 0, 0);

			for (size_t i = 0; i < mesh.Ranges().size(); ++i) {
				const auto& r = mesh.Ranges()[i];
				const auto& mat = mtls[r.materialIndex];
				if (!mat.hasOpacity) continue;

				mat.Bind(ctx);
				UseCB use{};
				use.useDiffuse = mat.hasDiffuse ? 1u : 0u;
				use.useNormal = (mat.hasNormal && !mDbg.disableNormal) ? 1u : 0u;
				use.useSpecular = (!mDbg.disableSpecular) ? (mat.hasSpecular ? 1u : 2u) : 0u;
				use.useEmissive = (mat.hasEmissive && !mDbg.disableEmissive) ? 1u : 0u;
				use.useOpacity = 1u;          // alpha-test
				use.alphaCut = mDbg.alphaCut;
				ctx->UpdateSubresource(m_pUseCB, 0, nullptr, &use, 0, 0);
				ctx->PSSetConstantBuffers(2, 1, &m_pUseCB);

				mesh.DrawSubmesh(ctx, (UINT)i);
				MaterialGPU::Unbind(ctx);
			}
		};
	auto DrawTransparentOnly = [&](StaticMesh& mesh, const std::vector<MaterialGPU>& mtls, const Matrix& world)
		{
			if (mDbg.forceAlphaClip) return;

			ConstantBuffer local = cb;
			local.mWorld = XMMatrixTranspose(world);
			local.mWorldInvTranspose = world.Invert();
			ctx->UpdateSubresource(m_pConstantBuffer, 0, nullptr, &local, 0, 0);

			for (size_t i = 0; i < mesh.Ranges().size(); ++i) {
				const auto& r = mesh.Ranges()[i];
				const auto& mat = mtls[r.materialIndex];
				if (!mat.hasOpacity) continue;

				mat.Bind(ctx);
				UseCB use{};
				use.useDiffuse = mat.hasDiffuse ? 1u : 0u;
				use.useNormal = (mat.hasNormal && !mDbg.disableNormal) ? 1u : 0u;
				use.useSpecular = (!mDbg.disableSpecular) ? (mat.hasSpecular ? 1u : 2u) : 0u;
				use.useEmissive = (mat.hasEmissive && !mDbg.disableEmissive) ? 1u : 0u;
				use.useOpacity = 1u;           // 투명 블렌드
				use.alphaCut = mDbg.forceAlphaClip ? mDbg.alphaCut : -1.0f;
				ctx->UpdateSubresource(m_pUseCB, 0, nullptr, &use, 0, 0);
				ctx->PSSetConstantBuffers(2, 1, &m_pUseCB);

				mesh.DrawSubmesh(ctx, (UINT)i);
				MaterialGPU::Unbind(ctx);
			}
		};

	// ───────────────────────────────────────────────────────────────
	// 6) OPAQUE
	// ───────────────────────────────────────────────────────────────
	{
		float bf[4] = { 0,0,0,0 };
		ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
		ctx->OMSetDepthStencilState(mDbg.depthWriteOff && m_pDSS_Disabled ? m_pDSS_Disabled : m_pDSS_Opaque, 0);

		if (mDbg.showOpaque) {
			BindStatic();
			if (mTreeX.enabled)  DrawOpaqueOnly(gTree, gTreeMtls, ComposeSRT(mTreeX));
			if (mCharX.enabled)  DrawOpaqueOnly(gChar, gCharMtls, ComposeSRT(mCharX));
			if (mZeldaX.enabled) DrawOpaqueOnly(gZelda, gZeldaMtls, ComposeSRT(mZeldaX));

			if (mBoxRig && mBoxX.enabled) {
				mBoxRig->DrawOpaqueOnly(ctx, ComposeSRT(mBoxX),
					view, m_Projection, m_pConstantBuffer, m_pUseCB,
					cb.vLightDir, cb.vLightColor, eye,
					m_Ka, m_Ks, m_Shininess, m_Ia,
					mDbg.disableNormal, mDbg.disableSpecular, mDbg.disableEmissive);
			}
			if (mSkinRig && mSkinX.enabled) {
				BindSkinned();
				mSkinRig->DrawOpaqueOnly(ctx, ComposeSRT(mSkinX),
					view, m_Projection, m_pConstantBuffer, m_pUseCB, m_pBoneCB,
					cb.vLightDir, cb.vLightColor, eye,
					m_Ka, m_Ks, m_Shininess, m_Ia,
					mDbg.disableNormal, mDbg.disableSpecular, mDbg.disableEmissive);
				BindStatic();
			}

			// B) OPAQUE 블록 맨 끝쪽에 붙여라
			if (mDbg.showGrid) {
				float bf[4] = { 0,0,0,0 };
				ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
				ctx->OMSetDepthStencilState(m_pDSS_Opaque, 0);
				ctx->RSSetState(m_pCullBackRS); // 윗면 보이게 만든 그 상태

				ConstantBuffer local = {};
				local.mWorld = XMMatrixTranspose(Matrix::Identity);
				local.mWorldInvTranspose = Matrix::Identity;
				local.mView = XMMatrixTranspose(view);
				local.mProjection = XMMatrixTranspose(m_Projection);
				local.vLightDir = cb.vLightDir;     // ← 조명 동일
				local.vLightColor = cb.vLightColor;
				ctx->UpdateSubresource(m_pConstantBuffer, 0, nullptr, &local, 0, 0);
				ctx->VSSetConstantBuffers(0, 1, &m_pConstantBuffer);
				ctx->PSSetConstantBuffers(0, 1, &m_pConstantBuffer);

				UINT stride = sizeof(DirectX::XMFLOAT3), offset = 0;
				ctx->IASetInputLayout(mGridIL.Get());
				ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				ctx->IASetVertexBuffers(0, 1, mGridVB.GetAddressOf(), &stride, &offset);
				ctx->IASetIndexBuffer(mGridIB.Get(), DXGI_FORMAT_R16_UINT, 0);
				ctx->VSSetShader(mGridVS.Get(), nullptr, 0);
				ctx->PSSetShader(mGridPS.Get(), nullptr, 0);
				ctx->DrawIndexed(mGridIndexCount, 0, 0);
			}


		}
	}

	// ───────────────────────────────────────────────────────────────
	// 7) CUTOUT (alpha-test 강제 모드)
	// ───────────────────────────────────────────────────────────────
	if (mDbg.forceAlphaClip) {
		float bf[4] = { 0,0,0,0 };
		ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
		ctx->OMSetDepthStencilState(m_pDSS_Opaque, 0);

		// RS (wire/cullNone 유지)
		if (mDbg.cullNone && m_pDbgRS) ctx->RSSetState(m_pDbgRS);

		if (mDbg.showTransparent) {
			BindStatic();
			if (mTreeX.enabled)  DrawAlphaCutOnly(gTree, gTreeMtls, ComposeSRT(mTreeX));
			if (mCharX.enabled)  DrawAlphaCutOnly(gChar, gCharMtls, ComposeSRT(mCharX));
			if (mZeldaX.enabled) DrawAlphaCutOnly(gZelda, gZeldaMtls, ComposeSRT(mZeldaX));

			if (mBoxRig && mBoxX.enabled) {
				mBoxRig->DrawAlphaCutOnly(
					ctx,
					ComposeSRT(mBoxX),
					view, m_Projection,
					m_pConstantBuffer,
					m_pUseCB,
					mDbg.alphaCut,
					cb.vLightDir, cb.vLightColor,
					eye,
					m_Ka, m_Ks, m_Shininess, m_Ia,
					mDbg.disableNormal, mDbg.disableSpecular, mDbg.disableEmissive
				);
			}

			if (mSkinRig && mSkinX.enabled) {
				BindSkinned();
				mSkinRig->DrawAlphaCutOnly(ctx, ComposeSRT(mSkinX),
					view, m_Projection, m_pConstantBuffer, m_pUseCB, m_pBoneCB,
					cb.vLightDir, cb.vLightColor, eye,
					m_Ka, m_Ks, m_Shininess, m_Ia,
					mDbg.disableNormal, mDbg.disableSpecular, mDbg.disableEmissive);
				BindStatic();
			}
		}
	}

	// ───────────────────────────────────────────────────────────────
	// 8) TRANSPARENT
	// ───────────────────────────────────────────────────────────────
	{
		ID3D11BlendState* oldBS = nullptr; float oldBF[4]; UINT oldSM = 0xFFFFFFFF;
		ctx->OMGetBlendState(&oldBS, oldBF, &oldSM);
		ID3D11DepthStencilState* oldDSS = nullptr; UINT oldSR = 0;
		ctx->OMGetDepthStencilState(&oldDSS, &oldSR);

		float bf[4] = { 0,0,0,0 };
		ctx->OMSetBlendState(m_pBS_Alpha, bf, 0xFFFFFFFF);
		ctx->OMSetDepthStencilState(mDbg.depthWriteOff && m_pDSS_Disabled ? m_pDSS_Disabled : m_pDSS_Trans, 0);

		if (mDbg.showTransparent) {
			BindStatic();
			if (mTreeX.enabled)  DrawTransparentOnly(gTree, gTreeMtls, ComposeSRT(mTreeX));
			if (mCharX.enabled)  DrawTransparentOnly(gChar, gCharMtls, ComposeSRT(mCharX));
			if (mZeldaX.enabled) DrawTransparentOnly(gZelda, gZeldaMtls, ComposeSRT(mZeldaX));

			if (mBoxRig && mBoxX.enabled) {
				mBoxRig->DrawTransparentOnly(ctx, ComposeSRT(mBoxX),
					view, m_Projection, m_pConstantBuffer, m_pUseCB,
					cb.vLightDir, cb.vLightColor, eye,
					m_Ka, m_Ks, m_Shininess, m_Ia,
					mDbg.disableNormal, mDbg.disableSpecular, mDbg.disableEmissive);
			}
			if (mSkinRig && mSkinX.enabled) {
				BindSkinned();
				mSkinRig->DrawTransparentOnly(ctx, ComposeSRT(mSkinX),
					view, m_Projection, m_pConstantBuffer, m_pUseCB, m_pBoneCB,
					cb.vLightDir, cb.vLightColor, eye,
					m_Ka, m_Ks, m_Shininess, m_Ia,
					mDbg.disableNormal, mDbg.disableSpecular, mDbg.disableEmissive);
				BindStatic();
			}
		}

		ctx->OMSetBlendState(oldBS, oldBF, oldSM);
		ctx->OMSetDepthStencilState(oldDSS, oldSR);
		SAFE_RELEASE(oldBS); SAFE_RELEASE(oldDSS);
	}

	// ───────────────────────────────────────────────────────────────
	// 9) 디버그(광원 화살표, 그리드)
	// ───────────────────────────────────────────────────────────────
	if (mDbg.showLightArrow) {
		Vector3 D = -dirV; D.Normalize();
		Matrix worldArrow = Matrix::CreateScale(m_ArrowScale) * Matrix::CreateWorld(m_ArrowPos, D, Vector3::UnitY);

		ConstantBuffer local = cb;
		local.mWorld = XMMatrixTranspose(worldArrow);
		local.mWorldInvTranspose = worldArrow.Invert();
		ctx->UpdateSubresource(m_pConstantBuffer, 0, nullptr, &local, 0, 0);
		ctx->VSSetConstantBuffers(0, 1, &m_pConstantBuffer);

		// 상태 백업
		ID3D11RasterizerState* oRS = nullptr; ctx->RSGetState(&oRS);
		ID3D11DepthStencilState* oDSS = nullptr; UINT oRef = 0; ctx->OMGetDepthStencilState(&oDSS, &oRef);
		ID3D11BlendState* oBS = nullptr; float oBF[4]; UINT oSM = 0xFFFFFFFF; ctx->OMGetBlendState(&oBS, oBF, &oSM);
		ID3D11InputLayout* oIL = nullptr; ctx->IAGetInputLayout(&oIL);
		ID3D11VertexShader* oVS = nullptr; ctx->VSGetShader(&oVS, nullptr, 0);
		ID3D11PixelShader* oPS = nullptr; ctx->PSGetShader(&oPS, nullptr, 0);

		float bf[4] = { 0,0,0,0 };
		ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
		ctx->OMSetDepthStencilState(m_pDSS_Opaque, 0);
		if (m_pDbgRS) ctx->RSSetState(m_pDbgRS);

		UINT stride = sizeof(DirectX::XMFLOAT3) + sizeof(DirectX::XMFLOAT4), offset = 0;
		ctx->IASetInputLayout(m_pDbgIL);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->IASetVertexBuffers(0, 1, &m_pArrowVB, &stride, &offset);
		ctx->IASetIndexBuffer(m_pArrowIB, DXGI_FORMAT_R16_UINT, 0);
		ctx->VSSetShader(m_pDbgVS, nullptr, 0);
		ctx->PSSetShader(m_pDbgPS, nullptr, 0);

		const UINT indexCount = 6 + 24 + 6 + 12;
		const DirectX::XMFLOAT4 kBright = { 1.0f, 0.95f, 0.2f, 1.0f };
		ctx->UpdateSubresource(m_pDbgCB, 0, nullptr, &kBright, 0, 0);
		ctx->PSSetConstantBuffers(3, 1, &m_pDbgCB);

		ID3D11ShaderResourceView* nullAll[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
		ctx->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, nullAll);

		ctx->DrawIndexed(indexCount, 0, 0);

		// 상태 복구
		ctx->VSSetShader(oVS, nullptr, 0);
		ctx->PSSetShader(oPS, nullptr, 0);
		ctx->IASetInputLayout(oIL);
		ctx->OMSetBlendState(oBS, oBF, oSM);
		ctx->OMSetDepthStencilState(oDSS, oRef);
		ctx->RSSetState(oRS);
		SAFE_RELEASE(oVS); SAFE_RELEASE(oPS); SAFE_RELEASE(oIL);
		SAFE_RELEASE(oBS); SAFE_RELEASE(oDSS); SAFE_RELEASE(oRS);
	}

#ifdef _DEBUG
	UpdateImGUI();
#endif
	m_pSwapChain->Present(1, 0);
}

//================================================================================================
//////////////////////////////////////////////////////////////////////////////////////////////////
//================================================================================================

bool TutorialApp::InitScene()
{
	CreateShadowResources(m_pDevice);
	CreateDepthOnlyShaders(m_pDevice);

	using Microsoft::WRL::ComPtr;

	// ---------- helpers ----------
	auto Compile = [&](const wchar_t* path, const char* entry, const char* profile, ComPtr<ID3DBlob>& blob) {
		HR_T(CompileShaderFromFile(path, entry, profile, &blob));
		};
	auto CreateVS = [&](ComPtr<ID3DBlob>& blob, ID3D11VertexShader** outVS) {
		HR_T(m_pDevice->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, outVS));
		};
	auto CreatePS = [&](ComPtr<ID3DBlob>& blob, ID3D11PixelShader** outPS) {
		HR_T(m_pDevice->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, outPS));
		};
	auto CreateIL = [&](const D3D11_INPUT_ELEMENT_DESC* il, UINT cnt, ComPtr<ID3DBlob>& vsBlob, ID3D11InputLayout** outIL) {
		HR_T(m_pDevice->CreateInputLayout(il, cnt, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), outIL));
		};

	// =========================================================
	// 1) Mesh(PNTT) shaders & IL
	// =========================================================
	{
		ComPtr<ID3DBlob> vsb, psb;
		Compile(L"Shader/VertexShader.hlsl", "main", "vs_5_0", vsb);
		CreateVS(vsb, &m_pMeshVS);

		const D3D11_INPUT_ELEMENT_DESC IL_PNTT[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TANGENT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		CreateIL(IL_PNTT, _countof(IL_PNTT), vsb, &m_pMeshIL);

		Compile(L"Shader/PixelShader.hlsl", "main", "ps_5_0", psb);
		CreatePS(psb, &m_pMeshPS);
	}

	// =========================================================
	// 2) DebugColor shaders & IL
	// =========================================================
	{
		ComPtr<ID3DBlob> vsb, psb;
		Compile(L"../Resource/DebugColor_VS.hlsl", "main", "vs_5_0", vsb);
		CreateVS(vsb, &m_pDbgVS);

		const D3D11_INPUT_ELEMENT_DESC IL_DBG[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		CreateIL(IL_DBG, _countof(IL_DBG), vsb, &m_pDbgIL);

		Compile(L"../Resource/DebugColor_PS.hlsl", "main", "ps_5_0", psb);
		CreatePS(psb, &m_pDbgPS);
	}

	// =========================================================
	// 3) Skinned VS(+IL)
	// =========================================================
	{
		D3D_SHADER_MACRO defs[] = { {"SKINNED","1"}, {nullptr,nullptr} };
		ComPtr<ID3DBlob> vsb;
		HR_T(D3DCompileFromFile(L"Shader/VertexShaderSkinning.hlsl",
			defs, D3D_COMPILE_STANDARD_FILE_INCLUDE,
			"main", "vs_5_0", 0, 0, &vsb, nullptr));
		HR_T(m_pDevice->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_pSkinnedVS));

		const D3D11_INPUT_ELEMENT_DESC IL_SKIN[] = {
			{"POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"TANGENT",      0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT,      0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0},
		};
		CreateIL(IL_SKIN, _countof(IL_SKIN), vsb, &m_pSkinnedIL);
	}

	// =========================================================
	// 4) Constant Buffers & Samplers
	// =========================================================
	{
		auto MakeCB = [&](UINT bytes, ID3D11Buffer** out) {
			D3D11_BUFFER_DESC bd{};
			bd.Usage = D3D11_USAGE_DEFAULT;
			bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			bd.ByteWidth = bytes;
			HR_T(m_pDevice->CreateBuffer(&bd, nullptr, out));
			};

		if (!m_pConstantBuffer) MakeCB(sizeof(ConstantBuffer), &m_pConstantBuffer);
		if (!m_pBlinnCB)        MakeCB(sizeof(BlinnPhongCB), &m_pBlinnCB);
		if (!m_pUseCB)          MakeCB(sizeof(UseCB), &m_pUseCB);
		if (!m_pToonCB)			MakeCB(sizeof(ToonCB_), &m_pToonCB);

		// Bone palette (VS b4)
		if (!m_pBoneCB) {
			D3D11_BUFFER_DESC cbd{};
			cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbd.Usage = D3D11_USAGE_DEFAULT;
			cbd.ByteWidth = sizeof(DirectX::XMFLOAT4X4) * 256; // 256 bones
			HR_T(m_pDevice->CreateBuffer(&cbd, nullptr, &m_pBoneCB));
		}

		// PS sampler (linear wrap)
		if (!m_pSamplerLinear) {
			D3D11_SAMPLER_DESC sd{};
			sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
			sd.MaxLOD = D3D11_FLOAT32_MAX;
			HR_T(m_pDevice->CreateSamplerState(&sd, &m_pSamplerLinear));
		}

		// Debug color CB (PS b3)
		if (!m_pDbgCB) {
			D3D11_BUFFER_DESC bd{};
			bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			bd.Usage = D3D11_USAGE_DEFAULT;
			bd.ByteWidth = 16; // float4
			HR_T(m_pDevice->CreateBuffer(&bd, nullptr, &m_pDbgCB));
		}
	}

	// =========================================================
	// 5) Debug Arrow (geometry)
	// =========================================================
	{
		struct V { DirectX::XMFLOAT3 p; DirectX::XMFLOAT4 c; };
		const float halfT = 6.0f, shaftLen = 120.0f, headLen = 30.0f, headHalf = 10.0f;
		const DirectX::XMFLOAT4 Y = { 1.0f, 0.9f, 0.1f, 1.0f };

		enum { s0, s1, s2, s3, s4, s5, s6, s7, h0, h1, h2, h3, tip, COUNT };
		V v[COUNT] = {
			{{-halfT,-halfT,0},Y}, {{+halfT,-halfT,0},Y}, {{+halfT,+halfT,0},Y}, {{-halfT,+halfT,0},Y},
			{{-halfT,-halfT,shaftLen},Y}, {{+halfT,-halfT,shaftLen},Y}, {{+halfT,+halfT,shaftLen},Y}, {{-halfT,+halfT,shaftLen},Y},
			{{-headHalf,-headHalf,shaftLen},Y}, {{+headHalf,-headHalf,shaftLen},Y},
			{{+headHalf,+headHalf,shaftLen},Y}, {{-headHalf,+headHalf,shaftLen},Y},
			{{0,0,shaftLen + headLen},Y},
		};
		const uint16_t idx[] = {
			s0,s2,s1, s0,s3,s2, s0,s1,s5, s0,s5,s4, s1,s2,s6, s1,s6,s5, s3,s7,s6, s3,s6,s2, s0,s4,s7, s0,s7,s3,
			h2,h1,h0, h3,h2,h0, h0,h1,tip, h1,h2,tip, h2,h3,tip, h3,h0,tip,
		};

		// VB
		D3D11_BUFFER_DESC vbd{ sizeof(v), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER };
		D3D11_SUBRESOURCE_DATA vinit{ v };
		HR_T(m_pDevice->CreateBuffer(&vbd, &vinit, &m_pArrowVB));
		// IB
		D3D11_BUFFER_DESC ibd{ sizeof(idx), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER };
		D3D11_SUBRESOURCE_DATA iinit{ idx };
		HR_T(m_pDevice->CreateBuffer(&ibd, &iinit, &m_pArrowIB));
	}

	// =========================================================
	// 6) Initial transforms
	// =========================================================
	mTreeX.pos = { -100, -150, 100 };  mTreeX.initPos = mTreeX.pos;  mTreeX.scl = { 100,100,100 };
	mCharX.pos = { 100, -150, 100 };  mCharX.initPos = mCharX.pos;
	mZeldaX.pos = { 0, -150, 250 };  mZeldaX.initPos = mZeldaX.pos;
	mBoxX.pos = { -200, -150, 400 }; mBoxX.scl = { 0.2f,0.2f,0.2f };
	mSkinX.pos = { 200, -150, 400 };

	//mTreeX.enabled = mCharX.enabled = mZeldaX.enabled = mBoxX.enabled = false;

	mTreeX.initScl = mTreeX.scl; mCharX.initScl = mCharX.scl; mZeldaX.initScl = mZeldaX.scl;
	mTreeX.initRotD = mTreeX.rotD; mCharX.initRotD = mCharX.rotD; mZeldaX.initRotD = mZeldaX.rotD;
	mBoxX.initScl = mBoxX.scl; mBoxX.initRotD = mBoxX.rotD; mBoxX.initPos = mBoxX.pos;
	mSkinX.initScl = mSkinX.scl; mSkinX.initRotD = mSkinX.rotD; mSkinX.initPos = mSkinX.pos;

	// =========================================================
	// 7) Load FBX + build GPU
	// =========================================================
	{
		auto BuildAll = [&](const std::wstring& fbx, const std::wstring& texDir,
			StaticMesh& mesh, std::vector<MaterialGPU>& mtls)
			{
				MeshData_PNTT cpu;
				if (!AssimpImporterEx::LoadFBX_PNTT_AndMaterials(fbx, cpu, /*flipUV*/true, /*leftHanded*/true))
					throw std::runtime_error("FBX load failed");
				if (!mesh.Build(m_pDevice, cpu))
					throw std::runtime_error("Mesh build failed");

				mtls.resize(cpu.materials.size());
				for (size_t i = 0; i < cpu.materials.size(); ++i)
					mtls[i].Build(m_pDevice, cpu.materials[i], texDir);
			};

		BuildAll(L"../Resource/Tree/Tree.fbx", L"../Resource/Tree/", gTree, gTreeMtls);
		BuildAll(L"../Resource/Character/Character.fbx", L"../Resource/Character/", gChar, gCharMtls);
		BuildAll(L"../Resource/Zelda/zeldaPosed001.fbx", L"../Resource/Zelda/", gZelda, gZeldaMtls);
		BuildAll(L"../Resource/BoxHuman/BoxHuman.fbx", L"../Resource/BoxHuman/", gBoxHuman, gBoxMtls);

		mBoxRig = RigidSkeletal::LoadFromFBX(m_pDevice,
			L"../Resource/BoxHuman/BoxHuman.fbx",
			L"../Resource/BoxHuman/");

		mSkinRig = SkinnedSkeletal::LoadFromFBX(m_pDevice,
			L"../Resource/Skinning/SkinningTest.fbx",
			L"../Resource/Skinning/");

		if (mSkinRig && m_pBoneCB) mSkinRig->WarmupBoneCB(m_pDeviceContext, m_pBoneCB);
	}

	// =========================================================
	// 8) Rasterizer / Depth / Blend states
	// =========================================================
	{
		// Back-cull (default)
		D3D11_RASTERIZER_DESC rsBack{}; rsBack.FillMode = D3D11_FILL_SOLID; rsBack.CullMode = D3D11_CULL_BACK;
		rsBack.DepthClipEnable = TRUE; HR_T(m_pDevice->CreateRasterizerState(&rsBack, &m_pCullBackRS));

		// Solid + Cull None
		D3D11_RASTERIZER_DESC rsNone{}; rsNone.FillMode = D3D11_FILL_SOLID; rsNone.CullMode = D3D11_CULL_NONE;
		rsNone.DepthClipEnable = TRUE; HR_T(m_pDevice->CreateRasterizerState(&rsNone, &m_pDbgRS));

		// Wireframe + Cull None
		D3D11_RASTERIZER_DESC rw{}; rw.FillMode = D3D11_FILL_WIREFRAME; rw.CullMode = D3D11_CULL_NONE;
		rw.DepthClipEnable = TRUE; HR_T(m_pDevice->CreateRasterizerState(&rw, &m_pWireRS));

		// Depth OFF (debug)
		D3D11_DEPTH_STENCIL_DESC dsOff{}; dsOff.DepthEnable = FALSE; dsOff.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		HR_T(m_pDevice->CreateDepthStencilState(&dsOff, &m_pDSS_Disabled));

		// Opaque depth (write ON)
		D3D11_DEPTH_STENCIL_DESC dsO{}; dsO.DepthEnable = TRUE; dsO.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dsO.DepthFunc = D3D11_COMPARISON_LESS_EQUAL; HR_T(m_pDevice->CreateDepthStencilState(&dsO, &m_pDSS_Opaque));

		// Transparent depth (write OFF)
		D3D11_DEPTH_STENCIL_DESC dsT{}; dsT.DepthEnable = TRUE; dsT.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dsT.DepthFunc = D3D11_COMPARISON_LESS_EQUAL; HR_T(m_pDevice->CreateDepthStencilState(&dsT, &m_pDSS_Trans));

		// Straight alpha blend
		D3D11_BLEND_DESC bd{}; auto& rt = bd.RenderTarget[0]; rt.BlendEnable = TRUE;
		rt.SrcBlend = D3D11_BLEND_SRC_ALPHA; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D11_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D11_BLEND_ONE;  rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		HR_T(m_pDevice->CreateBlendState(&bd, &m_pBS_Alpha));
	}

	// =========================================================
	// 9) Skybox: shaders/IL, geometry, texture/sampler, depth/RS
	// =========================================================
	{
		// shaders & IL (position-only)
		ComPtr<ID3DBlob> vsb, psb;
		Compile(L"../Resource/Sky_VS.hlsl", "main", "vs_5_0", vsb);
		CreateVS(vsb, &m_pSkyVS);
		const D3D11_INPUT_ELEMENT_DESC IL_SKY[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		CreateIL(IL_SKY, _countof(IL_SKY), vsb, &m_pSkyIL);

		Compile(L"../Resource/Sky_PS.hlsl", "main", "ps_5_0", psb);
		CreatePS(psb, &m_pSkyPS);

		// geometry (unit cube)
		struct SkyV { DirectX::XMFLOAT3 pos; };
		const SkyV v[] = {
			{{-1,-1,-1}}, {{-1,+1,-1}}, {{+1,+1,-1}}, {{+1,-1,-1}},
			{{-1,-1,+1}}, {{-1,+1,+1}}, {{+1,+1,+1}}, {{+1,-1,+1}},
		};
		const uint16_t idx[] = {
			0,1,2, 0,2,3,  4,6,5, 4,7,6,  4,5,1, 4,1,0,
			3,2,6, 3,6,7,  1,5,6, 1,6,2,  4,0,3, 4,3,7
		};
		D3D11_BUFFER_DESC vb{ sizeof(v),   D3D11_USAGE_DEFAULT,   D3D11_BIND_VERTEX_BUFFER };
		D3D11_BUFFER_DESC ib{ sizeof(idx), D3D11_USAGE_DEFAULT,   D3D11_BIND_INDEX_BUFFER };
		D3D11_SUBRESOURCE_DATA vsd{ v }, isd{ idx };
		HR_T(m_pDevice->CreateBuffer(&vb, &vsd, &m_pSkyVB));
		HR_T(m_pDevice->CreateBuffer(&ib, &isd, &m_pSkyIB));

		// texture + sampler
		HR_T(CreateDDSTextureFromFile(m_pDevice, L"../Resource/Cubemap.dds", nullptr, &m_pSkySRV));
		D3D11_SAMPLER_DESC ssd{}; ssd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		ssd.AddressU = ssd.AddressV = ssd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		ssd.MaxLOD = D3D11_FLOAT32_MAX;
		HR_T(m_pDevice->CreateSamplerState(&ssd, &m_pSkySampler));

		// depth/raster states
		D3D11_DEPTH_STENCIL_DESC sd{}; sd.DepthEnable = TRUE; sd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		sd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL; HR_T(m_pDevice->CreateDepthStencilState(&sd, &m_pSkyDSS));
		D3D11_RASTERIZER_DESC rs{}; rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_FRONT;
		HR_T(m_pDevice->CreateRasterizerState(&rs, &m_pSkyRS));
	}

	// =========================================================
	// 10) Debug Grid: geometry + shaders/IL
	// =========================================================
	{
		// geometry (XZ plane, CCW, up-facing)
		struct V { DirectX::XMFLOAT3 pos; };
		const float S = mGridHalfSize, Y = mGridY;
		V verts[4] = { {{-S,Y,-S}}, {{ S,Y,-S}}, {{ S,Y, S}}, {{-S,Y, S}} };
		const uint16_t idx[] = { 0,2,1, 0,3,2 };
		mGridIndexCount = 6;

		D3D11_BUFFER_DESC vb{ sizeof(verts), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER };
		D3D11_SUBRESOURCE_DATA vsd{ verts };
		HR_T(m_pDevice->CreateBuffer(&vb, &vsd, &mGridVB));

		D3D11_BUFFER_DESC ib{ sizeof(idx),   D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER };
		D3D11_SUBRESOURCE_DATA isd{ idx };
		HR_T(m_pDevice->CreateBuffer(&ib, &isd, &mGridIB));

		// shaders & IL
		ComPtr<ID3DBlob> vsb, psb;
		Compile(L"Shader/DbgGrid.hlsl", "VS_Main", "vs_5_0", vsb);
		Compile(L"Shader/DbgGrid.hlsl", "PS_Main", "ps_5_0", psb);
		HR_T(m_pDevice->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &mGridVS));
		HR_T(m_pDevice->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &mGridPS));

		const D3D11_INPUT_ELEMENT_DESC IL_GRID[] = {
			{ "POSITION",0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
		};
		CreateIL(IL_GRID, 1, vsb, &mGridIL);
	}

	{
		HR_T(DirectX::CreateWICTextureFromFile(
			m_pDevice, L"../Resource/RampTexture.png", nullptr, &m_pRampSRV));
	}

	return true;
}

//================================================================================================
//////////////////////////////////////////////////////////////////////////////////////////////////
//================================================================================================

bool TutorialApp::InitD3D()
{
	HRESULT hr = 0;

	DXGI_SWAP_CHAIN_DESC swapDesc = {};
	swapDesc.BufferCount = 1;
	swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapDesc.OutputWindow = m_hWnd;
	swapDesc.Windowed = true;
	swapDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapDesc.BufferDesc.Width = m_ClientWidth;
	swapDesc.BufferDesc.Height = m_ClientHeight;
	swapDesc.BufferDesc.RefreshRate.Numerator = 60;
	swapDesc.BufferDesc.RefreshRate.Denominator = 1;
	swapDesc.SampleDesc.Count = 1;
	swapDesc.SampleDesc.Quality = 0;

	UINT creationFlags = 0;
#ifdef _DEBUG
	creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
	HR_T(D3D11CreateDeviceAndSwapChain(
		NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
		creationFlags, NULL, NULL, D3D11_SDK_VERSION,
		&swapDesc,
		&m_pSwapChain,
		&m_pDevice, NULL,
		&m_pDeviceContext));

	ID3D11Texture2D* pBackBufferTexture = nullptr;

	HR_T(m_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBufferTexture));

	HR_T(m_pDevice->CreateRenderTargetView(pBackBufferTexture, NULL, &m_pRenderTargetView));
	SAFE_RELEASE(pBackBufferTexture);

	//==============================================================================================

	D3D11_TEXTURE2D_DESC dsDesc = {};
	dsDesc.Width = m_ClientWidth;
	dsDesc.Height = m_ClientHeight;
	dsDesc.MipLevels = 1;
	dsDesc.ArraySize = 1;
	dsDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; // 흔한 조합
	dsDesc.SampleDesc.Count = 1;  // 스왑체인과 동일하게
	dsDesc.SampleDesc.Quality = 0;
	dsDesc.Usage = D3D11_USAGE_DEFAULT;
	dsDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

	HR_T(m_pDevice->CreateTexture2D(&dsDesc, nullptr, &m_pDepthStencil));
	HR_T(m_pDevice->CreateDepthStencilView(m_pDepthStencil, nullptr, &m_pDepthStencilView));

	D3D11_DEPTH_STENCIL_DESC dss = {};
	dss.DepthEnable = TRUE;
	dss.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	dss.DepthFunc = D3D11_COMPARISON_LESS_EQUAL; // 스카이박스 쓸거면 LEQUAL이 편함. 기본은 LESS
	dss.StencilEnable = FALSE;
	HR_T(m_pDevice->CreateDepthStencilState(&dss, &m_pDepthStencilState));
	m_pDeviceContext->OMSetDepthStencilState(m_pDepthStencilState, 0);

	//==============================================================================================

	m_pDeviceContext->OMSetRenderTargets(1, &m_pRenderTargetView, m_pDepthStencilView);

	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = 0;
	viewport.TopLeftY = 0;
	viewport.Width = (float)m_ClientWidth;
	viewport.Height = (float)m_ClientHeight;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;

	m_pDeviceContext->RSSetViewports(1, &viewport);

	return true;
}

//================================================================================================

void TutorialApp::UninitScene()
{
	// FBX 전용 파이프라인 자원
	SAFE_RELEASE(m_pMeshIL);
	SAFE_RELEASE(m_pMeshVS);
	SAFE_RELEASE(m_pMeshPS);
	SAFE_RELEASE(m_pConstantBuffer);

	SAFE_RELEASE(m_pUseCB);
	SAFE_RELEASE(m_pNoCullRS);
	SAFE_RELEASE(m_pSamplerLinear);
	SAFE_RELEASE(m_pBlinnCB);

	SAFE_RELEASE(m_pSkyVS);
	SAFE_RELEASE(m_pSkyPS);
	SAFE_RELEASE(m_pSkyIL);
	SAFE_RELEASE(m_pSkyVB);
	SAFE_RELEASE(m_pSkyIB);
	SAFE_RELEASE(m_pSkySRV);
	SAFE_RELEASE(m_pSkySampler);
	SAFE_RELEASE(m_pSkyDSS);
	SAFE_RELEASE(m_pSkyRS);

	SAFE_RELEASE(m_pDbgRS);
	SAFE_RELEASE(m_pArrowIB);
	SAFE_RELEASE(m_pArrowVB);
	SAFE_RELEASE(m_pDbgIL);
	SAFE_RELEASE(m_pDbgVS);
	SAFE_RELEASE(m_pDbgPS);
	SAFE_RELEASE(m_pDbgCB);

	SAFE_RELEASE(m_pWireRS);
	SAFE_RELEASE(m_pCullBackRS);
	SAFE_RELEASE(m_pDSS_Disabled);

	SAFE_RELEASE(m_pBS_Alpha);
	SAFE_RELEASE(m_pDSS_Opaque);
	SAFE_RELEASE(m_pDSS_Trans);
	//머가 이리 많음
	SAFE_RELEASE(m_pSkinnedIL);
	SAFE_RELEASE(m_pSkinnedVS);
	SAFE_RELEASE(m_pBoneCB);
	//툰툰
	SAFE_RELEASE(m_pRampSRV);
	SAFE_RELEASE(m_pToonCB);
}

void TutorialApp::UninitD3D()
{
	SAFE_RELEASE(m_pDepthStencilState);
	SAFE_RELEASE(m_pDepthStencilView);
	SAFE_RELEASE(m_pDepthStencil);
	SAFE_RELEASE(m_pRenderTargetView);
	SAFE_RELEASE(m_pDeviceContext);
	SAFE_RELEASE(m_pSwapChain);
	SAFE_RELEASE(m_pDevice);
}

//================================================================================================

bool TutorialApp::InitImGUI()
{
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();
	//폰트 등록
	ImGuiIO& io = ImGui::GetIO();
	const ImWchar* kr = io.Fonts->GetGlyphRangesKorean();
	io.Fonts->Clear();
	io.Fonts->AddFontFromFileTTF("../Resource/fonts/Regular.ttf", 16.0f, nullptr, kr);

	ImGui_ImplWin32_Init(m_hWnd);
	ImGui_ImplDX11_Init(this->m_pDevice, this->m_pDeviceContext);


	return true;
}

void TutorialApp::UninitImGUI()
{
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
}

//================================================================================================

void TutorialApp::UpdateImGUI()
{
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
	if (ImGui::Begin(u8"임꾸이(IMGUI)"))
	{
		// 상단 상태
		ImGui::Text("FPS: %.1f (%.3f ms)", ImGui::GetIO().Framerate, 1000.0f / ImGui::GetIO().Framerate);
		ImGui::Separator();

		// 스냅샷 1회 저장
		static bool s_inited = false;
		static double s_initAnimT = 0.0;
		static float  s_initAnimSpeed = 1.0f;
		static Vector3 s_initCubePos{}, s_initCubeScale{};
		static float   s_initSpin = 0.0f, s_initFov = 60.0f, s_initNear = 0.1f, s_initFar = 1.0f;
		static Vector3 s_initLightColor{};
		static float   s_initLightYaw = 0.0f, s_initLightPitch = 0.0f, s_initLightIntensity = 1.0f;
		static Vector3 s_initKa{}, s_initIa{};
		static float   s_initKs = 0.5f, s_initShin = 32.0f;
		static Vector3 s_initArrowPos{}, s_initArrowScale{};
		if (!s_inited) {
			s_inited = true;
			s_initAnimT = mAnimT;
			s_initAnimSpeed = mAnimSpeed;
			s_initCubePos = cubeTransformA;   s_initCubeScale = cubeScale;   s_initSpin = spinSpeed;
			s_initFov = m_FovDegree;          s_initNear = m_Near;           s_initFar = m_Far;
			s_initLightColor = m_LightColor;  s_initLightYaw = m_LightYaw;   s_initLightPitch = m_LightPitch; s_initLightIntensity = m_LightIntensity;
			s_initKa = m_Ka; s_initIa = m_Ia; s_initKs = m_Ks; s_initShin = m_Shininess;
			s_initArrowPos = m_ArrowPos;      s_initArrowScale = m_ArrowScale;
		}

		// === Camera ===
		if (ImGui::CollapsingHeader(u8"Camera"))
		{
			ImGui::SliderFloat("FOV (deg)", &m_FovDegree, 10.0f, 120.0f, "%.1f");
			ImGui::DragFloat("Near", &m_Near, 0.001f, 0.0001f, 10.0f, "%.5f");
			ImGui::DragFloat("Far", &m_Far, 0.1f, 0.01f, 20000.0f);
			ImGui::Text(u8"카메라 속도 변경: F1 ~ F3");
			if (ImGui::Button(u8"카메라 초기화")) {
				m_FovDegree = s_initFov; m_Near = s_initNear; m_Far = s_initFar;
			}		
		}

		//---------------------------------------------------


		if (ImGui::Begin("Toon Shading"), ImGuiCond_FirstUseEver) {
			ImGui::Checkbox("Use Toon", &mDbg.useToon);
			ImGui::Checkbox("Half-Lambert", &mDbg.toonHalfLambert);
			ImGui::DragFloat("Spec Step", &mDbg.toonSpecStep, 0.01f, 0.0f, 1.0f, "%.2f");
			ImGui::DragFloat("Spec Boost", &mDbg.toonSpecBoost, 0.01f, 0.0f, 3.0f, "%.2f");
			ImGui::DragFloat("Shadow Min", &mDbg.toonShadowMin, 0.005f, 0.0f, 0.10f, "%.3f");
		}
		ImGui::End();

		if (ImGui::Begin("Shadow / Light Camera"))
		{
			// ── Lighting ────────────────────────────────────────────────
			ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
			if (ImGui::CollapsingHeader(u8"Lighting", ImGuiTreeNodeFlags_DefaultOpen))
			{
				ImGui::SliderAngle("Yaw", &m_LightYaw, -180.0f, 180.0f);
				ImGui::SliderAngle("Pitch", &m_LightPitch, -89.0f, 89.0f);
				ImGui::ColorEdit3("Color", (float*)&m_LightColor);
				ImGui::SliderFloat("Intensity", &m_LightIntensity, 0.0f, 5.0f);
				if (ImGui::Button(u8"조명 초기화")) {
					m_LightColor = s_initLightColor;
					m_LightYaw = s_initLightYaw;
					m_LightPitch = s_initLightPitch;
					m_LightIntensity = s_initLightIntensity;
				}
			}

			// ── ShadowMap Preview / Grid ────────────────────────────────
			ImGui::Checkbox("Show ShadowMap", &mShUI.showSRV);
			ImGui::Checkbox("Show Grid", &mDbg.showGrid);
			ImGui::Checkbox("Use Orthographic", &mShUI.useOrtho);
			ImGui::Checkbox("Use followCamera", &mShUI.followCamera);

			if (mShUI.showSRV) {
				ImTextureID id = (ImTextureID)mShadowSRV.Get();
				if (id) ImGui::Image(id, ImVec2(256, 256), ImVec2(0, 0), ImVec2(1, 1));
				else    ImGui::TextUnformatted("Shadow SRV is null");
			}

			// ── Focus / Light (카메라 전방 락) ──────────────────────────
			ImGui::SeparatorText("Focus & Light (locked to camera)");
			ImGui::DragFloat("FocusDist", &mShUI.focusDist, 0.1f, 0.1f, 5000.0f);
			ImGui::DragFloat("LightDist", &mShUI.lightDist, 0.1f, 0.1f, 10000.0f);
			ImGui::DragFloat("Margin", &mShUI.coverMargin, 0.01f, 1.0f, 2.0f);

			// ── DepthOnly 옵션 ───────────────────────────────────────────
			ImGui::SeparatorText("DepthOnly");
			ImGui::SliderFloat("AlphaCut (DepthOnly)", &mShadowAlphaCut, 0.0f, 1.0f, "%.3f");

			// ── Bias(비교/래스터) ───────────────────────────────────────
			ImGui::SeparatorText("Bias");
			ImGui::DragFloat("CmpBias", &mShadowCmpBias, 0.0001f, 0.0f, 0.02f, "%.5f");
			ImGui::DragInt("DepthBias", (int*)&mShadowDepthBias, 1, 0, 200000);
			ImGui::DragFloat("SlopeScaledBias", &mShadowSlopeBias, 0.01f, 0.0f, 32.0f, "%.2f");

			// ── ShadowMap 해상도 + 재생성 ───────────────────────────────
			ImGui::SeparatorText("Shadowmap Size");
			static int resIdx =
				(mShadowW >= 4096) ? 3 :
				(mShadowW >= 2048) ? 2 :
				(mShadowW >= 1024) ? 1 : 0;
			const char* kResItems[] = { "512", "1024", "2048", "4096" };
			ImGui::Combo("Resolution", &resIdx, kResItems, IM_ARRAYSIZE(kResItems));

			// 읽기 전용(자동 커버 결과 확인)
			ImGui::SeparatorText("Derived (read-only)");
			ImGui::Text("FovY: %.1f deg", DirectX::XMConvertToDegrees(mShadowFovY));
			ImGui::Text("Near/Far: %.3f / %.3f", mShadowNear, mShadowFar);

			// 적용 버튼: RS(바이어스) + 텍스처(SRV/DSV) 재생성
			if (ImGui::Button("Apply (recreate shadow map)")) {
				int sz = 512;
				if (resIdx == 1) sz = 1024;
				else if (resIdx == 2) sz = 2048;
				else if (resIdx == 3) sz = 4096;
				mShadowW = mShadowH = sz;
				CreateShadowResources(m_pDevice); // RS(DepthBias/SlopeBias)와 DSV/SRV 갱신
			}
		}
		ImGui::End();

		//---------------------------------------------------

		// === Material (Blinn-Phong) ===
		if (ImGui::CollapsingHeader(u8"Material"))
		{
			ImGui::ColorEdit3("I_a (ambient light)", (float*)&m_Ia);
			ImGui::ColorEdit3("k_a (ambient refl.)", (float*)&m_Ka);
			ImGui::SliderFloat("k_s (specular)", &m_Ks, 0.0f, 2.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SliderFloat("shininess", &m_Shininess, 2.0f, 256.0f, "%.0f");
			if (ImGui::Button(u8"재질 초기화")) {
				m_Ka = s_initKa; m_Ia = s_initIa; m_Ks = s_initKs; m_Shininess = s_initShin;
			}
		}

		// === Models ===
		if (ImGui::CollapsingHeader(u8"Models"))
		{
			auto ModelUI = [&](const char* name, XformUI& xf) {
				if (ImGui::TreeNode(name)) {
					ImGui::Checkbox("Enabled", &xf.enabled);
					ImGui::DragFloat3("Position", (float*)&xf.pos, 0.1f, -10000.0f, 10000.0f);
					ImGui::DragFloat3("Rotation (deg XYZ)", (float*)&xf.rotD, 0.5f, -720.0f, 720.0f);
					ImGui::DragFloat3("Scale", (float*)&xf.scl, 0.01f, 0.0001f, 1000.0f);
					if (ImGui::Button(u8"모델 초기화")) {
						xf.pos = xf.initPos; xf.rotD = xf.initRotD; xf.scl = xf.initScl; xf.enabled = true;
					}
					ImGui::TreePop();
				}
				};

			ModelUI("Tree", mTreeX);
			ModelUI("Character", mCharX);
			ModelUI("Zelda", mZeldaX);

			// Models 
			if (ImGui::TreeNode(u8"Debug Arrow (라이트 방향)")) {
				ImGui::Checkbox("Enabled", &mDbg.showLightArrow);
				ImGui::DragFloat3("Position", (float*)&m_ArrowPos, 0.1f, -10000.0f, 10000.0f);
				ImGui::DragFloat3("Scale", (float*)&m_ArrowScale, 0.01f, 0.0001f, 1000.0f);
				if (ImGui::Button(u8"화살표 초기화")) {
					m_ArrowPos = s_initArrowPos;
					m_ArrowScale = s_initArrowScale;
					mDbg.showLightArrow = true;
				}
				ImGui::TreePop();
			}

			if (ImGui::Button(u8"모든 모델 초기화")) {
				for (XformUI* p : { &mTreeX, &mCharX, &mZeldaX }) {
					p->pos = p->initPos; p->rotD = p->initRotD; p->scl = p->initScl; p->enabled = true;
				}
				m_ArrowPos = s_initArrowPos;
				m_ArrowScale = s_initArrowScale;
				mDbg.showLightArrow = true;
			}
		}

		if (ImGui::CollapsingHeader(u8"BoxHuman (RigidSkeletal)"))
		{
			// Transform
			ImGui::Checkbox("Enabled##Box", &mBoxX.enabled);
			ImGui::DragFloat3("Position##Box", (float*)&mBoxX.pos, 0.1f, -10000.0f, 10000.0f);
			ImGui::DragFloat3("Rotation (deg XYZ)##Box", (float*)&mBoxX.rotD, 0.5f, -720.0f, 720.0f);
			ImGui::DragFloat3("Scale##Box", (float*)&mBoxX.scl, 0.01f, 0.0001f, 1000.0f);
			if (ImGui::Button(u8"트랜스폼 초기화")) {
				mBoxX.pos = mBoxX.initPos; mBoxX.rotD = mBoxX.initRotD; mBoxX.scl = mBoxX.initScl; mBoxX.enabled = true;
			}

			ImGui::SeparatorText("Animation");
			if (mBoxRig)
			{
				const double tps = mBoxRig->GetTicksPerSecond();
				const double durS = mBoxRig->GetClipDurationSec();
				ImGui::Text("Ticks/sec: %.3f", tps);
				ImGui::Text("Duration : %.3f sec", durS);

				AnimUI("Controls",
					mBoxAC.play, mBoxAC.loop, mBoxAC.speed, mBoxAC.t,
					durS,
					[&](double tNow) { mBoxRig->EvaluatePose(tNow); });

				if (ImGui::Button(u8"애니메이션 초기화")) {
					mBoxAC.play = true; mBoxAC.loop = true; mBoxAC.speed = 1.0f; mBoxAC.t = 0.0;
					mBoxRig->EvaluatePose(mBoxAC.t);
				}
			}
			else {
				ImGui::TextDisabled("BoxHuman not loaded.");
			}
		}

		if (ImGui::CollapsingHeader(u8"SkinningTest (SkinnedSkeletal)"))
		{
			ImGui::Checkbox("Enabled", &mSkinX.enabled);
			ImGui::DragFloat3("Position", (float*)&mSkinX.pos, 0.1f, -10000.0f, 10000.0f);
			ImGui::DragFloat3("Rotation (deg XYZ)", (float*)&mSkinX.rotD, 0.5f, -720.0f, 720.0f);
			ImGui::DragFloat3("Scale", (float*)&mSkinX.scl, 0.01f, 0.0001f, 1000.0f);
			if (ImGui::Button(u8"트랜스폼 초기화##skin")) {
				mSkinX.pos = mSkinX.initPos; mSkinX.rotD = mSkinX.initRotD; mSkinX.scl = mSkinX.initScl; mSkinX.enabled = true;
			}

			ImGui::SeparatorText("Animation");
			if (mSkinRig)
			{
				const double durS = mSkinRig->DurationSec();
				ImGui::Text("Duration : %.3f sec", durS);

				AnimUI("Controls##skin",
					mSkinAC.play, mSkinAC.loop, mSkinAC.speed, mSkinAC.t,
					durS,
					[&](double tNow) { mSkinRig->EvaluatePose(tNow); });

				if (ImGui::Button(u8"애니메이션 초기화##skin")) {
					mSkinAC.play = true; mSkinAC.loop = true; mSkinAC.speed = 1.0f; mSkinAC.t = 0.0;
					mSkinRig->EvaluatePose(mSkinAC.t);
				}
			}
			else {
				ImGui::TextDisabled("Skinned rig not loaded.");
			}
		}


		// === Toggles / Render Debug ===
		if (ImGui::CollapsingHeader(u8"Toggles & Debug"))
		{
			ImGui::Checkbox("Show Skybox", &mDbg.showSky);
			ImGui::Checkbox("Show Opaque", &mDbg.showOpaque);
			ImGui::Checkbox("Show Transparent", &mDbg.showTransparent);

			ImGui::Separator();

			ImGui::Checkbox("Wireframe", &mDbg.wireframe); ImGui::SameLine();
			ImGui::Checkbox("Cull None", &mDbg.cullNone);
			ImGui::Checkbox("Depth Write/Test OFF (mesh)", &mDbg.depthWriteOff);
			ImGui::Checkbox("Freeze Time", &mDbg.freezeTime); // 이거 작동 안함

			ImGui::Separator();

			ImGui::Checkbox("Disable Normal", &mDbg.disableNormal);
			ImGui::Checkbox("Disable Specular", &mDbg.disableSpecular);
			ImGui::Checkbox("Disable Emissive", &mDbg.disableEmissive);
			ImGui::Checkbox("Force AlphaClip", &mDbg.forceAlphaClip);
			ImGui::DragFloat("alphaCut", &mDbg.alphaCut, 0.01f, 0.0f, 1.0f);

			if (ImGui::Button(u8"디버그 토글 초기화")) {
				mDbg = DebugToggles(); // 리셋
			}
		}
	}

	ImGui::End();
	ImGui::Render();
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

//================================================================================================

#ifdef _DEBUG
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
#endif

LRESULT CALLBACK TutorialApp::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
#ifdef _DEBUG
	if (ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
		return true;
#endif
	return __super::WndProc(hWnd, message, wParam, lParam);
}
