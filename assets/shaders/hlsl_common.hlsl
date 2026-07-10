#ifndef HLSL_COMMON_HLSL
#define HLSL_COMMON_HLSL

float3x3 NormalMatrixFromModel(float4x4 modelMatrix)
{
    float3x3 model3x3 = (float3x3)modelMatrix;
    const float det = determinant(model3x3);
    if (abs(det) < 1e-6)
    {
        return float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);
    }

    float3x3 invModel3x3;
    invModel3x3._11 = model3x3._22 * model3x3._33 - model3x3._23 * model3x3._32;
    invModel3x3._12 = model3x3._13 * model3x3._32 - model3x3._12 * model3x3._33;
    invModel3x3._13 = model3x3._12 * model3x3._23 - model3x3._13 * model3x3._22;
    invModel3x3._21 = model3x3._23 * model3x3._31 - model3x3._21 * model3x3._33;
    invModel3x3._22 = model3x3._11 * model3x3._33 - model3x3._13 * model3x3._31;
    invModel3x3._23 = model3x3._13 * model3x3._21 - model3x3._11 * model3x3._23;
    invModel3x3._31 = model3x3._21 * model3x3._32 - model3x3._22 * model3x3._31;
    invModel3x3._32 = model3x3._12 * model3x3._31 - model3x3._11 * model3x3._32;
    invModel3x3._33 = model3x3._11 * model3x3._22 - model3x3._12 * model3x3._21;
    invModel3x3 *= (1.0 / det);

    return transpose(invModel3x3);
}

#endif
