#include "ggml_extend.hpp"

#include "model.h"
#include "rng.hpp"
#include "rng_philox.hpp"
#include "stable-diffusion.h"
#include "util.h"

#include "conditioner.hpp"
#include "control.hpp"
#include "denoiser.hpp"
#include "diffusion_model.hpp"
#include "esrgan.hpp"
#include "lora.hpp"
#include "pmid.hpp"
#include "tae.hpp"
#include "vae.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#include "stb_image.h"

#include "latent-preview.h"

// #define STB_IMAGE_WRITE_IMPLEMENTATION
// #define STB_IMAGE_WRITE_STATIC
// #include "stb_image_write.h"

const char* model_version_to_str[] = {
    "SD 1.x",
    "SD 1.x Inpaint",
    "Instruct-Pix2Pix",
    "SD 2.x",
    "SD 2.x Inpaint",
    "SDXL",
    "SDXL Inpaint",
    "SDXL Instruct-Pix2Pix",
    "SVD",
    "SD3.x",
    "SD3 Instruct-Pix2Pix",
    "Flux",
    "Flux Fill",
    "Flux Control",
    "Flex.2",
};

const char* sampling_methods_str[] = {
    "Euler A",
    "Euler",
    "Heun",
    "DPM2",
    "DPM++ (2s)",
    "DPM++ (2M)",
    "modified DPM++ (2M)",
    "iPNDM",
    "iPNDM_v",
    "LCM",
    "DDIM \"trailing\"",
    "TCD"};

/*================================================== Helper Functions ================================================*/

void calculate_alphas_cumprod(float* alphas_cumprod,
                              float linear_start = 0.00085f,
                              float linear_end   = 0.0120,
                              int timesteps      = TIMESTEPS) {
    float ls_sqrt = sqrtf(linear_start);
    float le_sqrt = sqrtf(linear_end);
    float amount  = le_sqrt - ls_sqrt;
    float product = 1.0f;
    for (int i = 0; i < timesteps; i++) {
        float beta = ls_sqrt + amount * ((float)i / (timesteps - 1));
        product *= 1.0f - powf(beta, 2.0f);
        alphas_cumprod[i] = product;
    }
}

void suppress_pp(int step, int steps, float time, void* data) {
    (void)step;
    (void)steps;
    (void)time;
    (void)data;
    return;
}

/*=============================================== StableDiffusionGGML ================================================*/

class StableDiffusionGGML {
public:
    ggml_backend_t backend             = NULL;  // general backend
    ggml_backend_t clip_backend        = NULL;
    ggml_backend_t control_net_backend = NULL;
    ggml_backend_t vae_backend         = NULL;
    ggml_type model_wtype              = GGML_TYPE_COUNT;
    ggml_type conditioner_wtype        = GGML_TYPE_COUNT;
    ggml_type diffusion_model_wtype    = GGML_TYPE_COUNT;
    ggml_type vae_wtype                = GGML_TYPE_COUNT;

    SDVersion version;
    bool vae_decode_only         = false;
    bool free_params_immediately = false;

    std::shared_ptr<RNG> rng = std::make_shared<STDDefaultRNG>();
    int n_threads            = -1;
    float scale_factor       = 0.18215f;

    std::shared_ptr<Conditioner> cond_stage_model;
    std::shared_ptr<FrozenCLIPVisionEmbedder> clip_vision;  // for svd
    std::shared_ptr<DiffusionModel> diffusion_model;
    std::shared_ptr<AutoEncoderKL> first_stage_model;
    std::shared_ptr<TinyAutoEncoder> tae_first_stage;
    std::shared_ptr<ControlNet> control_net = NULL;
    std::shared_ptr<PhotoMakerIDEncoder> pmid_model;
    std::shared_ptr<LoraModel> pmid_lora;
    std::shared_ptr<PhotoMakerIDEmbed> pmid_id_embeds;

    std::string taesd_path;
    bool use_tiny_autoencoder = false;
    bool vae_tiling           = false;
    bool stacked_id           = false;

    bool is_using_v_parameterization     = false;
    bool is_using_edm_v_parameterization = false;

    std::map<std::string, struct ggml_tensor*> tensors;

    std::string lora_model_dir;
    // lora_name => multiplier
    std::unordered_map<std::string, float> curr_lora_state;

    std::shared_ptr<Denoiser> denoiser = std::make_shared<CompVisDenoiser>();

    StableDiffusionGGML() = default;

    StableDiffusionGGML(int n_threads,
                        bool vae_decode_only,
                        bool free_params_immediately,
                        std::string lora_model_dir,
                        rng_type_t rng_type)
        : n_threads(n_threads),
          vae_decode_only(vae_decode_only),
          free_params_immediately(free_params_immediately),
          lora_model_dir(lora_model_dir) {
        if (rng_type == STD_DEFAULT_RNG) {
            rng = std::make_shared<STDDefaultRNG>();
        } else if (rng_type == CUDA_RNG) {
            rng = std::make_shared<PhiloxRNG>();
        }
    }

    ~StableDiffusionGGML() {
        if (clip_backend != backend) {
            ggml_backend_free(clip_backend);
        }
        if (control_net_backend != backend) {
            ggml_backend_free(control_net_backend);
        }
        if (vae_backend != backend) {
            ggml_backend_free(vae_backend);
        }
        ggml_backend_free(backend);
    }

    bool load_from_file(const std::string& model_path,
                        const std::string& clip_l_path,
                        const std::string& clip_g_path,
                        const std::string& t5xxl_path,
                        const std::string& diffusion_model_path,
                        const std::string& vae_path,
                        const std::string control_net_path,
                        const std::string embeddings_path,
                        const std::string id_embeddings_path,
                        const std::string& taesd_path,
                        bool vae_tiling_,
                        ggml_type wtype,
                        schedule_t schedule,
                        bool clip_on_cpu,
                        bool control_net_cpu,
                        bool vae_on_cpu,
                        bool diffusion_flash_attn,
                        bool tae_preview_only) {
        use_tiny_autoencoder = taesd_path.size() > 0;
#ifdef SD_USE_CUDA
        LOG_DEBUG("Using CUDA backend");
        backend = ggml_backend_cuda_init(0);
#endif
#ifdef SD_USE_METAL
        LOG_DEBUG("Using Metal backend");
        ggml_log_set(ggml_log_callback_default, nullptr);
        backend = ggml_backend_metal_init();
#endif
#ifdef SD_USE_VULKAN
        LOG_DEBUG("Using Vulkan backend");
        size_t device          = 0;
        const int device_count = ggml_backend_vk_get_device_count();
        if (device_count) {
            const char* SD_VK_DEVICE = getenv("SD_VK_DEVICE");
            if (SD_VK_DEVICE != nullptr) {
                std::string sd_vk_device_str = SD_VK_DEVICE;
                try {
                    device = std::stoull(sd_vk_device_str);
                } catch (const std::invalid_argument&) {
                    LOG_WARN("SD_VK_DEVICE environment variable is not a valid integer (%s). Falling back to device 0.", SD_VK_DEVICE);
                    device = 0;
                } catch (const std::out_of_range&) {
                    LOG_WARN("SD_VK_DEVICE environment variable value is out of range for `unsigned long long` type (%s). Falling back to device 0.", SD_VK_DEVICE);
                    device = 0;
                }
                if (device >= device_count) {
                    LOG_WARN("Cannot find targeted vulkan device (%llu). Falling back to device 0.", device);
                    device = 0;
                }
            }
            LOG_INFO("Vulkan: Using device %llu", device);
            backend = ggml_backend_vk_init(device);
        }
        if (!backend) {
            LOG_WARN("Failed to initialize Vulkan backend");
        }
#endif
#ifdef SD_USE_SYCL
        LOG_DEBUG("Using SYCL backend");
        backend = ggml_backend_sycl_init(0);
#endif

        if (!backend) {
            LOG_DEBUG("Using CPU backend");
            backend = ggml_backend_cpu_init();
        }

        ModelLoader model_loader;

        vae_tiling = vae_tiling_;

        if (model_path.size() > 0) {
            LOG_INFO("loading model from '%s'", model_path.c_str());
            if (!model_loader.init_from_file(model_path)) {
                LOG_ERROR("init model loader from file failed: '%s'", model_path.c_str());
            }
        }

        if (diffusion_model_path.size() > 0) {
            LOG_INFO("loading diffusion model from '%s'", diffusion_model_path.c_str());
            if (!model_loader.init_from_file(diffusion_model_path, "model.diffusion_model.")) {
                LOG_WARN("loading diffusion model from '%s' failed", diffusion_model_path.c_str());
            }
        }

        bool is_unet = model_loader.model_is_unet();

        if (clip_l_path.size() > 0) {
            LOG_INFO("loading clip_l from '%s'", clip_l_path.c_str());
            if (!model_loader.init_from_file(clip_l_path, is_unet ? "cond_stage_model.transformer." : "text_encoders.clip_l.transformer.")) {
                LOG_WARN("loading clip_l from '%s' failed", clip_l_path.c_str());
            }
        }

        if (clip_g_path.size() > 0) {
            LOG_INFO("loading clip_g from '%s'", clip_g_path.c_str());
            if (!model_loader.init_from_file(clip_g_path, is_unet ? "cond_stage_model.1.transformer." : "text_encoders.clip_g.transformer.")) {
                LOG_WARN("loading clip_g from '%s' failed", clip_g_path.c_str());
            }
        }

        if (t5xxl_path.size() > 0) {
            LOG_INFO("loading t5xxl from '%s'", t5xxl_path.c_str());
            if (!model_loader.init_from_file(t5xxl_path, "text_encoders.t5xxl.transformer.")) {
                LOG_WARN("loading t5xxl from '%s' failed", t5xxl_path.c_str());
            }
        }

        if (vae_path.size() > 0) {
            LOG_INFO("loading vae from '%s'", vae_path.c_str());
            if (!model_loader.init_from_file(vae_path, "vae.")) {
                LOG_WARN("loading vae from '%s' failed", vae_path.c_str());
            }
        }

        version = model_loader.get_sd_version();
        if (version == VERSION_COUNT) {
            LOG_ERROR("get sd version from file failed: '%s'", model_path.c_str());
            return false;
        }

        LOG_INFO("Version: %s ", model_version_to_str[version]);
        if (wtype == GGML_TYPE_COUNT) {
            model_wtype = model_loader.get_sd_wtype();
            if (model_wtype == GGML_TYPE_COUNT) {
                model_wtype = GGML_TYPE_F32;
                LOG_WARN("can not get mode wtype frome weight, use f32");
            }
            conditioner_wtype = model_loader.get_conditioner_wtype();
            if (conditioner_wtype == GGML_TYPE_COUNT) {
                conditioner_wtype = wtype;
            }
            diffusion_model_wtype = model_loader.get_diffusion_model_wtype();
            if (diffusion_model_wtype == GGML_TYPE_COUNT) {
                diffusion_model_wtype = wtype;
            }
            vae_wtype = model_loader.get_vae_wtype();

            if (vae_wtype == GGML_TYPE_COUNT) {
                vae_wtype = wtype;
            }
        } else {
            model_wtype           = wtype;
            conditioner_wtype     = wtype;
            diffusion_model_wtype = wtype;
            vae_wtype             = wtype;
            model_loader.set_wtype_override(wtype);
        }

        if (sd_version_is_sdxl(version)) {
            vae_wtype = GGML_TYPE_F32;
            model_loader.set_wtype_override(GGML_TYPE_F32, "vae.");
        }

        LOG_INFO("Weight type:                 %s", model_wtype != SD_TYPE_COUNT ? ggml_type_name(model_wtype) : "??");
        LOG_INFO("Conditioner weight type:     %s", conditioner_wtype != SD_TYPE_COUNT ? ggml_type_name(conditioner_wtype) : "??");
        LOG_INFO("Diffusion model weight type: %s", diffusion_model_wtype != SD_TYPE_COUNT ? ggml_type_name(diffusion_model_wtype) : "??");
        LOG_INFO("VAE weight type:             %s", vae_wtype != SD_TYPE_COUNT ? ggml_type_name(vae_wtype) : "??");

        LOG_DEBUG("ggml tensor size = %d bytes", (int)sizeof(ggml_tensor));

        if (sd_version_is_sdxl(version)) {
            scale_factor = 0.13025f;
            if (vae_path.size() == 0 && taesd_path.size() == 0) {
                LOG_WARN(
                    "!!!It looks like you are using SDXL model. "
                    "If you find that the generated images are completely black, "
                    "try specifying SDXL VAE FP16 Fix with the --vae parameter. "
                    "You can find it here: https://huggingface.co/madebyollin/sdxl-vae-fp16-fix/blob/main/sdxl_vae.safetensors");
            }
        } else if (sd_version_is_sd3(version)) {
            scale_factor = 1.5305f;
        } else if (sd_version_is_flux(version)) {
            scale_factor = 0.3611;
            // TODO: shift_factor
        }

        if (sd_version_is_control(version)) {
            // Might need vae encode for control cond
            vae_decode_only = false;
        }

        if (version == VERSION_SVD) {
            clip_vision = std::make_shared<FrozenCLIPVisionEmbedder>(backend, model_loader.tensor_storages_types);
            clip_vision->alloc_params_buffer();
            clip_vision->get_param_tensors(tensors);

            diffusion_model = std::make_shared<UNetModel>(backend, model_loader.tensor_storages_types, version);
            diffusion_model->alloc_params_buffer();
            diffusion_model->get_param_tensors(tensors);

            first_stage_model = std::make_shared<AutoEncoderKL>(backend, model_loader.tensor_storages_types, "first_stage_model", vae_decode_only, true, version);
            LOG_DEBUG("vae_decode_only %d", vae_decode_only);
            first_stage_model->alloc_params_buffer();
            first_stage_model->get_param_tensors(tensors, "first_stage_model");
        } else {
            clip_backend   = backend;
            bool use_t5xxl = false;
            if (sd_version_is_dit(version)) {
                for (auto pair : model_loader.tensor_storages_types) {
                    if (pair.first.find("text_encoders.t5xxl") != std::string::npos) {
                        use_t5xxl = true;
                        break;
                    }
                }
            }
            if (!ggml_backend_is_cpu(backend) && use_t5xxl && conditioner_wtype != GGML_TYPE_F32) {
                clip_on_cpu = true;
                LOG_INFO("set clip_on_cpu to true");
            }
            if (clip_on_cpu && !ggml_backend_is_cpu(backend)) {
                LOG_INFO("CLIP: Using CPU backend");
                clip_backend = ggml_backend_cpu_init();
            }
            if (diffusion_flash_attn) {
                LOG_INFO("Using flash attention in the diffusion model");
            }
            if (sd_version_is_sd3(version)) {
                if (diffusion_flash_attn) {
                    LOG_WARN("flash attention in this diffusion model is currently unsupported!");
                }
                cond_stage_model = std::make_shared<SD3CLIPEmbedder>(clip_backend, model_loader.tensor_storages_types);
                diffusion_model  = std::make_shared<MMDiTModel>(backend, model_loader.tensor_storages_types);
            } else if (sd_version_is_flux(version)) {
                bool is_chroma = false;
                for (auto pair : model_loader.tensor_storages_types) {
                    if (pair.first.find("distilled_guidance_layer.in_proj.weight") != std::string::npos) {
                        is_chroma = true;
                        break;
                    }
                }
                if (is_chroma) {
                    cond_stage_model = std::make_shared<PixArtCLIPEmbedder>(clip_backend, model_loader.tensor_storages_types);
                } else {
                    cond_stage_model = std::make_shared<FluxCLIPEmbedder>(clip_backend, model_loader.tensor_storages_types);
                }
                diffusion_model = std::make_shared<FluxModel>(backend, model_loader.tensor_storages_types, version, diffusion_flash_attn);
            } else {
                if (id_embeddings_path.find("v2") != std::string::npos) {
                    cond_stage_model = std::make_shared<FrozenCLIPEmbedderWithCustomWords>(clip_backend, model_loader.tensor_storages_types, embeddings_path, version, PM_VERSION_2);
                } else {
                    cond_stage_model = std::make_shared<FrozenCLIPEmbedderWithCustomWords>(clip_backend, model_loader.tensor_storages_types, embeddings_path, version);
                }
                diffusion_model = std::make_shared<UNetModel>(backend, model_loader.tensor_storages_types, version, diffusion_flash_attn);
            }

            cond_stage_model->alloc_params_buffer();
            cond_stage_model->get_param_tensors(tensors);

            diffusion_model->alloc_params_buffer();
            diffusion_model->get_param_tensors(tensors);

            if (!use_tiny_autoencoder || tae_preview_only) {
                if (vae_on_cpu && !ggml_backend_is_cpu(backend)) {
                    LOG_INFO("VAE Autoencoder: Using CPU backend");
                    vae_backend = ggml_backend_cpu_init();
                } else {
                    vae_backend = backend;
                }
                first_stage_model = std::make_shared<AutoEncoderKL>(vae_backend, model_loader.tensor_storages_types, "first_stage_model", vae_decode_only, false, version);
                first_stage_model->alloc_params_buffer();
                first_stage_model->get_param_tensors(tensors, "first_stage_model");
            }
            if (use_tiny_autoencoder) {
                tae_first_stage = std::make_shared<TinyAutoEncoder>(backend, model_loader.tensor_storages_types, "decoder.layers", vae_decode_only, version);
            }
            // first_stage_model->get_param_tensors(tensors, "first_stage_model.");

            if (control_net_path.size() > 0) {
                ggml_backend_t controlnet_backend = NULL;
                if (control_net_cpu && !ggml_backend_is_cpu(backend)) {
                    LOG_DEBUG("ControlNet: Using CPU backend");
                    controlnet_backend = ggml_backend_cpu_init();
                } else {
                    controlnet_backend = backend;
                }
                control_net = std::make_shared<ControlNet>(controlnet_backend, model_loader.tensor_storages_types, version);
            }

            if (id_embeddings_path.find("v2") != std::string::npos) {
                pmid_model = std::make_shared<PhotoMakerIDEncoder>(backend, model_loader.tensor_storages_types, "pmid", version, PM_VERSION_2);
                LOG_INFO("using PhotoMaker Version 2");
            } else {
                pmid_model = std::make_shared<PhotoMakerIDEncoder>(backend, model_loader.tensor_storages_types, "pmid", version);
            }
            if (id_embeddings_path.size() > 0) {
                pmid_lora = std::make_shared<LoraModel>(backend, id_embeddings_path, "");
                if (!pmid_lora->load_from_file(true)) {
                    LOG_WARN("load photomaker lora tensors from %s failed", id_embeddings_path.c_str());
                    return false;
                }
                LOG_INFO("loading stacked ID embedding (PHOTOMAKER) model file from '%s'", id_embeddings_path.c_str());
                if (!model_loader.init_from_file(id_embeddings_path, "pmid.")) {
                    LOG_WARN("loading stacked ID embedding from '%s' failed", id_embeddings_path.c_str());
                } else {
                    stacked_id = true;
                }
            }
            if (stacked_id) {
                if (!pmid_model->alloc_params_buffer()) {
                    LOG_ERROR(" pmid model params buffer allocation failed");
                    return false;
                }
                pmid_model->get_param_tensors(tensors, "pmid");
            }
        }

        struct ggml_init_params params;
        params.mem_size   = static_cast<size_t>(10 * 1024) * 1024;  // 10M
        params.mem_buffer = NULL;
        params.no_alloc   = false;
        // LOG_DEBUG("mem_size %u ", params.mem_size);
        struct ggml_context* ctx = ggml_init(params);  // for  alphas_cumprod and is_using_v_parameterization check
        GGML_ASSERT(ctx != NULL);
        ggml_tensor* alphas_cumprod_tensor = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, TIMESTEPS);
        calculate_alphas_cumprod((float*)alphas_cumprod_tensor->data);

        // load weights
        LOG_DEBUG("loading weights");

        int64_t t0 = ggml_time_ms();

        std::set<std::string> ignore_tensors;
        tensors["alphas_cumprod"] = alphas_cumprod_tensor;
        if (use_tiny_autoencoder) {
            ignore_tensors.insert("first_stage_model.");
        }
        if (stacked_id) {
            ignore_tensors.insert("lora.");
        }

        if (vae_decode_only) {
            ignore_tensors.insert("first_stage_model.encoder");
            ignore_tensors.insert("first_stage_model.quant");
        }
        if (version == VERSION_SVD) {
            ignore_tensors.insert("conditioner.embedders.3");
        }
        bool success = model_loader.load_tensors(tensors, backend, ignore_tensors);
        if (!success) {
            LOG_ERROR("load tensors from model loader failed");
            ggml_free(ctx);
            return false;
        }

        // LOG_DEBUG("model size = %.2fMB", total_size / 1024.0 / 1024.0);

        if (version == VERSION_SVD) {
            // diffusion_model->test();
            // first_stage_model->test();
            // return false;
        } else {
            size_t clip_params_mem_size = cond_stage_model->get_params_buffer_size();
            size_t unet_params_mem_size = diffusion_model->get_params_buffer_size();
            size_t vae_params_mem_size  = 0;
            if (!use_tiny_autoencoder || tae_preview_only) {
                vae_params_mem_size = first_stage_model->get_params_buffer_size();
            }
            if (use_tiny_autoencoder) {
                if (!tae_first_stage->load_from_file(taesd_path)) {
                    return false;
                }
                vae_params_mem_size = tae_first_stage->get_params_buffer_size();
            }
            size_t control_net_params_mem_size = 0;
            if (control_net) {
                if (!control_net->load_from_file(control_net_path)) {
                    return false;
                }
                control_net_params_mem_size = control_net->get_params_buffer_size();
            }
            size_t pmid_params_mem_size = 0;
            if (stacked_id) {
                pmid_params_mem_size = pmid_model->get_params_buffer_size();
            }

            size_t total_params_ram_size  = 0;
            size_t total_params_vram_size = 0;
            if (ggml_backend_is_cpu(clip_backend)) {
                total_params_ram_size += clip_params_mem_size + pmid_params_mem_size;
            } else {
                total_params_vram_size += clip_params_mem_size + pmid_params_mem_size;
            }

            if (ggml_backend_is_cpu(backend)) {
                total_params_ram_size += unet_params_mem_size;
            } else {
                total_params_vram_size += unet_params_mem_size;
            }

            if (ggml_backend_is_cpu(vae_backend)) {
                total_params_ram_size += vae_params_mem_size;
            } else {
                total_params_vram_size += vae_params_mem_size;
            }

            if (ggml_backend_is_cpu(control_net_backend)) {
                total_params_ram_size += control_net_params_mem_size;
            } else {
                total_params_vram_size += control_net_params_mem_size;
            }

            size_t total_params_size = total_params_ram_size + total_params_vram_size;
            LOG_INFO(
                "total params memory size = %.2fMB (VRAM %.2fMB, RAM %.2fMB): "
                "clip %.2fMB(%s), unet %.2fMB(%s), vae %.2fMB(%s), controlnet %.2fMB(%s), pmid %.2fMB(%s)",
                total_params_size / 1024.0 / 1024.0,
                total_params_vram_size / 1024.0 / 1024.0,
                total_params_ram_size / 1024.0 / 1024.0,
                clip_params_mem_size / 1024.0 / 1024.0,
                ggml_backend_is_cpu(clip_backend) ? "RAM" : "VRAM",
                unet_params_mem_size / 1024.0 / 1024.0,
                ggml_backend_is_cpu(backend) ? "RAM" : "VRAM",
                vae_params_mem_size / 1024.0 / 1024.0,
                ggml_backend_is_cpu(vae_backend) ? "RAM" : "VRAM",
                control_net_params_mem_size / 1024.0 / 1024.0,
                ggml_backend_is_cpu(control_net_backend) ? "RAM" : "VRAM",
                pmid_params_mem_size / 1024.0 / 1024.0,
                ggml_backend_is_cpu(clip_backend) ? "RAM" : "VRAM");
        }

        int64_t t1 = ggml_time_ms();
        LOG_INFO("loading model from '%s' completed, taking %.2fs", model_path.c_str(), (t1 - t0) * 1.0f / 1000);

        // check is_using_v_parameterization_for_sd2

        if (sd_version_is_sd2(version)) {
            if (is_using_v_parameterization_for_sd2(ctx, sd_version_is_inpaint(version))) {
                is_using_v_parameterization = true;
            }
        } else if (sd_version_is_sdxl(version)) {
            if (model_loader.tensor_storages_types.find("edm_vpred.sigma_max") != model_loader.tensor_storages_types.end()) {
                // CosXL models
                // TODO: get sigma_min and sigma_max values from file
                is_using_edm_v_parameterization = true;
            }
            if (model_loader.tensor_storages_types.find("v_pred") != model_loader.tensor_storages_types.end()) {
                is_using_v_parameterization = true;
            }
        } else if (version == VERSION_SVD) {
            // TODO: V_PREDICTION_EDM
            is_using_v_parameterization = true;
        }

        if (sd_version_is_sd3(version)) {
            LOG_INFO("running in FLOW mode");
            denoiser = std::make_shared<DiscreteFlowDenoiser>();
        } else if (sd_version_is_flux(version)) {
            LOG_INFO("running in Flux FLOW mode");
            float shift = 1.0f;  // TODO: validate
            for (auto pair : model_loader.tensor_storages_types) {
                if (pair.first.find("model.diffusion_model.guidance_in.in_layer.weight") != std::string::npos) {
                    shift = 1.15f;
                    break;
                }
            }
            denoiser = std::make_shared<FluxFlowDenoiser>(shift);
        } else if (is_using_v_parameterization) {
            LOG_INFO("running in v-prediction mode");
            denoiser = std::make_shared<CompVisVDenoiser>();
        } else if (is_using_edm_v_parameterization) {
            LOG_INFO("running in v-prediction EDM mode");
            denoiser = std::make_shared<EDMVDenoiser>();
        } else {
            LOG_INFO("running in eps-prediction mode");
        }

        if (schedule != DEFAULT) {
            switch (schedule) {
                case DISCRETE:
                    LOG_INFO("running with discrete schedule");
                    denoiser->schedule = std::make_shared<DiscreteSchedule>();
                    break;
                case KARRAS:
                    LOG_INFO("running with Karras schedule");
                    denoiser->schedule = std::make_shared<KarrasSchedule>();
                    break;
                case EXPONENTIAL:
                    LOG_INFO("running exponential schedule");
                    denoiser->schedule = std::make_shared<ExponentialSchedule>();
                    break;
                case AYS:
                    LOG_INFO("Running with Align-Your-Steps schedule");
                    denoiser->schedule          = std::make_shared<AYSSchedule>();
                    denoiser->schedule->version = version;
                    break;
                case GITS:
                    LOG_INFO("Running with GITS schedule");
                    denoiser->schedule          = std::make_shared<GITSSchedule>();
                    denoiser->schedule->version = version;
                    break;
                case DEFAULT:
                    // Don't touch anything.
                    break;
                default:
                    LOG_ERROR("Unknown schedule %i", schedule);
                    abort();
            }
        }

        auto comp_vis_denoiser = std::dynamic_pointer_cast<CompVisDenoiser>(denoiser);
        if (comp_vis_denoiser) {
            for (int i = 0; i < TIMESTEPS; i++) {
                comp_vis_denoiser->sigmas[i]     = std::sqrt((1 - ((float*)alphas_cumprod_tensor->data)[i]) / ((float*)alphas_cumprod_tensor->data)[i]);
                comp_vis_denoiser->log_sigmas[i] = std::log(comp_vis_denoiser->sigmas[i]);
            }
        }

        LOG_DEBUG("finished loaded file");
        ggml_free(ctx);
        use_tiny_autoencoder = use_tiny_autoencoder && !tae_preview_only;
        return true;
    }

    bool is_using_v_parameterization_for_sd2(ggml_context* work_ctx, bool is_inpaint = false) {
        struct ggml_tensor* x_t = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, 8, 8, 4, 1);
        ggml_set_f32(x_t, 0.5);
        struct ggml_tensor* c = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, 1024, 2, 1, 1);
        ggml_set_f32(c, 0.5);

        struct ggml_tensor* timesteps = ggml_new_tensor_1d(work_ctx, GGML_TYPE_F32, 1);
        ggml_set_f32(timesteps, 999);

        struct ggml_tensor* concat = is_inpaint ? ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, 8, 8, 5, 1) : NULL;
        if (concat != NULL) {
            ggml_set_f32(concat, 0);
        }

        int64_t t0              = ggml_time_ms();
        struct ggml_tensor* out = ggml_dup_tensor(work_ctx, x_t);
        diffusion_model->compute(n_threads, x_t, timesteps, c, concat, NULL, NULL, {}, -1, {}, 0.f, &out);
        diffusion_model->free_compute_buffer();

        double result = 0.f;
        {
            float* vec_x   = (float*)x_t->data;
            float* vec_out = (float*)out->data;

            int64_t n = ggml_nelements(out);

            for (int i = 0; i < n; i++) {
                result += ((double)vec_out[i] - (double)vec_x[i]);
            }
            result /= n;
        }
        int64_t t1 = ggml_time_ms();
        LOG_DEBUG("check is_using_v_parameterization_for_sd2, taking %.2fs", (t1 - t0) * 1.0f / 1000);
        return result < -1;
    }

    void apply_lora(const std::string& lora_name, float multiplier) {
        int64_t t0                 = ggml_time_ms();
        std::string st_file_path   = path_join(lora_model_dir, lora_name + ".safetensors");
        std::string ckpt_file_path = path_join(lora_model_dir, lora_name + ".ckpt");
        std::string file_path;
        if (file_exists(st_file_path)) {
            file_path = st_file_path;
        } else if (file_exists(ckpt_file_path)) {
            file_path = ckpt_file_path;
        } else {
            LOG_WARN("can not find %s or %s for lora %s", st_file_path.c_str(), ckpt_file_path.c_str(), lora_name.c_str());
            return;
        }
        LoraModel lora(backend, file_path);
        if (!lora.load_from_file()) {
            LOG_WARN("load lora tensors from %s failed", file_path.c_str());
            return;
        }

        lora.multiplier = multiplier;
        // TODO: send version?
        lora.apply(tensors, version, n_threads);
        lora.free_params_buffer();

        int64_t t1 = ggml_time_ms();

        LOG_INFO("lora '%s' applied, taking %.2fs", lora_name.c_str(), (t1 - t0) * 1.0f / 1000);
    }

    void apply_loras(const std::unordered_map<std::string, float>& lora_state) {
        if (lora_state.size() > 0 && model_wtype != GGML_TYPE_F16 && model_wtype != GGML_TYPE_F32) {
            LOG_WARN("In quantized models when applying LoRA, the images have poor quality.");
        }
        std::unordered_map<std::string, float> lora_state_diff;
        for (auto& kv : lora_state) {
            const std::string& lora_name = kv.first;
            float multiplier             = kv.second;
            lora_state_diff[lora_name] += multiplier;
        }
        for (auto& kv : curr_lora_state) {
            const std::string& lora_name = kv.first;
            float curr_multiplier        = kv.second;
            lora_state_diff[lora_name] -= curr_multiplier;
        }

        size_t rm = lora_state_diff.size() - lora_state.size();
        if (rm != 0) {
            LOG_INFO("Attempting to apply %lu LoRAs (removing %lu applied LoRAs)", lora_state.size(), rm);
        } else {
            LOG_INFO("Attempting to apply %lu LoRAs", lora_state.size());
        }

        for (auto& kv : lora_state_diff) {
            apply_lora(kv.first, kv.second);
        }

        curr_lora_state = lora_state;
    }

    ggml_tensor* id_encoder(ggml_context* work_ctx,
                            ggml_tensor* init_img,
                            ggml_tensor* prompts_embeds,
                            ggml_tensor* id_embeds,
                            std::vector<bool>& class_tokens_mask) {
        ggml_tensor* res = NULL;
        pmid_model->compute(n_threads, init_img, prompts_embeds, id_embeds, class_tokens_mask, &res, work_ctx);
        return res;
    }

    SDCondition get_svd_condition(ggml_context* work_ctx,
                                  sd_image_t init_image,
                                  int width,
                                  int height,
                                  int fps                    = 6,
                                  int motion_bucket_id       = 127,
                                  float augmentation_level   = 0.f,
                                  bool force_zero_embeddings = false) {
        // c_crossattn
        int64_t t0                      = ggml_time_ms();
        struct ggml_tensor* c_crossattn = NULL;
        {
            if (force_zero_embeddings) {
                c_crossattn = ggml_new_tensor_1d(work_ctx, GGML_TYPE_F32, clip_vision->vision_model.projection_dim);
                ggml_set_f32(c_crossattn, 0.f);
            } else {
                sd_image_f32_t image         = sd_image_t_to_sd_image_f32_t(init_image);
                sd_image_f32_t resized_image = clip_preprocess(image, clip_vision->vision_model.image_size);
                free(image.data);
                image.data = NULL;

                ggml_tensor* pixel_values = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, resized_image.width, resized_image.height, 3, 1);
                sd_image_f32_to_tensor(resized_image.data, pixel_values, false);
                free(resized_image.data);
                resized_image.data = NULL;

                // print_ggml_tensor(pixel_values);
                clip_vision->compute(n_threads, pixel_values, &c_crossattn, work_ctx);
                // print_ggml_tensor(c_crossattn);
            }
        }

        // c_concat
        struct ggml_tensor* c_concat = NULL;
        {
            if (force_zero_embeddings) {
                c_concat = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, width / 8, height / 8, 4, 1);
                ggml_set_f32(c_concat, 0.f);
            } else {
                ggml_tensor* init_img = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, width, height, 3, 1);

                if (width != init_image.width || height != init_image.height) {
                    sd_image_f32_t image         = sd_image_t_to_sd_image_f32_t(init_image);
                    sd_image_f32_t resized_image = resize_sd_image_f32_t(image, width, height);
                    free(image.data);
                    image.data = NULL;
                    sd_image_f32_to_tensor(resized_image.data, init_img, false);
                    free(resized_image.data);
                    resized_image.data = NULL;
                } else {
                    sd_image_to_tensor(init_image.data, init_img);
                }
                if (augmentation_level > 0.f) {
                    struct ggml_tensor* noise = ggml_dup_tensor(work_ctx, init_img);
                    ggml_tensor_set_f32_randn(noise, rng);
                    // encode_pixels += torch.randn_like(pixels) * augmentation_level
                    ggml_tensor_scale(noise, augmentation_level);
                    ggml_tensor_add(init_img, noise);
                }
                ggml_tensor* moments = encode_first_stage(work_ctx, init_img);
                c_concat             = get_first_stage_encoding(work_ctx, moments);
            }
        }

        // y
        struct ggml_tensor* y = NULL;
        {
            y                            = ggml_new_tensor_1d(work_ctx, GGML_TYPE_F32, diffusion_model->get_adm_in_channels());
            int out_dim                  = 256;
            int fps_id                   = fps - 1;
            std::vector<float> timesteps = {(float)fps_id, (float)motion_bucket_id, augmentation_level};
            set_timestep_embedding(timesteps, y, out_dim);
        }
        int64_t t1 = ggml_time_ms();
        LOG_DEBUG("computing svd condition graph completed, taking %" PRId64 " ms", t1 - t0);
        return {c_crossattn, y, c_concat};
    }

    void silent_tiling(ggml_tensor* input, ggml_tensor* output, const int scale, const int tile_size, const float tile_overlap_factor, on_tile_process on_processing) {
        sd_progress_cb_t cb = sd_get_progress_callback();
        void* cbd           = sd_get_progress_callback_data();
        sd_set_progress_callback((sd_progress_cb_t)suppress_pp, NULL);
        sd_tiling(input, output, scale, tile_size, tile_overlap_factor, on_processing);
        sd_set_progress_callback(cb, cbd);
    }

    void preview_image(ggml_context* work_ctx,
                       int step,
                       struct ggml_tensor* latents,
                       enum SDVersion version,
                       sd_preview_t preview_mode,
                       ggml_tensor* result,
                       std::function<void(int, sd_image_t)> step_callback) {
        const uint32_t channel = 3;
        uint32_t width         = latents->ne[0];
        uint32_t height        = latents->ne[1];
        uint32_t dim           = latents->ne[2];
        if (preview_mode == SD_PREVIEW_PROJ) {
            const float (*latent_rgb_proj)[channel];

            if (dim == 16) {
                // 16 channels VAE -> Flux or SD3

                if (sd_version_is_sd3(version)) {
                    latent_rgb_proj = sd3_latent_rgb_proj;
                } else if (sd_version_is_flux(version)) {
                    latent_rgb_proj = flux_latent_rgb_proj;
                } else {
                    LOG_WARN("No latent to RGB projection known for this model");
                    // unknown model
                    return;
                }

            } else if (dim == 4) {
                // 4 channels VAE
                if (sd_version_is_sdxl(version)) {
                    latent_rgb_proj = sdxl_latent_rgb_proj;
                } else if (sd_version_is_sd1(version) || sd_version_is_sd2(version)) {
                    latent_rgb_proj = sd_latent_rgb_proj;
                } else {
                    // unknown model
                    LOG_WARN("No latent to RGB projection known for this model");
                    return;
                }
            } else {
                LOG_WARN("No latent to RGB projection known for this model");
                // unknown latent space
                return;
            }
            uint8_t* data = (uint8_t*)malloc(width * height * channel * sizeof(uint8_t));

            preview_latent_image(data, latents, latent_rgb_proj, width, height, dim);
            sd_image_t image = {
                width,
                height,
                channel,
                data};
            step_callback(step, image);
            free(image.data);
        } else {
            if (preview_mode == SD_PREVIEW_VAE) {
                ggml_tensor_scale(latents, 1.0f / scale_factor);
                if (vae_tiling) {
                    // split latent in 32x32 tiles and compute in several steps
                    auto on_tiling = [&](ggml_tensor* in, ggml_tensor* out, bool init) {
                        first_stage_model->compute(n_threads, in, true, &out);
                    };
                    silent_tiling(latents, result, 8, 32, 0.5f, on_tiling);

                } else {
                    first_stage_model->compute(n_threads, latents, true, &result);
                }
                first_stage_model->free_compute_buffer();
                ggml_tensor_scale(latents, scale_factor);

                ggml_tensor_scale_output(result);
            } else if (preview_mode == SD_PREVIEW_TAE) {
                if (tae_first_stage == nullptr) {
                    LOG_WARN("TAE not found for preview");
                    return;
                }
                if (vae_tiling) {
                    // split latent in 64x64 tiles and compute in several steps
                    auto on_tiling = [&](ggml_tensor* in, ggml_tensor* out, bool init) {
                        tae_first_stage->compute(n_threads, in, true, &out);
                    };
                    silent_tiling(latents, result, 8, 64, 0.5f, on_tiling);
                } else {
                    tae_first_stage->compute(n_threads, latents, true, &result);
                }
                tae_first_stage->free_compute_buffer();
            } else {
                return;
            }
            ggml_tensor_clamp(result, 0.0f, 1.0f);
            sd_image_t image = {
                width * 8,
                height * 8,
                channel,
                sd_tensor_to_image(result)};
            ggml_tensor_scale(result, 0);
            step_callback(step, image);
            free(image.data);
        }
    }

    ggml_tensor* sample(ggml_context* work_ctx,
                        ggml_tensor* init_latent,
                        ggml_tensor* noise,
                        SDCondition cond,
                        SDCondition uncond,
                        ggml_tensor* control_hint,
                        float control_strength,
                        sd_guidance_params_t guidance,
                        float eta,
                        sample_method_t method,
                        const std::vector<float>& sigmas,
                        int start_merge_step,
                        SDCondition id_cond,
                        std::vector<struct ggml_tensor*> ref_latents = {},
                        ggml_tensor* denoise_mask                    = nullptr) {
        std::vector<int> skip_layers(guidance.slg.layers, guidance.slg.layers + guidance.slg.layer_count);

        float cfg_scale     = guidance.txt_cfg;
        float img_cfg_scale = guidance.img_cfg;
        float slg_scale     = guidance.slg.scale;

        float min_cfg = guidance.min_cfg;

        if (img_cfg_scale != cfg_scale && !sd_version_use_concat(version)) {
            LOG_WARN("2-conditioning CFG is not supported with this model, disabling it for better performance...");
            img_cfg_scale = cfg_scale;
        }

        LOG_DEBUG("Sample");
        struct ggml_init_params params;
        size_t data_size = ggml_row_size(init_latent->type, init_latent->ne[0]);
        for (int i = 1; i < 4; i++) {
            data_size *= init_latent->ne[i];
        }
        data_size += 1024;
        params.mem_size       = data_size * 3;
        params.mem_buffer     = NULL;
        params.no_alloc       = false;
        ggml_context* tmp_ctx = ggml_init(params);

        size_t steps = sigmas.size() - 1;
        // noise = load_tensor_from_file(work_ctx, "./rand0.bin");
        // print_ggml_tensor(noise);
        struct ggml_tensor* x = ggml_dup_tensor(work_ctx, init_latent);
        copy_ggml_tensor(x, init_latent);
        x = denoiser->noise_scaling(sigmas[0], noise, x);

        struct ggml_tensor* noised_input = ggml_dup_tensor(work_ctx, noise);

        bool has_unconditioned = img_cfg_scale != 1.0 && uncond.c_crossattn != NULL;
        bool has_img_guidance  = cfg_scale != img_cfg_scale && uncond.c_crossattn != NULL;
        bool has_skiplayer     = (guidance.slg.scale != 0.0 || guidance.slg.slg_uncond) && skip_layers.size() > 0;

        // denoise wrapper
        struct ggml_tensor* out_cond     = ggml_dup_tensor(work_ctx, x);
        struct ggml_tensor* out_uncond   = NULL;
        struct ggml_tensor* out_skip     = NULL;
        struct ggml_tensor* out_img_cond = NULL;

        if (has_unconditioned) {
            out_uncond = ggml_dup_tensor(work_ctx, x);
        }
        if (has_skiplayer) {
            if (sd_version_is_dit(version)) {
                if (guidance.slg.scale != 0.0) {
                    out_skip = ggml_dup_tensor(work_ctx, x);
                }
            } else {
                has_skiplayer = false;
                LOG_WARN("SLG is incompatible with %s models", model_version_to_str[version]);
            }
        }
        if (has_img_guidance) {
            out_img_cond = ggml_dup_tensor(work_ctx, x);
        }
        struct ggml_tensor* denoised = ggml_dup_tensor(work_ctx, x);

        struct ggml_tensor* preview_tensor = NULL;
        auto sd_preview_mode               = sd_get_preview_mode();
        if (sd_preview_mode != SD_PREVIEW_NONE && sd_preview_mode != SD_PREVIEW_PROJ) {
            preview_tensor = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32,
                                                (denoised->ne[0] * 8),
                                                (denoised->ne[1] * 8),
                                                3,
                                                denoised->ne[3]);
        }

        std::vector<float> apg_momentum_buffer;
        if (guidance.apg.momentum != 0)
            apg_momentum_buffer.resize((size_t)ggml_nelements(denoised));

        auto denoise = [&](ggml_tensor* input, float sigma, int step) -> ggml_tensor* {
            if (step == 1) {
                pretty_progress(0, (int)steps, 0);
            }
            int64_t t0 = ggml_time_us();

            std::vector<float> scaling = denoiser->get_scalings(sigma);
            GGML_ASSERT(scaling.size() == 3);
            float c_skip = scaling[0];
            float c_out  = scaling[1];
            float c_in   = scaling[2];

            float t = denoiser->sigma_to_t(sigma);
            std::vector<float> timesteps_vec(x->ne[3], t);  // [N, ]
            auto timesteps = vector_to_ggml_tensor(work_ctx, timesteps_vec);
            std::vector<float> guidance_vec(x->ne[3], guidance.distilled_guidance);
            auto guidance_tensor = vector_to_ggml_tensor(work_ctx, guidance_vec);

            copy_ggml_tensor(noised_input, input);
            // noised_input = noised_input * c_in
            ggml_tensor_scale(noised_input, c_in);

            std::vector<struct ggml_tensor*> controls;

            if (control_hint != NULL && control_net != NULL) {
                control_net->compute(n_threads, noised_input, control_hint, timesteps, cond.c_crossattn, cond.c_vector);
                controls = control_net->controls;
                // print_ggml_tensor(controls[12]);
                // GGML_ASSERT(0);
            }

            if (start_merge_step == -1 || step <= start_merge_step) {
                // cond
                diffusion_model->compute(n_threads,
                                         noised_input,
                                         timesteps,
                                         cond.c_crossattn,
                                         cond.c_concat,
                                         cond.c_vector,
                                         guidance_tensor,
                                         ref_latents,
                                         -1,
                                         controls,
                                         control_strength,
                                         &out_cond);
            } else {
                diffusion_model->compute(n_threads,
                                         noised_input,
                                         timesteps,
                                         id_cond.c_crossattn,
                                         cond.c_concat,
                                         id_cond.c_vector,
                                         guidance_tensor,
                                         ref_latents,
                                         -1,
                                         controls,
                                         control_strength,
                                         &out_cond);
            }
            int step_count         = sigmas.size();
            bool is_skiplayer_step = has_skiplayer && step > (int)(guidance.slg.layer_start * step_count) && step < (int)(guidance.slg.layer_end * step_count);

            float* negative_data = NULL;
            if (has_unconditioned) {
                // uncond
                if (control_hint != NULL && control_net != NULL) {
                    control_net->compute(n_threads, noised_input, control_hint, timesteps, uncond.c_crossattn, uncond.c_vector);
                    controls = control_net->controls;
                }
                if (is_skiplayer_step && guidance.slg.slg_uncond) {
                    LOG_DEBUG("Skipping layers at uncond step %d\n", step);
                    diffusion_model->compute(n_threads,
                                             noised_input,
                                             timesteps,
                                             uncond.c_crossattn,
                                             uncond.c_concat,
                                             uncond.c_vector,
                                             guidance_tensor,
                                             ref_latents,
                                             -1,
                                             controls,
                                             control_strength,
                                             &out_uncond,
                                             NULL,
                                             skip_layers);
                } else {
                    diffusion_model->compute(n_threads,
                                             noised_input,
                                             timesteps,
                                             uncond.c_crossattn,
                                             uncond.c_concat,
                                             uncond.c_vector,
                                             guidance_tensor,
                                             ref_latents,
                                             -1,
                                             controls,
                                             control_strength,
                                             &out_uncond);
                }
                negative_data = (float*)out_uncond->data;
            }

            float* img_cond_data = NULL;
            if (has_img_guidance) {
                diffusion_model->compute(n_threads,
                                         noised_input,
                                         timesteps,
                                         uncond.c_crossattn,
                                         cond.c_concat,
                                         uncond.c_vector,
                                         guidance_tensor,
                                         ref_latents,
                                         -1,
                                         controls,
                                         control_strength,
                                         &out_img_cond);
                img_cond_data = (float*)out_img_cond->data;
            }

            float* skip_layer_data = NULL;
            if (is_skiplayer_step && guidance.slg.scale != 0.0) {
                LOG_DEBUG("Skipping layers at step %d\n", step);
                // skip layer (same as conditionned)
                diffusion_model->compute(n_threads,
                                         noised_input,
                                         timesteps,
                                         cond.c_crossattn,
                                         cond.c_concat,
                                         cond.c_vector,
                                         guidance_tensor,
                                         ref_latents,
                                         -1,
                                         controls,
                                         control_strength,
                                         &out_skip,
                                         NULL,
                                         skip_layers);
                skip_layer_data = (float*)out_skip->data;
            }
            float* vec_denoised  = (float*)denoised->data;
            float* vec_input     = (float*)input->data;
            float* positive_data = (float*)out_cond->data;
            int ne_elements      = (int)ggml_nelements(denoised);

            float* deltas = vec_denoised;

            // APG: https://arxiv.org/pdf/2410.02416

            bool log_cfg_norm                 = false;
            const char* SD_LOG_CFG_DELTA_NORM = getenv("SD_LOG_CFG_DELTA_NORM");
            if (SD_LOG_CFG_DELTA_NORM != nullptr) {
                std::string sd_log_cfg_norm_str = SD_LOG_CFG_DELTA_NORM;
                if (sd_log_cfg_norm_str == "ON" || sd_log_cfg_norm_str == "TRUE") {
                    log_cfg_norm = true;
                } else if (sd_log_cfg_norm_str != "OFF" && sd_log_cfg_norm_str != "FALSE") {
                    LOG_WARN("SD_LOG_CFG_DELTA_NORM environment variable has unexpected value. Assuming default (\"OFF\"). (Expected \"ON\"/\"TRUE\" or\"OFF\"/\"FALSE\", got \"%s\")", SD_LOG_CFG_DELTA_NORM);
                }
            }
            float apg_scale_factor = 1.;
            float diff_norm        = 0;
            float cond_norm_sq     = 0;
            float dot              = 0;
            if (has_unconditioned || has_img_guidance) {
                for (int i = 0; i < ne_elements; i++) {
                    float delta;
                    if (has_img_guidance) {
                        if (cfg_scale == 1) {
                            // Weird guidance (important: use img_cfg_scale instead of cfg_scale in the final formula)
                            delta = img_cond_data[i] - negative_data[i];
                        } else if (has_unconditioned) {
                            // 2-conditioning CFG (img_cfg_scale != cfg_scale != 1)
                            delta = positive_data[i] + (negative_data[i] * (1 - img_cfg_scale) + img_cond_data[i] * (img_cfg_scale - cfg_scale)) / (cfg_scale - 1);
                        } else {
                            // pure img CFG (img_cfg_scale == 1, cfg_scale !=1)
                            delta = positive_data[i] - img_cond_data[i];
                        }

                    } else {
                        // classic CFG (img_cfg_scale == cfg_scale != 1)
                        delta = positive_data[i] - negative_data[i];
                    }
                    if (guidance.apg.momentum != 0) {
                        delta += guidance.apg.momentum * apg_momentum_buffer[i];
                        apg_momentum_buffer[i] = delta;
                    }
                    if (guidance.apg.norm_treshold > 0 || log_cfg_norm) {
                        diff_norm += delta * delta;
                    }
                    if (guidance.apg.eta != 1.0f) {
                        cond_norm_sq += positive_data[i] * positive_data[i];
                        dot += positive_data[i] * delta;
                    }
                    deltas[i] = delta;
                }
                if (log_cfg_norm) {
                    LOG_INFO("CFG Delta norm: %.2f", sqrtf(diff_norm));
                }
                if (guidance.apg.norm_treshold > 0) {
                    diff_norm = sqrtf(diff_norm);
                    if (guidance.apg.norm_treshold_smoothing <= 0) {
                        apg_scale_factor = std::min(1.0f, guidance.apg.norm_treshold / diff_norm);
                    } else {
                        // Experimental: smooth saturate
                        float x          = guidance.apg.norm_treshold / diff_norm;
                        apg_scale_factor = x / std::pow(1 + std::pow(x, 1.0 / guidance.apg.norm_treshold_smoothing), guidance.apg.norm_treshold_smoothing);
                    }
                }
                if (guidance.apg.eta != 1.0f) {
                    dot *= apg_scale_factor;
                    // pre-normalize (avoids one square root and ne_elements extra divs)
                    dot /= cond_norm_sq;
                }

                for (int i = 0; i < ne_elements; i++) {
                    deltas[i] *= apg_scale_factor;
                    if (guidance.apg.eta != 1.0f) {
                        float apg_parallel   = dot * positive_data[i];
                        float apg_orthogonal = deltas[i] - apg_parallel;

                        // tweak deltas
                        deltas[i] = apg_orthogonal + guidance.apg.eta * apg_parallel;
                    }
                }
            }

            for (int i = 0; i < ne_elements; i++) {
                float latent_result = positive_data[i];
                if (has_unconditioned || has_img_guidance) {
                    // out_uncond + cfg_scale * (out_cond - out_uncond)
                    int64_t ne3 = out_cond->ne[3];
                    if (min_cfg != cfg_scale && ne3 != 1) {
                        int64_t i3  = i / out_cond->ne[0] * out_cond->ne[1] * out_cond->ne[2];
                        float scale = min_cfg + (cfg_scale - min_cfg) * (i3 * 1.0f / ne3);
                    } else {
                        float delta = deltas[i];
                        if (cfg_scale != 1) {
                            latent_result = positive_data[i] + (cfg_scale - 1) * delta;
                        } else if (has_img_guidance) {
                            // disables apg
                            latent_result = positive_data[i] + (img_cfg_scale - 1) * delta;
                        }
                    }
                } else if (has_img_guidance) {
                    // img_cfg_scale == 1
                    latent_result = img_cond_data[i] + cfg_scale * (positive_data[i] - img_cond_data[i]);
                }
                if (is_skiplayer_step && guidance.slg.scale != 0.0) {
                    latent_result = latent_result + (positive_data[i] - skip_layer_data[i]) * guidance.slg.scale;
                }
                // v = latent_result, eps = latent_result
                // denoised = (v * c_out + input * c_skip) or (input + eps * c_out)
                vec_denoised[i] = latent_result * c_out + vec_input[i] * c_skip;
            }
            int64_t t1 = ggml_time_us();
            if (denoise_mask != nullptr) {
                for (int64_t x = 0; x < denoised->ne[0]; x++) {
                    for (int64_t y = 0; y < denoised->ne[1]; y++) {
                        float mask = ggml_tensor_get_f32(denoise_mask, x, y);
                        for (int64_t k = 0; k < denoised->ne[2]; k++) {
                            float init = ggml_tensor_get_f32(init_latent, x, y, k);
                            float den  = ggml_tensor_get_f32(denoised, x, y, k);
                            ggml_tensor_set_f32(denoised, init + mask * (den - init), x, y, k);
                        }
                    }
                }
            }
            if (step > 0) {
                pretty_progress(step, (int)steps, (t1 - t0) / 1000000.f);
                // LOG_INFO("step %d sampling completed taking %.2fs", step, (t1 - t0) * 1.0f / 1000000);
            }
            auto sd_preview_cb   = sd_get_preview_callback();
            auto sd_preview_mode = sd_get_preview_mode();
            if (sd_preview_cb != NULL) {
                if (step % sd_get_preview_interval() == 0) {
                    preview_image(work_ctx, step, denoised, version, sd_preview_mode, preview_tensor, sd_preview_cb);
                }
            }
            return denoised;
        };

        if (!sample_k_diffusion(method, denoise, work_ctx, x, sigmas, rng, eta)) {
            LOG_ERROR("Diffusion model sampling failed");
            if (control_net) {
                control_net->free_control_ctx();
                control_net->free_compute_buffer();
            }
            diffusion_model->free_compute_buffer();
            return NULL;
        }

        x = denoiser->inverse_noise_scaling(sigmas[sigmas.size() - 1], x);

        if (control_net) {
            control_net->free_control_ctx();
            control_net->free_compute_buffer();
        }
        diffusion_model->free_compute_buffer();
        return x;
    }

    // ldm.models.diffusion.ddpm.LatentDiffusion.get_first_stage_encoding
    ggml_tensor*
    get_first_stage_encoding(ggml_context* work_ctx, ggml_tensor* moments) {
        // ldm.modules.distributions.distributions.DiagonalGaussianDistribution.sample
        ggml_tensor* latent       = ggml_new_tensor_4d(work_ctx, moments->type, moments->ne[0], moments->ne[1], moments->ne[2] / 2, moments->ne[3]);
        struct ggml_tensor* noise = ggml_dup_tensor(work_ctx, latent);
        ggml_tensor_set_f32_randn(noise, rng);
        // noise = load_tensor_from_file(work_ctx, "noise.bin");
        {
            float mean   = 0;
            float logvar = 0;
            float value  = 0;
            float std_   = 0;
            for (int i = 0; i < latent->ne[3]; i++) {
                for (int j = 0; j < latent->ne[2]; j++) {
                    for (int k = 0; k < latent->ne[1]; k++) {
                        for (int l = 0; l < latent->ne[0]; l++) {
                            mean   = ggml_tensor_get_f32(moments, l, k, j, i);
                            logvar = ggml_tensor_get_f32(moments, l, k, j + (int)latent->ne[2], i);
                            logvar = std::max(-30.0f, std::min(logvar, 20.0f));
                            std_   = std::exp(0.5f * logvar);
                            value  = mean + std_ * ggml_tensor_get_f32(noise, l, k, j, i);
                            value  = value * scale_factor;
                            // printf("%d %d %d %d -> %f\n", i, j, k, l, value);
                            ggml_tensor_set_f32(latent, value, l, k, j, i);
                        }
                    }
                }
            }
        }
        return latent;
    }

    ggml_tensor*
    get_first_stage_encoding_mode(ggml_context* work_ctx, ggml_tensor* moments) {
        // ldm.modules.distributions.distributions.DiagonalGaussianDistribution.sample
        ggml_tensor* latent       = ggml_new_tensor_4d(work_ctx, moments->type, moments->ne[0], moments->ne[1], moments->ne[2] / 2, moments->ne[3]);
        struct ggml_tensor* noise = ggml_dup_tensor(work_ctx, latent);
        ggml_tensor_set_f32_randn(noise, rng);
        // noise = load_tensor_from_file(work_ctx, "noise.bin");
        {
            float mean = 0;
            for (int i = 0; i < latent->ne[3]; i++) {
                for (int j = 0; j < latent->ne[2]; j++) {
                    for (int k = 0; k < latent->ne[1]; k++) {
                        for (int l = 0; l < latent->ne[0]; l++) {
                            // mode and mean are the same for gaussians
                            mean = ggml_tensor_get_f32(moments, l, k, j, i);
                            ggml_tensor_set_f32(latent, mean, l, k, j, i);
                        }
                    }
                }
            }
        }
        return latent;
    }

    ggml_tensor* compute_first_stage(ggml_context* work_ctx, ggml_tensor* x, bool decode) {
        int64_t W = x->ne[0];
        int64_t H = x->ne[1];
        int64_t C = 8;
        if (use_tiny_autoencoder) {
            C = 4;
        } else {
            if (sd_version_is_sd3(version)) {
                C = 32;
            } else if (sd_version_is_flux(version)) {
                C = 32;
            }
        }
        ggml_tensor* result = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32,
                                                 decode ? (W * 8) : (W / 8),  // width
                                                 decode ? (H * 8) : (H / 8),  // height
                                                 decode ? 3 : C,
                                                 x->ne[3]);  // channels
        int64_t t0          = ggml_time_ms();

        // TODO: args instead of env for tile size / overlap?

        float tile_overlap          = 0.5f;
        const char* SD_TILE_OVERLAP = getenv("SD_TILE_OVERLAP");
        if (SD_TILE_OVERLAP != nullptr) {
            std::string sd_tile_overlap_str = SD_TILE_OVERLAP;
            try {
                tile_overlap = std::stof(sd_tile_overlap_str);
                if (tile_overlap < 0.0) {
                    LOG_WARN("SD_TILE_OVERLAP too low, setting it to 0.0");
                    tile_overlap = 0.0;
                } else if (tile_overlap > 0.5) {
                    LOG_WARN("SD_TILE_OVERLAP too high, setting it to 0.5");
                    tile_overlap = 0.5;
                }
            } catch (const std::invalid_argument&) {
                LOG_WARN("SD_TILE_OVERLAP is invalid, keeping the default");
            } catch (const std::out_of_range&) {
                LOG_WARN("SD_TILE_OVERLAP is out of range, keeping the default");
            }
        }

        int tile_size_x          = 32;
        int tile_size_y          = 32;
        const char* SD_TILE_SIZE = getenv("SD_TILE_SIZE");
        if (SD_TILE_SIZE != nullptr) {
            // format is AxB, or just A (equivalent to AxA)
            // A and B can be integers (tile size) or floating point
            // floating point <= 1 means simple fraction of the latent dimension
            // floating point > 1 means number of tiles across that dimension
            // a single number gets applied to both
            auto get_tile_factor = [tile_overlap](const std::string& factor_str) {
                float factor = std::stof(factor_str);
                if (factor > 1.0)
                    factor = 1 / (factor - factor * tile_overlap + tile_overlap);
                return factor;
            };
            const int latent_x           = W / (decode ? 1 : 8);
            const int latent_y           = H / (decode ? 1 : 8);
            const int min_tile_dimension = 4;
            std::string sd_tile_size_str = SD_TILE_SIZE;
            size_t x_pos                 = sd_tile_size_str.find('x');
            try {
                int tmp_x = tile_size_x, tmp_y = tile_size_y;
                if (x_pos != std::string::npos) {
                    std::string tile_x_str = sd_tile_size_str.substr(0, x_pos);
                    std::string tile_y_str = sd_tile_size_str.substr(x_pos + 1);
                    if (tile_x_str.find('.') != std::string::npos) {
                        tmp_x = std::round(latent_x * get_tile_factor(tile_x_str));
                    } else {
                        tmp_x = std::stoi(tile_x_str);
                    }
                    if (tile_y_str.find('.') != std::string::npos) {
                        tmp_y = std::round(latent_y * get_tile_factor(tile_y_str));
                    } else {
                        tmp_y = std::stoi(tile_y_str);
                    }
                } else {
                    if (sd_tile_size_str.find('.') != std::string::npos) {
                        float tile_factor = get_tile_factor(sd_tile_size_str);
                        tmp_x             = std::round(latent_x * tile_factor);
                        tmp_y             = std::round(latent_y * tile_factor);
                    } else {
                        tmp_x = tmp_y = std::stoi(sd_tile_size_str);
                    }
                }
                tile_size_x = std::max(std::min(tmp_x, latent_x), min_tile_dimension);
                tile_size_y = std::max(std::min(tmp_y, latent_y), min_tile_dimension);
            } catch (const std::invalid_argument&) {
                LOG_WARN("SD_TILE_SIZE is invalid, keeping the default");
            } catch (const std::out_of_range&) {
                LOG_WARN("SD_TILE_SIZE is out of range, keeping the default");
            }
        }

        if (!decode) {
            // TODO: also use and arg for this one?
            // to keep the compute buffer size consistent
            tile_size_x *= 1.30539;
            tile_size_y *= 1.30539;
        }
        if (!use_tiny_autoencoder) {
            if (decode) {
                ggml_tensor_scale(x, 1.0f / scale_factor);
            } else {
                ggml_tensor_scale_input(x);
            }
            if (vae_tiling) {
                if (SD_TILE_SIZE != nullptr) {
                    LOG_INFO("VAE Tile size: %dx%d", tile_size_x, tile_size_y);
                }
                if (SD_TILE_OVERLAP != nullptr) {
                    LOG_INFO("VAE Tile overlap: %.2f", tile_overlap);
                }
                // split latent in 32x32 tiles and compute in several steps
                auto on_tiling = [&](ggml_tensor* in, ggml_tensor* out, bool init) {
                    first_stage_model->compute(n_threads, in, decode, &out);
                };
                sd_tiling_non_square(x, result, 8, tile_size_x, tile_size_y, tile_overlap, on_tiling);
            } else {
                first_stage_model->compute(n_threads, x, decode, &result);
            }
            first_stage_model->free_compute_buffer();
            if (decode) {
                ggml_tensor_scale_output(result);
            }
        } else {
            if (vae_tiling) {
                // split latent in 64x64 tiles and compute in several steps
                auto on_tiling = [&](ggml_tensor* in, ggml_tensor* out, bool init) {
                    tae_first_stage->compute(n_threads, in, decode, &out);
                };
                sd_tiling(x, result, 8, 64, 0.5f, on_tiling);
            } else {
                tae_first_stage->compute(n_threads, x, decode, &result);
            }
            tae_first_stage->free_compute_buffer();
        }

        int64_t t1 = ggml_time_ms();
        LOG_DEBUG("computing vae [mode: %s] graph completed, taking %.2fs", decode ? "DECODE" : "ENCODE", (t1 - t0) * 1.0f / 1000);
        if (decode) {
            ggml_tensor_clamp(result, 0.0f, 1.0f);
        }
        return result;
    }

    ggml_tensor* encode_first_stage(ggml_context* work_ctx, ggml_tensor* x) {
        return compute_first_stage(work_ctx, x, false);
    }

    ggml_tensor* decode_first_stage(ggml_context* work_ctx, ggml_tensor* x) {
        return compute_first_stage(work_ctx, x, true);
    }
};

/*================================================= SD API ==================================================*/

struct sd_ctx_t {
    StableDiffusionGGML* sd = NULL;
};

sd_ctx_t* new_sd_ctx(const char* model_path_c_str,
                     const char* clip_l_path_c_str,
                     const char* clip_g_path_c_str,
                     const char* t5xxl_path_c_str,
                     const char* diffusion_model_path_c_str,
                     const char* vae_path_c_str,
                     const char* taesd_path_c_str,
                     const char* control_net_path_c_str,
                     const char* lora_model_dir_c_str,
                     const char* embed_dir_c_str,
                     const char* id_embed_dir_c_str,
                     bool vae_decode_only,
                     bool vae_tiling,
                     bool free_params_immediately,
                     int n_threads,
                     enum sd_type_t wtype,
                     enum rng_type_t rng_type,
                     enum schedule_t s,
                     bool keep_clip_on_cpu,
                     bool keep_control_net_cpu,
                     bool keep_vae_on_cpu,
                     bool diffusion_flash_attn,
                     bool tae_preview_only) {
    sd_ctx_t* sd_ctx = (sd_ctx_t*)malloc(sizeof(sd_ctx_t));
    if (sd_ctx == NULL) {
        return NULL;
    }
    std::string model_path(model_path_c_str);
    std::string clip_l_path(clip_l_path_c_str);
    std::string clip_g_path(clip_g_path_c_str);
    std::string t5xxl_path(t5xxl_path_c_str);
    std::string diffusion_model_path(diffusion_model_path_c_str);
    std::string vae_path(vae_path_c_str);
    std::string taesd_path(taesd_path_c_str);
    std::string control_net_path(control_net_path_c_str);
    std::string embd_path(embed_dir_c_str);
    std::string id_embd_path(id_embed_dir_c_str);
    std::string lora_model_dir(lora_model_dir_c_str);

    sd_ctx->sd = new StableDiffusionGGML(n_threads,
                                         vae_decode_only,
                                         free_params_immediately,
                                         lora_model_dir,
                                         rng_type);
    if (sd_ctx->sd == NULL) {
        return NULL;
    }

    if (!sd_ctx->sd->load_from_file(model_path,
                                    clip_l_path,
                                    clip_g_path,
                                    t5xxl_path_c_str,
                                    diffusion_model_path,
                                    vae_path,
                                    control_net_path,
                                    embd_path,
                                    id_embd_path,
                                    taesd_path,
                                    vae_tiling,
                                    (ggml_type)wtype,
                                    s,
                                    keep_clip_on_cpu,
                                    keep_control_net_cpu,
                                    keep_vae_on_cpu,
                                    diffusion_flash_attn,
                                    tae_preview_only)) {
        delete sd_ctx->sd;
        sd_ctx->sd = NULL;
        free(sd_ctx);
        return NULL;
    }
    return sd_ctx;
}

void free_sd_ctx(sd_ctx_t* sd_ctx) {
    if (sd_ctx->sd != NULL) {
        delete sd_ctx->sd;
        sd_ctx->sd = NULL;
    }
    free(sd_ctx);
}

sd_image_t* generate_image(sd_ctx_t* sd_ctx,
                           struct ggml_context* work_ctx,
                           ggml_tensor* init_latent,
                           std::string prompt,
                           std::string negative_prompt,
                           int clip_skip,
                           sd_guidance_params_t guidance,
                           float eta,
                           int width,
                           int height,
                           enum sample_method_t sample_method,
                           const std::vector<float>& sigmas,
                           int64_t seed,
                           int batch_count,
                           const sd_image_t* control_cond,
                           float control_strength,
                           float style_ratio,
                           bool normalize_input,
                           std::string input_id_images_path,
                           std::vector<struct ggml_tensor*> ref_latents,
                           ggml_tensor* concat_latent = NULL,
                           ggml_tensor* denoise_mask  = NULL) {
    if (seed < 0) {
        // Generally, when using the provided command line, the seed is always >0.
        // However, to prevent potential issues if 'stable-diffusion.cpp' is invoked as a library
        // by a third party with a seed <0, let's incorporate randomization here.
        srand((int)time(NULL));
        seed = rand();
    }

    // for (auto v : sigmas) {
    //     std::cout << v << " ";
    // }
    // std::cout << std::endl;

    int sample_steps = sigmas.size() - 1;

    // Apply lora
    auto result_pair                                = extract_and_remove_lora(prompt);
    std::unordered_map<std::string, float> lora_f2m = result_pair.first;  // lora_name -> multiplier

    for (auto& kv : lora_f2m) {
        LOG_DEBUG("lora %s:%.2f", kv.first.c_str(), kv.second);
    }

    prompt = result_pair.second;
    LOG_DEBUG("prompt after extract and remove lora: \"%s\"", prompt.c_str());

    int64_t t0 = ggml_time_ms();
    sd_ctx->sd->apply_loras(lora_f2m);
    int64_t t1 = ggml_time_ms();
    LOG_INFO("apply_loras completed, taking %.2fs", (t1 - t0) * 1.0f / 1000);

    // Photo Maker
    std::string prompt_text_only;
    ggml_tensor* init_img = NULL;
    SDCondition id_cond;
    std::vector<bool> class_tokens_mask;
    if (sd_ctx->sd->stacked_id) {
        if (!sd_ctx->sd->pmid_lora->applied) {
            t0 = ggml_time_ms();
            sd_ctx->sd->pmid_lora->apply(sd_ctx->sd->tensors, sd_ctx->sd->version, sd_ctx->sd->n_threads);
            t1                             = ggml_time_ms();
            sd_ctx->sd->pmid_lora->applied = true;
            LOG_INFO("pmid_lora apply completed, taking %.2fs", (t1 - t0) * 1.0f / 1000);
            if (sd_ctx->sd->free_params_immediately) {
                sd_ctx->sd->pmid_lora->free_params_buffer();
            }
        }
        // preprocess input id images
        std::vector<sd_image_t*> input_id_images;
        bool pmv2 = sd_ctx->sd->pmid_model->get_version() == PM_VERSION_2;
        if (sd_ctx->sd->pmid_model && input_id_images_path.size() > 0) {
            std::vector<std::string> img_files = get_files_from_dir(input_id_images_path);
            for (std::string img_file : img_files) {
                int c = 0;
                int width, height;
                if (ends_with(img_file, "safetensors")) {
                    continue;
                }
                uint8_t* input_image_buffer = stbi_load(img_file.c_str(), &width, &height, &c, 3);
                if (input_image_buffer == NULL) {
                    LOG_ERROR("PhotoMaker load image from '%s' failed", img_file.c_str());
                    continue;
                } else {
                    LOG_INFO("PhotoMaker loaded image from '%s'", img_file.c_str());
                }
                sd_image_t* input_image = NULL;
                input_image             = new sd_image_t{(uint32_t)width,
                                             (uint32_t)height,
                                             3,
                                             input_image_buffer};
                input_image             = preprocess_id_image(input_image);
                if (input_image == NULL) {
                    LOG_ERROR("preprocess input id image from '%s' failed", img_file.c_str());
                    continue;
                }
                input_id_images.push_back(input_image);
            }
        }
        if (input_id_images.size() > 0) {
            sd_ctx->sd->pmid_model->style_strength = style_ratio;
            int32_t w                              = input_id_images[0]->width;
            int32_t h                              = input_id_images[0]->height;
            int32_t channels                       = input_id_images[0]->channel;
            int32_t num_input_images               = (int32_t)input_id_images.size();
            init_img                               = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, w, h, channels, num_input_images);
            // TODO: move these to somewhere else and be user settable
            float mean[] = {0.48145466f, 0.4578275f, 0.40821073f};
            float std[]  = {0.26862954f, 0.26130258f, 0.27577711f};
            for (int i = 0; i < num_input_images; i++) {
                sd_image_t* init_image = input_id_images[i];
                if (normalize_input)
                    sd_mul_images_to_tensor(init_image->data, init_img, i, mean, std);
                else
                    sd_mul_images_to_tensor(init_image->data, init_img, i, NULL, NULL);
            }
            t0                            = ggml_time_ms();
            auto cond_tup                 = sd_ctx->sd->cond_stage_model->get_learned_condition_with_trigger(work_ctx,
                                                                                                             sd_ctx->sd->n_threads, prompt,
                                                                                                             clip_skip,
                                                                                                             width,
                                                                                                             height,
                                                                                                             num_input_images,
                                                                                                             sd_ctx->sd->diffusion_model->get_adm_in_channels());
            id_cond                       = std::get<0>(cond_tup);
            class_tokens_mask             = std::get<1>(cond_tup);  //
            struct ggml_tensor* id_embeds = NULL;
            if (pmv2) {
                // id_embeds = sd_ctx->sd->pmid_id_embeds->get();
                id_embeds = load_tensor_from_file(work_ctx, path_join(input_id_images_path, "id_embeds.bin"));
                // print_ggml_tensor(id_embeds, true, "id_embeds:");
            }
            id_cond.c_crossattn = sd_ctx->sd->id_encoder(work_ctx, init_img, id_cond.c_crossattn, id_embeds, class_tokens_mask);
            t1                  = ggml_time_ms();
            LOG_INFO("Photomaker ID Stacking, taking %" PRId64 " ms", t1 - t0);
            if (sd_ctx->sd->free_params_immediately) {
                sd_ctx->sd->pmid_model->free_params_buffer();
            }
            // Encode input prompt without the trigger word for delayed conditioning
            prompt_text_only = sd_ctx->sd->cond_stage_model->remove_trigger_from_prompt(work_ctx, prompt);
            // printf("%s || %s \n", prompt.c_str(), prompt_text_only.c_str());
            prompt = prompt_text_only;  //
            // if (sample_steps < 50) {
            //     LOG_INFO("sampling steps increases from %d to 50 for PHOTOMAKER", sample_steps);
            //     sample_steps = 50;
            // }
        } else {
            LOG_WARN("Provided PhotoMaker model file, but NO input ID images");
            LOG_WARN("Turn off PhotoMaker");
            sd_ctx->sd->stacked_id = false;
        }
        for (sd_image_t* img : input_id_images) {
            free(img->data);
        }
        input_id_images.clear();
    }

    // Get learned condition
    t0               = ggml_time_ms();
    SDCondition cond = sd_ctx->sd->cond_stage_model->get_learned_condition(work_ctx,
                                                                           sd_ctx->sd->n_threads,
                                                                           prompt,
                                                                           clip_skip,
                                                                           width,
                                                                           height,
                                                                           sd_ctx->sd->diffusion_model->get_adm_in_channels());

    SDCondition uncond;
    if (guidance.txt_cfg != 1.0 || sd_version_use_concat(sd_ctx->sd->version) && guidance.txt_cfg != guidance.img_cfg) {
        bool force_zero_embeddings = false;
        if (sd_version_is_sdxl(sd_ctx->sd->version) && negative_prompt.size() == 0 && !sd_ctx->sd->is_using_edm_v_parameterization) {
            force_zero_embeddings = true;
        }
        uncond = sd_ctx->sd->cond_stage_model->get_learned_condition(work_ctx,
                                                                     sd_ctx->sd->n_threads,
                                                                     negative_prompt,
                                                                     clip_skip,
                                                                     width,
                                                                     height,
                                                                     sd_ctx->sd->diffusion_model->get_adm_in_channels(),
                                                                     force_zero_embeddings);
    }
    t1 = ggml_time_ms();
    LOG_INFO("get_learned_condition completed, taking %" PRId64 " ms", t1 - t0);

    if (sd_ctx->sd->free_params_immediately) {
        sd_ctx->sd->cond_stage_model->free_params_buffer();
    }

    // Control net hint
    struct ggml_tensor* image_hint = NULL;
    if (control_cond != NULL) {
        image_hint = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, width, height, 3, 1);
        sd_image_to_tensor(control_cond->data, image_hint);
    }

    // Sample
    std::vector<struct ggml_tensor*> final_latents;  // collect latents to decode
    int C = 4;
    if (sd_version_is_sd3(sd_ctx->sd->version)) {
        C = 16;
    } else if (sd_version_is_flux(sd_ctx->sd->version)) {
        C = 16;
    }
    int W = width / 8;
    int H = height / 8;
    LOG_INFO("sampling using %s method", sampling_methods_str[sample_method]);

    struct ggml_tensor* control_latent = NULL;
    if (sd_version_is_control(sd_ctx->sd->version) && image_hint != NULL) {
        if (!sd_ctx->sd->use_tiny_autoencoder) {
            struct ggml_tensor* control_moments = sd_ctx->sd->encode_first_stage(work_ctx, image_hint);
            control_latent                      = sd_ctx->sd->get_first_stage_encoding(work_ctx, control_moments);
        } else {
            control_latent = sd_ctx->sd->encode_first_stage(work_ctx, image_hint);
        }
        ggml_tensor_scale(control_latent, control_strength);
    }

    if (sd_version_is_inpaint(sd_ctx->sd->version)) {
        int64_t mask_channels = 1;
        if (sd_ctx->sd->version == VERSION_FLUX_FILL) {
            mask_channels = 8 * 8;  // flatten the whole mask
        } else if (sd_ctx->sd->version == VERSION_FLEX_2) {
            mask_channels = 1 + init_latent->ne[2];
        }
        auto empty_latent = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, init_latent->ne[0], init_latent->ne[1], mask_channels + init_latent->ne[2], 1);
        // no mask, set the whole image as masked
        for (int64_t x = 0; x < empty_latent->ne[0]; x++) {
            for (int64_t y = 0; y < empty_latent->ne[1]; y++) {
                if (sd_ctx->sd->version == VERSION_FLUX_FILL) {
                    // TODO: this might be wrong
                    for (int64_t c = 0; c < init_latent->ne[2]; c++) {
                        ggml_tensor_set_f32(empty_latent, 0, x, y, c);
                    }
                    for (int64_t c = init_latent->ne[2]; c < empty_latent->ne[2]; c++) {
                        ggml_tensor_set_f32(empty_latent, 1, x, y, c);
                    }
                } else if (sd_ctx->sd->version == VERSION_FLEX_2) {
                    for (int64_t c = 0; c < empty_latent->ne[2]; c++) {
                        // 0x16,1x1,0x16
                        ggml_tensor_set_f32(empty_latent, c == init_latent->ne[2], x, y, c);
                    }
                } else {
                    ggml_tensor_set_f32(empty_latent, 1, x, y, 0);
                    for (int64_t c = 1; c < empty_latent->ne[2]; c++) {
                        ggml_tensor_set_f32(empty_latent, 0, x, y, c);
                    }
                }
            }
        }

        if (sd_ctx->sd->version == VERSION_FLEX_2 && control_latent != NULL && sd_ctx->sd->control_net == NULL) {
            bool no_inpaint = concat_latent == NULL;
            if (no_inpaint) {
                concat_latent = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, init_latent->ne[0], init_latent->ne[1], mask_channels + init_latent->ne[2], 1);
            }
            // fill in the control image here
            for (int64_t x = 0; x < control_latent->ne[0]; x++) {
                for (int64_t y = 0; y < control_latent->ne[1]; y++) {
                    if (no_inpaint) {
                        for (int64_t c = 0; c < concat_latent->ne[2] - control_latent->ne[2]; c++) {
                            // 0x16,1x1,0x16
                            ggml_tensor_set_f32(concat_latent, c == init_latent->ne[2], x, y, c);
                        }
                    }
                    for (int64_t c = 0; c < control_latent->ne[2]; c++) {
                        float v = ggml_tensor_get_f32(control_latent, x, y, c);
                        ggml_tensor_set_f32(concat_latent, v, x, y, concat_latent->ne[2] - control_latent->ne[2] + c);
                    }
                }
            }
        } else if (concat_latent == NULL) {
            concat_latent = empty_latent;
        }
        cond.c_concat   = concat_latent;
        uncond.c_concat = empty_latent;
        denoise_mask    = NULL;
    } else if (sd_version_is_edit(sd_ctx->sd->version) || sd_version_is_control(sd_ctx->sd->version)) {
        LOG_INFO("HERE");
        auto empty_latent = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, init_latent->ne[0], init_latent->ne[1], init_latent->ne[2], init_latent->ne[3]);
        LOG_INFO("HERE");
        ggml_set_f32(empty_latent, 0);
        uncond.c_concat = empty_latent;
        if (sd_version_is_control(sd_ctx->sd->version) && control_latent != NULL && sd_ctx->sd->control_net == NULL) {
            concat_latent = control_latent;
        }
        if (concat_latent == NULL) {
            concat_latent = empty_latent;
        }
        LOG_INFO("HERE");

        cond.c_concat = concat_latent;
    }

    for (int b = 0; b < batch_count; b++) {
        int64_t sampling_start = ggml_time_ms();
        int64_t cur_seed       = seed + b;
        LOG_INFO("generating image: %i/%i - seed %" PRId64, b + 1, batch_count, cur_seed);

        sd_ctx->sd->rng->manual_seed(cur_seed);
        struct ggml_tensor* x_t   = init_latent;
        struct ggml_tensor* noise = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, W, H, C, 1);
        ggml_tensor_set_f32_randn(noise, sd_ctx->sd->rng);

        int start_merge_step = -1;
        if (sd_ctx->sd->stacked_id) {
            start_merge_step = int(sd_ctx->sd->pmid_model->style_strength / 100.f * sample_steps);
            // if (start_merge_step > 30)
            //     start_merge_step = 30;
            LOG_INFO("PHOTOMAKER: start_merge_step: %d", start_merge_step);
        }

        // Disable min_cfg
        guidance.min_cfg = guidance.txt_cfg;

        struct ggml_tensor* x_0 = sd_ctx->sd->sample(work_ctx,
                                                     x_t,
                                                     noise,
                                                     cond,
                                                     uncond,
                                                     image_hint,
                                                     control_strength,
                                                     guidance,
                                                     eta,
                                                     sample_method,
                                                     sigmas,
                                                     start_merge_step,
                                                     id_cond,
                                                     ref_latents,
                                                     denoise_mask);

        // struct ggml_tensor* x_0 = load_tensor_from_file(ctx, "samples_ddim.bin");
        // print_ggml_tensor(x_0);
        int64_t sampling_end = ggml_time_ms();
        LOG_INFO("sampling completed, taking %.2fs", (sampling_end - sampling_start) * 1.0f / 1000);
        final_latents.push_back(x_0);
    }

    if (sd_ctx->sd->free_params_immediately) {
        sd_ctx->sd->diffusion_model->free_params_buffer();
    }
    int64_t t3 = ggml_time_ms();
    LOG_INFO("generating %" PRId64 " latent images completed, taking %.2fs", final_latents.size(), (t3 - t1) * 1.0f / 1000);

    // Decode to image
    LOG_INFO("decoding %zu latents", final_latents.size());
    std::vector<struct ggml_tensor*> decoded_images;  // collect decoded images
    for (size_t i = 0; i < final_latents.size(); i++) {
        t1                      = ggml_time_ms();
        struct ggml_tensor* img = sd_ctx->sd->decode_first_stage(work_ctx, final_latents[i] /* x_0 */);
        // print_ggml_tensor(img);
        if (img != NULL) {
            decoded_images.push_back(img);
        }
        int64_t t2 = ggml_time_ms();
        LOG_INFO("latent %" PRId64 " decoded, taking %.2fs", i + 1, (t2 - t1) * 1.0f / 1000);
    }

    int64_t t4 = ggml_time_ms();
    LOG_INFO("decode_first_stage completed, taking %.2fs", (t4 - t3) * 1.0f / 1000);
    if (sd_ctx->sd->free_params_immediately && !sd_ctx->sd->use_tiny_autoencoder) {
        sd_ctx->sd->first_stage_model->free_params_buffer();
    }
    sd_image_t* result_images = (sd_image_t*)calloc(batch_count, sizeof(sd_image_t));
    if (result_images == NULL) {
        ggml_free(work_ctx);
        return NULL;
    }

    for (size_t i = 0; i < decoded_images.size(); i++) {
        result_images[i].width   = width;
        result_images[i].height  = height;
        result_images[i].channel = 3;
        result_images[i].data    = sd_tensor_to_image(decoded_images[i]);
    }
    ggml_free(work_ctx);

    return result_images;
}

sd_image_t* txt2img(sd_ctx_t* sd_ctx,
                    const char* prompt_c_str,
                    const char* negative_prompt_c_str,
                    int clip_skip,
                    sd_guidance_params_t guidance,
                    float eta,
                    int width,
                    int height,
                    enum sample_method_t sample_method,
                    int sample_steps,
                    int64_t seed,
                    int batch_count,
                    const sd_image_t* control_cond,
                    float control_strength,
                    float style_ratio,
                    bool normalize_input,
                    const char* input_id_images_path_c_str) {
    LOG_DEBUG("txt2img %dx%d", width, height);
    if (sd_ctx == NULL) {
        return NULL;
    }

    struct ggml_init_params params;
    params.mem_size = static_cast<size_t>(20 * 1024 * 1024);  // 20 MB
    if (sd_version_is_sd3(sd_ctx->sd->version)) {
        params.mem_size *= 3;
    }
    if (sd_version_is_flux(sd_ctx->sd->version)) {
        params.mem_size *= 4;
    }
    if (sd_ctx->sd->stacked_id) {
        params.mem_size += static_cast<size_t>(10 * 1024 * 1024);  // 10 MB
    }
    auto sd_preview_mode = sd_get_preview_mode();
    if (sd_preview_mode != SD_PREVIEW_NONE && sd_preview_mode != SD_PREVIEW_PROJ) {
        params.mem_size *= 2;
    }
    params.mem_size += width * height * 3 * sizeof(float);
    params.mem_size *= batch_count;
    params.mem_buffer = NULL;
    params.no_alloc   = false;
    // LOG_DEBUG("mem_size %u ", params.mem_size);

    struct ggml_context* work_ctx = ggml_init(params);
    if (!work_ctx) {
        LOG_ERROR("ggml_init() failed");
        return NULL;
    }

    size_t t0 = ggml_time_ms();

    std::vector<float> sigmas = sd_ctx->sd->denoiser->get_sigmas(sample_steps);

    int C = 4;
    if (sd_version_is_sd3(sd_ctx->sd->version)) {
        C = 16;
    } else if (sd_version_is_flux(sd_ctx->sd->version)) {
        C = 16;
    }
    int W                    = width / 8;
    int H                    = height / 8;
    ggml_tensor* init_latent = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, W, H, C, 1);
    if (sd_version_is_sd3(sd_ctx->sd->version)) {
        ggml_set_f32(init_latent, 0.0609f);
    } else if (sd_version_is_flux(sd_ctx->sd->version)) {
        ggml_set_f32(init_latent, 0.1159f);
    } else {
        ggml_set_f32(init_latent, 0.f);
    }

    if (sd_version_is_inpaint(sd_ctx->sd->version)) {
        LOG_WARN("This is an inpainting model, this should only be used in img2img mode with a mask");
    }

    sd_image_t* result_images = generate_image(sd_ctx,
                                               work_ctx,
                                               init_latent,
                                               prompt_c_str,
                                               negative_prompt_c_str,
                                               clip_skip,
                                               guidance,
                                               eta,
                                               width,
                                               height,
                                               sample_method,
                                               sigmas,
                                               seed,
                                               batch_count,
                                               control_cond,
                                               control_strength,
                                               style_ratio,
                                               normalize_input,
                                               input_id_images_path_c_str,
                                               {});

    size_t t1 = ggml_time_ms();

    LOG_INFO("txt2img completed in %.2fs", (t1 - t0) * 1.0f / 1000);

    return result_images;
}

sd_image_t* img2img(sd_ctx_t* sd_ctx,
                    sd_image_t init_image,
                    sd_image_t mask,
                    const char* prompt_c_str,
                    const char* negative_prompt_c_str,
                    int clip_skip,
                    sd_guidance_params_t guidance,
                    float eta,
                    int width,
                    int height,
                    sample_method_t sample_method,
                    int sample_steps,
                    float strength,
                    int64_t seed,
                    int batch_count,
                    const sd_image_t* control_cond,
                    float control_strength,
                    float style_ratio,
                    bool normalize_input,
                    const char* input_id_images_path_c_str) {
    LOG_DEBUG("img2img %dx%d", width, height);
    if (sd_ctx == NULL) {
        return NULL;
    }

    struct ggml_init_params params;
    params.mem_size = static_cast<size_t>(20 * 1024 * 1024);  // 20 MB
    if (sd_version_is_sd3(sd_ctx->sd->version)) {
        params.mem_size *= 2;
    }
    if (sd_version_is_flux(sd_ctx->sd->version)) {
        params.mem_size *= 3;
    }
    if (sd_ctx->sd->stacked_id) {
        params.mem_size += static_cast<size_t>(10 * 1024 * 1024);  // 10 MB
    }
    params.mem_size += width * height * 3 * sizeof(float) * 3;
    params.mem_size *= batch_count;
    params.mem_buffer = NULL;
    params.no_alloc   = false;
    // LOG_DEBUG("mem_size %u ", params.mem_size);

    struct ggml_context* work_ctx = ggml_init(params);
    if (!work_ctx) {
        LOG_ERROR("ggml_init() failed");
        return NULL;
    }

    size_t t0 = ggml_time_ms();

    if (seed < 0) {
        srand((int)time(NULL));
        seed = rand();
    }
    sd_ctx->sd->rng->manual_seed(seed);

    ggml_tensor* init_img = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, width, height, 3, 1);
    ggml_tensor* mask_img = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, width, height, 1, 1);

    sd_mask_to_tensor(mask.data, mask_img);

    sd_image_to_tensor(init_image.data, init_img);

    ggml_tensor* concat_latent = NULL;
    ggml_tensor* denoise_mask  = NULL;

    ggml_tensor* init_latent  = NULL;
    ggml_tensor* init_moments = NULL;
    if (!sd_ctx->sd->use_tiny_autoencoder) {
        init_moments = sd_ctx->sd->encode_first_stage(work_ctx, init_img);
        init_latent  = sd_ctx->sd->get_first_stage_encoding(work_ctx, init_moments);
    } else {
        init_latent = sd_ctx->sd->encode_first_stage(work_ctx, init_img);
    }

    if (sd_version_is_inpaint(sd_ctx->sd->version)) {
        int64_t mask_channels = 1;
        if (sd_ctx->sd->version == VERSION_FLUX_FILL) {
            mask_channels = 8 * 8;  // flatten the whole mask
        } else if (sd_ctx->sd->version == VERSION_FLEX_2) {
            mask_channels = 1 + init_latent->ne[2];
        }
        ggml_tensor* masked_latent_0 = NULL;
        if (sd_ctx->sd->version != VERSION_FLEX_2) {
            // most inpaint models mask before vae
            ggml_tensor* masked_img = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, width, height, 3, 1);
            // Restore init_img (encode_first_stage has side effects) TODO: remove the side effects?
            sd_image_to_tensor(init_image.data, init_img);
            sd_apply_mask(init_img, mask_img, masked_img);
            if (!sd_ctx->sd->use_tiny_autoencoder) {
                ggml_tensor* moments = sd_ctx->sd->encode_first_stage(work_ctx, masked_img);
                masked_latent_0      = sd_ctx->sd->get_first_stage_encoding(work_ctx, moments);
            } else {
                masked_latent_0 = sd_ctx->sd->encode_first_stage(work_ctx, masked_img);
            }
        } else {
            // mask after vae
            masked_latent_0 = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, init_latent->ne[0], init_latent->ne[1], init_latent->ne[2], 1);
            sd_apply_mask(init_latent, mask_img, masked_latent_0, 0.);
        }
        concat_latent = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, masked_latent_0->ne[0], masked_latent_0->ne[1], mask_channels + masked_latent_0->ne[2], 1);
        for (int ix = 0; ix < masked_latent_0->ne[0]; ix++) {
            for (int iy = 0; iy < masked_latent_0->ne[1]; iy++) {
                int mx = ix * 8;
                int my = iy * 8;
                if (sd_ctx->sd->version == VERSION_FLUX_FILL) {
                    for (int k = 0; k < masked_latent_0->ne[2]; k++) {
                        float v = ggml_tensor_get_f32(masked_latent_0, ix, iy, k);
                        ggml_tensor_set_f32(concat_latent, v, ix, iy, k);
                    }
                    // "Encode" 8x8 mask chunks into a flattened 1x64 vector, and concatenate to masked image
                    for (int x = 0; x < 8; x++) {
                        for (int y = 0; y < 8; y++) {
                            float m = ggml_tensor_get_f32(mask_img, mx + x, my + y);
                            // TODO: check if the way the mask is flattened is correct (is it supposed to be x*8+y or x+8*y?)
                            // python code was using "b (h 8) (w 8) -> b (8 8) h w"
                            ggml_tensor_set_f32(concat_latent, m, ix, iy, masked_latent_0->ne[2] + x * 8 + y);
                        }
                    }
                } else if (sd_ctx->sd->version == VERSION_FLEX_2) {
                    float m = ggml_tensor_get_f32(mask_img, mx, my);
                    ggml_tensor_set_f32(concat_latent, m, ix, iy, 0);
                    for (int k = 0; k < masked_latent_0->ne[2]; k++) {
                        float v = ggml_tensor_get_f32(masked_latent_0, ix, iy, k);
                        ggml_tensor_set_f32(concat_latent, v, ix, iy, k + mask_channels);
                    }
                }
            }
        }
    } else {
        if (sd_version_is_edit(sd_ctx->sd->version)) {
            // Not actually masked, we're just highjacking the masked_latent variable since it will be used the same way
            if (!sd_ctx->sd->use_tiny_autoencoder) {
                if (sd_ctx->sd->is_using_edm_v_parameterization) {
                    // for CosXL edit
                    concat_latent = sd_ctx->sd->get_first_stage_encoding(work_ctx, init_moments);
                } else {
                    concat_latent = sd_ctx->sd->get_first_stage_encoding_mode(work_ctx, init_moments);
                }
            } else {
                concat_latent = init_latent;
            }
        }
        // LOG_WARN("Inpainting with a base model is not great");
        denoise_mask = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, width / 8, height / 8, 1, 1);
        for (int ix = 0; ix < denoise_mask->ne[0]; ix++) {
            for (int iy = 0; iy < denoise_mask->ne[1]; iy++) {
                int mx  = ix * 8;
                int my  = iy * 8;
                float m = ggml_tensor_get_f32(mask_img, mx, my);
                ggml_tensor_set_f32(denoise_mask, m, ix, iy);
            }
        }
    }

    print_ggml_tensor(init_latent, true);
    size_t t1 = ggml_time_ms();
    LOG_INFO("encode_first_stage completed, taking %.2fs", (t1 - t0) * 1.0f / 1000);

    std::vector<float> sigmas = sd_ctx->sd->denoiser->get_sigmas(sample_steps);
    size_t t_enc              = static_cast<size_t>(sample_steps * strength);
    if (t_enc == sample_steps)
        t_enc--;
    LOG_INFO("target t_enc is %zu steps", t_enc);
    std::vector<float> sigma_sched;
    sigma_sched.assign(sigmas.begin() + sample_steps - t_enc - 1, sigmas.end());

    sd_image_t* result_images = generate_image(sd_ctx,
                                               work_ctx,
                                               init_latent,
                                               prompt_c_str,
                                               negative_prompt_c_str,
                                               clip_skip,
                                               guidance,
                                               eta,
                                               width,
                                               height,
                                               sample_method,
                                               sigma_sched,
                                               seed,
                                               batch_count,
                                               control_cond,
                                               control_strength,
                                               style_ratio,
                                               normalize_input,
                                               input_id_images_path_c_str, {}, concat_latent, denoise_mask);

    size_t t2 = ggml_time_ms();

    LOG_INFO("img2img completed in %.2fs", (t1 - t0) * 1.0f / 1000);

    return result_images;
}

SD_API sd_image_t* img2vid(sd_ctx_t* sd_ctx,
                           sd_image_t init_image,
                           int width,
                           int height,
                           int video_frames,
                           int motion_bucket_id,
                           int fps,
                           float augmentation_level,
                           sd_guidance_params_t guidance,
                           enum sample_method_t sample_method,
                           int sample_steps,
                           float strength,
                           int64_t seed) {
    if (sd_ctx == NULL) {
        return NULL;
    }

    LOG_INFO("img2vid %dx%d", width, height);

    std::vector<float> sigmas = sd_ctx->sd->denoiser->get_sigmas(sample_steps);

    struct ggml_init_params params;
    params.mem_size = static_cast<size_t>(10 * 1024) * 1024;  // 10 MB
    params.mem_size += width * height * 3 * sizeof(float) * video_frames;
    params.mem_buffer = NULL;
    params.no_alloc   = false;
    // LOG_DEBUG("mem_size %u ", params.mem_size);

    // draft context
    struct ggml_context* work_ctx = ggml_init(params);
    if (!work_ctx) {
        LOG_ERROR("ggml_init() failed");
        return NULL;
    }

    if (seed < 0) {
        seed = (int)time(NULL);
    }

    sd_ctx->sd->rng->manual_seed(seed);

    int64_t t0 = ggml_time_ms();

    SDCondition cond = sd_ctx->sd->get_svd_condition(work_ctx,
                                                     init_image,
                                                     width,
                                                     height,
                                                     fps,
                                                     motion_bucket_id,
                                                     augmentation_level);

    auto uc_crossattn = ggml_dup_tensor(work_ctx, cond.c_crossattn);
    ggml_set_f32(uc_crossattn, 0.f);

    auto uc_concat = ggml_dup_tensor(work_ctx, cond.c_concat);
    ggml_set_f32(uc_concat, 0.f);

    auto uc_vector = ggml_dup_tensor(work_ctx, cond.c_vector);

    SDCondition uncond = SDCondition(uc_crossattn, uc_vector, uc_concat);

    int64_t t1 = ggml_time_ms();
    LOG_INFO("get_learned_condition completed, taking %" PRId64 " ms", t1 - t0);
    if (sd_ctx->sd->free_params_immediately) {
        sd_ctx->sd->clip_vision->free_params_buffer();
    }

    sd_ctx->sd->rng->manual_seed(seed);
    int C                   = 4;
    int W                   = width / 8;
    int H                   = height / 8;
    struct ggml_tensor* x_t = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, W, H, C, video_frames);
    ggml_set_f32(x_t, 0.f);

    struct ggml_tensor* noise = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, W, H, C, video_frames);
    ggml_tensor_set_f32_randn(noise, sd_ctx->sd->rng);

    LOG_INFO("sampling using %s method", sampling_methods_str[sample_method]);
    struct ggml_tensor* x_0 = sd_ctx->sd->sample(work_ctx,
                                                 x_t,
                                                 noise,
                                                 cond,
                                                 uncond,
                                                 {},
                                                 0.f,
                                                 guidance,
                                                 0.f,
                                                 sample_method,
                                                 sigmas,
                                                 -1,
                                                 SDCondition(NULL, NULL, NULL),
                                                 std::vector<struct ggml_tensor*>(),
                                                 NULL);

    int64_t t2 = ggml_time_ms();
    LOG_INFO("sampling completed, taking %.2fs", (t2 - t1) * 1.0f / 1000);
    if (sd_ctx->sd->free_params_immediately) {
        sd_ctx->sd->diffusion_model->free_params_buffer();
    }

    struct ggml_tensor* img = sd_ctx->sd->decode_first_stage(work_ctx, x_0);
    if (sd_ctx->sd->free_params_immediately) {
        sd_ctx->sd->first_stage_model->free_params_buffer();
    }
    if (img == NULL) {
        ggml_free(work_ctx);
        return NULL;
    }

    sd_image_t* result_images = (sd_image_t*)calloc(video_frames, sizeof(sd_image_t));
    if (result_images == NULL) {
        ggml_free(work_ctx);
        return NULL;
    }

    for (size_t i = 0; i < video_frames; i++) {
        auto img_i = ggml_view_3d(work_ctx, img, img->ne[0], img->ne[1], img->ne[2], img->nb[1], img->nb[2], img->nb[3] * i);

        result_images[i].width   = width;
        result_images[i].height  = height;
        result_images[i].channel = 3;
        result_images[i].data    = sd_tensor_to_image(img_i);
    }
    ggml_free(work_ctx);

    int64_t t3 = ggml_time_ms();

    LOG_INFO("img2vid completed in %.2fs", (t3 - t0) * 1.0f / 1000);

    return result_images;
}

sd_image_t* edit(sd_ctx_t* sd_ctx,
                 sd_image_t* ref_images,
                 int ref_images_count,
                 const char* prompt_c_str,
                 const char* negative_prompt_c_str,
                 int clip_skip,
                 sd_guidance_params_t guidance,
                 float eta,
                 int width,
                 int height,
                 enum sample_method_t sample_method,
                 int sample_steps,
                 int64_t seed,
                 int batch_count,
                 const sd_image_t* control_cond,
                 float control_strength,
                 float style_ratio,
                 bool normalize_input,
                 const char* input_id_images_path_c_str) {
    LOG_DEBUG("edit %dx%d", width, height);
    if (sd_ctx == NULL) {
        return NULL;
    }
    if (ref_images_count <= 0) {
        LOG_ERROR("ref images count should > 0");
        return NULL;
    }

    struct ggml_init_params params;
    params.mem_size = static_cast<size_t>(30 * 1024 * 1024);  // 10 MB
    params.mem_size += width * height * 3 * sizeof(float) * 3 * ref_images_count;
    params.mem_size *= batch_count;
    params.mem_buffer = NULL;
    params.no_alloc   = false;
    // LOG_DEBUG("mem_size %u ", params.mem_size);

    struct ggml_context* work_ctx = ggml_init(params);
    if (!work_ctx) {
        LOG_ERROR("ggml_init() failed");
        return NULL;
    }

    if (seed < 0) {
        srand((int)time(NULL));
        seed = rand();
    }
    sd_ctx->sd->rng->manual_seed(seed);

    int C = 4;
    if (sd_version_is_sd3(sd_ctx->sd->version)) {
        C = 16;
    } else if (sd_version_is_flux(sd_ctx->sd->version)) {
        C = 16;
    }
    int W                    = width / 8;
    int H                    = height / 8;
    ggml_tensor* init_latent = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, W, H, C, 1);
    if (sd_version_is_sd3(sd_ctx->sd->version)) {
        ggml_set_f32(init_latent, 0.0609f);
    } else if (sd_version_is_flux(sd_ctx->sd->version)) {
        ggml_set_f32(init_latent, 0.1159f);
    } else {
        ggml_set_f32(init_latent, 0.f);
    }

    size_t t0 = ggml_time_ms();

    std::vector<struct ggml_tensor*> ref_latents;
    for (int i = 0; i < ref_images_count; i++) {
        ggml_tensor* img = ggml_new_tensor_4d(work_ctx, GGML_TYPE_F32, ref_images[i].width, ref_images[i].height, 3, 1);
        sd_image_to_tensor(ref_images[i].data, img);

        ggml_tensor* latent = NULL;
        if (!sd_ctx->sd->use_tiny_autoencoder) {
            ggml_tensor* moments = sd_ctx->sd->encode_first_stage(work_ctx, img);
            latent               = sd_ctx->sd->get_first_stage_encoding(work_ctx, moments);
        } else {
            latent = sd_ctx->sd->encode_first_stage(work_ctx, img);
        }
        ref_latents.push_back(latent);
    }

    size_t t1 = ggml_time_ms();
    LOG_INFO("encode_first_stage completed, taking %.2fs", (t1 - t0) * 1.0f / 1000);

    std::vector<float> sigmas = sd_ctx->sd->denoiser->get_sigmas(sample_steps);

    sd_image_t* result_images = generate_image(sd_ctx,
                                               work_ctx,
                                               init_latent,
                                               prompt_c_str,
                                               negative_prompt_c_str,
                                               clip_skip,
                                               guidance,
                                               eta,
                                               width,
                                               height,
                                               sample_method,
                                               sigmas,
                                               seed,
                                               batch_count,
                                               control_cond,
                                               control_strength,
                                               style_ratio,
                                               normalize_input,
                                               NULL,
                                               ref_latents);

    size_t t2 = ggml_time_ms();

    LOG_INFO("edit completed in %.2fs", (t2 - t0) * 1.0f / 1000);

    return result_images;
}