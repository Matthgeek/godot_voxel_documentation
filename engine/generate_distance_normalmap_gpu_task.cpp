#include "generate_distance_normalmap_gpu_task.h"
#include "../util/godot/funcs.h"
#include "../util/profiling.h"
#include "compute_shader.h"
#include "compute_shader_parameters.h"
#include "generate_distance_normalmap_task.h"
#include "voxel_engine.h"

#include "../util/godot/rd_sampler_state.h"
#include "../util/godot/rd_shader_source.h"
#include "../util/godot/rd_shader_spirv.h"
#include "../util/godot/rd_texture_format.h"
#include "../util/godot/rd_texture_view.h"
#include "../util/godot/rd_uniform.h"
#include "../util/godot/rendering_device.h"

namespace zylann::voxel {

void GenerateDistanceNormalMapGPUTask::prepare(GPUTaskContext &ctx) {
	ZN_PROFILE_SCOPE();

	ERR_FAIL_COND(mesh_vertices.size() == 0);
	ERR_FAIL_COND(mesh_indices.size() == 0);
	ERR_FAIL_COND(cell_triangles.size() == 0);

	ERR_FAIL_COND(shader == nullptr);
	ERR_FAIL_COND(!shader->is_valid());

	RenderingDevice &rd = ctx.rendering_device;

	// Size can vary each time so we have to recreate the format...
	Ref<RDTextureFormat> texture_format;
	texture_format.instantiate();
	texture_format->set_width(texture_width);
	texture_format->set_height(texture_height);
	texture_format->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UINT);
	texture_format->set_usage_bits(RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			// TODO Not sure if `TEXTURE_USAGE_CAN_UPDATE_BIT` is necessary, we only generate the texture
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	texture_format->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);

	// TODO Is there anything I can cache in all this? It looks very unoptimized.
	// - It seems I could pool storage buffers and call `buffer_update`?
	// - Creating a texture (image...) also requires a pool, but perhaps a more specialized one? I can't create a
	// max-sized texture to fit all cases, since it has to be downloaded after, and download speed directly depends on
	// the size of the data. Also, why should I use an image anyways?
	// - Do I really have to create a new uniform set every time I modify just one of the passed values?

	// We can't create resources while making the compute list, so we'll spaghetti a bit and have to create them first
	// for all shaders, and only then we'll create the list.

	// First output image

	Ref<RDTextureView> texture0_view;
	texture0_view.instantiate();

	// TODO Do I have to use a texture? Is it better than a storage buffer?
	_normalmap_texture0_rid = texture_create(rd, **texture_format, **texture0_view, TypedArray<PackedByteArray>());
	ERR_FAIL_COND(_normalmap_texture0_rid.is_null());

	Ref<RDUniform> image0_uniform;
	image0_uniform.instantiate();
	image0_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	image0_uniform->add_id(_normalmap_texture0_rid);

	// Second temporary image

	Ref<RDTextureView> texture1_view;
	texture1_view.instantiate();

	_normalmap_texture1_rid = texture_create(rd, **texture_format, **texture1_view, TypedArray<PackedByteArray>());
	ERR_FAIL_COND(_normalmap_texture1_rid.is_null());

	Ref<RDUniform> image1_uniform;
	image1_uniform.instantiate();
	image1_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	image1_uniform->add_id(_normalmap_texture1_rid);

	// Mesh vertices

	PackedByteArray mesh_vertices_pba;
	copy_bytes_to<Vector4f>(mesh_vertices_pba, to_span(mesh_vertices));

	_mesh_vertices_rid = rd.storage_buffer_create(mesh_vertices_pba.size(), mesh_vertices_pba);
	ERR_FAIL_COND(_mesh_vertices_rid.is_null());

	Ref<RDUniform> mesh_vertices_uniform;
	mesh_vertices_uniform.instantiate();
	mesh_vertices_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	mesh_vertices_uniform->add_id(_mesh_vertices_rid);

	// Mesh indices

	PackedByteArray mesh_indices_pba;
	copy_bytes_to<int32_t>(mesh_indices_pba, to_span(mesh_indices));

	_mesh_indices_rid = rd.storage_buffer_create(mesh_indices_pba.size(), mesh_indices_pba);
	ERR_FAIL_COND(_mesh_indices_rid.is_null());

	Ref<RDUniform> mesh_indices_uniform;
	mesh_indices_uniform.instantiate();
	mesh_indices_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	mesh_indices_uniform->add_id(_mesh_indices_rid);

	// Cell tris

	PackedByteArray cell_triangles_pba;
	copy_bytes_to<int32_t>(cell_triangles_pba, to_span(cell_triangles));

	_cell_triangles_rid = rd.storage_buffer_create(cell_triangles_pba.size(), cell_triangles_pba);
	ERR_FAIL_COND(_cell_triangles_rid.is_null());

	Ref<RDUniform> cell_triangles_uniform;
	cell_triangles_uniform.instantiate();
	cell_triangles_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	cell_triangles_uniform->add_id(_cell_triangles_rid);

	// Tiles data

	PackedByteArray tile_data_pba;
	copy_bytes_to<TileData>(tile_data_pba, to_span(tile_data));

	_tile_data_rid = rd.storage_buffer_create(tile_data_pba.size(), tile_data_pba);
	ERR_FAIL_COND(_tile_data_rid.is_null());

	Ref<RDUniform> tile_data_uniform;
	tile_data_uniform.instantiate();
	tile_data_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	tile_data_uniform->add_id(_tile_data_rid);

	// Gather hits params

	struct GatherHitsParams {
		Vector3f block_origin_world;
		float pixel_world_step;
		int32_t tile_size_pixels;
	};

	PackedByteArray gather_hits_params_pba;
	copy_bytes_to(gather_hits_params_pba,
			GatherHitsParams{ params.block_origin_world, params.pixel_world_step, params.tile_size_pixels });

	// TODO Might be better to use a Uniform Buffer for this. They might be faster for small amounts of data, but need
	// to care more about alignment
	_gather_hits_params_rid = rd.storage_buffer_create(gather_hits_params_pba.size(), gather_hits_params_pba);
	ERR_FAIL_COND(_gather_hits_params_rid.is_null());

	Ref<RDUniform> gather_hits_params_uniform;
	gather_hits_params_uniform.instantiate();
	gather_hits_params_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	gather_hits_params_uniform->add_id(_gather_hits_params_rid);

	// Hit buffer

	const unsigned int hit_positions_buffer_size_bytes =
			tile_data.size() * math::squared(params.tile_size_pixels) * sizeof(float) * 4;
	_hit_positions_buffer_rid = rd.storage_buffer_create(hit_positions_buffer_size_bytes);

	Ref<RDUniform> hit_positions_uniform;
	hit_positions_uniform.instantiate();
	hit_positions_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	hit_positions_uniform->add_id(_hit_positions_buffer_rid);

	// Modifier params

	struct ModifierParams {
		enum Ops { OP_UNION = 0, OP_SUBTRACT = 1, OP_REPLACE };

		int32_t tile_size_pixels;
		float pxiel_world_step;
		int32_t operation;
	};

	PackedByteArray modifier_params_pba;
	// TODO We may have more than one to apply in the future
	copy_bytes_to(modifier_params_pba,
			ModifierParams{ params.tile_size_pixels, params.pixel_world_step, ModifierParams::OP_REPLACE });

	_modifier_params_rid = rd.storage_buffer_create(modifier_params_pba.size(), modifier_params_pba);

	Ref<RDUniform> modifier_params_uniform;
	modifier_params_uniform.instantiate();
	modifier_params_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	modifier_params_uniform->add_id(_modifier_params_rid);

	// SD buffers

	// TODO Maybe using half-precision would work well enough?
	const unsigned int sd_buffer_size_bytes =
			tile_data.size() * math::squared(params.tile_size_pixels) * 4 * sizeof(float);

	_sd_buffer0_rid = rd.storage_buffer_create(sd_buffer_size_bytes);
	_sd_buffer1_rid = rd.storage_buffer_create(sd_buffer_size_bytes);

	Ref<RDUniform> sd_buffer0_uniform;
	sd_buffer0_uniform.instantiate();
	sd_buffer0_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	sd_buffer0_uniform->add_id(_sd_buffer0_rid);

	Ref<RDUniform> sd_buffer1_uniform;
	sd_buffer1_uniform.instantiate();
	sd_buffer1_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	sd_buffer1_uniform->add_id(_sd_buffer1_rid);

	// Normalmap params

	struct NormalmapParams {
		int32_t tile_size_pixels;
		int32_t tiles_x;
		float max_deviation_cosine;
		float max_deviation_sine;
	};

	PackedByteArray normalmap_params_pba;
	copy_bytes_to(normalmap_params_pba,
			NormalmapParams{
					params.tile_size_pixels, params.tiles_x, params.max_deviation_cosine, params.max_deviation_sine });

	_normalmap_params_rid = rd.storage_buffer_create(normalmap_params_pba.size(), normalmap_params_pba);

	Ref<RDUniform> normalmap_params_uniform;
	normalmap_params_uniform.instantiate();
	normalmap_params_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	normalmap_params_uniform->add_id(_normalmap_params_rid);

	// Dilation params

	PackedByteArray dilation_params_pba;
	// I need only 4 but apparently the minimum size for UBO is 16 bytes
	dilation_params_pba.resize(16);
	*reinterpret_cast<int32_t *>(dilation_params_pba.ptrw()) = params.tile_size_pixels;

	// TODO Might be better to use a Uniform Buffer for this. They might be faster for small amounts of data.
	_dilation_params_rid = rd.uniform_buffer_create(dilation_params_pba.size(), dilation_params_pba);
	ERR_FAIL_COND(_dilation_params_rid.is_null());

	Ref<RDUniform> dilation_params_uniform;
	dilation_params_uniform.instantiate();
	dilation_params_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	dilation_params_uniform->add_id(_dilation_params_rid);

	// Pipelines
	// Not sure what a pipeline is required for in compute shaders, it seems to be required "just because"

	const RID gather_hits_shader_rid = VoxelEngine::get_singleton().get_detail_gather_hits_compute_shader().get_rid();
	ERR_FAIL_COND(gather_hits_shader_rid.is_null());
	// TODO Perhaps we could cache this pipeline?
	_gather_hits_pipeline_rid = rd.compute_pipeline_create(gather_hits_shader_rid);
	ERR_FAIL_COND(_gather_hits_pipeline_rid.is_null());

	const RID shader_rid = shader->get_rid();
	_detail_modifier_pipeline_rid = rd.compute_pipeline_create(shader_rid);
	ERR_FAIL_COND(_detail_modifier_pipeline_rid.is_null());

	const RID detail_normalmap_shader_rid =
			VoxelEngine::get_singleton().get_detail_normalmap_compute_shader().get_rid();
	ERR_FAIL_COND(detail_normalmap_shader_rid.is_null());
	// TODO Perhaps we could cache this pipeline?
	_detail_normalmap_pipeline_rid = rd.compute_pipeline_create(detail_normalmap_shader_rid);
	ERR_FAIL_COND(_detail_normalmap_pipeline_rid.is_null());

	const RID dilation_shader_rid = VoxelEngine::get_singleton().get_dilate_normalmap_compute_shader().get_rid();
	ERR_FAIL_COND(dilation_shader_rid.is_null());
	// TODO Perhaps we could cache this pipeline?
	_normalmap_dilation_pipeline_rid = rd.compute_pipeline_create(dilation_shader_rid);
	ERR_FAIL_COND(_normalmap_dilation_pipeline_rid.is_null());

	// Make compute list

	const int compute_list_id = rd.compute_list_begin();

	// Gather hits
	{
		mesh_vertices_uniform->set_binding(0);
		mesh_indices_uniform->set_binding(1);
		cell_triangles_uniform->set_binding(2);
		tile_data_uniform->set_binding(3);
		gather_hits_params_uniform->set_binding(4);
		hit_positions_uniform->set_binding(5);

		Array gather_hits_uniforms;
		gather_hits_uniforms.resize(6);
		gather_hits_uniforms[0] = mesh_vertices_uniform;
		gather_hits_uniforms[1] = mesh_indices_uniform;
		gather_hits_uniforms[2] = cell_triangles_uniform;
		gather_hits_uniforms[3] = tile_data_uniform;
		gather_hits_uniforms[4] = gather_hits_params_uniform;
		gather_hits_uniforms[5] = hit_positions_uniform;

		const RID gather_hits_uniform_set_rid = uniform_set_create(rd, gather_hits_uniforms, gather_hits_shader_rid, 0);

		rd.compute_list_bind_compute_pipeline(compute_list_id, _gather_hits_pipeline_rid);
		rd.compute_list_bind_uniform_set(compute_list_id, gather_hits_uniform_set_rid, 0);

		const int local_group_size_x = 4;
		const int local_group_size_y = 4;
		const int local_group_size_z = 4;
		rd.compute_list_dispatch(compute_list_id, //
				math::ceildiv(params.tile_size_pixels, local_group_size_x),
				math::ceildiv(params.tile_size_pixels, local_group_size_y),
				math::ceildiv(tile_data.size(), local_group_size_z));
	}

	// Ensure dependencies are ready before running dilation on the result (I though this was automatically handled?
	// Why is barrier necessary anyways? Is dependency resolution actually not automatic?)
	rd.compute_list_add_barrier(compute_list_id);

	// Generate signed distances
	{
		hit_positions_uniform->set_binding(0);
		modifier_params_uniform->set_binding(1);
		sd_buffer0_uniform->set_binding(2);
		sd_buffer1_uniform->set_binding(3);

		Array detail_modifier_uniforms;
		detail_modifier_uniforms.resize(4);
		detail_modifier_uniforms[0] = hit_positions_uniform;
		detail_modifier_uniforms[1] = modifier_params_uniform;
		detail_modifier_uniforms[2] = sd_buffer0_uniform;
		detail_modifier_uniforms[3] = sd_buffer1_uniform;

		// Extra params
		if (shader_params != nullptr && shader_params->params.size() > 0) {
			for (ComputeShaderParameters::Param &p : shader_params->params) {
				ZN_ASSERT(p.resource.is_valid());
				// Only textures expected for now
				ZN_ASSERT(p.resource.get_type() == ComputeShaderResource::TYPE_TEXTURE);

				Ref<RDUniform> tex_uniform;
				tex_uniform.instantiate();
				tex_uniform->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
				tex_uniform->set_binding(p.binding);
				// This API is just weird. Also, it switches to using a dynamically allocated vector if more than one ID
				// is provided. But seriously, why is it not a fixed-size vector, considering that I can't think of
				// cases that would require more than a few IDs?
				tex_uniform->add_id(VoxelEngine::get_singleton().get_filtering_sampler());
				tex_uniform->add_id(p.resource.get_rid());
				detail_modifier_uniforms.append(tex_uniform);
			}
		}

		const RID detail_modifier_uniform_set = uniform_set_create(rd, detail_modifier_uniforms, shader_rid, 0);

		rd.compute_list_bind_compute_pipeline(compute_list_id, _detail_modifier_pipeline_rid);
		rd.compute_list_bind_uniform_set(compute_list_id, detail_modifier_uniform_set, 0);

		const int local_group_size_x = 4;
		const int local_group_size_y = 4;
		const int local_group_size_z = 4;
		rd.compute_list_dispatch(compute_list_id, //
				math::ceildiv(params.tile_size_pixels, local_group_size_x),
				math::ceildiv(params.tile_size_pixels, local_group_size_y),
				math::ceildiv(tile_data.size(), local_group_size_z));
	}

	rd.compute_list_add_barrier(compute_list_id);

	// Normalmap rendering
	{
		sd_buffer1_uniform->set_binding(0);
		mesh_vertices_uniform->set_binding(1);
		mesh_indices_uniform->set_binding(2);
		hit_positions_uniform->set_binding(3);
		normalmap_params_uniform->set_binding(4);
		image0_uniform->set_binding(5);

		Array detail_normalmap_uniforms;
		detail_normalmap_uniforms.resize(6);
		detail_normalmap_uniforms[0] = sd_buffer1_uniform;
		detail_normalmap_uniforms[1] = mesh_vertices_uniform;
		detail_normalmap_uniforms[2] = mesh_indices_uniform;
		detail_normalmap_uniforms[3] = hit_positions_uniform;
		detail_normalmap_uniforms[4] = normalmap_params_uniform;
		detail_normalmap_uniforms[5] = image0_uniform;

		const RID detail_normalmap_uniform_set_rid =
				uniform_set_create(rd, detail_normalmap_uniforms, detail_normalmap_shader_rid, 0);

		rd.compute_list_bind_compute_pipeline(compute_list_id, _detail_normalmap_pipeline_rid);
		rd.compute_list_bind_uniform_set(compute_list_id, detail_normalmap_uniform_set_rid, 0);

		const int local_group_size_x = 4;
		const int local_group_size_y = 4;
		const int local_group_size_z = 4;
		rd.compute_list_dispatch(compute_list_id, //
				math::ceildiv(params.tile_size_pixels, local_group_size_x),
				math::ceildiv(params.tile_size_pixels, local_group_size_y),
				math::ceildiv(tile_data.size(), local_group_size_z));
	}

	rd.compute_list_add_barrier(compute_list_id);

	// Dilation step 1
	{
		image0_uniform->set_binding(0);
		image1_uniform->set_binding(1);
		dilation_params_uniform->set_binding(2);

		Array dilation_uniforms;
		dilation_uniforms.resize(3);
		// Bindings should be respectively 0 and 1 at this point
		dilation_uniforms[0] = image0_uniform;
		dilation_uniforms[1] = image1_uniform;
		dilation_uniforms[2] = dilation_params_uniform;
		const RID dilation_uniform_set_rid = uniform_set_create(rd, dilation_uniforms, dilation_shader_rid, 0);

		rd.compute_list_bind_compute_pipeline(compute_list_id, _normalmap_dilation_pipeline_rid);
		rd.compute_list_bind_uniform_set(compute_list_id, dilation_uniform_set_rid, 0);

		const int local_group_size_x = 8;
		const int local_group_size_y = 8;
		const int local_group_size_z = 1;
		rd.compute_list_dispatch(compute_list_id, math::ceildiv(texture_width, local_group_size_x),
				math::ceildiv(texture_height, local_group_size_y), local_group_size_z);
	}

	rd.compute_list_add_barrier(compute_list_id);

	// Dilation step 2
	{
		// Swap images

		image1_uniform->set_binding(0);
		image0_uniform->set_binding(1);

		// Uniform set

		Array dilation_uniforms;
		dilation_uniforms.resize(3);
		dilation_uniforms[0] = image1_uniform;
		dilation_uniforms[1] = image0_uniform;
		dilation_uniforms[2] = dilation_params_uniform;
		// TODO Do I really have to create a new uniform set every time I modify just one of the passed values?
		const RID dilation_uniform_set_rid = uniform_set_create(rd, dilation_uniforms, dilation_shader_rid, 0);

		rd.compute_list_bind_uniform_set(compute_list_id, dilation_uniform_set_rid, 0);

		const int local_group_size_x = 8;
		const int local_group_size_y = 8;
		const int local_group_size_z = 1;
		rd.compute_list_dispatch(compute_list_id, math::ceildiv(texture_width, local_group_size_x),
				math::ceildiv(texture_height, local_group_size_y), local_group_size_z);
	}

	// Final result should be in image0.

	rd.compute_list_end();
}

PackedByteArray GenerateDistanceNormalMapGPUTask::collect_texture_and_cleanup(RenderingDevice &rd) {
	ZN_PROFILE_SCOPE();

	// TODO This is incredibly slow and should not happen in the first place.
	// But due to how Godot is designed right now, it is not possible to create a texture from the output of a compute
	// shader without first downloading it back to RAM...
	PackedByteArray texture_data = rd.texture_get_data(_normalmap_texture0_rid, 0);

	{
		ZN_PROFILE_SCOPE_NAMED("Cleanup");

		free_rendering_device_rid(rd, _normalmap_texture0_rid);
		free_rendering_device_rid(rd, _normalmap_texture1_rid);

		free_rendering_device_rid(rd, _gather_hits_pipeline_rid);
		free_rendering_device_rid(rd, _detail_modifier_pipeline_rid);
		free_rendering_device_rid(rd, _detail_normalmap_pipeline_rid);
		free_rendering_device_rid(rd, _normalmap_dilation_pipeline_rid);

		free_rendering_device_rid(rd, _mesh_vertices_rid);
		free_rendering_device_rid(rd, _mesh_indices_rid);
		free_rendering_device_rid(rd, _cell_triangles_rid);
		free_rendering_device_rid(rd, _tile_data_rid);
		free_rendering_device_rid(rd, _gather_hits_params_rid);
		free_rendering_device_rid(rd, _dilation_params_rid);
		free_rendering_device_rid(rd, _hit_positions_buffer_rid);
		free_rendering_device_rid(rd, _modifier_params_rid);
		free_rendering_device_rid(rd, _sd_buffer0_rid);
		free_rendering_device_rid(rd, _sd_buffer1_rid);
		free_rendering_device_rid(rd, _normalmap_params_rid);
	}

	// Uniform sets auto-free themselves once their contents are freed.
	// rd.free(_uniform_set_rid);
	return texture_data;
}

void GenerateDistanceNormalMapGPUTask::collect(GPUTaskContext &ctx) {
	ZN_PROFILE_SCOPE();
	RenderingDevice &rd = ctx.rendering_device;

	PackedByteArray texture_data = collect_texture_and_cleanup(rd);

	{
		std::vector<NormalMapData::Tile> tile_data2;
		tile_data2.reserve(tile_data.size());
		for (const TileData &td : tile_data) {
			tile_data2.push_back(NormalMapData::Tile{ td.cell_x, td.cell_y, td.cell_z, uint8_t(td.data & 0x3) });
		}

		RenderVirtualTexturePass2Task *task = ZN_NEW(RenderVirtualTexturePass2Task);
		task->atlas_data = texture_data;
		task->tile_data = std::move(tile_data2);
		task->edited_tiles_normalmap_data = std::move(edited_tiles_normalmap_data);
		task->virtual_textures = output;
		task->volume_id = volume_id;
		task->mesh_block_position = block_position;
		task->mesh_block_size = block_size;
		task->atlas_width = texture_width;
		task->atlas_height = texture_height;
		task->lod_index = lod_index;
		task->tile_size_pixels = params.tile_size_pixels;

		VoxelEngine::get_singleton().push_async_task(task);
	}
}

} // namespace zylann::voxel
