/* Copyright (c) 2017-2021 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "volumetric_diffuse.hpp"
#include "device.hpp"
#include "scene.hpp"
#include "task_composer.hpp"
#include "render_context.hpp"
#include "scene_renderer.hpp"
#include "muglm/matrix_helper.hpp"
#include "quirks.hpp"
#include "clusterer.hpp"

namespace Granite
{
static constexpr float ZNear = 0.1f;
static constexpr float ZFar = 200.0f;
static constexpr unsigned NumProbeLayers = 4;
static constexpr unsigned ProbeResolution = 8;

VolumetricDiffuseLightManager::VolumetricDiffuseLightManager()
{
	for (unsigned face = 0; face < 6; face++)
	{
		mat4 proj, view;
		compute_cube_render_transform(vec3(0.0f), face, proj, view, ZNear, ZFar);
		inv_view_projections[face] = inverse(proj * view);
	}

	probe_pos_jitter[0] = vec4(-3.0f / 16.0f, +1.0f / 16.0f, +5.0f / 16.0f, 0.0f);
	probe_pos_jitter[1] = vec4(+1.0f / 16.0f, -3.0f / 16.0f, +3.0f / 16.0f, 0.0f);
	probe_pos_jitter[2] = vec4(-1.0f / 16.0f, +3.0f / 16.0f, -5.0f / 16.0f, 0.0f);
	probe_pos_jitter[3] = vec4(+3.0f / 16.0f, -1.0f / 16.0f, -3.0f / 16.0f, 0.0f);
}

void VolumetricDiffuseLightManager::set_scene(Scene *scene_)
{
	scene = scene_;
	volumetric_diffuse = &scene->get_entity_pool().get_component_group<VolumetricDiffuseLightComponent>();
}

void VolumetricDiffuseLightManager::set_base_renderer(const RendererSuite *suite_)
{
	suite = suite_;
}

enum class TransitionMode { Discard, Read };

static void transition_gbuffer(Vulkan::CommandBuffer &cmd,
                               const VolumetricDiffuseLight::GBuffer &gbuffer,
                               TransitionMode mode)
{
	const Vulkan::Image *colors[] = {
		gbuffer.emissive.get(),
		gbuffer.albedo.get(),
		gbuffer.normal.get(),
		gbuffer.pbr.get(),
	};

	VkPipelineStageFlags src_color, src_depth, dst_color, dst_depth;
	VkAccessFlags src_access_color, src_access_depth, dst_access_color, dst_access_depth;
	VkImageLayout old_color, new_color, old_depth, new_depth;

	if (mode == TransitionMode::Discard)
	{
		src_color = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		src_depth = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		dst_color = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dst_depth = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

		src_access_color = 0;
		src_access_depth = 0;
		dst_access_color = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		dst_access_depth = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

		old_color = VK_IMAGE_LAYOUT_UNDEFINED;
		old_depth = VK_IMAGE_LAYOUT_UNDEFINED;
		new_color = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		new_depth = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	}
	else
	{
		src_color = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		src_depth = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		src_access_color = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		src_access_depth = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		dst_color = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		dst_depth = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		dst_access_color = VK_ACCESS_SHADER_READ_BIT;
		dst_access_depth = VK_ACCESS_SHADER_READ_BIT;

		old_color = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		old_depth = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		new_color = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		new_depth = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	for (auto *image : colors)
	{
		cmd.image_barrier(*image, old_color, new_color,
		                  src_color, src_access_color,
		                  dst_color, dst_access_color);
	}

	cmd.image_barrier(*gbuffer.depth, old_depth, new_depth,
	                  src_depth, src_access_depth,
	                  dst_depth, dst_access_depth);
}

static unsigned layer_to_probe_jitter(unsigned layer, unsigned x, unsigned y)
{
	return (layer + (y & 1) * 2 + (x & 1)) % NumProbeLayers;
}

void VolumetricDiffuseLightManager::light_probe_buffer(Vulkan::CommandBuffer &cmd,
                                                       VolumetricDiffuseLightComponent &light)
{
	struct Push
	{
		uint32_t gbuffer_layer;
		uint32_t patch_resolution;
		uint32_t face_resolution;
		float inv_patch_resolution;
		float inv_patch_resolution2;
	} push = {};

	struct ProbeTransform
	{
		vec4 texture_to_world[3];
		vec4 world_to_texture[3];
		vec3 inv_resolution;
	};

	push.gbuffer_layer = light.update_iteration % NumProbeLayers;
	light.update_iteration++;

	auto *probe_transform = cmd.allocate_typed_constant_data<ProbeTransform>(3, 1, 1);
	memcpy(probe_transform->texture_to_world, light.texture_to_world, sizeof(light.texture_to_world));
	memcpy(probe_transform->world_to_texture, light.world_to_texture, sizeof(light.world_to_texture));
	probe_transform->inv_resolution = vec3(1.0f) / vec3(light.light.get_resolution());

	push.patch_resolution = ProbeResolution / 2;
	push.face_resolution = ProbeResolution;
	push.inv_patch_resolution = 1.0f / float(push.patch_resolution);
	push.inv_patch_resolution2 = push.inv_patch_resolution * push.inv_patch_resolution;

	uvec3 res = light.light.get_resolution();

	auto flags = Renderer::get_mesh_renderer_options_from_lighting(*fallback_render_context->get_lighting_parameters());
	flags &= ~(Renderer::VOLUMETRIC_FOG_ENABLE_BIT |
	           Renderer::AMBIENT_OCCLUSION_BIT |
	           Renderer::VOLUMETRIC_DIFFUSE_ENABLE_BIT);
	auto defines = Renderer::build_defines_from_renderer_options(RendererType::GeneralForward, flags);

	if (flags & Renderer::SHADOW_CASCADE_ENABLE_BIT)
	{
		auto &subgroup = cmd.get_device().get_device_features().subgroup_properties;
		if ((subgroup.supportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0 &&
		    !Vulkan::ImplementationQuirks::get().force_no_subgroups &&
		    (subgroup.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) != 0)
		{
			defines.emplace_back("SUBGROUP_ARITHMETIC", 1);
		}
	}

	cmd.set_program("builtin://shaders/lights/volumetric_hemisphere_integral.comp", defines);
	cmd.push_constants(&push, 0, sizeof(push));
	cmd.set_storage_texture(2, 0, *light.light.get_volume_view());
	cmd.set_texture(2, 1, light.light.get_gbuffer().emissive->get_view());
	cmd.set_texture(2, 2, light.light.get_gbuffer().albedo->get_view());
	cmd.set_texture(2, 3, light.light.get_gbuffer().normal->get_view());
	cmd.set_texture(2, 4, light.light.get_gbuffer().depth->get_view());
	cmd.set_texture(2, 5, *light.light.get_prev_volume_view(), Vulkan::StockSampler::LinearClamp);
	cmd.dispatch(res.x, res.y, res.z);
}

struct ContextRenderers
{
	RenderContext contexts[6];
	RenderPassSceneRenderer renderers[6];
};

static std::shared_ptr<ContextRenderers> create_cube_renderer(const RenderPassSceneRenderer::Setup &base)
{
	auto renderers = std::make_shared<ContextRenderers>();
	for (unsigned face = 0; face < 6; face++)
	{
		RenderPassSceneRenderer::Setup setup = base;
		setup.context = &renderers->contexts[face];
		renderers->renderers[face].init(setup);
		renderers->renderers[face].set_extra_flush_flags(Renderer::FRONT_FACE_CLOCKWISE_BIT);
	}
	return renderers;
}

TaskGroupHandle VolumetricDiffuseLightManager::create_probe_gbuffer(TaskComposer &composer, TaskGroup &incoming,
                                                                    const RenderContext &context,
                                                                    VolumetricDiffuseLightComponent &light)
{
	auto &device = context.get_device();

	VolumetricDiffuseLight::GBuffer allocated_gbuffer;

	uvec3 resolution = light.light.get_resolution();
	auto gbuffer_info = Vulkan::ImageCreateInfo::render_target(ProbeResolution * resolution.x * 6,
	                                                           ProbeResolution * resolution.y * resolution.z,
	                                                           VK_FORMAT_R8G8B8A8_SRGB);
	gbuffer_info.layers = NumProbeLayers;
	gbuffer_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	gbuffer_info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	allocated_gbuffer.albedo = device.create_image(gbuffer_info);

	bool supports_32bpp =
			device.image_format_is_supported(VK_FORMAT_B10G11R11_UFLOAT_PACK32,
			                                 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
	gbuffer_info.format = supports_32bpp ? VK_FORMAT_B10G11R11_UFLOAT_PACK32 : VK_FORMAT_R16G16B16A16_SFLOAT;
	allocated_gbuffer.emissive = device.create_image(gbuffer_info);

	gbuffer_info.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	allocated_gbuffer.normal = device.create_image(gbuffer_info);

	gbuffer_info.format = VK_FORMAT_R8G8_UNORM;
	allocated_gbuffer.pbr = device.create_image(gbuffer_info);

	gbuffer_info.format = device.get_default_depth_stencil_format();
	gbuffer_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	allocated_gbuffer.depth = device.create_image(gbuffer_info);

	device.set_name(*allocated_gbuffer.emissive, "probe-emissive");
	device.set_name(*allocated_gbuffer.albedo, "probe-albedo");
	device.set_name(*allocated_gbuffer.normal, "probe-normal");
	device.set_name(*allocated_gbuffer.pbr, "probe-pbr");
	device.set_name(*allocated_gbuffer.depth, "probe-depth");

	light.light.set_probe_gbuffer(std::move(allocated_gbuffer));

	RenderPassSceneRenderer::Setup setup = {};
	setup.flags = SCENE_RENDERER_DEFERRED_GBUFFER_BIT;
	setup.deferred_lights = nullptr;
	setup.suite = suite;
	setup.scene = scene;

	std::vector<std::shared_ptr<ContextRenderers>> slice_renderers;
	slice_renderers.reserve(resolution.x);
	for (unsigned x = 0; x < resolution.x; x++)
		slice_renderers.push_back(create_cube_renderer(setup));

	TaskComposer probe_composer(*incoming.get_thread_group());
	probe_composer.set_incoming_task(composer.get_pipeline_stage_dependency());

	Vulkan::RenderPassInfo rp;
	memset(rp.clear_color, 0, sizeof(rp.clear_color));
	rp.clear_depth_stencil.depth = 1.0f;
	rp.clear_depth_stencil.stencil = 0;
	rp.clear_attachments = 0xf;
	rp.store_attachments = 0xf;
	rp.op_flags = Vulkan::RENDER_PASS_OP_CLEAR_DEPTH_STENCIL_BIT | Vulkan::RENDER_PASS_OP_STORE_DEPTH_STENCIL_BIT;
	rp.num_color_attachments = 4;
	auto &gbuffer = light.light.get_gbuffer();
	rp.color_attachments[0] = &gbuffer.emissive->get_view();
	rp.color_attachments[1] = &gbuffer.albedo->get_view();
	rp.color_attachments[2] = &gbuffer.normal->get_view();
	rp.color_attachments[3] = &gbuffer.pbr->get_view();
	rp.depth_stencil = &gbuffer.depth->get_view();

	auto &discard_stage = probe_composer.begin_pipeline_stage();
	discard_stage.enqueue_task([&device, &light]() {
		auto cmd = device.request_command_buffer();
		transition_gbuffer(*cmd, light.light.get_gbuffer(), TransitionMode::Discard);
		device.submit(cmd);
	});

	for (unsigned layer = 0; layer < NumProbeLayers; layer++)
	{
		for (unsigned z = 0; z < resolution.z; z++)
		{
			for (unsigned y = 0; y < resolution.y; y++)
			{
				auto &context_setup = probe_composer.begin_pipeline_stage();

				for (unsigned x = 0; x < resolution.x; x++)
				{
					auto &renderers = slice_renderers[x];
					for (unsigned face = 0; face < 6; face++)
					{
						context_setup.enqueue_task([this, x, y, z, face, layer, resolution, renderers, &light]() {
							vec3 tex = (vec3(x, y, z) + 0.5f + probe_pos_jitter[layer_to_probe_jitter(layer, x, y)].xyz()) / vec3(resolution);
							vec3 center = vec3(
									dot(light.texture_to_world[0], vec4(tex, 1.0f)),
									dot(light.texture_to_world[1], vec4(tex, 1.0f)),
									dot(light.texture_to_world[2], vec4(tex, 1.0f)));

							mat4 proj, view;
							compute_cube_render_transform(center, face, proj, view, ZNear, ZFar);
							renderers->contexts[face].set_camera(proj, view);
						});
					}
				}

				auto &prepare_stage = probe_composer.begin_pipeline_stage();

				for (unsigned x = 0; x < resolution.x; x++)
				{
					auto &renderers = slice_renderers[x];
					for (unsigned face = 0; face < 6; face++)
					{
						TaskComposer face_composer(probe_composer.get_thread_group());
						face_composer.set_incoming_task(probe_composer.get_pipeline_stage_dependency());
						VkSubpassContents contents = VK_SUBPASS_CONTENTS_INLINE;
						renderers->renderers[face].enqueue_prepare_render_pass(face_composer, rp, 0, contents);
						probe_composer.get_thread_group().add_dependency(prepare_stage, *face_composer.get_outgoing_task());
					}
				}

				auto &render_task = probe_composer.begin_pipeline_stage();

				for (unsigned x = 0; x < resolution.x; x++)
				{
					auto &renderers = slice_renderers[x];
					render_task.enqueue_task([x, y, z, layer, renderers, rp, &light, &device]() {
						auto cmd = device.request_command_buffer();
						cmd->begin_region("render-probe-gbuffer");

						auto slice_rp = rp;
						slice_rp.render_area.offset.x = 6 * x * ProbeResolution;
						slice_rp.render_area.offset.y =
								(z * light.light.get_resolution().y + y) * ProbeResolution;
						slice_rp.render_area.extent.width = ProbeResolution * 6;
						slice_rp.render_area.extent.height = ProbeResolution;
						slice_rp.base_layer = layer;

						cmd->begin_render_pass(slice_rp);
						slice_rp.render_area.extent.width = ProbeResolution;

						for (unsigned face = 0; face < 6; face++)
						{
							const VkViewport vp = {
								float(slice_rp.render_area.offset.x),
								float(slice_rp.render_area.offset.y),
								float(slice_rp.render_area.extent.width),
								float(slice_rp.render_area.extent.height),
								0.0f, 1.0f,
							};
							cmd->set_viewport(vp);
							cmd->set_scissor(slice_rp.render_area);
							renderers->renderers[face].build_render_pass(*cmd);
							slice_rp.render_area.offset.x += ProbeResolution;
						}
						cmd->end_render_pass();
						cmd->end_region();
						device.submit(cmd);

						LOGI("Rendering gbuffer probe ... X = %u, Y = %u, Z = %u, layer = %u.\n",
						     x, y, z, layer);
					});
				}

				auto &drain_task = probe_composer.begin_pipeline_stage();
				drain_task.enqueue_task([&device]() {
					// We're going to be consuming a fair bit of memory,
					// so make sure to pump frame contexts through.
					// This code is not assumed to be hot (should be pre-baked).
					device.next_frame_context();
				});
			}
		}
	}

	auto &task = probe_composer.begin_pipeline_stage();
	task.enqueue_task([&device, &light]() {
		auto cmd = device.request_command_buffer();
		transition_gbuffer(*cmd, light.light.get_gbuffer(), TransitionMode::Read);
		uvec3 res = light.light.get_resolution();

		if (!light.light.get_volume_view())
		{
			auto info = Vulkan::ImageCreateInfo::immutable_3d_image(6 * res.x, res.y, res.z, VK_FORMAT_R16G16B16A16_SFLOAT);
			info.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
			info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

			auto image = device.create_image(info);
			auto prev_image = device.create_image(info);
			device.set_name(*image, "probe-light-1");
			device.set_name(*prev_image, "probe-light-2");
			image->set_layout(Vulkan::Layout::General);
			prev_image->set_layout(Vulkan::Layout::General);

			const auto clear = [](Vulkan::CommandBuffer &clear_cmd, Vulkan::Image &clear_image) {
				clear_cmd.image_barrier(clear_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
				                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
				                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
				clear_cmd.clear_image(clear_image, {});
				clear_cmd.image_barrier(clear_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
				                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
				                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
			};

			clear(*cmd, *image);
			clear(*cmd, *prev_image);

			light.light.set_volumes(std::move(image), std::move(prev_image));
		}

		device.submit(cmd);
		device.next_frame_context();
	});

	return probe_composer.get_outgoing_task();
}

void VolumetricDiffuseLightManager::refresh(const RenderContext &context, TaskComposer &composer)
{
	if (!volumetric_diffuse)
		return;
	auto &group = composer.begin_pipeline_stage();

	for (auto &light_tuple : *volumetric_diffuse)
	{
		auto *light = get_component<VolumetricDiffuseLightComponent>(light_tuple);
		light->light.swap_volumes();

		if (!light->light.get_gbuffer().emissive)
		{
			auto task = create_probe_gbuffer(composer, group, context, *light);
			composer.get_thread_group().add_dependency(group, *task);
		}
	}
}

void VolumetricDiffuseLightManager::message(const std::string &, uint32_t, uint32_t x, uint32_t y, uint32_t z,
                                            uint32_t word_count, const Vulkan::DebugChannelInterface::Word *words)
{
	LOGI("Probe: (%u, %u, %u)\n", x, y, z);
	for (uint32_t i = 0; i < word_count; i++)
	{
		LOGI("  %f\n", words[i].f32);
	}
}

void VolumetricDiffuseLightManager::add_render_passes(RenderGraph &graph)
{
	auto &light_pass = graph.add_pass("probe-light", RENDER_GRAPH_QUEUE_COMPUTE_BIT);
	light_pass.add_proxy_output("probe-light-proxy", VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	light_pass.set_build_render_pass([this](Vulkan::CommandBuffer &cmd) {
		Renderer::bind_global_parameters(cmd, *fallback_render_context);
		Renderer::bind_lighting_parameters(cmd, *fallback_render_context);

		if (fallback_render_context->get_lighting_parameters() &&
		    fallback_render_context->get_lighting_parameters()->cluster)
		{
			auto *global_transforms = cmd.allocate_typed_constant_data<ClustererGlobalTransforms>(3, 2, 1);
			memcpy(global_transforms,
			       &fallback_render_context->get_lighting_parameters()->cluster->get_cluster_global_transforms_bindless(),
			       sizeof(*global_transforms));
		}

		struct GlobalTransform
		{
			mat4 inv_view_proj_for_face[6];
			vec4 probe_pos_jitter[NumProbeLayers];
		};

		auto *transforms = cmd.allocate_typed_constant_data<GlobalTransform>(3, 0, 1);
		memcpy(transforms->inv_view_proj_for_face, inv_view_projections, sizeof(inv_view_projections));
		memcpy(transforms->probe_pos_jitter, probe_pos_jitter, sizeof(probe_pos_jitter));

		for (auto &light_tuple : *volumetric_diffuse)
		{
			// TODO: Check visibility?
			auto *light = get_component<VolumetricDiffuseLightComponent>(light_tuple);
			light_probe_buffer(cmd, *light);
		}
	});

	light_pass.add_texture_input("shadow-fallback");
}

void VolumetricDiffuseLightManager::set_base_render_context(const RenderContext *)
{
}

void VolumetricDiffuseLightManager::set_fallback_render_context(const RenderContext *context)
{
	fallback_render_context = context;
}

void VolumetricDiffuseLightManager::setup_render_pass_dependencies(RenderGraph &graph, RenderPass &target)
{
	target.add_proxy_input("probe-light-proxy", VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	auto *light_pass = graph.find_pass("probe-light");
	assert(light_pass);
	if (graph.find_pass("clustering-bindless"))
	{
		light_pass->add_storage_read_only_input("cluster-transforms");
		light_pass->add_external_lock("bindless-shadowmaps", VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	}

	if (graph.find_pass("shadow-fallback"))
		light_pass->add_texture_input("shadow-fallback");
}

void VolumetricDiffuseLightManager::setup_render_pass_resources(RenderGraph &)
{
}
}