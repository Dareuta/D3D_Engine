#include "07_0_Shared.hlsli"

#ifndef NORMALMAP_FLIP_GREEN   // 노말맵의 G(Y축)을 한번 뒤집어보는건 어떨까요?
#define NORMALMAP_FLIP_GREEN 0 // 뒤집지 말아주세요
#endif                         // 넵

// 여기서 해야하는거
// [1] 알베도 샘플(sRGB → Linear자동/수동) << 먼지 몰루겟슴
// [2] 노멀맵 적용(TBN, Y플립옵션, 정규화) << 이거랑 블린퐁을 알음
// [3] 블린–퐁 조명(ambient+ diffuse+ specular)
// [4] 출력 공간확인(백버퍼 sRGB여부) << 이게머임

//===========================================

float4 main(PS_INPUT input) : SV_Target // 이게 그 유명한 SV_Target임(색상)
{
    //===========================================
    // DDS포맷이 현재 선형임(어째서인지는 모름)    
    // 알베도 - 확산 반사의 기본 색(base)    
    float3 albedo = txDiffuse.Sample(samLinear, input.Tex).rgb;
    // txDiffuse에서, Tex좌표(uv)에 해당하는 색상만(RGB)를 찝어오는건데,
    // 그 방식이 samLinear에 기록됨(선형 보간 / LOD선택 등)
    
    // SRV가 UNORM이면 선형화를 해야한다는데 뭔말인지 몰루겟슴
    // 애초에 내 DDS - 베이스가 뭔 포멧인데    
    // 끼뺫 엄벌기 삣삐
           
    // 아무튼 albedo = uv좌표의 RGB값(기본색상)
    
    //===========================================
    // 노말맵 언팩
    
    float3 n_t = txNormal.Sample(samLinear, input.Tex).xyz * 2.0f - 1.0f;    
    // txNormal에서, Tex좌표(uv)에 담겨있는 색상(xyz)를 가져와서
    // 그 값의 2.0을 곱하고 -1을 함(xyz값이 0~1로 정규화 되어있음)
    // 그렇게 된다면, 0~2가 되고, -1하니까, -1 ~ 1로 원상복구 됨
    
    // n_t = uv 좌표의 법선벡터(탄젠트 공간)
    
    //===========================================
    
#if NORMALMAP_FLIP_GREEN
    n_t.y = -n_t.y; // 뒤집어잇 << 어디서 구웠는지에 따라, 원산지보고 결정함
#endif
    n_t = normalize(n_t); // 뒤집지 마잇
    
    //===========================================
    // TBN 하는거임
    
    float3 T = normalize(input.TangentW.xyz);    
    float3 N = normalize(input.NormalW);
    
    // T,B가 N과 비직교 일 수 있으니, 보정 - 
    // ㄹㅇ 호들갑으로밖에 안보임 이거
    
    T = normalize(T - N * dot(T, N)); // Gram-Schmidt(그람-슈미트) 
                                      // 만약 두 백터가 직교한다면 0이니까, 변화없음
                                      // 차이가 생기면 뭔가 하겠지
                                      // 아, 차이가 생긴만큼 T를 이동시키는거구나(대충 이해함)
    
    float3 B = normalize(cross(N, T) * input.TangentW.w); // 오른손계 유지   
    // 외적에서는 둘이 평행하면 큰일나는거 기억해두자, 나중에 터지면 여기 봐야할 수 있음
    // 물론 앞에서 그람머시깽 해서 1차 안전은 보장되긴 함

    //input.TangentW.w에는 handedness가 들어있음(1 or -1)
    
    // T B N 순서임        
    
    float3x3 TBN = float3x3(T, B, N);        

    //명명백백한 법선의 왕
    float3 Nw = normalize(mul(n_t, TBN)); // 순서 중요함(백터 X 행렬)    
    //TBN 기저를 곱해버리면, 월드 좌표로 복원됨

    //===========================================
    
    // 퐁 머시깽에 쓰는 Light, View, Half 방향벡터들임
    float3 L = normalize(-vLightDir.xyz); 
    // 빛은 들어오는 방향에 반대로 생각하면 됨
    float3 V = normalize(EyePosW.xyz - input.WorldPos);
    // 뷰는 눈의 좌표(카메라)와 월드좌표로 벡터를 만들면 됨)
    float3 H = normalize(L + V);
    // 두 벡터 더하고 노말하면 대충 중앙값 나오는데,
    // 이거랑, 법선벡터가 얼마나 일치하는지 내적해서 반짝이게 하면 됨

    //===========================================
    
    // 파라미터
    float ks = kSAlpha.x; // x값에 ks
    float shin = max(1.0f, kSAlpha.y); // y값에 alpha(shininess) 들어감 - 최소 1
    
    //===========================================

    // 디퓨즈 
    float NdotL = saturate(dot(Nw, L)); // 빛벡터와 법선을 내적
    float3 diff = albedo * NdotL; // 기본 베이스 색 * 0 ~ 1
    
    //===========================================    

    // 텍스쳐 * ks(상수) * (N·H)^shin
    float specTex = txSpecular.Sample(samLinear, input.Tex).r; 
    // 스펙큘러 맵에서, uv좌표의 r(rgb중 하나)를 읽어와서 그 값을 저장함
    // 이것도 samLinear에서 지정한 방식 - 뭐 선형보간 처리 후에 들어옴
    // 즉, 해당 uv좌표의 반사정도를 받는거임(값은 0 ~ 1) 선형이니까
    
    
    float3 spec = specTex * ks * pow(saturate(dot(Nw, H)), shin);
    // 엥 왜 제곱함? 이상한디 << 지수함수적으로 기하급수적인 변화를 주기 위해 제곱해주는거
    // 그러니까, 식 내용은
    // 해당 uv 좌표의 반사 정도 * 물체의 반사 정도 * (법선과 하프벡터의 내적(0~1) ^ 광택정도-반짝이라고 주는 값)
    // shin << 이거 값은 그냥 예쁜값으로 쓰는듯? - 일단 넘겨줌(모양만드는 용임)
    
    //===========================================
    
    // 앰비언트
    float3 ambient = I_ambient.rgb * kA.rgb * albedo;
    // 환경광의 색 * 환경광 반사정도 * 기본 베이스
    
    //===========================================
    // 명명백백한 컬러의 왕
    
    float3 color = ambient + vLightColor.rgb * (diff + spec);
    // 최솟값(엠비언트) + 비추어진 빛의 색 * (난반사 + 정반사)

    //===========================================
    
    float3 color_srgb = pow(saturate(color), 1.0 / 2.2); 
    // 선형 -> sRGB로 변경해줌            
    
    return float4(color, 1.0f); // 투명도 1 고정
}
