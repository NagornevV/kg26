cbuffer CBPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gViewProj;
    float3   gLightDir;     float gPad0;
    float3   gLightColor;   float gPad1;
    float3   gEyePos;       float gPad2;
    float3   gObjectColor;  float gPad3;
    float2   gUVScale;      // тайлинг
    float2   gUVOffset;     // анимация
};

Texture2D    gDiffuseMap : register(t0);
SamplerState gSampler    : register(s0);

struct VSIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC    : TEXCOORD;
};

struct VSOut
{
    float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC    : TEXCOORD;
};

VSOut VS(VSIn vin)
{
    VSOut vout;
    float4 posW  = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW    = posW.xyz;
    vout.PosH    = mul(posW, gViewProj);
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);
    vout.TexC    = vin.TexC * gUVScale + gUVOffset;
    return vout;
}

float4 PS(VSOut pin) : SV_Target
{
    float4 texColor = gDiffuseMap.Sample(gSampler, pin.TexC);

    float3 N = normalize(pin.NormalW);
    float3 L = normalize(-gLightDir);
    float3 V = normalize(gEyePos - pin.PosW);
    float3 R = reflect(-L, N);

    float3 ambient  = 0.15f * gLightColor;
    float  diff     = max(dot(N, L), 0.0f);
    float3 diffuse  = diff * gLightColor;
    float  spec     = pow(max(dot(V, R), 0.0f), 32.0f);
    float3 specular = 0.3f * spec * gLightColor;

    float3 color = (ambient + diffuse + specular) * texColor.rgb * gObjectColor;
    return float4(color, texColor.a);
}
