# SSR Neural Reconstruction Research Report (Build 002)

## 1. Industry Landscape & Publicly Documented Benchmarks
- **[NVIDIA DLSS 4.5](https://developer.nvidia.com/blog/nvidia-dlss-4-5-delivers-super-resolution-upgrades-and-new-dynamic-multi-frame-generation/):** Uses a 2nd-generation Vision Transformer architecture for Super Resolution with 5x more compute than DLSS 4, improving context awareness, pixel sampling, motion vector handling, and temporal stability across challenging lighting and disocclusions.
- **[AMD FSR 4.1](https://www.thelec.net/news/articleView.html?idxno=14816):** Employs INT8 ML-accelerated temporal upscaling with Vulkan support across Radeon RX 9000/7000/6000 GPUs and Ryzen AI APUs.
- **[Intel XeSS-SR](https://www.intel.com/content/www/us/en/developer/topic-technology/gamedev/xess.html):** Dual-path ML temporal super resolution using XMX matrix engines on Intel Arc/Iris Xe GPUs and a Shader Model 6.4+ DP4a cross-vendor fallback.
- **[PlayStation PSSR / QSSR](https://blog.playstation.com/2026/10/01/ai-upscaling-is-coming-to-ps5/):** Project Amethyst ML upscaler spanning PS5 Pro (PSSR 2.0) and a streamlined base-PS5 QSSR variant.

## 2. Comparative Architecture Analysis for SSR 0.3+
| Architecture | Temporal Fidelity | Ghosting / Disocclusion | Compute Cost (1080p->4K) | Vulkan 1.4 Suitability |
| :--- | :--- | :--- | :--- | :--- |
| **Analytical Heuristic (SSR 0.2)** | High on static/linear motion; limited on subpixel foliage | Explicit depth + variance clipping | Very Low (`< 0.5 ms` GPU est.) | Native GLSL 460 compute |
| **Lightweight U-Net CNN (INT8/FP16)** | Strong spatial-temporal kernel prediction (KPCN) | Learns soft disocclusion weights | Low-Moderate (`~1.2–2.0 ms`) | `VK_KHR_cooperative_matrix` / DP4a |
| **Recurrent Feature-Grid + ConvGRU** | Very high (carries 4–8ch latent state across frames) | Resistant to single-frame noise | Moderate (`~1.5–2.5 ms`) | Ping-pong `RGBA16F` feature textures |
| **Windowed Transformer (DLSS 4.5 style)** | Highest (non-local spatial-temporal attention) | Best handling of thin/reflective disocclusions | High (`> 3.5 ms` without Tensor Cores) | Requires FP16/INT8 `VK_KHR_cooperative_matrix` |

## 3. Recommended SSR 0.3 Neural Roadmap (Zero Paid APIs / Open Weights)
1. **Hybrid Analytical + Lightweight Recurrent Kernel-Predicting CNN (KPCN):** Keep SSR 0.2's deterministic Catmull-Rom + YCoCg variance clipping as the fallback/guardrail, and train a compact 4-level depthwise-separable encoder-decoder (~65K parameters) predicting per-pixel 3x3 spatial-temporal reconstruction kernels and blend alpha.
2. **Quantized Vulkan 1.4 Inference Path:** Target FP16 (`GL_EXT_shader_explicit_arithmetic_types_float16`) and INT8 DP4a (`GL_EXT_integer_dot_product`) compute shaders with optional `VK_KHR_cooperative_matrix` acceleration on supported hardware.
3. **Training & Dataset Plan:** Generate synthetic ground-truth sequences (8x8 supersampled reference + jittered low-res + depth + motion vectors + reactive/transparency masks) using SAIGE's own offline renderer and open-source PBR scenes; train with L1 + perceptual (LPIPS/SSIM) + temporal gradient loss using PyTorch on local hardware.
