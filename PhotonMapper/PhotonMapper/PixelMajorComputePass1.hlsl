#ifndef COMPUTE_PASS_1
#define COMPUTE_PASS_1

#define HLSL
#include "RaytracingHlslCompat.h"

#define blocksize 128

#define HLSL
#include "RaytracingHlslCompat.h"

// Render Target for visualizing the photons - can be removed later on
RWTexture2D<float4> RenderTarget : register(u0);

// G-Buffers
RWTexture2DArray<uint> GPhotonCount : register(u1);
RWTexture2DArray<uint> GPhotonScan : register(u2);
RWTexture2DArray<uint> GPhotonTempIndex : register(u3);

RWTexture2DArray<float4> GPhotonPos : register(u4);
RWTexture2DArray<float4> GPhotonColor : register(u5);
RWTexture2DArray<float4> GPhotonSortedPos : register(u6);
RWTexture2DArray<float4> GPhotonSortedCol : register(u7);

ConstantBuffer<PixelMajorComputeConstantBuffer> CKernelParams : register(b0);

uint3 PosToCellId(float3 worldPosition)
{
	float3 correctWorldPos = (worldPosition + float3(MAX_SCENE_SIZE, MAX_SCENE_SIZE, MAX_SCENE_SIZE)) / 2.0;
	return uint3(floor(correctWorldPos / CELL_SIZE));
}

uint Cell3DTo1D(uint3 cellId)
{
	return uint(cellId.x + MAX_SCENE_SIZE * cellId.y + MAX_SCENE_SIZE * MAX_SCENE_SIZE * cellId.z); // TODO check if correct
}

uint3 Cell1DTo3D(uint id)
{
	uint3 temp;
	temp.x = id % MAX_SCENE_SIZE;
	temp.y = (id / MAX_SCENE_SIZE) % MAX_SCENE_SIZE;
	temp.z = id / (MAX_SCENE_SIZE * MAX_SCENE_SIZE);
	return temp;
}

// From Stream Compaction hw
int ilog2(int x) {
    int lg = 0;
    while (x >>= 1) {
        ++lg;
    }
    return lg;
}

int ilog2ceil(int x) {
    return x == 1 ? 0 : ilog2(x - 1) + 1;
}

void CSScan(uint3 DTid) {
    int index = DTid.x;

    //const int two_d = CKernelParams.param1;
    //const int two_d_1 = two_d * 2;

    const int log_n = ilog2ceil(CKernelParams.param2);

    // Up sweep
    int power_2 = 1;

    int d = 0;

    [unroll]
    for (int d = 0; d < log_n; ++d) {
        GroupMemoryBarrierWithGroupSync();

        power_2 = (1 << d);

        int two_d = power_2;
        int two_d_1 = two_d * 2;

        
        if (index % two_d_1 != 0)
        {
            return;
        }
        

        int oldIndex = index + two_d - 1;
        int newIndex = index + two_d_1 - 1;

        uint oldcellX = CELL_1D_TO_3D_X(oldIndex);
        uint oldcellY = CELL_1D_TO_3D_Y(oldIndex);
        uint oldcellZ = CELL_1D_TO_3D_Z(oldIndex);

        uint cellX = CELL_1D_TO_3D_X(newIndex);
        uint cellY = CELL_1D_TO_3D_Y(newIndex);
        uint cellZ = CELL_1D_TO_3D_Z(newIndex);

        uint3 oldCell = uint3(oldcellX, oldcellY, oldcellZ);
        uint3 newCell = uint3(cellX, cellY, cellZ);

        int currData = GPhotonScan[newCell];
        int oldData = GPhotonScan[oldCell];
        //GPhotonScan[newCell] = newIndex != (CKernelParams.param2 - 1) ? (currData + oldData) : 0;
        GPhotonScan[newCell] = oldData + currData;
    }

}



// Exclusive Scan, Up-sweep
[numthreads(blocksize, 1, 1)]
void CSMain(uint3 Gid : SV_GroupID, uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint GI : SV_GroupIndex)
{
    //CSScan(DTid);

    
	int index = DTid.x;

	const int two_d = CKernelParams.param1;
	const int two_d_1 = two_d * 2;

	if (index % two_d_1 != 0)
	{
		return;
	}

	const int oldIndex = index + two_d - 1;
	const int newIndex = index + two_d_1 - 1;

    // TODO: Optimize this. I have written this out like this to debug and verify that everythign works first.

    const uint oldcellX = CELL_1D_TO_3D_X(oldIndex);
    const uint oldcellY = CELL_1D_TO_3D_Y(oldIndex);
    const uint oldcellZ = CELL_1D_TO_3D_Z(oldIndex);

    const uint cellX = CELL_1D_TO_3D_X(newIndex);
    const uint cellY = CELL_1D_TO_3D_Y(newIndex);
    const uint cellZ = CELL_1D_TO_3D_Z(newIndex);

    const uint3 oldCell = uint3(oldcellX, oldcellY, oldcellZ);
    const uint3 newCell = uint3(cellX, cellY, cellZ);

	const int currData = GPhotonScan[newCell];
    const int oldData = GPhotonScan[oldCell];
	//GPhotonScan[newCell] = newIndex != (CKernelParams.param2 - 1) ? (currData + oldData) : 0;
    GPhotonScan[newCell] = oldData + currData;
    //GPhotonScan[newCell] = two_d;
    

    /*
    int index = DTid.x;
    const uint cellX = CELL_1D_TO_3D_X(index);
    const uint cellY = CELL_1D_TO_3D_Y(index);
    const uint cellZ = CELL_1D_TO_3D_Z(index);
    const uint3 newCell = uint3(cellX, cellY, cellZ);

    const int oldData = GPhotonScan[newCell];

    GPhotonScan[newCell] = oldData + 1;
    */
}

#endif // COMPUTE_PASS_1
