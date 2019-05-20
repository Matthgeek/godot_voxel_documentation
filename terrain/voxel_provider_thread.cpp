#include "voxel_provider_thread.h"
#include "../providers/voxel_provider.h"
#include "../util/utility.h"

VoxelDataLoader::VoxelDataLoader(int thread_count, Ref<VoxelProvider> provider, int block_size_pow2) {

	Processor processors[Mgr::MAX_JOBS];

	// Note: more than one thread can make sense for generators,
	// but won't be as useful for file and network streams
	for (int i = 0; i < thread_count; ++i) {
		Processor &p = processors[i];
		p.block_size_pow2 = block_size_pow2;
		if (i == 0) {
			p.provider = provider;
		} else {
			p.provider = provider->duplicate();
		}
	}

	// TODO Re-enable duplicate rejection, was turned off to investigate some bugs
	_mgr = memnew(Mgr(thread_count, 500, processors, false));
}

VoxelDataLoader::~VoxelDataLoader() {
	if (_mgr) {
		memdelete(_mgr);
	}
}

void VoxelDataLoader::Processor::process_block(const InputBlockData &input, OutputBlockData &output, Vector3i block_position, unsigned int lod) {

	int bs = 1 << block_size_pow2;
	Ref<VoxelBuffer> buffer;
	buffer.instance();
	buffer->create(bs, bs, bs);

	Vector3i block_origin_in_voxels = block_position * (bs << lod);
	provider->emerge_block(buffer, block_origin_in_voxels, lod);

	output.voxels_loaded = buffer;
}
