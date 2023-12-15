#include "voxel_lod_terrain_update_clipbox_streaming.h"
#include "../../util/math/conv.h"
#include "../../util/profiling.h"
#include "../../util/string_funcs.h"
#include "voxel_lod_terrain_update_task.h"

// #include <fstream>

namespace zylann::voxel {

// Note:
// This streaming method allows every LOD to load in parallel, even before meshes are ready. That means if a data block
// is loaded somewhere and gets edited, there is no guarantee its parent LODs are loaded! It can be made more likely,
// but not guaranteed.
// Hopefully however, this should not be a problem if LODs are just cosmetic representations of
// LOD0. If an edit happens at LOD0 and sibling chunks are present, they can be used to produce the parent LOD.
// Alternatively, LOD updates could wait. In worst case, parent LODs will not update.

// TODO Octree streaming was polling constantly, but clipbox isn't. So if a task is dropped due to being too far away,
// it might cause a chunk hole or blocked lods, because it won't be requested again...
// Either we should handle "dropped" responses and retrigger if still needed (as we did before), or we could track every
// loading tasks with a shared boolean owned by both the task and the requester, which the requester sets to false if
// it's not needed anymore, and otherwise doesn't get cancelled.

namespace {

bool find_index(Span<const std::pair<ViewerID, VoxelEngine::Viewer>> viewers, ViewerID id, unsigned int &out_index) {
	for (unsigned int i = 0; i < viewers.size(); ++i) {
		if (viewers[i].first == id) {
			out_index = i;
			return true;
		}
	}
	return false;
}

bool contains(Span<const std::pair<ViewerID, VoxelEngine::Viewer>> viewers, ViewerID id) {
	unsigned int _unused;
	return find_index(viewers, id, _unused);
}

bool find_index(
		const std::vector<VoxelLodTerrainUpdateData::PairedViewer> &viewers, ViewerID id, unsigned int &out_index) {
	for (unsigned int i = 0; i < viewers.size(); ++i) {
		if (viewers[i].id == id) {
			out_index = i;
			return true;
		}
	}
	return false;
}

Box3i get_base_box_in_chunks(Vector3i viewer_position_voxels, int distance_voxels, int chunk_size, bool make_even) {
	// Get min and max positions
	Vector3i minp = viewer_position_voxels - Vector3iUtil::create(distance_voxels);
	Vector3i maxp = viewer_position_voxels +
			Vector3iUtil::create(distance_voxels
					// When distance is a multiple of chunk size, we should be able to get a consistent box size,
					// however without that +1 there are still very specific coordinates that makes the box shrink due
					// to rounding
					+ 1);

	// Convert to chunk coordinates
	minp = math::floordiv(minp, chunk_size);
	maxp = math::ceildiv(maxp, chunk_size);

	if (make_even) {
		// Round to be even outwards (partly required for subdivision rule)
		// TODO Maybe there is a more clever way to do this
		minp = math::floordiv(minp, 2) * 2;
		maxp = math::ceildiv(maxp, 2) * 2;
	}

	return Box3i::from_min_max(minp, maxp);
}

inline int get_lod_distance_in_mesh_chunks(float lod_distance_in_voxels, int mesh_block_size) {
	return math::max(static_cast<int>(Math::ceil(lod_distance_in_voxels)) / mesh_block_size, 1);
}

void process_viewers(VoxelLodTerrainUpdateData::ClipboxStreamingState &cs,
		const VoxelLodTerrainUpdateData::Settings &volume_settings, unsigned int lod_count,
		Span<const std::pair<ViewerID, VoxelEngine::Viewer>> viewers, const Transform3D &volume_transform,
		Box3i volume_bounds_in_voxels, int data_block_size_po2, bool can_mesh,
		// Ordered by ascending index in paired viewers list
		std::vector<unsigned int> &unpaired_viewers_to_remove) {
	ZN_PROFILE_SCOPE();

	// Destroyed viewers
	for (size_t paired_viewer_index = 0; paired_viewer_index < cs.paired_viewers.size(); ++paired_viewer_index) {
		VoxelLodTerrainUpdateData::PairedViewer &pv = cs.paired_viewers[paired_viewer_index];

		if (!contains(viewers, pv.id)) {
			ZN_PRINT_VERBOSE(format("Detected destroyed viewer {} in VoxelLodTerrain", pv.id));

			// Interpret removal as nullified view distance so the same code handling loading of blocks
			// will be used to unload those viewed by this viewer.
			// We'll actually remove unpaired viewers in a second pass.
			pv.state.view_distance_voxels = 0;

			// Also update boxes, they won't be updated since the viewer has been removed.
			// Assign prev state, otherwise in some cases resetting boxes would make them equal to prev state,
			// therefore causing no unload
			pv.prev_state = pv.state;

			for (unsigned int lod_index = 0; lod_index < pv.state.data_box_per_lod.size(); ++lod_index) {
				pv.state.data_box_per_lod[lod_index] = Box3i();
			}
			for (unsigned int lod_index = 0; lod_index < pv.state.mesh_box_per_lod.size(); ++lod_index) {
				pv.state.mesh_box_per_lod[lod_index] = Box3i();
			}

			unpaired_viewers_to_remove.push_back(paired_viewer_index);
		}
	}

	// TODO Pair/Unpair viewers as they intersect volume bounds

	const Transform3D world_to_local_transform = volume_transform.affine_inverse();

	// Note, this does not support non-uniform scaling
	// TODO There is probably a better way to do this
	const float view_distance_scale = world_to_local_transform.basis.xform(Vector3(1, 0, 0)).length();

	const int data_block_size = 1 << data_block_size_po2;

	const int mesh_block_size = 1 << volume_settings.mesh_block_size_po2;
	const int mesh_to_data_factor = mesh_block_size / data_block_size;

	const int lod_distance_in_mesh_chunks =
			get_lod_distance_in_mesh_chunks(volume_settings.lod_distance, mesh_block_size);

	// Data chunks are driven by mesh chunks, because mesh needs data
	const int lod_distance_in_data_chunks = lod_distance_in_mesh_chunks * mesh_to_data_factor;

	// const Box3i volume_bounds_in_data_blocks = volume_bounds_in_voxels.downscaled(1 << data_block_size_po2);
	// const Box3i volume_bounds_in_mesh_blocks = volume_bounds_in_voxels.downscaled(1 << mesh_block_size_po2);

	// New viewers and existing viewers.
	// Removed viewers won't be iterated but are still paired until later.
	for (const std::pair<ViewerID, VoxelEngine::Viewer> &viewer_and_id : viewers) {
		const ViewerID viewer_id = viewer_and_id.first;
		const VoxelEngine::Viewer &viewer = viewer_and_id.second;

		unsigned int paired_viewer_index;
		if (!find_index(cs.paired_viewers, viewer_id, paired_viewer_index)) {
			// New viewer
			VoxelLodTerrainUpdateData::PairedViewer pv;
			pv.id = viewer_id;
			paired_viewer_index = cs.paired_viewers.size();
			cs.paired_viewers.push_back(pv);
			ZN_PRINT_VERBOSE(format("Pairing viewer {} to VoxelLodTerrain", viewer_id));
		}

		VoxelLodTerrainUpdateData::PairedViewer &paired_viewer = cs.paired_viewers[paired_viewer_index];

		// Move current state to be the previous state
		paired_viewer.prev_state = paired_viewer.state;

		const int view_distance_voxels =
				static_cast<int>(static_cast<float>(viewer.view_distance) * view_distance_scale);
		paired_viewer.state.view_distance_voxels =
				math::min(view_distance_voxels, static_cast<int>(volume_settings.view_distance_voxels));

		// The last LOD should extend at least up to view distance. It must also be at least the distance specified by
		// "lod distance"
		const int last_lod_mesh_block_size = mesh_block_size << (lod_count - 1);
		const int last_lod_distance_in_mesh_chunks =
				math::max(math::ceildiv(paired_viewer.state.view_distance_voxels, last_lod_mesh_block_size),
						lod_distance_in_mesh_chunks);
		const int last_lod_distance_in_data_chunks = last_lod_mesh_block_size * mesh_to_data_factor;

		const Vector3 local_position = world_to_local_transform.xform(viewer.world_position);

		paired_viewer.state.local_position_voxels = math::floor_to_int(local_position);
		paired_viewer.state.requires_collisions = viewer.require_collisions;
		paired_viewer.state.requires_meshes = viewer.require_visuals && can_mesh;

		// Viewers can request any box they like, but they must follow these rules:
		// - Boxes of parent LODs must contain child boxes (when converted into world coordinates)
		// - Mesh boxes that have a parent LOD must have an even size and even position, in order to support subdivision
		// - Mesh boxes must be contained within data boxes, in order to guarantee that meshes have access to consistent
		//   voxel blocks and their neighbors

		// TODO The root LOD should not need to have even size.
		// However if we do that, one corner case is when LOD count is changed in the editor, it might cause errors
		// since every LOD is assumed to have an even size when handling subdivisions

		// Update data and mesh boxes
		if (paired_viewer.state.requires_collisions || paired_viewer.state.requires_meshes) {
			// Meshes are required

			for (unsigned int lod_index = 0; lod_index < lod_count; ++lod_index) {
				const int lod_mesh_block_size_po2 = volume_settings.mesh_block_size_po2 + lod_index;
				const int lod_mesh_block_size = 1 << lod_mesh_block_size_po2;

				const Box3i volume_bounds_in_mesh_blocks = volume_bounds_in_voxels.downscaled(lod_mesh_block_size);

				const int ld =
						(lod_index == (lod_count - 1) ? last_lod_distance_in_mesh_chunks : lod_distance_in_mesh_chunks);

				// Box3i new_mesh_box = get_lod_box_in_chunks(
				// 		paired_viewer.state.local_position_voxels, ld, volume_settings.mesh_block_size_po2, lod_index);

				Box3i new_mesh_box = get_base_box_in_chunks(paired_viewer.state.local_position_voxels,
						// Making sure that distance is a multiple of chunk size, for consistent box size
						ld * lod_mesh_block_size, lod_mesh_block_size,
						// Make min and max coordinates even in child LODs, to respect subdivision rule.
						// Root LOD doesn't need to respect that,
						lod_index != lod_count - 1);

				if (lod_index > 0) {
					// Post-process the box to enforce neighboring rule

					// Must be even to respect subdivision rule
					const int min_pad = 2;
					const Box3i &child_box = paired_viewer.state.mesh_box_per_lod[lod_index - 1];
					// Note, subdivision rule enforces the child box position and size to be even, so it won't round to
					// zero when converted to the parent LOD's coordinate system.
					Box3i min_box = Box3i(child_box.pos >> 1, child_box.size >> 1)
											// Enforce neighboring rule by padding boxes outwards by a minimum amount,
											// so there is at least N chunks in the current LOD between LOD+1 and LOD-1
											.padded(min_pad);

					if (lod_index != lod_count - 1) {
						// Make sure it stays even
						min_box = min_box.downscaled(2).scaled(2);
					}

					// Usually this won't modify the box, except in cases where lod distance is small
					new_mesh_box.merge_with(min_box);
				}

				// Clip last
				new_mesh_box.clip(volume_bounds_in_mesh_blocks);

				paired_viewer.state.mesh_box_per_lod[lod_index] = new_mesh_box;
			}

			// TODO We should have a flag server side to force data boxes to be based on mesh boxes, even though the
			// server might not actually need meshes. That would help the server to provide data chunks to clients,
			// which need them for visual meshes

			// Data boxes must be based on mesh boxes so the right data chunks are loaded to make the corresponding
			// meshes (also including the tweaks we do to mesh boxes to enforce the neighboring rule)
			for (unsigned int lod_index = 0; lod_index < lod_count; ++lod_index) {
				const unsigned int lod_data_block_size_po2 = data_block_size_po2 + lod_index;

				// Should be correct as long as bounds size is a multiple of the biggest LOD chunk
				const Box3i volume_bounds_in_data_blocks = Box3i( //
						volume_bounds_in_voxels.pos >> lod_data_block_size_po2, //
						volume_bounds_in_voxels.size >> lod_data_block_size_po2);

				// const int ld =
				// 		(lod_index == (lod_count - 1) ? lod_distance_in_data_chunks : last_lod_distance_in_data_chunks);

				// const Box3i new_data_box =
				// 		get_lod_box_in_chunks(paired_viewer.state.local_position_voxels, lod_distance_in_data_chunks,
				// 				data_block_size_po2, lod_index)
				// 				// To account for meshes requiring neighbor data chunks.
				// 				// It technically breaks the subdivision rule (where every parent block always has 8
				// 				// children), but it should only matter in areas where meshes must actually spawn
				// 				.padded(1)
				// 				.clipped(volume_bounds_in_data_blocks);

				const Box3i &mesh_box = paired_viewer.state.mesh_box_per_lod[lod_index];

				const Box3i data_box =
						Box3i(mesh_box.pos * mesh_to_data_factor, mesh_box.size * mesh_to_data_factor)
								// To account for meshes requiring neighbor data chunks.
								// It technically breaks the subdivision rule (where every parent block always has 8
								// children), but it should only matter in areas where meshes must actually spawn
								.padded(1)
								.clipped(volume_bounds_in_data_blocks);

				paired_viewer.state.data_box_per_lod[lod_index] = data_box;
			}

		} else {
			for (unsigned int lod_index = 0; lod_index < lod_count; ++lod_index) {
				paired_viewer.state.mesh_box_per_lod[lod_index] = Box3i();
			}

			for (unsigned int lod_index = 0; lod_index < lod_count; ++lod_index) {
				const int lod_data_block_size_po2 = data_block_size_po2 + lod_index;
				const int lod_data_block_size = 1 << lod_data_block_size_po2;

				// Should be correct as long as bounds size is a multiple of the biggest LOD chunk
				const Box3i volume_bounds_in_data_blocks = Box3i( //
						volume_bounds_in_voxels.pos >> lod_data_block_size_po2, //
						volume_bounds_in_voxels.size >> lod_data_block_size_po2);

				const int ld =
						(lod_index == (lod_count - 1) ? lod_distance_in_data_chunks : last_lod_distance_in_data_chunks);

				const Box3i new_data_box = get_base_box_in_chunks(paired_viewer.state.local_position_voxels,
						// Making sure that distance is a multiple of chunk size, for consistent box size
						ld * lod_data_block_size, lod_data_block_size,
						// Make min and max coordinates even in child LODs, to respect subdivision rule.
						// Root LOD doesn't need to respect that,
						lod_index != lod_count - 1)
												   .clipped(volume_bounds_in_data_blocks);

				// const Box3i new_data_box = get_lod_box_in_chunks(paired_viewer.state.local_position_voxels,
				// 		lod_distance_in_data_chunks, data_block_size_po2, lod_index)
				// 								   .clipped(volume_bounds_in_data_blocks);

				paired_viewer.state.data_box_per_lod[lod_index] = new_data_box;
			}
		}
	}
}

void remove_unpaired_viewers(const std::vector<unsigned int> &unpaired_viewers_to_remove,
		std::vector<VoxelLodTerrainUpdateData::PairedViewer> &paired_viewers) {
	// Iterating backward so indexes of paired viewers that need removal will not change because of the removal itself
	for (auto it = unpaired_viewers_to_remove.rbegin(); it != unpaired_viewers_to_remove.rend(); ++it) {
		const unsigned int vi = *it;
		ZN_PRINT_VERBOSE(format("Unpairing viewer {} from VoxelLodTerrain", paired_viewers[vi].id));
		paired_viewers[vi] = paired_viewers.back();
		paired_viewers.pop_back();
	}
}

bool add_loading_block(VoxelLodTerrainUpdateData::Lod &lod, Vector3i position) {
	auto it = lod.loading_blocks.find(position);

	if (it == lod.loading_blocks.end()) {
		// First viewer to request it
		VoxelLodTerrainUpdateData::LoadingDataBlock new_loading_block;
		new_loading_block.viewers.add();

		lod.loading_blocks.insert({ position, new_loading_block });

		return true;

	} else {
		it->second.viewers.add();
	}

	return false;
}

void process_data_blocks_sliding_box(VoxelLodTerrainUpdateData::State &state, VoxelData &data,
		std::vector<VoxelData::BlockToSave> &blocks_to_save,
		// TODO We should be able to work in BOXES to load, it can help compressing network messages
		std::vector<VoxelLodTerrainUpdateData::BlockLocation> &data_blocks_to_load,
		const VoxelLodTerrainUpdateData::Settings &settings, int lod_count, bool can_load) {
	ZN_PROFILE_SCOPE();
	ZN_ASSERT_RETURN_MSG(data.is_streaming_enabled(), "This function is not meant to run in full load mode");

	const int data_block_size = data.get_block_size();
	const int data_block_size_po2 = data.get_block_size_po2();
	const Box3i bounds_in_voxels = data.get_bounds();

	const int mesh_block_size = 1 << settings.mesh_block_size_po2;
	// const int mesh_to_data_factor = mesh_block_size / data_block_size;

	// const int lod_distance_in_mesh_chunks = get_lod_distance_in_mesh_chunks(settings.lod_distance, mesh_block_size);

	// // Data chunks are driven by mesh chunks, because mesh needs data
	// const int lod_distance_in_data_chunks = lod_distance_in_mesh_chunks * mesh_to_data_factor
	// 		// To account for the fact meshes need neighbor data chunks
	// 		+ 1;

	static thread_local std::vector<Vector3i> tls_missing_blocks;
	static thread_local std::vector<Vector3i> tls_found_blocks_positions;

#ifdef DEV_ENABLED
	Box3i debug_parent_box;
#endif

	for (const VoxelLodTerrainUpdateData::PairedViewer &paired_viewer : state.clipbox_streaming.paired_viewers) {
		// Iterating from big to small LOD so we can exit earlier if bounds don't intersect.
		for (int lod_index = lod_count - 1; lod_index >= 0; --lod_index) {
			ZN_PROFILE_SCOPE();
			VoxelLodTerrainUpdateData::Lod &lod = state.lods[lod_index];

			// Each LOD keeps a box of loaded blocks, and only some of the blocks will get polygonized.
			// The player can edit them so changes can be propagated to lower lods.

			const unsigned int lod_data_block_size_po2 = data_block_size_po2 + lod_index;

			// Should be correct as long as bounds size is a multiple of the biggest LOD chunk
			const Box3i bounds_in_data_blocks = Box3i( //
					bounds_in_voxels.pos >> lod_data_block_size_po2, //
					bounds_in_voxels.size >> lod_data_block_size_po2);

			// const Box3i new_data_box = get_lod_box_in_chunks(
			// 		viewer_pos_in_lod0_voxels, lod_distance_in_data_chunks, data_block_size_po2, lod_index)
			// 								   .clipped(bounds_in_data_blocks);

			const Box3i &new_data_box = paired_viewer.state.data_box_per_lod[lod_index];
			const Box3i &prev_data_box = paired_viewer.prev_state.data_box_per_lod[lod_index];

#ifdef DEV_ENABLED
			if (lod_index + 1 != lod_count) {
				const Box3i debug_parent_box_in_current_lod(debug_parent_box.pos << 1, debug_parent_box.size << 1);
				ZN_ASSERT(debug_parent_box_in_current_lod.contains(new_data_box));
			}
			debug_parent_box = new_data_box;
#endif

			// const Box3i prev_data_box = get_lod_box_in_chunks(
			// 		state.clipbox_streaming.viewer_pos_in_lod0_voxels_previous_update,
			// 		state.clipbox_streaming.lod_distance_in_data_chunks_previous_update, data_block_size_po2, lod_index)
			// 									.clipped(bounds_in_data_blocks);

			if (!new_data_box.intersects(bounds_in_data_blocks) && !prev_data_box.intersects(bounds_in_data_blocks)) {
				// If this box doesn't intersect either now or before, there is no chance a smaller one will
				break;
			}

			if (prev_data_box != new_data_box) {
				// Detect blocks to load.
				if (can_load) {
					tls_missing_blocks.clear();

					new_data_box.difference(prev_data_box, [&data, lod_index, &data_blocks_to_load](Box3i box_to_load) {
						data.view_area(box_to_load, lod_index, &tls_missing_blocks, nullptr, nullptr);
					});

					for (const Vector3i bpos : tls_missing_blocks) {
						if (add_loading_block(lod, bpos)) {
							data_blocks_to_load.push_back(
									VoxelLodTerrainUpdateData::BlockLocation{ bpos, static_cast<uint8_t>(lod_index) });
						}
					}
				}

				// Detect blocks to unload
				{
					tls_missing_blocks.clear();
					tls_found_blocks_positions.clear();

					prev_data_box.difference(new_data_box, [&data, &blocks_to_save, lod_index](Box3i box_to_remove) {
						data.unview_area(box_to_remove, lod_index, &tls_found_blocks_positions, &tls_missing_blocks,
								&blocks_to_save);
					});

					// Remove loading blocks (those were loaded and had their refcount reach zero)
					for (const Vector3i bpos : tls_found_blocks_positions) {
						// emit_data_block_unloaded(bpos);

						// TODO If they were loaded, why would they be in loading blocks?
						// Maybe to make sure they are not in here regardless
						lod.loading_blocks.erase(bpos);
					}

					// Remove refcount from loading blocks, and cancel loading if it reaches zero
					for (const Vector3i bpos : tls_missing_blocks) {
						auto loading_block_it = lod.loading_blocks.find(bpos);
						if (loading_block_it == lod.loading_blocks.end()) {
							ZN_PRINT_VERBOSE("Request to unview a loading block that was never requested");
							// Not expected, but fine I guess
							return;
						}

						VoxelLodTerrainUpdateData::LoadingDataBlock &loading_block = loading_block_it->second;
						loading_block.viewers.remove();

						if (loading_block.viewers.get() == 0) {
							// No longer want to load it, no data box contains it
							lod.loading_blocks.erase(loading_block_it);

							VoxelLodTerrainUpdateData::BlockLocation bloc{ bpos, static_cast<uint8_t>(lod_index) };
							for (size_t i = 0; i < data_blocks_to_load.size(); ++i) {
								if (data_blocks_to_load[i] == bloc) {
									data_blocks_to_load[i] = data_blocks_to_load.back();
									data_blocks_to_load.pop_back();
									break;
								}
							}
						}
					}
				}
			}

			// TODO Why do we do this here? Sounds like it should be done in the mesh clipbox logic
			{
				ZN_PROFILE_SCOPE_NAMED("Cancel updates");
				// Cancel mesh block updates that are not within the padded region
				// (since neighbors are always required to remesh)

				// TODO This might break at terrain borders
				const Box3i padded_new_box = new_data_box.padded(-1);
				Box3i mesh_box;
				if (mesh_block_size > data_block_size) {
					const int factor = mesh_block_size / data_block_size;
					mesh_box = padded_new_box.downscaled_inner(factor);
				} else {
					mesh_box = padded_new_box;
				}

				unordered_remove_if(lod.mesh_blocks_pending_update, [&lod, mesh_box](Vector3i bpos) {
					if (mesh_box.contains(bpos)) {
						return false;
					} else {
						auto mesh_block_it = lod.mesh_map_state.map.find(bpos);
						if (mesh_block_it != lod.mesh_map_state.map.end()) {
							mesh_block_it->second.state = VoxelLodTerrainUpdateData::MESH_NEED_UPDATE;
						}
						return true;
					}
				});
			}

		} // for each lod
	} // for each viewer

	// state.clipbox_streaming.lod_distance_in_data_chunks_previous_update = lod_distance_in_data_chunks;
}

// TODO Copypasta from octree streaming file
VoxelLodTerrainUpdateData::MeshBlockState &insert_new(
		std::unordered_map<Vector3i, VoxelLodTerrainUpdateData::MeshBlockState> &mesh_map, Vector3i pos) {
#ifdef DEBUG_ENABLED
	// We got here because the map didn't contain the element. If it did contain it already, that's a bug.
	static VoxelLodTerrainUpdateData::MeshBlockState s_default;
	ERR_FAIL_COND_V(mesh_map.find(pos) != mesh_map.end(), s_default);
#endif
	// C++ standard says if the element is not present, it will be default-constructed.
	// So here is how to insert a default, non-movable struct into an unordered_map.
	// https://stackoverflow.com/questions/22229773/map-unordered-map-with-non-movable-default-constructible-value-type
	VoxelLodTerrainUpdateData::MeshBlockState &block = mesh_map[pos];

	// This approach doesn't compile, had to workaround with the writing [] operator.
	/*
	auto p = lod.mesh_map_state.map.emplace(pos, VoxelLodTerrainUpdateData::MeshBlockState());
	// We got here because the map didn't contain the element. If it did contain it already, that's a bug.
	CRASH_COND(p.second == false);
	*/

	return block;
}

inline Vector3i get_child_position_from_first_sibling(Vector3i first_sibling_position, unsigned int child_index) {
	return Vector3i( //
			first_sibling_position.x + (child_index & 1), //
			first_sibling_position.y + ((child_index & 2) >> 1), //
			first_sibling_position.z + ((child_index & 4) >> 2));
}

inline Vector3i get_child_position(Vector3i parent_position, unsigned int child_index) {
	return get_child_position_from_first_sibling(parent_position * 2, child_index);
}

// void hide_children_recursive(
// 		VoxelLodTerrainUpdateData::State &state, unsigned int parent_lod_index, Vector3i parent_cpos) {
// 	ZN_ASSERT_RETURN(parent_lod_index > 0);
// 	const unsigned int lod_index = parent_lod_index - 1;
// 	VoxelLodTerrainUpdateData::Lod &lod = state.lods[lod_index];

// 	for (unsigned int child_index = 0; child_index < 8; ++child_index) {
// 		const Vector3i cpos = get_child_position(parent_cpos, child_index);
// 		auto mesh_it = lod.mesh_map_state.map.find(cpos);

// 		if (mesh_it != lod.mesh_map_state.map.end()) {
// 			VoxelLodTerrainUpdateData::MeshBlockState &mesh_block = mesh_it->second;

// 			if (mesh_block.active) {
// 				mesh_block.active = false;
// 				lod.mesh_blocks_to_deactivate.push_back(cpos);

// 			} else if (lod_index > 0) {
// 				hide_children_recursive(state, lod_index, cpos);
// 			}
// 		}
// 	}
// }

void process_mesh_blocks_sliding_box(VoxelLodTerrainUpdateData::State &state,
		const VoxelLodTerrainUpdateData::Settings &settings, const Box3i bounds_in_voxels, int lod_count,
		bool is_full_load_mode, bool can_load) {
	ZN_PROFILE_SCOPE();

	const int mesh_block_size_po2 = settings.mesh_block_size_po2;
	// const int mesh_block_size = 1 << mesh_block_size_po2;

	// const int lod_distance_in_mesh_chunks = get_lod_distance_in_mesh_chunks(settings.lod_distance, mesh_block_size);

#ifdef DEV_ENABLED
	Box3i debug_parent_box;
#endif

	for (const VoxelLodTerrainUpdateData::PairedViewer &paired_viewer : state.clipbox_streaming.paired_viewers) {
		// Iterating from big to small LOD so we can exit earlier if bounds don't intersect.
		for (int lod_index = lod_count - 1; lod_index >= 0; --lod_index) {
			ZN_PROFILE_SCOPE();
			VoxelLodTerrainUpdateData::Lod &lod = state.lods[lod_index];

			const int lod_mesh_block_size_po2 = mesh_block_size_po2 + lod_index;
			const int lod_mesh_block_size = 1 << lod_mesh_block_size_po2;
			// const Vector3i viewer_block_pos_within_lod = math::floor_to_int(p_viewer_pos) >> block_size_po2;

			const Box3i bounds_in_mesh_blocks = bounds_in_voxels.downscaled(lod_mesh_block_size);

			// const Box3i new_mesh_box = get_lod_box_in_chunks(
			// 		viewer_pos_in_lod0_voxels, lod_distance_in_mesh_chunks, mesh_block_size_po2, lod_index)
			// 								   .clipped(bounds_in_mesh_blocks);

			const Box3i &new_mesh_box = paired_viewer.state.mesh_box_per_lod[lod_index];
			const Box3i &prev_mesh_box = paired_viewer.prev_state.mesh_box_per_lod[lod_index];

#ifdef DEV_ENABLED
			if (lod_index + 1 != lod_count) {
				const Box3i debug_parent_box_in_current_lod(debug_parent_box.pos << 1, debug_parent_box.size << 1);
				ZN_ASSERT(debug_parent_box_in_current_lod.contains(new_mesh_box));
			}
			debug_parent_box = new_mesh_box;
#endif

			// const Box3i prev_mesh_box = get_lod_box_in_chunks(
			// 		state.clipbox_streaming.viewer_pos_in_lod0_voxels_previous_update,
			// 		state.clipbox_streaming.lod_distance_in_mesh_chunks_previous_update, mesh_block_size_po2, lod_index)
			// 									.clipped(bounds_in_mesh_blocks);

			if (!new_mesh_box.intersects(bounds_in_mesh_blocks) && !prev_mesh_box.intersects(bounds_in_mesh_blocks)) {
				// If this box doesn't intersect either now or before, there is no chance a smaller one will
				break;
			}

			if (prev_mesh_box != new_mesh_box) {
				RWLockWrite wlock(lod.mesh_map_state.map_lock);

				// Add meshes entering range
				if (can_load) {
					new_mesh_box.difference(prev_mesh_box, [&lod, lod_index, is_full_load_mode](Box3i box_to_add) {
						box_to_add.for_each_cell([&lod, is_full_load_mode](Vector3i bpos) {
							VoxelLodTerrainUpdateData::MeshBlockState *mesh_block;
							auto mesh_block_it = lod.mesh_map_state.map.find(bpos);

							if (mesh_block_it == lod.mesh_map_state.map.end()) {
								// RWLockWrite wlock(lod.mesh_map_state.map_lock);
								mesh_block = &insert_new(lod.mesh_map_state.map, bpos);

								if (is_full_load_mode) {
									// Everything is loaded up-front, so we directly trigger meshing instead of reacting
									// to data chunks being loaded
									lod.mesh_blocks_pending_update.push_back(bpos);
									mesh_block->state = VoxelLodTerrainUpdateData::MESH_UPDATE_NOT_SENT;
								}

							} else {
								mesh_block = &mesh_block_it->second;
							}

							// TODO Viewer options
							mesh_block->mesh_viewers.add();
							mesh_block->collision_viewers.add();
						});
					});
				}

				// Remove meshes out or range
				prev_mesh_box.difference(new_mesh_box, [&lod, lod_index, lod_count, &state](Box3i out_of_range_box) {
					out_of_range_box.for_each_cell([&lod](Vector3i bpos) {
						auto mesh_block_it = lod.mesh_map_state.map.find(bpos);

						if (mesh_block_it != lod.mesh_map_state.map.end()) {
							VoxelLodTerrainUpdateData::MeshBlockState &mesh_block = mesh_block_it->second;

							mesh_block.mesh_viewers.remove();
							mesh_block.collision_viewers.remove();

							if (mesh_block.mesh_viewers.get() == 0 && mesh_block.collision_viewers.get() == 0) {
								lod.mesh_map_state.map.erase(bpos);
								lod.mesh_blocks_to_unload.push_back(bpos);
							}
						}
					});

					// Immediately show parent when children are removed.
					// This is a cheap approach as the parent mesh will be available most of the time.
					// However, at high speeds, if loading can't keep up, holes and overlaps will start happening in the
					// opposite direction of movement.
					const int parent_lod_index = lod_index + 1;
					if (parent_lod_index < lod_count) {
						// Should always work without reaching zero size because non-max LODs are always
						// multiple of 2 due to subdivision rules
						const Box3i parent_box = Box3i(out_of_range_box.pos >> 1, out_of_range_box.size >> 1);

						VoxelLodTerrainUpdateData::Lod &parent_lod = state.lods[parent_lod_index];

						// Show parents when children are removed
						parent_box.for_each_cell([&parent_lod, parent_lod_index, &lod, &state](Vector3i bpos) {
							auto mesh_it = parent_lod.mesh_map_state.map.find(bpos);

							if (mesh_it != parent_lod.mesh_map_state.map.end()) {
								VoxelLodTerrainUpdateData::MeshBlockState &mesh_block = mesh_it->second;

								if (!mesh_block.active) {
									// Only do merging logic if child chunks were ACTUALLY removed.
									// In multi-viewer scenarios, the clipbox might have moved away from chunks of the
									// child LOD, but another viewer could still reference them, so we should not merge
									// them yet.
									// This check assumes there is always 8 children or no children
									const Vector3i child_bpos0 = bpos << 1;
									auto child_mesh0_it = lod.mesh_map_state.map.find(child_bpos0);
									if (child_mesh0_it != lod.mesh_map_state.map.end()) {
										// Child still referenced by another viewer, don't activate parent to avoid
										// overlap
										return;
									}

									mesh_block.active = true;
									parent_lod.mesh_blocks_to_activate.push_back(bpos);

									// We know parent_lod_index must be > 0
									// if (parent_lod_index > 0) {
									// This would actually do nothing because children were removed
									// hide_children_recursive(state, parent_lod_index, bpos);
									// }
								}
							}
						});
					}
				});
			}

			{
				ZN_PROFILE_SCOPE_NAMED("Cancel updates");
				// Cancel block updates that are not within the new region
				unordered_remove_if(lod.mesh_blocks_pending_update, [new_mesh_box](Vector3i bpos) { //
					return !new_mesh_box.contains(bpos);
				});
			}
		}
	}

	// VoxelLodTerrainUpdateData::ClipboxStreamingState &clipbox_streaming = state.clipbox_streaming;
	// clipbox_streaming.lod_distance_in_mesh_chunks_previous_update = lod_distance_in_mesh_chunks;
}

void process_loaded_data_blocks_trigger_meshing(const VoxelData &data, VoxelLodTerrainUpdateData::State &state,
		const VoxelLodTerrainUpdateData::Settings &settings, const Box3i bounds_in_voxels) {
	ZN_PROFILE_SCOPE();
	// This function should only be used when data streaming is on.
	// When everything is loaded, there is also the assumption that blocks can be generated on the fly, so loading
	// events come in sparsely for only edited areas. So it doesn't make much sense to trigger meshing in reaction to
	// data loading.
	ZN_ASSERT_RETURN(data.is_streaming_enabled());

	const int mesh_block_size_po2 = settings.mesh_block_size_po2;

	VoxelLodTerrainUpdateData::ClipboxStreamingState &clipbox_streaming = state.clipbox_streaming;

	// Get list of data blocks that were loaded since the last update
	static thread_local std::vector<VoxelLodTerrainUpdateData::BlockLocation> tls_loaded_blocks;
	tls_loaded_blocks.clear();
	{
		MutexLock mlock(clipbox_streaming.loaded_data_blocks_mutex);
		append_array(tls_loaded_blocks, clipbox_streaming.loaded_data_blocks);
		clipbox_streaming.loaded_data_blocks.clear();
	}

	// TODO Pool memory
	FixedArray<std::unordered_set<Vector3i>, constants::MAX_LOD> checked_mesh_blocks_per_lod;

	const int data_to_mesh_shift = mesh_block_size_po2 - data.get_block_size_po2();

	for (VoxelLodTerrainUpdateData::BlockLocation bloc : tls_loaded_blocks) {
		// Multiple mesh blocks may be interested because of neighbor dependencies.

		// We could group loaded blocks by LOD so we could compute a few things less times?
		const int lod_data_block_size_po2 = data.get_block_size_po2() + bloc.lod;
		const Box3i bounds_in_data_blocks = Box3i( //
				bounds_in_voxels.pos >> lod_data_block_size_po2, //
				bounds_in_voxels.size >> lod_data_block_size_po2);

		const Box3i data_neighboring =
				Box3i(bloc.position - Vector3i(1, 1, 1), Vector3i(3, 3, 3)).clipped(bounds_in_data_blocks);

		std::unordered_set<Vector3i> &checked_mesh_blocks = checked_mesh_blocks_per_lod[bloc.lod];
		VoxelLodTerrainUpdateData::Lod &lod = state.lods[bloc.lod];

		const unsigned int lod_index = bloc.lod;

		data_neighboring.for_each_cell([data_to_mesh_shift, &checked_mesh_blocks, &lod, &data, lod_index,
											   &bounds_in_data_blocks](Vector3i data_bpos) {
			const Vector3i mesh_block_pos = data_bpos >> data_to_mesh_shift;
			if (!checked_mesh_blocks.insert(mesh_block_pos).second) {
				// Already checked
				return;
			}

			// We don't add/remove items from the map here, and only the update task can do that, so no need
			// to lock
			// RWLockRead rlock(lod.mesh_map_state.map_lock);
			auto mesh_it = lod.mesh_map_state.map.find(mesh_block_pos);
			if (mesh_it == lod.mesh_map_state.map.end()) {
				// Not requested
				return;
			}
			VoxelLodTerrainUpdateData::MeshBlockState &mesh_block = mesh_it->second;
			const VoxelLodTerrainUpdateData::MeshState mesh_state = mesh_block.state;

			if (mesh_state != VoxelLodTerrainUpdateData::MESH_NEED_UPDATE &&
					mesh_state != VoxelLodTerrainUpdateData::MESH_NEVER_UPDATED) {
				// Already updated or updating
				return;
			}

			bool data_available = true;
			// if (data.is_streaming_enabled()) {
			const Box3i data_box = Box3i((mesh_block_pos << data_to_mesh_shift) - Vector3i(1, 1, 1),
					Vector3iUtil::create((1 << data_to_mesh_shift) + 2))
										   .clipped(bounds_in_data_blocks);
			// TODO Do a single grid query up-front, they will overlap so we do redundant lookups!
			data_available = data.has_all_blocks_in_area(data_box, lod_index);
			// } else {
			// 	if (!data.is_full_load_completed()) {
			// 		ZN_PRINT_ERROR("This function should not run until full load has completed");
			// 	}
			// }

			if (data_available) {
				lod.mesh_blocks_pending_update.push_back(mesh_block_pos);
				mesh_block.state = VoxelLodTerrainUpdateData::MESH_UPDATE_NOT_SENT;
				// We assume data blocks won't unload after this, until data is gathered, because unloading
				// runs before this logic.
			}
		});
	}
}

// void debug_dump_mesh_maps(const VoxelLodTerrainUpdateData::State &state, unsigned int lod_count) {
// 	std::ofstream ofs("ddd_meshmaps.json", std::ios::binary | std::ios::trunc);
// 	ofs << "[";
// 	for (unsigned int lod_index = 0; lod_index < lod_count; ++lod_index) {
// 		const VoxelLodTerrainUpdateData::Lod &lod = state.lods[lod_index];
// 		if (lod_index > 0) {
// 			ofs << ",";
// 		}
// 		ofs << "[";
// 		for (auto it = lod.mesh_map_state.map.begin(); it != lod.mesh_map_state.map.end(); ++it) {
// 			const Vector3i pos = it->first;
// 			const VoxelLodTerrainUpdateData::MeshBlockState &ms = it->second;
// 			if (it != lod.mesh_map_state.map.begin()) {
// 				ofs << ",";
// 			}
// 			ofs << "[";
// 			ofs << "[";
// 			ofs << pos.x;
// 			ofs << ",";
// 			ofs << pos.y;
// 			ofs << ",";
// 			ofs << pos.z;
// 			ofs << "],";
// 			ofs << static_cast<int>(ms.state);
// 			ofs << ",";
// 			ofs << ms.active;
// 			ofs << ",";
// 			ofs << ms.loaded;
// 			ofs << ",";
// 			ofs << ms.mesh_viewers.get();
// 			ofs << "]";
// 		}
// 		ofs << "]";
// 	}
// 	ofs << "]";
// 	ofs.close();
// }

// Activates mesh blocks when loaded. Activates higher LODs and hides lower LODs when possible.
// This essentially runs octree subdivision logic, but only from a specific node and its descendants.
void update_mesh_block_load(
		VoxelLodTerrainUpdateData::State &state, Vector3i bpos, unsigned int lod_index, unsigned int lod_count) {
	VoxelLodTerrainUpdateData::Lod &lod = state.lods[lod_index];
	auto mesh_it = lod.mesh_map_state.map.find(bpos);

	if (mesh_it == lod.mesh_map_state.map.end()) {
		return;
	}
	VoxelLodTerrainUpdateData::MeshBlockState &mesh_block = mesh_it->second;
	if (!mesh_block.loaded) {
		return;
	}

	// The mesh is loaded

	const unsigned int parent_lod_index = lod_index + 1;
	if (parent_lod_index == lod_count) {
		// Root
		// We don't need to bother about subdivison rules here (no need to check siblings) because there is no parent

		if (!mesh_block.active) {
			mesh_block.active = true;
			lod.mesh_blocks_to_activate.push_back(bpos);
		}

		if (lod_index > 0) {
			const unsigned int child_lod_index = lod_index - 1;
			for (unsigned int child_index = 0; child_index < 8; ++child_index) {
				const Vector3i child_bpos = get_child_position(bpos, child_index);
				update_mesh_block_load(state, child_bpos, child_lod_index, lod_count);
			}
		}

	} else {
		// Not root
		// We'll have to consider siblings since we can't activate only one at a time, it has to be all or none

		const Vector3i parent_bpos = bpos >> 1;
		VoxelLodTerrainUpdateData::Lod &parent_lod = state.lods[parent_lod_index];

		auto parent_mesh_it = parent_lod.mesh_map_state.map.find(parent_bpos);
		// if (parent_mesh_it == parent_lod.mesh_map_state.map.end()) {
		// 	debug_dump_mesh_maps(state, lod_count);
		// }
		// The parent must exist because sliding boxes contain each other. Maybe in the future that won't always be true
		// if a viewer has special behavior?
		ZN_ASSERT_RETURN_MSG(parent_mesh_it != parent_lod.mesh_map_state.map.end(),
				"Expected parent due to subdivision rules, bug?");

		VoxelLodTerrainUpdateData::MeshBlockState &parent_mesh_block = parent_mesh_it->second;

		if (parent_mesh_block.active) {
			bool all_siblings_loaded = true;

			// TODO This needs to be optimized. Store a cache in parent?
			for (unsigned int sibling_index = 0; sibling_index < 8; ++sibling_index) {
				const Vector3i sibling_bpos = get_child_position(parent_bpos, sibling_index);
				auto sibling_it = lod.mesh_map_state.map.find(sibling_bpos);
				if (sibling_it == lod.mesh_map_state.map.end()) {
					// Finding this in the mesh map would be weird due to subdivision rules. We don't expect a sibling
					// to be missing, because every mesh block always has 8 children.
					ZN_PRINT_ERROR("Didn't expect missing sibling");
					all_siblings_loaded = false;
					break;
				}
				const VoxelLodTerrainUpdateData::MeshBlockState &sibling = sibling_it->second;
				if (!sibling.loaded) {
					all_siblings_loaded = false;
					break;
				}
			}

			if (all_siblings_loaded) {
				// Hide parent
				parent_mesh_block.active = false;
				parent_lod.mesh_blocks_to_deactivate.push_back(parent_bpos);

				// Show siblings
				for (unsigned int sibling_index = 0; sibling_index < 8; ++sibling_index) {
					const Vector3i sibling_bpos = get_child_position(parent_bpos, sibling_index);
					auto sibling_it = lod.mesh_map_state.map.find(sibling_bpos);
					VoxelLodTerrainUpdateData::MeshBlockState &sibling = sibling_it->second;
					// TODO Optimize: if that sibling itself subdivides, it should not need to be made visible.
					// Maybe make `update_mesh_block_load` return that info so we can avoid scheduling activation?
					sibling.active = true;
					lod.mesh_blocks_to_activate.push_back(sibling_bpos);

					if (lod_index > 0) {
						const unsigned int child_lod_index = lod_index - 1;
						for (unsigned int child_index = 0; child_index < 8; ++child_index) {
							const Vector3i child_bpos = get_child_position(sibling_bpos, sibling_index);
							update_mesh_block_load(state, child_bpos, child_lod_index, lod_count);
						}
					}
				}
			}
		}
	}
}

void process_loaded_mesh_blocks_trigger_visibility_changes(
		VoxelLodTerrainUpdateData::State &state, unsigned int lod_count, bool enable_transition_updates) {
	ZN_PROFILE_SCOPE();

	VoxelLodTerrainUpdateData::ClipboxStreamingState &clipbox_streaming = state.clipbox_streaming;

	// Get list of mesh blocks that were loaded since the last update
	// TODO Use the same pool buffer as data blocks?
	static thread_local std::vector<VoxelLodTerrainUpdateData::BlockLocation> tls_loaded_blocks;
	tls_loaded_blocks.clear();
	{
		// If this has contention, we can afford trying to lock and skip if it fails
		MutexLock mlock(clipbox_streaming.loaded_mesh_blocks_mutex);
		append_array(tls_loaded_blocks, clipbox_streaming.loaded_mesh_blocks);
		clipbox_streaming.loaded_mesh_blocks.clear();
	}

	for (const VoxelLodTerrainUpdateData::BlockLocation bloc : tls_loaded_blocks) {
		update_mesh_block_load(state, bloc.position, bloc.lod, lod_count);
	}

	if (enable_transition_updates) {
		uint32_t lods_to_update_transitions = 0;
		for (const VoxelLodTerrainUpdateData::BlockLocation bloc : tls_loaded_blocks) {
			lods_to_update_transitions |= (0b111 << bloc.lod);
		}
		// TODO This is quite slow (see implementation).
		// Maybe there is a way to optimize it with the clipbox logic (updates could be grouped per new/old boxes,
		// however it wouldn't work as-is because mesh updates take time before they actually become visible. Could also
		// update masks incrementally somehow?).
		// The initial reason this streaming system was added was to help with
		// server-side performance. This feature is client-only, so it didn't need to be optimized too at the moment.
		update_transition_masks(state, lods_to_update_transitions, lod_count, true);
	}
}

} // namespace

void process_clipbox_streaming(VoxelLodTerrainUpdateData::State &state, VoxelData &data,
		Span<const std::pair<ViewerID, VoxelEngine::Viewer>> viewers, const Transform3D &volume_transform,
		std::vector<VoxelData::BlockToSave> &data_blocks_to_save,
		std::vector<VoxelLodTerrainUpdateData::BlockLocation> &data_blocks_to_load,
		const VoxelLodTerrainUpdateData::Settings &settings, Ref<VoxelStream> stream, bool can_load, bool can_mesh) {
	ZN_PROFILE_SCOPE();

	const unsigned int lod_count = data.get_lod_count();
	const Box3i bounds_in_voxels = data.get_bounds();
	const unsigned int data_block_size_po2 = data.get_block_size_po2();
	const bool streaming_enabled = data.is_streaming_enabled();
	const bool full_load_completed = data.is_full_load_completed();

	std::vector<unsigned int> unpaired_viewers_to_remove;

	process_viewers(state.clipbox_streaming, settings, lod_count, viewers, volume_transform, bounds_in_voxels,
			data_block_size_po2, can_mesh, unpaired_viewers_to_remove);

	if (streaming_enabled) {
		process_data_blocks_sliding_box(
				state, data, data_blocks_to_save, data_blocks_to_load, settings, lod_count, can_load);
	} else {
		if (full_load_completed == false) {
			// Don't do anything until things are loaded, because we'll trigger meshing directly when mesh blocks get
			// created. If we let this happen before, mesh blocks will get created but we won't have a way to tell when
			// to trigger meshing per block. If we need to do that in the future though, we could diff the "fully
			// loaded" state and iterate all mesh blocks when it becomes true?
			return;
		}
	}

	process_mesh_blocks_sliding_box(state, settings, bounds_in_voxels, lod_count, !streaming_enabled, can_load);

	// Removing paired viewers after box diffs because we interpret viewer removal as boxes becoming zero-size, so we
	// need one processing step to handle that before actually removing them
	remove_unpaired_viewers(unpaired_viewers_to_remove, state.clipbox_streaming.paired_viewers);

	if (streaming_enabled) {
		process_loaded_data_blocks_trigger_meshing(data, state, settings, bounds_in_voxels);
	}

	process_loaded_mesh_blocks_trigger_visibility_changes(state, lod_count,
			// TODO Have an option to disable transition updates, for network servers. It's a rendering feature.
			true);

	// state.clipbox_streaming.viewer_pos_in_lod0_voxels_previous_update = viewer_pos_in_lod0_voxels;
}

} // namespace zylann::voxel
