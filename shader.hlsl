cbuffer CBPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gViewProj;
    float3   gObjectColor; float gPad0;
    float2   gUVScale;
    float2   gUVOffset;
};

struct PointLight
{
    float3 Position; float Radius;
    float3 Color;    float Intensity;
};

struct SpotLight
{
    float3 Position;  float Radius;
    float3 Direction; float SpotPower;
    float3 Color;     float Intensity;
};

cbuffer CBFrameLights : register(b1)
{
    float3 gEyePos;        float gPointCount;
    float3 gDirLightDir;   float gSpotCount;
    float3 gDirLightColor; float gPad1;
    PointLight gPoints[16];
    SpotLight  gSpots[4];
};

Texture2D    gTexture0   : register(t0);
Texture2D    gNormalMap  : register(t1);
Texture2D    gPosMap     : register(t2);
SamplerState gSampler    : register(s0);

struct VSIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC    : TEXCOORD;
};

struct GeoVSOut
{
    float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC    : TEXCOORD;
};

GeoVSOut GeometryVS(VSIn vin)
{
    GeoVSOut vout;
    float4 posW  = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW    = posW.xyz;
    vout.PosH    = mul(posW, gViewProj);
    vout.NormalW = normalize(mul(vin.NormalL, (float3x3)gWorld));
    vout.TexC    = vin.TexC * gUVScale + gUVOffset;
    return vout;
}

struct GBufferOut
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Position : SV_Target2;
};

GBufferOut GeometryPS(GeoVSOut pin)
{
    GBufferOut gout;
    float4 texColor = gTexture0.Sample(gSampler, pin.TexC);
    gout.Albedo   = float4(texColor.rgb * gObjectColor, texColor.a);
    gout.Normal   = float4(normalize(pin.NormalW) * 0.5f + 0.5f, 1.0f);
    gout.Position = float4(pin.PosW, 1.0f);
    return gout;
}

struct LightVSOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

LightVSOut LightingVS(uint vertexID : SV_VertexID)
{
    LightVSOut o;
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    o.TexC = uv;
    o.PosH = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float3 CalcPoint(PointLight l, float3 posW, float3 normalW, float3 viewDir)
{
    float3 toLight = l.Position - posW;
    float dist = length(toLight);
    float3 L = toLight / max(dist, 0.001f);
    float att = saturate(1.0f - dist / l.Radius);
    att *= att;

    float diff = max(dot(normalW, L), 0.0f);
    float3 H = normalize(L + viewDir);
    float spec = pow(max(dot(normalW, H), 0.0f), 32.0f);

    return (diff + 0.25f * spec) * l.Color * l.Intensity * att;
}

float3 CalcSpot(SpotLight l, float3 posW, float3 normalW, float3 viewDir)
{
    float3 toLight = l.Position - posW;
    float dist = length(toLight);
    float3 L = toLight / max(dist, 0.001f);
    float att = saturate(1.0f - dist / l.Radius);
    att *= att;

    float spot = pow(saturate(dot(-L, normalize(l.Direction))), l.SpotPower);
    float diff = max(dot(normalW, L), 0.0f);
    float3 H = normalize(L + viewDir);
    float spec = pow(max(dot(normalW, H), 0.0f), 32.0f);

    return (diff + 0.3f * spec) * l.Color * l.Intensity * att * spot;
}

float4 LightingPS(LightVSOut pin) : SV_Target
{
    float3 albedo = gTexture0.Sample(gSampler, pin.TexC).rgb;
    float3 normalW = normalize(gNormalMap.Sample(gSampler, pin.TexC).rgb * 2.0f - 1.0f);
    float3 posW = gPosMap.Sample(gSampler, pin.TexC).xyz;

    // Пустой пиксель GBuffer — фон.
    if (length(albedo) < 0.001f)
        return float4(0.02f, 0.02f, 0.025f, 1.0f);

    float3 viewDir = normalize(gEyePos - posW);
    float3 lighting = 0.08f; // ambient

    // Directional light.
    float3 L = normalize(-gDirLightDir);
    float diff = max(dot(normalW, L), 0.0f);
    float3 H = normalize(L + viewDir);
    float spec = pow(max(dot(normalW, H), 0.0f), 32.0f);
    lighting += (diff + 0.18f * spec) * gDirLightColor;

    [loop]
    for (int i = 0; i < (int)gPointCount; ++i)
        lighting += CalcPoint(gPoints[i], posW, normalW, viewDir);

    [loop]
    for (int s = 0; s < (int)gSpotCount; ++s)
        lighting += CalcSpot(gSpots[s], posW, normalW, viewDir);

    return float4(saturate(albedo * lighting), 1.0f);
}
