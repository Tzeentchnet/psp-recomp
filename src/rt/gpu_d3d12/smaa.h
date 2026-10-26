#ifndef GE_D3D12_SMAA_H
#define GE_D3D12_SMAA_H

#include <d3d12.h>

bool ged3d12_smaa_init(UINT width, UINT height);
void ged3d12_smaa_shutdown(void);
bool ged3d12_smaa_record(ID3D12Resource *input);
ID3D12Resource *ged3d12_smaa_output(void);

#endif
