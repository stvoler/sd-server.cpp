#ifndef __STABLE_DIFFUSION_H__
#define __STABLE_DIFFUSION_H__

#if defined(_WIN32) || defined(__CYGWIN__)
#ifndef SD_BUILD_SHARED_LIB
#define SD_API
#else
#ifdef SD_BUILD_DLL
#define SD_API __declspec(dllexport)
#else
#define SD_API __declspec(dllimport)
#endif
#endif
#else
#if __GNUC__ >= 4
#define SD_API __attribute__((visibility("default")))
#else
#define SD_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum rng_type_t {
    STD_DEFAULT_RNG,
    CUDA_RNG
};

enum sample_method_t {
    EULER_A,
    EULER,
    HEUN,
    DPM2,
    DPMPP2S_A,
    DPMPP2M,
    DPMPP2Mv2,
    IPNDM,
    IPNDM_V,
    LCM,
    DDIM_TRAILING,
    TCD,
    N_SAMPLE_METHODS
};

enum schedule_t {
    DEFAULT,
    DISCRETE,
    KARRAS,
    EXPONENTIAL,
    AYS,
    GITS,
    N_SCHEDULES
};

// same as enum ggml_type
enum sd_type_t {
    SD_TYPE_F32  = 0,
    SD_TYPE_F16  = 1,
    SD_TYPE_Q4_0 = 2,
    SD_TYPE_Q4_1 = 3,
    // SD_TYPE_Q4_2 = 4, support has been removed
    // SD_TYPE_Q4_3 = 5, support has been removed
    SD_TYPE_Q5_0    = 6,
    SD_TYPE_Q5_1    = 7,
    SD_TYPE_Q8_0    = 8,
    SD_TYPE_Q8_1    = 9,
    SD_TYPE_Q2_K    = 10,
    SD_TYPE_Q3_K    = 11,
    SD_TYPE_Q4_K    = 12,
    SD_TYPE_Q5_K    = 13,
    SD_TYPE_Q6_K    = 14,
    SD_TYPE_Q8_K    = 15,
    SD_TYPE_IQ2_XXS = 16,
    SD_TYPE_IQ2_XS  = 17,
    SD_TYPE_IQ3_XXS = 18,
    SD_TYPE_IQ1_S   = 19,
    SD_TYPE_IQ4_NL  = 20,
    SD_TYPE_IQ3_S   = 21,
    SD_TYPE_IQ2_S   = 22,
    SD_TYPE_IQ4_XS  = 23,
    SD_TYPE_I8      = 24,
    SD_TYPE_I16     = 25,
    SD_TYPE_I32     = 26,
    SD_TYPE_I64     = 27,
    SD_TYPE_F64     = 28,
    SD_TYPE_IQ1_M   = 29,
    SD_TYPE_BF16    = 30,
    // SD_TYPE_Q4_0_4_4 = 31, support has been removed from gguf files
    // SD_TYPE_Q4_0_4_8 = 32,
    // SD_TYPE_Q4_0_8_8 = 33,
    SD_TYPE_TQ1_0 = 34,
    SD_TYPE_TQ2_0 = 35,
    // SD_TYPE_IQ4_NL_4_4 = 36,
    // SD_TYPE_IQ4_NL_4_8 = 37,
    // SD_TYPE_IQ4_NL_8_8 = 38,
    SD_TYPE_COUNT = 39,
};

SD_API const char* sd_type_name(enum sd_type_t type);

enum sd_log_level_t {
    SD_LOG_DEBUG,
    SD_LOG_INFO,
    SD_LOG_WARN,
    SD_LOG_ERROR
};

enum sd_preview_t {
    SD_PREVIEW_NONE,
    SD_PREVIEW_PROJ,
    SD_PREVIEW_TAE,
    SD_PREVIEW_VAE,
    N_PREVIEWS
};

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t channel;
    uint8_t* data;
} sd_image_t;

typedef void (*sd_log_cb_t)(enum sd_log_level_t level, const char* text, void* data);
typedef void (*sd_progress_cb_t)(int step, int steps, float time, void* data);
typedef void (*sd_preview_cb_t)(int, sd_image_t);
typedef bool (*sd_graph_eval_callback_t)(struct ggml_tensor* t, bool ask, void* user_data);

SD_API void sd_set_log_callback(sd_log_cb_t sd_log_cb, void* data);
SD_API void sd_set_progress_callback(sd_progress_cb_t cb, void* data);
SD_API void sd_set_preview_callback(sd_preview_cb_t cb, sd_preview_t mode, int interval);
SD_API void sd_set_backend_eval_callback(sd_graph_eval_callback_t cb, void* data);
SD_API int32_t get_num_physical_cores();
SD_API const char* sd_get_system_info();

typedef struct sd_ctx_t sd_ctx_t;

typedef struct {
    float eta;
    float momentum;
    float norm_treshold;
    float norm_treshold_smoothing;
} sd_apg_params_t;

typedef struct sd_slg_params_t {
    int* layers;
    size_t layer_count;
    float layer_start;
    float layer_end;
    float scale;
    bool slg_uncond;
} sd_slg_params_t;

typedef struct sd_guidance_params_t {
    float txt_cfg;
    float img_cfg;
    float min_cfg;
    float distilled_guidance;
    sd_slg_params_t slg;
    sd_apg_params_t apg;
} sd_guidance_params_t;

SD_API sd_ctx_t* new_sd_ctx(const char* model_path,
                            const char* clip_l_path,
                            const char* clip_g_path,
                            const char* t5xxl_path,
                            const char* diffusion_model_path,
                            const char* vae_path,
                            const char* taesd_path,
                            const char* control_net_path_c_str,
                            const char* lora_model_dir,
                            const char* embed_dir_c_str,
                            const char* stacked_id_embed_dir_c_str,
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
                            bool tae_preview_only);

SD_API void free_sd_ctx(sd_ctx_t* sd_ctx);

SD_API sd_image_t* txt2img(sd_ctx_t* sd_ctx,
                           const char* prompt,
                           const char* negative_prompt,
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
                           float style_strength,
                           bool normalize_input,
                           const char* input_id_images_path);

SD_API sd_image_t* img2img(sd_ctx_t* sd_ctx,
                           sd_image_t init_image,
                           sd_image_t mask_image,
                           const char* prompt,
                           const char* negative_prompt,
                           int clip_skip,
                           sd_guidance_params_t guidance,
                           float eta,
                           int width,
                           int height,
                           enum sample_method_t sample_method,
                           int sample_steps,
                           float strength,
                           int64_t seed,
                           int batch_count,
                           const sd_image_t* control_cond,
                           float control_strength,
                           float style_strength,
                           bool normalize_input,
                           const char* input_id_images_path);

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
                           int64_t seed);

SD_API sd_image_t* edit(sd_ctx_t* sd_ctx,
                        sd_image_t* ref_images,
                        int ref_images_count,
                        const char* prompt,
                        const char* negative_prompt,
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
                        float style_strength,
                        bool normalize_input,
                        const char* input_id_images_path);

typedef struct upscaler_ctx_t upscaler_ctx_t;

SD_API upscaler_ctx_t* new_upscaler_ctx(const char* esrgan_path,
                                        int n_threads);
SD_API void free_upscaler_ctx(upscaler_ctx_t* upscaler_ctx);

SD_API sd_image_t upscale(upscaler_ctx_t* upscaler_ctx, sd_image_t input_image, uint32_t upscale_factor);

SD_API bool convert(const char* model_path, const char* clip_l_path, const char* clip_g_path, const char* t5xxl_path, const char* diffusion_model_path, const char* vae_path, const char* output_path, enum sd_type_t output_type);

SD_API uint8_t* preprocess_canny(uint8_t* img,
                                 int width,
                                 int height,
                                 float high_threshold,
                                 float low_threshold,
                                 float weak,
                                 float strong,
                                 bool inverse);

SD_API bool loadImatrix(const char* imatrix_path);
SD_API void saveImatrix(const char* imatrix_path);
SD_API void enableImatrixCollection();
SD_API void disableImatrixCollection();

#ifdef __cplusplus
}
#endif

#endif  // __STABLE_DIFFUSION_H__
