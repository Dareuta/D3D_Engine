#include "07_0_Shared.hlsli"

PS_INPUT main(VS_INPUT input)
{
    PS_INPUT output;
    
    //===========================================
    
    float4 wpos = mul(float4(input.Pos, 1.0f), World);
    output.WorldPos = wpos.xyz;
    // 1.0f float4 맞출려고 넣어주는건데, 여기서 좌표(점)이기 때문에 1.0f임
    // 만약 방향백터라면 0이 들어갔겠지?
    // 로컬좌표(input.Pos)를 월드좌표로 먼저 변환해줌
    // 이후 해당성분을 기록해두었다가 빼둠        
    
    output.PosH = mul(mul(wpos, View), Projection);
    // VP 합쳐서 MVP 완성임, 아주아주 유명한 MVP변환임 아주 심오하지
    
    //===========================================
    
    output.Tex = input.Tex; // 이건 그냥 그대로 넘겨줌 여기서 쓰는거 아님
        
    output.NormalW = normalize(mul(float4(input.Norm, 0.0f), WorldInvTranspose).xyz);
    // 여기서도 마찬가지로, flaot3 형태에 0.0f를 추가했음
    // 0.0f라는건, 좌표이동을 무시하는 형태가 됨
    // 즉, 방향벡터임
    // 입력받은 노말벡터와 탄젠트 백터에 월드 역전치를 곱해주고 다시 정규화시킴
    // 역전치 곱하는 이유? 만약 위쪽에서 월드 변환 시점에서 비균등 스케일이 발생했을 수 있기 때문에
    // 그 월드 행렬의 역전치(사실상 역수의 개념)을 곱해주는거임 - 직각을 유지하기 위해서
    // MV의 역전치를 곱하는 경우가 있는데, 그건 뷰 공간에서 벡터를 사용하기 위해서임
    // 우리는 월드에서 사용하기 때문에, 월드의 역전치만 곱해주는것으로 처리함
        
    float sign = input.Tang.w; // w값(좌수 / 우수)임
    
    //float3x3 world3 = (float3x3) World;
    // 생각해보면, 탄젠트 벡터에는 그냥 월드좌표를 곱해야함
    // 탄젠트는 면과 동일하게 움직여줘야함       
    
    //float3 Traw = mul(input.Tang.xyz, world3);
    // Gram-Schmidt(그람-슈미트) 라고 하는데, 아무리 봐도 호들갑임
    // PS에서 한번 처리하는거로 퉁침, 여기서 하는건 진짜 심각한 호들갑이
    //float3 Tw = normalize(Traw - Nw * dot(Traw, Nw));
    
    //output.NormalW = Nw;
    
    output.TangentW = float4(normalize(mul(input.Tang.xyz, (float3x3) World)), sign);
    
    // 비탄젠트는 PS에서 알아서 계산해서 쓰셈, 넘기는거 생각보다 자원 많이먹는다고 함(잘모름)

    //===========================================
    
    return output;
}