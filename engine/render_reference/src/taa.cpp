#include "saige/render_reference/taa.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::render_reference {

double halton(unsigned index, unsigned base)
{
    if (index == 0 || base < 2) throw std::invalid_argument("halton: the index must be >= 1 and the base >= 2");
    double f = 1.0, r = 0.0;
    unsigned i = index;
    while (i > 0) {
        f /= static_cast<double>(base);
        r += f * static_cast<double>(i % base);
        i /= base;
    }
    return r;
}

std::array<double, 2> jitter_offset(unsigned frame, unsigned length)
{
    if (length == 0 || length > 1024) throw std::invalid_argument("jitter_offset: the sequence length must be 1 to 1024");
    const unsigned i = frame % length + 1;
    return {halton(i, 2) - 0.5, halton(i, 3) - 0.5};
}

std::array<double, 2> jitter_ndc(const std::array<double, 2>& j, unsigned width, unsigned height)
{
    if (width == 0 || height == 0) throw std::invalid_argument("jitter_ndc: the target must not be empty");
    return {2.0 * j[0] / static_cast<double>(width), -2.0 * j[1] / static_cast<double>(height)};
}

double texture_lod_bias(unsigned render_width, unsigned output_width)
{
    if (render_width == 0 || output_width == 0) throw std::invalid_argument("texture_lod_bias: the widths must be non-zero");
    return std::log2(static_cast<double>(render_width) / static_cast<double>(output_width));
}

Image::Image(unsigned w, unsigned h) : width(w), height(h), rgb(static_cast<std::size_t>(w) * h * 3, 0.0f) {}
std::array<float, 3> Image::at(unsigned x, unsigned y) const
{
    const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 3;
    return {rgb[i], rgb[i + 1], rgb[i + 2]};
}
void Image::set(unsigned x, unsigned y, const std::array<float, 3>& c)
{
    const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 3;
    rgb[i] = c[0];
    rgb[i + 1] = c[1];
    rgb[i + 2] = c[2];
}

MotionField::MotionField(unsigned w, unsigned h, float mx, float my) : width(w), height(h), mv(static_cast<std::size_t>(w) * h * 2)
{
    for (std::size_t i = 0; i < mv.size(); i += 2) {
        mv[i] = mx;
        mv[i + 1] = my;
    }
}
std::array<float, 2> MotionField::at(unsigned x, unsigned y) const
{
    const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 2;
    return {mv[i], mv[i + 1]};
}
void MotionField::set(unsigned x, unsigned y, float mx, float my)
{
    const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 2;
    mv[i] = mx;
    mv[i + 1] = my;
}

std::array<float, 3> to_ycocg(const std::array<float, 3>& c)
{
    return {0.25f * c[0] + 0.5f * c[1] + 0.25f * c[2], 0.5f * c[0] - 0.5f * c[2], -0.25f * c[0] + 0.5f * c[1] - 0.25f * c[2]};
}
std::array<float, 3> from_ycocg(const std::array<float, 3>& c)
{
    return {c[0] + c[1] - c[2], c[0] + c[2], c[0] - c[1] - c[2]};
}

namespace {

/// Bilinear sample of `img` at continuous pixel coordinates (centres at +0.5), clamped to the edge.
std::array<float, 3> bilinear(const Image& img, double px, double py)
{
    const double x = std::clamp(px - 0.5, 0.0, static_cast<double>(img.width - 1));
    const double y = std::clamp(py - 0.5, 0.0, static_cast<double>(img.height - 1));
    const unsigned x0 = static_cast<unsigned>(x), y0 = static_cast<unsigned>(y);
    const unsigned x1 = std::min(x0 + 1, img.width - 1), y1 = std::min(y0 + 1, img.height - 1);
    const float fx = static_cast<float>(x - x0), fy = static_cast<float>(y - y0);
    const auto a = img.at(x0, y0), b = img.at(x1, y0), c = img.at(x0, y1), d = img.at(x1, y1);
    std::array<float, 3> out{};
    for (std::size_t k = 0; k < 3; ++k) out[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
    return out;
}

} // namespace

Image taa_resolve(const Image& current, const Image& history, const MotionField& motion, const TaaParams& p, bool history_valid)
{
    if (current.width == 0 || current.height == 0 || current.rgb.size() != static_cast<std::size_t>(current.width) * current.height * 3) throw std::invalid_argument("taa_resolve: the current image is malformed");
    if (history.width != current.width || history.height != current.height || history.rgb.size() != current.rgb.size() || motion.width != current.width || motion.height != current.height ||
        motion.mv.size() != static_cast<std::size_t>(current.width) * current.height * 2)
        throw std::invalid_argument("taa_resolve: the history and the motion field must match the current image's size");
    if (!(p.blend > 0.0f && p.blend <= 1.0f) || !(p.motion_blend >= 0.0f) || !(p.gamma > 0.0f)) throw std::invalid_argument("taa_resolve: the blend must be in (0, 1], the motion blend >= 0 and the gamma positive");
    if (!history_valid) return current;

    Image out(current.width, current.height);
    for (unsigned y = 0; y < current.height; ++y) {
        for (unsigned x = 0; x < current.width; ++x) {
            const auto cur = current.at(x, y);
            const auto mv = motion.at(x, y);
            const double hx = static_cast<double>(x) + 0.5 + static_cast<double>(mv[0]);
            const double hy = static_cast<double>(y) + 0.5 + static_cast<double>(mv[1]);
            if (hx < 0.0 || hy < 0.0 || hx > static_cast<double>(current.width) || hy > static_cast<double>(current.height)) {
                out.set(x, y, cur);  // the surface was off-screen last frame: nothing to reuse
                continue;
            }
            std::array<float, 3> hist = to_ycocg(bilinear(history, hx, hy));
            if (p.clip_history) {
                std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f}, sum{}, sum2{};
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const unsigned nx = static_cast<unsigned>(std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(current.width) - 1));
                        const unsigned ny = static_cast<unsigned>(std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(current.height) - 1));
                        const auto s = to_ycocg(current.at(nx, ny));
                        for (std::size_t k = 0; k < 3; ++k) {
                            lo[k] = std::min(lo[k], s[k]);
                            hi[k] = std::max(hi[k], s[k]);
                            sum[k] += s[k];
                            sum2[k] += s[k] * s[k];
                        }
                    }
                }
                std::array<float, 3> centre{}, extent{};
                for (std::size_t k = 0; k < 3; ++k) {
                    const float mean = sum[k] / 9.0f;
                    const float var = std::max(sum2[k] / 9.0f - mean * mean, 0.0f);
                    const float sigma = std::sqrt(var);
                    const float b_lo = std::max(lo[k], mean - p.gamma * sigma), b_hi = std::min(hi[k], mean + p.gamma * sigma);
                    centre[k] = 0.5f * (b_lo + b_hi);
                    extent[k] = 0.5f * (b_hi - b_lo);
                }
                // Clip along the line to the box centre: scale the offset by the largest per-axis ratio that exceeds the box.
                float t = 1.0f;
                for (std::size_t k = 0; k < 3; ++k) {
                    const float off = std::abs(hist[k] - centre[k]);
                    if (off > extent[k]) t = std::max(t, extent[k] > 1e-9f ? off / extent[k] : 1e30f);
                }
                if (t > 1.0f) {
                    if (t >= 1e29f) {
                        hist = centre;  // a zero-width box: the neighbourhood is flat and the history disagrees
                    } else {
                        for (std::size_t k = 0; k < 3; ++k) hist[k] = centre[k] + (hist[k] - centre[k]) / t;
                    }
                }
            }
            const float speed = std::hypot(mv[0], mv[1]);
            const float alpha = std::min(1.0f, p.blend + p.motion_blend * speed);
            const auto cur_y = to_ycocg(cur);
            std::array<float, 3> mixed{};
            for (std::size_t k = 0; k < 3; ++k) mixed[k] = hist[k] + (cur_y[k] - hist[k]) * alpha;
            out.set(x, y, from_ycocg(mixed));
        }
    }
    return out;
}

} // namespace saige::render_reference
