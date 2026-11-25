#include "saige/gfx_assets/gltf.hpp"

#include "saige/gfx_assets/animation.hpp"

#include "saige/gfx_assets/json.hpp"
#include "saige/gfx_assets/resource_uri.hpp"
#include "saige/gfx_assets/tangents.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string_view>

namespace saige::gfx {

using render::Float3;
using render::Mat4;

void Aabb::expand(Float3 p) noexcept
{
    min = {std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
    max = {std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
}

void Aabb::expand(const Aabb& other) noexcept
{
    if (other.valid()) {
        expand(other.min);
        expand(other.max);
    }
}

namespace {

struct GltfError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string& message)
{
    throw GltfError(message);
}

constexpr uint32_t component_byte = 5120;
constexpr uint32_t component_ubyte = 5121;
constexpr uint32_t component_short = 5122;
constexpr uint32_t component_ushort = 5123;
constexpr uint32_t component_uint = 5125;
constexpr uint32_t component_float = 5126;

constexpr uint32_t glb_magic = 0x46546C67;       // "glTF"
constexpr uint32_t glb_chunk_json = 0x4E4F534A;  // "JSON"
constexpr uint32_t glb_chunk_bin = 0x004E4942;   // "BIN\0"

uint32_t read_u32_le(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

std::size_t component_size(uint32_t type)
{
    switch (type) {
    case component_byte:
    case component_ubyte: return 1;
    case component_short:
    case component_ushort: return 2;
    case component_uint:
    case component_float: return 4;
    default: return 0;
    }
}

int component_count(std::string_view type)
{
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    return 0;
}

struct BufferData {
    std::vector<uint8_t> bytes;
    std::size_t length = 0;  // declared byteLength; bytes.size() >= length
};

struct BufferView {
    std::size_t buffer = 0;
    std::size_t offset = 0;
    std::size_t length = 0;
    std::size_t stride = 0;  // 0 = tightly packed
};

struct Accessor {
    std::size_t view = 0;
    std::size_t offset = 0;
    uint32_t component_type = 0;
    bool normalized = false;
    std::size_t count = 0;
    int components = 0;
};

struct NodeInfo {
    std::string name;
    int64_t mesh = -1;
    int64_t skin = -1;
    bool decomposable = true;  ///< the local transform is a translation, rotation and scale (always, unless a `matrix` has shear or a negative scale)
    render::Transform trs;     ///< glTF space (not yet converted to engine space)
    std::vector<std::size_t> children;
    Mat4 local = Mat4::identity();
};

class Loader {
public:
    Loader(std::span<const uint8_t> bytes, const ResourceReader& reader, const GltfLimits& limits)
        : bytes_(bytes), reader_(reader), limits_(limits)
    {
    }

    GltfAsset run()
    {
        if (bytes_.size() > limits_.max_file_bytes) {
            fail("the file is " + std::to_string(bytes_.size()) + " bytes, over the limit of " +
                 std::to_string(limits_.max_file_bytes));
        }
        split_container();
        parse_document();
        check_asset_and_extensions();
        load_buffers();
        load_buffer_views();
        load_accessors();
        load_samplers();
        load_images();
        load_textures();
        load_materials();
        load_meshes();
        generate_missing_tangents();
        load_nodes();
        load_skins();
        load_animations();
        build_instances();
        asset_.warnings.assign(warnings_.begin(), warnings_.end());
        return std::move(asset_);
    }

private:
    // ----------------------------------------------------------------------------------------------- container
    void split_container()
    {
        if (bytes_.size() >= 4 && read_u32_le(bytes_.data()) == glb_magic) {
            if (bytes_.size() < 12) {
                fail("GLB: truncated header");
            }
            const uint32_t version = read_u32_le(bytes_.data() + 4);
            const uint32_t total = read_u32_le(bytes_.data() + 8);
            if (version != 2) {
                fail("GLB: unsupported container version " + std::to_string(version) + " (expected 2)");
            }
            if (total < 12 || total > bytes_.size()) {
                fail("GLB: the header says " + std::to_string(total) + " bytes but the file has " +
                     std::to_string(bytes_.size()));
            }
            std::size_t pos = 12;
            bool first = true;
            while (pos < total) {
                if (total - pos < 8) {
                    fail("GLB: truncated chunk header");
                }
                const std::size_t chunk_length = read_u32_le(bytes_.data() + pos);
                const uint32_t chunk_type = read_u32_le(bytes_.data() + pos + 4);
                pos += 8;
                if (chunk_length > total - pos) {
                    fail("GLB: a chunk extends past the end of the file");
                }
                if (chunk_length % 4 != 0) {
                    fail("GLB: chunk length is not a multiple of 4");
                }
                const std::span<const uint8_t> data = bytes_.subspan(pos, chunk_length);
                if (first) {
                    if (chunk_type != glb_chunk_json) {
                        fail("GLB: the first chunk must be JSON");
                    }
                    json_text_ = std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
                    first = false;
                } else if (chunk_type == glb_chunk_bin && !have_bin_) {
                    glb_bin_ = data;
                    have_bin_ = true;
                }  // other or repeated chunks are ignored, as the specification requires
                pos += chunk_length;
            }
            if (first) {
                fail("GLB: no JSON chunk");
            }
        } else {
            json_text_ = std::string_view(reinterpret_cast<const char*>(bytes_.data()), bytes_.size());
        }
    }

    void parse_document()
    {
        JsonParseResult parsed = parse_json(json_text_);
        if (!parsed.ok()) {
            fail("invalid JSON: " + parsed.error);
        }
        root_ = std::move(*parsed.value);
        if (!root_.is_object()) {
            fail("the document root must be a JSON object");
        }
    }

    void warn(const std::string& message) { warnings_.insert(message); }

    void check_asset_and_extensions()
    {
        const JsonValue& asset = root_["asset"];
        if (!asset.is_object()) {
            fail("missing required 'asset' object");
        }
        const std::string& version = asset["version"].as_string();
        if (version.empty()) {
            fail("asset.version is missing");
        }
        if (version != "2.0" && !(version.size() > 2 && version[0] == '2' && version[1] == '.')) {
            fail("unsupported glTF version '" + version + "' (this importer reads 2.x)");
        }
        const std::string& min_version = asset["minVersion"].as_string();
        if (!min_version.empty() && min_version != "2.0") {
            fail("asset.minVersion '" + min_version + "' is newer than 2.0");
        }
        static const std::set<std::string> supported = {"KHR_materials_emissive_strength"};
        const JsonValue& required = root_["extensionsRequired"];
        for (std::size_t i = 0; i < required.size(); ++i) {
            const std::string& name = required.at(i).as_string();
            if (supported.count(name) == 0) {
                fail("required extension '" + name + "' is not supported");
            }
        }
        const JsonValue& used = root_["extensionsUsed"];
        for (std::size_t i = 0; i < used.size(); ++i) {
            const std::string& name = used.at(i).as_string();
            if (supported.count(name) == 0) {
                warn("extension '" + name + "' is not supported and was ignored");
            }
        }
    }

    // ----------------------------------------------------------------------------------------------- buffers
    void load_buffers()
    {
        const JsonValue& buffers = root_["buffers"];
        std::size_t total = 0;
        for (std::size_t i = 0; i < buffers.size(); ++i) {
            const JsonValue& b = buffers.at(i);
            const std::string label = "buffer " + std::to_string(i);
            const int64_t length = b["byteLength"].as_index();
            if (length <= 0) {
                fail(label + ": byteLength must be a positive integer");
            }
            if (static_cast<std::size_t>(length) > limits_.max_buffer_bytes) {
                fail(label + ": byteLength " + std::to_string(length) + " is over the limit of " +
                     std::to_string(limits_.max_buffer_bytes));
            }
            total += static_cast<std::size_t>(length);
            if (total > limits_.max_total_buffer_bytes) {
                fail("buffers together exceed the limit of " + std::to_string(limits_.max_total_buffer_bytes) + " bytes");
            }
            BufferData data;
            data.length = static_cast<std::size_t>(length);
            const JsonValue* uri = b.find("uri");
            if (uri == nullptr) {
                if (i != 0 || !have_bin_) {
                    fail(label + ": no uri (only buffer 0 of a GLB may use the BIN chunk)");
                }
                data.bytes.assign(glb_bin_.begin(), glb_bin_.end());
            } else {
                data.bytes = read_resource(uri->as_string(), label, nullptr);
            }
            if (data.bytes.size() < data.length) {
                fail(label + ": the data is " + std::to_string(data.bytes.size()) + " bytes but byteLength says " +
                     std::to_string(data.length));
            }
            buffers_.push_back(std::move(data));
        }
    }

    /// Resolves a URI to bytes: a data URI, or a validated relative path read through the reader.
    std::vector<uint8_t> read_resource(const std::string& uri, const std::string& label, std::string* mime_out)
    {
        if (is_data_uri(uri)) {
            std::optional<DataUri> decoded = decode_data_uri(uri);
            if (!decoded) {
                fail(label + ": malformed data URI (only base64 data URIs are supported)");
            }
            if (mime_out != nullptr) {
                *mime_out = decoded->mime_type;
            }
            return std::move(decoded->bytes);
        }
        const ResolvedUri resolved = resolve_relative_uri(uri);
        if (!resolved.ok) {
            fail(label + ": resource URI '" + uri + "' rejected: " + resolved.error);
        }
        if (!reader_) {
            fail(label + ": external resource '" + resolved.path + "' is not available (no resource reader)");
        }
        std::optional<std::vector<uint8_t>> bytes = reader_(resolved.path);
        if (!bytes) {
            fail(label + ": cannot read resource '" + resolved.path + "'");
        }
        if (bytes->size() > limits_.max_buffer_bytes) {
            fail(label + ": resource '" + resolved.path + "' is over the size limit");
        }
        return std::move(*bytes);
    }

    void load_buffer_views()
    {
        const JsonValue& views = root_["bufferViews"];
        for (std::size_t i = 0; i < views.size(); ++i) {
            const JsonValue& v = views.at(i);
            const std::string label = "bufferView " + std::to_string(i);
            const int64_t buffer = v["buffer"].as_index();
            if (buffer < 0 || static_cast<std::size_t>(buffer) >= buffers_.size()) {
                fail(label + ": invalid buffer index");
            }
            const int64_t offset = v["byteOffset"].is_null() ? 0 : v["byteOffset"].as_index();
            const int64_t length = v["byteLength"].as_index();
            if (offset < 0 || length <= 0) {
                fail(label + ": invalid byteOffset/byteLength");
            }
            const BufferData& data = buffers_[static_cast<std::size_t>(buffer)];
            if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(length) > data.length) {
                fail(label + ": extends past the end of its buffer");
            }
            BufferView view;
            view.buffer = static_cast<std::size_t>(buffer);
            view.offset = static_cast<std::size_t>(offset);
            view.length = static_cast<std::size_t>(length);
            if (!v["byteStride"].is_null()) {
                const int64_t stride = v["byteStride"].as_index();
                if (stride < 4 || stride > 252 || stride % 4 != 0) {
                    fail(label + ": byteStride must be a multiple of 4 between 4 and 252");
                }
                view.stride = static_cast<std::size_t>(stride);
            }
            views_.push_back(view);
        }
    }

    void load_accessors()
    {
        const JsonValue& accessors = root_["accessors"];
        for (std::size_t i = 0; i < accessors.size(); ++i) {
            const JsonValue& a = accessors.at(i);
            const std::string label = "accessor " + std::to_string(i);
            if (a.find("sparse") != nullptr) {
                fail(label + ": sparse accessors are not supported");
            }
            Accessor acc;
            acc.component_type = static_cast<uint32_t>(std::max<int64_t>(a["componentType"].as_index(), 0));
            const std::size_t csize = component_size(acc.component_type);
            if (csize == 0) {
                fail(label + ": invalid componentType");
            }
            acc.components = component_count(a["type"].as_string());
            if (acc.components == 0) {
                fail(label + ": invalid type '" + a["type"].as_string() + "'");
            }
            acc.normalized = a["normalized"].as_bool(false);
            const int64_t count = a["count"].as_index();
            if (count <= 0) {
                fail(label + ": count must be a positive integer");
            }
            if (static_cast<std::size_t>(count) > limits_.max_accessor_count) {
                fail(label + ": count " + std::to_string(count) + " is over the limit of " +
                     std::to_string(limits_.max_accessor_count));
            }
            acc.count = static_cast<std::size_t>(count);
            const int64_t view_index = a["bufferView"].as_index();
            if (a["bufferView"].is_null()) {
                fail(label + ": accessors without a bufferView (zero-filled) are not supported");
            }
            if (view_index < 0 || static_cast<std::size_t>(view_index) >= views_.size()) {
                fail(label + ": invalid bufferView index");
            }
            acc.view = static_cast<std::size_t>(view_index);
            const int64_t offset = a["byteOffset"].is_null() ? 0 : a["byteOffset"].as_index();
            if (offset < 0) {
                fail(label + ": invalid byteOffset");
            }
            acc.offset = static_cast<std::size_t>(offset);

            const BufferView& view = views_[acc.view];
            const std::size_t element = csize * static_cast<std::size_t>(acc.components);
            const std::size_t stride = view.stride != 0 ? view.stride : element;
            if (stride < element) {
                fail(label + ": the bufferView byteStride is smaller than one element");
            }
            // The last element ends at offset + (count - 1) * stride + element. count is bounded by max_accessor_count
            // and stride by 252, so none of this can overflow 64 bits.
            const uint64_t end = static_cast<uint64_t>(acc.offset) + (static_cast<uint64_t>(acc.count) - 1) * stride + element;
            if (end > view.length) {
                fail(label + ": reads past the end of its bufferView");
            }
            accessors_.push_back(acc);
        }
    }

    const Accessor& accessor(int64_t index, const std::string& what) const
    {
        if (index < 0 || static_cast<std::size_t>(index) >= accessors_.size()) {
            fail(what + ": invalid accessor index");
        }
        return accessors_[static_cast<std::size_t>(index)];
    }

    /// Pointer to element `i` of an accessor (bounds were validated when the accessor was loaded).
    const uint8_t* element(const Accessor& acc, std::size_t i) const
    {
        const BufferView& view = views_[acc.view];
        const std::size_t element_size = component_size(acc.component_type) * static_cast<std::size_t>(acc.components);
        const std::size_t stride = view.stride != 0 ? view.stride : element_size;
        return buffers_[view.buffer].bytes.data() + view.offset + acc.offset + i * stride;
    }

    static float read_float_le(const uint8_t* p)
    {
        return std::bit_cast<float>(read_u32_le(p));
    }

    /// Reads `acc` as floats. FLOAT is read directly; BYTE/UBYTE/SHORT/USHORT are accepted only when normalized.
    void read_floats(const Accessor& acc, int expected_components, bool allow_normalized, const std::string& what,
                     std::vector<float>& out) const
    {
        if (acc.components != expected_components) {
            fail(what + ": expected " + std::to_string(expected_components) + " components per element");
        }
        const bool is_float = acc.component_type == component_float;
        if (!is_float && !(allow_normalized && acc.normalized && acc.component_type != component_uint)) {
            fail(what + ": must be FLOAT" + std::string(allow_normalized ? " or a normalized 8/16-bit integer" : ""));
        }
        const std::size_t csize = component_size(acc.component_type);
        out.resize(acc.count * static_cast<std::size_t>(expected_components));
        for (std::size_t i = 0; i < acc.count; ++i) {
            const uint8_t* p = element(acc, i);
            for (int c = 0; c < expected_components; ++c) {
                const uint8_t* q = p + static_cast<std::size_t>(c) * csize;
                float v = 0.0f;
                switch (acc.component_type) {
                case component_float:
                    v = read_float_le(q);
                    if (!std::isfinite(v)) {
                        fail(what + ": contains a non-finite value");
                    }
                    break;
                case component_ubyte: v = static_cast<float>(q[0]) / 255.0f; break;
                case component_byte: v = std::max(static_cast<float>(static_cast<int8_t>(q[0])) / 127.0f, -1.0f); break;
                case component_ushort: v = static_cast<float>(q[0] | (q[1] << 8)) / 65535.0f; break;
                case component_short:
                    v = std::max(static_cast<float>(static_cast<int16_t>(q[0] | (q[1] << 8))) / 32767.0f, -1.0f);
                    break;
                default: fail(what + ": unsupported component type");
                }
                out[i * static_cast<std::size_t>(expected_components) + static_cast<std::size_t>(c)] = v;
            }
        }
    }

    void read_indices(const Accessor& acc, const std::string& what, std::vector<uint32_t>& out) const
    {
        if (acc.components != 1) {
            fail(what + ": indices must be SCALAR");
        }
        if (acc.component_type != component_ubyte && acc.component_type != component_ushort &&
            acc.component_type != component_uint) {
            fail(what + ": indices must be UNSIGNED_BYTE, UNSIGNED_SHORT or UNSIGNED_INT");
        }
        out.resize(acc.count);
        for (std::size_t i = 0; i < acc.count; ++i) {
            const uint8_t* p = element(acc, i);
            out[i] = acc.component_type == component_ubyte    ? p[0]
                     : acc.component_type == component_ushort ? static_cast<uint32_t>(p[0] | (p[1] << 8))
                                                              : read_u32_le(p);
        }
    }

    /// JOINTS_0: VEC4 of UNSIGNED_BYTE or UNSIGNED_SHORT (never normalized), one joint index per influence.
    void read_joints(const Accessor& acc, std::size_t expected_count, const std::string& what, std::vector<uint16_t>& out) const
    {
        if (acc.count != expected_count) {
            fail(what + ": has " + std::to_string(acc.count) + " elements but POSITION has " + std::to_string(expected_count));
        }
        if (acc.components != 4 || acc.normalized || (acc.component_type != component_ubyte && acc.component_type != component_ushort)) {
            fail(what + ": must be a VEC4 of UNSIGNED_BYTE or UNSIGNED_SHORT, not normalized");
        }
        const std::size_t csize = component_size(acc.component_type);
        out.resize(acc.count * 4);
        for (std::size_t i = 0; i < acc.count; ++i) {
            const uint8_t* p = element(acc, i);
            for (std::size_t c = 0; c < 4; ++c) {
                const uint8_t* q = p + c * csize;
                out[i * 4 + c] = acc.component_type == component_ubyte ? q[0] : static_cast<uint16_t>(q[0] | (q[1] << 8));
            }
        }
    }

    // ----------------------------------------------------------------------------------------------- images etc.
    void load_samplers()
    {
        const JsonValue& samplers = root_["samplers"];
        for (std::size_t i = 0; i < samplers.size(); ++i) {
            const JsonValue& s = samplers.at(i);
            Sampler out;
            const auto take = [&s](const char* key, uint32_t fallback) {
                const int64_t v = s[key].as_index();
                return v >= 0 ? static_cast<uint32_t>(v) : fallback;
            };
            out.mag_filter = take("magFilter", 0);
            out.min_filter = take("minFilter", 0);
            out.wrap_s = take("wrapS", 10497);
            out.wrap_t = take("wrapT", 10497);
            asset_.samplers.push_back(out);
        }
    }

    static std::string guess_mime(const std::string& path)
    {
        std::string ext = path.substr(path.find_last_of('.') == std::string::npos ? path.size() : path.find_last_of('.'));
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".png") return "image/png";
        if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
        return {};
    }

    void load_images()
    {
        const JsonValue& images = root_["images"];
        for (std::size_t i = 0; i < images.size(); ++i) {
            const JsonValue& im = images.at(i);
            const std::string label = "image " + std::to_string(i);
            Image out;
            out.name = im["name"].as_string();
            out.mime_type = im["mimeType"].as_string();
            const JsonValue* uri = im.find("uri");
            if (uri != nullptr) {
                std::string mime;
                out.bytes = read_resource(uri->as_string(), label, &mime);
                if (out.mime_type.empty()) {
                    out.mime_type = mime;
                }
                if (!is_data_uri(uri->as_string())) {
                    out.path = resolve_relative_uri(uri->as_string()).path;
                    if (out.mime_type.empty()) {
                        out.mime_type = guess_mime(out.path);
                    }
                }
            } else {
                const int64_t view_index = im["bufferView"].as_index();
                if (view_index < 0 || static_cast<std::size_t>(view_index) >= views_.size()) {
                    fail(label + ": needs a uri or a valid bufferView");
                }
                if (out.mime_type.empty()) {
                    fail(label + ": an image in a bufferView needs a mimeType");
                }
                const BufferView& view = views_[static_cast<std::size_t>(view_index)];
                const uint8_t* base = buffers_[view.buffer].bytes.data() + view.offset;
                out.bytes.assign(base, base + view.length);
            }
            if (out.bytes.empty()) {
                fail(label + ": is empty");
            }
            asset_.images.push_back(std::move(out));
        }
    }

    void load_textures()
    {
        const JsonValue& textures = root_["textures"];
        for (std::size_t i = 0; i < textures.size(); ++i) {
            const JsonValue& t = textures.at(i);
            const std::string label = "texture " + std::to_string(i);
            const int64_t source = t["source"].as_index();
            if (source < 0 || static_cast<std::size_t>(source) >= asset_.images.size()) {
                fail(label + ": needs a valid 'source' image (extension-defined sources are not supported)");
            }
            Texture out;
            out.image = static_cast<int32_t>(source);
            if (!t["sampler"].is_null()) {
                const int64_t sampler = t["sampler"].as_index();
                if (sampler < 0 || static_cast<std::size_t>(sampler) >= asset_.samplers.size()) {
                    fail(label + ": invalid sampler index");
                }
                out.sampler = static_cast<int32_t>(sampler);
            }
            asset_.textures.push_back(out);
        }
    }

    // ----------------------------------------------------------------------------------------------- materials
    TextureRef texture_ref(const JsonValue& v, const std::string& what)
    {
        TextureRef ref;
        if (v.is_null()) {
            return ref;
        }
        const int64_t index = v["index"].as_index();
        if (index < 0 || static_cast<std::size_t>(index) >= asset_.textures.size()) {
            fail(what + ": invalid texture index");
        }
        ref.texture = static_cast<int32_t>(index);
        const int64_t tex_coord = v["texCoord"].is_null() ? 0 : v["texCoord"].as_index();
        ref.tex_coord = tex_coord < 0 ? 0u : static_cast<uint32_t>(tex_coord);
        if (ref.tex_coord != 0) {
            warn(what + ": texCoord " + std::to_string(ref.tex_coord) + " is kept in the data but the renderer samples TEXCOORD_0 only");
        }
        return ref;
    }

    void read_factor(const JsonValue& v, std::size_t n, float* out, const std::string& what, float lo, float hi)
    {
        if (v.is_null()) {
            return;
        }
        if (!v.is_array() || v.size() != n) {
            fail(what + ": expected an array of " + std::to_string(n) + " numbers");
        }
        for (std::size_t i = 0; i < n; ++i) {
            const double d = v.at(i).as_number(std::nan(""));
            if (!std::isfinite(d)) {
                fail(what + ": contains a non-numeric or non-finite value");
            }
            out[i] = std::clamp(static_cast<float>(d), lo, hi);
        }
    }

    void load_materials()
    {
        const JsonValue& materials = root_["materials"];
        for (std::size_t i = 0; i < materials.size(); ++i) {
            const JsonValue& m = materials.at(i);
            const std::string label = "material " + std::to_string(i);
            Material out;
            out.name = m["name"].as_string();
            const JsonValue& pbr = m["pbrMetallicRoughness"];
            read_factor(pbr["baseColorFactor"], 4, out.base_color, label + " baseColorFactor", 0.0f, 1.0f);
            if (!pbr["metallicFactor"].is_null()) {
                out.metallic = std::clamp(static_cast<float>(pbr["metallicFactor"].as_number(1.0)), 0.0f, 1.0f);
            }
            if (!pbr["roughnessFactor"].is_null()) {
                out.roughness = std::clamp(static_cast<float>(pbr["roughnessFactor"].as_number(1.0)), 0.0f, 1.0f);
            }
            out.base_color_texture = texture_ref(pbr["baseColorTexture"], label + " baseColorTexture");
            out.metallic_roughness_texture = texture_ref(pbr["metallicRoughnessTexture"], label + " metallicRoughnessTexture");
            out.normal_texture = texture_ref(m["normalTexture"], label + " normalTexture");
            if (!m["normalTexture"]["scale"].is_null()) {
                out.normal_scale = static_cast<float>(m["normalTexture"]["scale"].as_number(1.0));
            }
            out.occlusion_texture = texture_ref(m["occlusionTexture"], label + " occlusionTexture");
            if (!m["occlusionTexture"]["strength"].is_null()) {
                out.occlusion_strength = std::clamp(static_cast<float>(m["occlusionTexture"]["strength"].as_number(1.0)), 0.0f, 1.0f);
            }
            out.emissive_texture = texture_ref(m["emissiveTexture"], label + " emissiveTexture");
            read_factor(m["emissiveFactor"], 3, out.emissive, label + " emissiveFactor", 0.0f, 1.0f);
            const JsonValue& strength = m["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"];
            if (!strength.is_null()) {
                const float s = std::max(0.0f, static_cast<float>(strength.as_number(1.0)));
                for (float& e : out.emissive) {
                    e *= s;
                }
            }
            const std::string& mode = m["alphaMode"].as_string();
            if (mode.empty() || mode == "OPAQUE") {
                out.alpha_mode = AlphaMode::Opaque;
            } else if (mode == "MASK") {
                out.alpha_mode = AlphaMode::Mask;
            } else if (mode == "BLEND") {
                out.alpha_mode = AlphaMode::Blend;
            } else {
                fail(label + ": unknown alphaMode '" + mode + "'");
            }
            if (!m["alphaCutoff"].is_null()) {
                out.alpha_cutoff = std::clamp(static_cast<float>(m["alphaCutoff"].as_number(0.5)), 0.0f, 1.0f);
            }
            out.double_sided = m["doubleSided"].as_bool(false);
            asset_.materials.push_back(std::move(out));
        }
    }

    // ----------------------------------------------------------------------------------------------- meshes
    void load_meshes()
    {
        const JsonValue& meshes = root_["meshes"];
        for (std::size_t i = 0; i < meshes.size(); ++i) {
            const JsonValue& m = meshes.at(i);
            Mesh mesh;
            mesh.name = m["name"].as_string();
            const JsonValue& primitives = m["primitives"];
            if (!primitives.is_array() || primitives.size() == 0) {
                fail("mesh " + std::to_string(i) + ": needs at least one primitive");
            }
            for (std::size_t p = 0; p < primitives.size(); ++p) {
                const std::string label = "mesh " + std::to_string(i) + " primitive " + std::to_string(p);
                if (std::optional<Primitive> prim = build_primitive(primitives.at(p), label)) {
                    mesh.bounds.expand(prim->bounds);
                    mesh.primitives.push_back(std::move(*prim));
                }
            }
            asset_.meshes.push_back(std::move(mesh));
        }
    }

    std::optional<Primitive> build_primitive(const JsonValue& json, const std::string& label)
    {
        if (!json["mode"].is_null() && json["mode"].as_index() != 4) {
            warn(label + ": only TRIANGLES primitives are supported (mode " +
                 std::to_string(json["mode"].as_index()) + " was skipped)");
            return std::nullopt;
        }
        const JsonValue& attributes = json["attributes"];
        if (!attributes.is_object()) {
            fail(label + ": missing 'attributes'");
        }
        for (std::size_t a = 0; a < attributes.size(); ++a) {
            const std::string& name = attributes.key_at(a);
            if (name != "POSITION" && name != "NORMAL" && name != "TANGENT" && name != "TEXCOORD_0" && name != "COLOR_0" && name != "JOINTS_0" &&
                name != "WEIGHTS_0") {
                warn("attribute '" + name + "' is not supported and was ignored");
            }
        }
        if (attributes["POSITION"].is_null()) {
            fail(label + ": POSITION is required");
        }

        Primitive prim;
        std::vector<float> positions;
        const Accessor& pos_acc = accessor(attributes["POSITION"].as_index(), label + " POSITION");
        if (pos_acc.count > limits_.max_vertices_per_primitive) {
            fail(label + ": " + std::to_string(pos_acc.count) + " vertices is over the limit of " +
                 std::to_string(limits_.max_vertices_per_primitive));
        }
        read_floats(pos_acc, 3, false, label + " POSITION", positions);
        const std::size_t n = pos_acc.count;

        auto matching = [&](const char* name, int comps, bool allow_norm, std::vector<float>& out) {
            const JsonValue& v = attributes[name];
            if (v.is_null()) {
                return false;
            }
            const Accessor& acc = accessor(v.as_index(), label + " " + name);
            if (acc.count != n) {
                fail(label + ": " + name + " has " + std::to_string(acc.count) + " elements but POSITION has " + std::to_string(n));
            }
            read_floats(acc, comps, allow_norm, label + " " + name, out);
            return true;
        };
        std::vector<float> normals, tangents, uvs, colors;
        const bool have_normals = matching("NORMAL", 3, false, normals);
        const bool have_tangents = matching("TANGENT", 4, false, tangents);
        const bool have_uvs = matching("TEXCOORD_0", 2, true, uvs);
        bool have_colors = false;
        if (!attributes["COLOR_0"].is_null()) {
            const Accessor& acc = accessor(attributes["COLOR_0"].as_index(), label + " COLOR_0");
            if (acc.count != n) {
                fail(label + ": COLOR_0 has " + std::to_string(acc.count) + " elements but POSITION has " + std::to_string(n));
            }
            std::vector<float> raw;
            read_floats(acc, acc.components, true, label + " COLOR_0", raw);
            if (acc.components != 3 && acc.components != 4) {
                fail(label + ": COLOR_0 must be VEC3 or VEC4");
            }
            colors.resize(n * 4);
            for (std::size_t v = 0; v < n; ++v) {
                for (std::size_t c = 0; c < 4; ++c) {
                    colors[v * 4 + c] = c < static_cast<std::size_t>(acc.components)
                                            ? raw[v * static_cast<std::size_t>(acc.components) + c]
                                            : 1.0f;
                }
            }
            have_colors = true;
        }

        std::vector<uint16_t> joints;
        std::vector<float> weights;
        const bool have_joints = !attributes["JOINTS_0"].is_null();
        if (have_joints != !attributes["WEIGHTS_0"].is_null()) {
            fail(label + ": JOINTS_0 and WEIGHTS_0 must come together");
        }
        if (have_joints) {
            read_joints(accessor(attributes["JOINTS_0"].as_index(), label + " JOINTS_0"), n, label + " JOINTS_0", joints);
            const Accessor& wacc = accessor(attributes["WEIGHTS_0"].as_index(), label + " WEIGHTS_0");
            if (wacc.count != n) {
                fail(label + ": WEIGHTS_0 has " + std::to_string(wacc.count) + " elements but POSITION has " + std::to_string(n));
            }
            read_floats(wacc, 4, true, label + " WEIGHTS_0", weights);
            for (std::size_t v = 0; v < n; ++v) {
                float sum = 0.0f;
                for (std::size_t k = 0; k < 4; ++k) {
                    float& w = weights[v * 4 + k];
                    if (w < 0.0f) fail(label + ": WEIGHTS_0 has a negative weight");
                    sum += w;
                }
                if (!(sum > 0.0f)) {  // bound to nothing: the first joint takes it, so the vertex still follows the skeleton
                    weights[v * 4] = 1.0f;
                    sum = 1.0f;
                }
                for (std::size_t k = 0; k < 4; ++k) weights[v * 4 + k] /= sum;
            }
        }
        if (attributes["JOINTS_1"].is_null() == false || attributes["WEIGHTS_1"].is_null() == false) {
            warn("more than four joint influences per vertex (JOINTS_1 / WEIGHTS_1) are not imported: the extra influences are dropped");
        }

        std::vector<uint32_t> indices;
        if (!json["indices"].is_null()) {
            read_indices(accessor(json["indices"].as_index(), label + " indices"), label + " indices", indices);
            if (indices.size() % 3 != 0) {
                fail(label + ": the index count " + std::to_string(indices.size()) + " is not a multiple of 3");
            }
            for (const uint32_t index : indices) {
                if (index >= n) {
                    fail(label + ": an index (" + std::to_string(index) + ") is out of range for " + std::to_string(n) + " vertices");
                }
            }
        } else {
            if (n % 3 != 0) {
                fail(label + ": a non-indexed primitive needs a vertex count that is a multiple of 3");
            }
            indices.resize(n);
            for (std::size_t k = 0; k < n; ++k) {
                indices[k] = static_cast<uint32_t>(k);
            }
        }
        if (!json["material"].is_null()) {
            const int64_t mat = json["material"].as_index();
            if (mat < 0 || static_cast<std::size_t>(mat) >= asset_.materials.size()) {
                fail(label + ": invalid material index");
            }
            prim.material = static_cast<int32_t>(mat);
        }

        // Convert to engine space: a proper rotation, so winding is preserved.
        for (std::size_t v = 0; v < n; ++v) {
            const Float3 p = gltf_to_engine({positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]});
            positions[v * 3] = p.x;
            positions[v * 3 + 1] = p.y;
            positions[v * 3 + 2] = p.z;
            if (have_normals) {
                Float3 nrm = gltf_to_engine({normals[v * 3], normals[v * 3 + 1], normals[v * 3 + 2]});
                nrm = render::length(nrm) > 1e-12f ? render::normalize(nrm) : Float3{0, 0, 1};
                normals[v * 3] = nrm.x;
                normals[v * 3 + 1] = nrm.y;
                normals[v * 3 + 2] = nrm.z;
            }
            if (have_tangents) {
                const Float3 t = gltf_to_engine({tangents[v * 4], tangents[v * 4 + 1], tangents[v * 4 + 2]});
                tangents[v * 4] = t.x;
                tangents[v * 4 + 1] = t.y;
                tangents[v * 4 + 2] = t.z;  // w (handedness) is unchanged by a proper rotation
            }
        }

        if (!have_normals) {
            // glTF requires flat normals when none are given. Flat shading needs unshared vertices, so de-index.
            const std::size_t tri_count = indices.size() / 3;
            Primitive flat;
            flat.positions.reserve(indices.size() * 3);
            flat.normals.reserve(indices.size() * 3);
            if (have_uvs) flat.uv0.reserve(indices.size() * 2);
            if (have_colors) flat.colors.reserve(indices.size() * 4);
            if (have_tangents) flat.tangents.reserve(indices.size() * 4);
            if (have_joints) {
                flat.joints.reserve(indices.size() * 4);
                flat.weights.reserve(indices.size() * 4);
            }
            for (std::size_t t = 0; t < tri_count; ++t) {
                Float3 corner[3];
                for (std::size_t k = 0; k < 3; ++k) {
                    const std::size_t v = indices[t * 3 + k];
                    corner[k] = {positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]};
                }
                Float3 face = render::cross(corner[1] - corner[0], corner[2] - corner[0]);
                face = render::length(face) > 1e-20f ? render::normalize(face) : Float3{0, 0, 1};
                for (std::size_t k = 0; k < 3; ++k) {
                    const std::size_t v = indices[t * 3 + k];
                    flat.positions.insert(flat.positions.end(), {corner[k].x, corner[k].y, corner[k].z});
                    flat.normals.insert(flat.normals.end(), {face.x, face.y, face.z});
                    if (have_uvs) flat.uv0.insert(flat.uv0.end(), {uvs[v * 2], uvs[v * 2 + 1]});
                    if (have_colors) flat.colors.insert(flat.colors.end(), colors.begin() + static_cast<std::ptrdiff_t>(v * 4), colors.begin() + static_cast<std::ptrdiff_t>(v * 4 + 4));
                    if (have_tangents) flat.tangents.insert(flat.tangents.end(), tangents.begin() + static_cast<std::ptrdiff_t>(v * 4), tangents.begin() + static_cast<std::ptrdiff_t>(v * 4 + 4));
                    if (have_joints) {
                        flat.joints.insert(flat.joints.end(), joints.begin() + static_cast<std::ptrdiff_t>(v * 4), joints.begin() + static_cast<std::ptrdiff_t>(v * 4 + 4));
                        flat.weights.insert(flat.weights.end(), weights.begin() + static_cast<std::ptrdiff_t>(v * 4), weights.begin() + static_cast<std::ptrdiff_t>(v * 4 + 4));
                    }
                    flat.indices.push_back(static_cast<uint32_t>(t * 3 + k));
                }
            }
            flat.material = prim.material;
            flat.normals_generated = true;
            prim = std::move(flat);
        } else {
            prim.positions = std::move(positions);
            prim.normals = std::move(normals);
            prim.tangents = std::move(tangents);
            prim.uv0 = std::move(uvs);
            prim.colors = std::move(colors);
            prim.joints = std::move(joints);
            prim.weights = std::move(weights);
            prim.indices = std::move(indices);
        }
        for (std::size_t v = 0; v < prim.positions.size() / 3; ++v) {
            prim.bounds.expand(Float3{prim.positions[v * 3], prim.positions[v * 3 + 1], prim.positions[v * 3 + 2]});
        }
        return prim;
    }

    /// A primitive whose material has a normal texture needs tangents. The file's own are used; otherwise they are generated
    /// (a deterministic CPU step, see tangents.hpp). Materials without a normal texture never get tangents they would not use.
    void generate_missing_tangents()
    {
        for (Mesh& mesh : asset_.meshes) {
            for (Primitive& prim : mesh.primitives) {
                if (prim.material < 0 || static_cast<std::size_t>(prim.material) >= asset_.materials.size() ||
                    !asset_.materials[static_cast<std::size_t>(prim.material)].normal_texture.valid() || !prim.tangents.empty()) {
                    continue;
                }
                const TangentReport report = generate_tangents(prim);
                if (report.result == TangentResult::Generated) {
                    warn("a primitive with a normal texture has no TANGENT attribute: tangents were generated (Lengyel-style, "
                         "not MikkTSpace; they can differ from a MikkTSpace result at UV seams)");
                    if (report.fallback_vertices != 0) {
                        warn("tangent generation found vertices without a usable UV mapping and gave them an arbitrary frame");
                    }
                    if (report.split_vertices != 0) {
                        warn("tangent generation split vertices shared by mirrored UV islands (the file did not split them)");
                    }
                    if (report.mixed_handedness_triangles != 0) {
                        warn("some triangles have normals that contradict their winding: their tangent handedness is inconsistent");
                    }
                } else if (report.result == TangentResult::Invalid) {
                    warn("a primitive with a normal texture has inconsistent vertex arrays: no tangents were generated");
                } else if (report.result == TangentResult::NoUvs) {
                    warn("a primitive has a normal texture but no TEXCOORD_0: its normal map cannot be applied");
                }
            }
        }
    }

    // ----------------------------------------------------------------------------------------------- nodes
    void read_numbers(const JsonValue& v, std::size_t n, double* out, const std::string& what)
    {
        if (!v.is_array() || v.size() != n) {
            fail(what + ": expected an array of " + std::to_string(n) + " numbers");
        }
        for (std::size_t i = 0; i < n; ++i) {
            out[i] = v.at(i).as_number(std::nan(""));
            if (!std::isfinite(out[i])) {
                fail(what + ": contains a non-numeric or non-finite value");
            }
        }
    }

    void load_nodes()
    {
        const JsonValue& nodes = root_["nodes"];
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const JsonValue& n = nodes.at(i);
            const std::string label = "node " + std::to_string(i);
            NodeInfo info;
            info.name = n["name"].as_string();
            if (!n["mesh"].is_null()) {
                info.mesh = n["mesh"].as_index();
                if (info.mesh < 0 || static_cast<std::size_t>(info.mesh) >= asset_.meshes.size()) {
                    fail(label + ": invalid mesh index");
                }
            }
            const JsonValue& children = n["children"];
            for (std::size_t c = 0; c < children.size(); ++c) {
                const int64_t child = children.at(c).as_index();
                if (child < 0 || static_cast<std::size_t>(child) >= nodes.size() || static_cast<std::size_t>(child) == i) {
                    fail(label + ": invalid child index");
                }
                info.children.push_back(static_cast<std::size_t>(child));
            }
            if (!n["camera"].is_null()) warn("cameras are not imported");
            if (!n["skin"].is_null()) {
                info.skin = n["skin"].as_index();
                if (info.skin < 0 || static_cast<std::size_t>(info.skin) >= root_["skins"].size()) {
                    fail(label + ": invalid skin index");
                }
            }
            if (!n["weights"].is_null()) warn("morph target weights are not imported");
            if (!n["matrix"].is_null()) {
                double m[16];
                read_numbers(n["matrix"], 16, m, label + " matrix");
                for (std::size_t k = 0; k < 16; ++k) {
                    info.local.m[k] = static_cast<float>(m[k]);  // glTF matrices are column-major, like Mat4
                }
                info.decomposable = decompose(info.local, info.trs);
            } else {
                render::Transform t;
                double v[4];
                if (!n["translation"].is_null()) {
                    read_numbers(n["translation"], 3, v, label + " translation");
                    t.position = {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
                }
                if (!n["rotation"].is_null()) {
                    read_numbers(n["rotation"], 4, v, label + " rotation");
                    t.rotation = render::normalize({static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]),
                                                    static_cast<float>(v[3])});
                }
                if (!n["scale"].is_null()) {
                    read_numbers(n["scale"], 3, v, label + " scale");
                    t.scale = {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
                }
                info.trs = t;
                info.local = render::to_matrix(t);
            }
            nodes_.push_back(std::move(info));
        }
    }

    /// Splits an affine matrix into translation, rotation and scale. False (and `out` unspecified) for a shear, a projective part or a
    /// degenerate or mirrored one, which a TRS cannot represent.
    static bool decompose(const Mat4& m, render::Transform& out)
    {
        if (std::abs(m.at(3, 0)) > 1e-6f || std::abs(m.at(3, 1)) > 1e-6f || std::abs(m.at(3, 2)) > 1e-6f || std::abs(m.at(3, 3) - 1.0f) > 1e-6f) return false;
        out.position = {m.at(0, 3), m.at(1, 3), m.at(2, 3)};
        Float3 col[3];
        float s[3];
        for (int c = 0; c < 3; ++c) {
            col[c] = {m.at(0, c), m.at(1, c), m.at(2, c)};
            s[c] = render::length(col[c]);
            if (!(s[c] > 1e-9f)) return false;
            col[c] = col[c] * (1.0f / s[c]);
        }
        if (render::dot(render::cross(col[0], col[1]), col[2]) < 0.0f) return false;  // mirrored
        if (std::abs(render::dot(col[0], col[1])) > 1e-4f || std::abs(render::dot(col[0], col[2])) > 1e-4f || std::abs(render::dot(col[1], col[2])) > 1e-4f) return false;  // sheared
        out.scale = {s[0], s[1], s[2]};
        const float r00 = col[0].x, r01 = col[1].x, r02 = col[2].x, r10 = col[0].y, r11 = col[1].y, r12 = col[2].y, r20 = col[0].z, r21 = col[1].z, r22 = col[2].z;
        const float trace = r00 + r11 + r22;
        render::Quat q;
        if (trace > 0.0f) {
            const float k = std::sqrt(trace + 1.0f) * 2.0f;
            q = {(r21 - r12) / k, (r02 - r20) / k, (r10 - r01) / k, 0.25f * k};
        } else if (r00 > r11 && r00 > r22) {
            const float k = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
            q = {0.25f * k, (r01 + r10) / k, (r02 + r20) / k, (r21 - r12) / k};
        } else if (r11 > r22) {
            const float k = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
            q = {(r01 + r10) / k, 0.25f * k, (r12 + r21) / k, (r02 - r20) / k};
        } else {
            const float k = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
            q = {(r02 + r20) / k, (r12 + r21) / k, 0.25f * k, (r10 - r01) / k};
        }
        out.rotation = render::normalize(q);
        return true;
    }

    // ----------------------------------------------------------------------------------------------- skins
    static Mat4 conversion()
    {
        Mat4 c = Mat4{};
        c.at(0, 2) = 1.0f;
        c.at(1, 0) = 1.0f;
        c.at(2, 1) = 1.0f;
        c.at(3, 3) = 1.0f;
        return c;
    }

    /// The glTF-space world matrix of every node (a node's ancestors are found through the children lists, which must form a forest).
    std::vector<Mat4> node_worlds(std::vector<int64_t>& parent) const
    {
        parent.assign(nodes_.size(), -1);
        for (std::size_t i = 0; i < nodes_.size(); ++i) {
            for (const std::size_t c : nodes_[i].children) {
                if (parent[c] >= 0) fail("node " + std::to_string(c) + " has more than one parent (the node graph must be a forest)");
                parent[c] = static_cast<int64_t>(i);
            }
        }
        std::vector<Mat4> world(nodes_.size(), Mat4::identity());
        std::vector<uint8_t> state(nodes_.size(), 0);  // 0 unknown, 1 in progress, 2 done
        std::vector<std::size_t> chain;
        for (std::size_t start = 0; start < nodes_.size(); ++start) {
            chain.clear();
            for (std::size_t n = start; state[n] != 2;) {
                if (state[n] == 1) fail("the node hierarchy has a cycle");
                state[n] = 1;
                chain.push_back(n);
                if (chain.size() > limits_.max_node_depth + 1) fail("the node hierarchy is deeper than " + std::to_string(limits_.max_node_depth));
                if (parent[n] < 0) break;
                n = static_cast<std::size_t>(parent[n]);
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                const std::size_t n = *it;
                world[n] = (parent[n] < 0 ? Mat4::identity() : world[static_cast<std::size_t>(parent[n])]) * nodes_[n].local;
                state[n] = 2;
            }
        }
        return world;
    }

    void load_skins()
    {
        const JsonValue& skins = root_["skins"];
        if (skins.size() == 0) return;
        if (skins.size() > limits_.max_skins) fail("the file has " + std::to_string(skins.size()) + " skins, over the limit of " + std::to_string(limits_.max_skins));
        std::vector<int64_t> parent;
        const std::vector<Mat4> world = node_worlds(parent);
        const Mat4 c = conversion();
        const Mat4 c_inverse = render::transpose(c);
        for (std::size_t i = 0; i < skins.size(); ++i) {
            const JsonValue& s = skins.at(i);
            const std::string label = "skin " + std::to_string(i);
            Skin skin;
            skin.name = s["name"].as_string();
            const JsonValue& joints = s["joints"];
            if (!joints.is_array() || joints.size() == 0) fail(label + ": needs at least one joint");
            if (joints.size() > limits_.max_joints_per_skin || joints.size() > 65536) {
                fail(label + ": " + std::to_string(joints.size()) + " joints is over the limit of " + std::to_string(std::min<std::size_t>(limits_.max_joints_per_skin, 65536)));
            }
            std::vector<int64_t> joint_of_node(nodes_.size(), -1);
            for (std::size_t j = 0; j < joints.size(); ++j) {
                const int64_t node = joints.at(j).as_index();
                if (node < 0 || static_cast<std::size_t>(node) >= nodes_.size()) fail(label + ": joint " + std::to_string(j) + " names an invalid node");
                if (joint_of_node[static_cast<std::size_t>(node)] >= 0) fail(label + ": node " + std::to_string(node) + " is listed as a joint twice");
                joint_of_node[static_cast<std::size_t>(node)] = static_cast<int64_t>(j);
                skin.joint_nodes.push_back(static_cast<int32_t>(node));
            }
            std::vector<float> ibm;
            const bool have_ibm = !s["inverseBindMatrices"].is_null();
            if (have_ibm) {
                const Accessor& acc = accessor(s["inverseBindMatrices"].as_index(), label + " inverseBindMatrices");
                if (acc.count != joints.size()) fail(label + ": inverseBindMatrices has " + std::to_string(acc.count) + " matrices for " + std::to_string(joints.size()) + " joints");
                read_floats(acc, 16, false, label + " inverseBindMatrices", ibm);
            }
            for (std::size_t j = 0; j < joints.size(); ++j) {
                const std::size_t node = static_cast<std::size_t>(skin.joint_nodes[j]);
                const NodeInfo& info = nodes_[node];
                if (!info.decomposable) fail(label + ": joint node " + std::to_string(node) + " has a matrix with shear, mirroring or no scale, which a skeleton cannot pose");
                // The parent joint is the node's parent when that is a joint. A non-joint node between two joints is not supported.
                int32_t parent_joint = -1;
                Mat4 base = Mat4::identity();
                for (int64_t up = parent[node]; up >= 0; up = parent[static_cast<std::size_t>(up)]) {
                    if (joint_of_node[static_cast<std::size_t>(up)] >= 0) {
                        if (up != parent[node]) fail(label + ": joint node " + std::to_string(node) + " has a non-joint node between it and its parent joint, which is not supported");
                        parent_joint = static_cast<int32_t>(joint_of_node[static_cast<std::size_t>(up)]);
                        break;
                    }
                }
                if (parent_joint < 0 && parent[node] >= 0) base = c * world[static_cast<std::size_t>(parent[node])] * c_inverse;
                skin.joint_names.push_back(info.name);
                skin.parent.push_back(parent_joint);
                skin.base.push_back(base);
                render::Transform rest;
                rest.position = gltf_to_engine(info.trs.position);
                rest.rotation = {info.trs.rotation.z, info.trs.rotation.x, info.trs.rotation.y, info.trs.rotation.w};
                rest.scale = gltf_to_engine(info.trs.scale);
                skin.rest.push_back(rest);
                Mat4 m = Mat4::identity();
                if (have_ibm) {
                    for (std::size_t k = 0; k < 16; ++k) m.m[k] = ibm[j * 16 + k];
                    m = c * m * c_inverse;
                }
                skin.inverse_bind.push_back(m);
            }
            asset_.skins.push_back(std::move(skin));
        }
    }

    // ----------------------------------------------------------------------------------------------- animations
    void load_animations()
    {
        const JsonValue& animations = root_["animations"];
        if (animations.size() == 0) return;
        if (animations.size() > limits_.max_animations) {
            fail("the file has " + std::to_string(animations.size()) + " animations, over the limit of " + std::to_string(limits_.max_animations));
        }
        for (std::size_t a = 0; a < animations.size(); ++a) {
            const JsonValue& anim = animations.at(a);
            const std::string label = "animation " + std::to_string(a);
            AnimationClip clip;
            clip.name = anim["name"].as_string();
            const JsonValue& samplers = anim["samplers"];
            const JsonValue& channels = anim["channels"];
            if (!samplers.is_array() || !channels.is_array()) fail(label + ": needs 'samplers' and 'channels'");
            struct Raw {
                int64_t output = -1;
                bool used = false;
            };
            std::vector<Raw> raw(samplers.size());
            for (std::size_t s = 0; s < samplers.size(); ++s) {
                const JsonValue& js = samplers.at(s);
                const std::string slabel = label + " sampler " + std::to_string(s);
                AnimationSampler sampler;
                const std::string mode = js["interpolation"].is_null() ? "LINEAR" : js["interpolation"].as_string();
                if (mode == "LINEAR") sampler.interpolation = Interpolation::Linear;
                else if (mode == "STEP") sampler.interpolation = Interpolation::Step;
                else if (mode == "CUBICSPLINE") sampler.interpolation = Interpolation::CubicSpline;
                else fail(slabel + ": unknown interpolation '" + mode + "'");
                const Accessor& input = accessor(js["input"].as_index(), slabel + " input");
                if (input.count > limits_.max_animation_keys) fail(slabel + ": " + std::to_string(input.count) + " keys is over the limit of " + std::to_string(limits_.max_animation_keys));
                read_floats(input, 1, false, slabel + " input", sampler.times);
                float previous = -1.0f;
                for (const float time : sampler.times) {
                    if (!(time >= 0.0f) || !(time > previous)) fail(slabel + ": key times must be non-negative and strictly increasing");
                    previous = time;
                }
                if (sampler.times.size() < (sampler.interpolation == Interpolation::CubicSpline ? 2u : 1u)) {
                    fail(slabel + ": needs at least " + std::string(sampler.interpolation == Interpolation::CubicSpline ? "two keys for cubic spline interpolation" : "one key"));
                }
                raw[s].output = js["output"].as_index();
                clip.samplers.push_back(std::move(sampler));
            }
            for (std::size_t ch = 0; ch < channels.size(); ++ch) {
                const JsonValue& jc = channels.at(ch);
                const std::string clabel = label + " channel " + std::to_string(ch);
                const JsonValue& target = jc["target"];
                const int64_t sampler_index = jc["sampler"].as_index();
                if (sampler_index < 0 || static_cast<std::size_t>(sampler_index) >= clip.samplers.size()) fail(clabel + ": invalid sampler index");
                if (target["node"].is_null()) continue;  // targets an extension's object, not a node: nothing to animate here
                const int64_t node = target["node"].as_index();
                if (node < 0 || static_cast<std::size_t>(node) >= nodes_.size()) fail(clabel + ": invalid target node");
                const std::string path = target["path"].as_string();
                AnimationPath p;
                if (path == "translation") p = AnimationPath::Translation;
                else if (path == "rotation") p = AnimationPath::Rotation;
                else if (path == "scale") p = AnimationPath::Scale;
                else {
                    warn("animation channels that target '" + path + "' (morph target weights and others) are not imported");
                    continue;
                }
                AnimationSampler& sampler = clip.samplers[static_cast<std::size_t>(sampler_index)];
                const int components = p == AnimationPath::Rotation ? 4 : 3;
                Raw& r = raw[static_cast<std::size_t>(sampler_index)];
                if (r.used && sampler.components != components) fail(clabel + ": its sampler is shared by channels with different paths");
                sampler.components = components;
                if (!r.used) {
                    r.used = true;
                    const Accessor& out = accessor(r.output, clabel + " output");
                    const std::size_t per_key = sampler.interpolation == Interpolation::CubicSpline ? 3u : 1u;
                    if (out.count != sampler.times.size() * per_key) {
                        fail(clabel + ": the output has " + std::to_string(out.count) + " values for " + std::to_string(sampler.times.size()) + " keys" +
                             (per_key == 3 ? " (cubic spline stores an in-tangent, a value and an out-tangent per key)" : ""));
                    }
                    read_floats(out, components, false, clabel + " output", sampler.values);
                    // glTF to engine space, per triple (z, x, y) and per quaternion (z, x, y, w): the same proper rotation of the axes.
                    const std::size_t count = sampler.values.size() / static_cast<std::size_t>(components);
                    for (std::size_t k = 0; k < count; ++k) {
                        float* v = sampler.values.data() + k * static_cast<std::size_t>(components);
                        const float x = v[0], y = v[1], z = v[2];
                        v[0] = z;
                        v[1] = x;
                        v[2] = y;
                        if (components == 4 && sampler.interpolation != Interpolation::CubicSpline) {
                            const render::Quat q = render::normalize(render::Quat{v[0], v[1], v[2], v[3]});
                            v[0] = q.x; v[1] = q.y; v[2] = q.z; v[3] = q.w;
                        }
                    }
                }
                clip.channels.push_back({static_cast<int32_t>(node), p, static_cast<int32_t>(sampler_index)});
            }
            for (const AnimationSampler& s : clip.samplers) {
                if (!s.times.empty() && !s.values.empty()) clip.duration = std::max(clip.duration, s.times.back());
            }
            asset_.animations.push_back(std::move(clip));
        }
    }

    void build_instances()
    {
        std::vector<std::size_t> roots;
        const JsonValue& scenes = root_["scenes"];
        if (scenes.size() > 0) {
            int64_t scene = root_["scene"].is_null() ? 0 : root_["scene"].as_index();
            if (scene < 0 || static_cast<std::size_t>(scene) >= scenes.size()) {
                fail("invalid default scene index");
            }
            const JsonValue& list = scenes.at(static_cast<std::size_t>(scene))["nodes"];
            for (std::size_t i = 0; i < list.size(); ++i) {
                const int64_t node = list.at(i).as_index();
                if (node < 0 || static_cast<std::size_t>(node) >= nodes_.size()) {
                    fail("scene lists an invalid node index");
                }
                roots.push_back(static_cast<std::size_t>(node));
            }
        } else {
            std::vector<bool> is_child(nodes_.size(), false);
            for (const NodeInfo& n : nodes_) {
                for (const std::size_t c : n.children) {
                    is_child[c] = true;
                }
            }
            for (std::size_t i = 0; i < nodes_.size(); ++i) {
                if (!is_child[i]) {
                    roots.push_back(i);
                }
            }
        }

        // glTF space -> engine space and back. C is a cyclic axis permutation, so C^-1 = C^T.
        Mat4 c = Mat4{};
        c.at(0, 2) = 1.0f;
        c.at(1, 0) = 1.0f;
        c.at(2, 1) = 1.0f;
        c.at(3, 3) = 1.0f;
        const Mat4 c_inverse = render::transpose(c);

        struct Frame {
            std::size_t node;
            Mat4 parent_world;
            std::size_t depth;
        };
        std::vector<uint8_t> visited(nodes_.size(), 0);
        std::vector<Frame> stack;
        for (auto it = roots.rbegin(); it != roots.rend(); ++it) {
            stack.push_back({*it, Mat4::identity(), 0});
        }
        while (!stack.empty()) {
            const Frame frame = stack.back();
            stack.pop_back();
            if (visited[frame.node] != 0) {
                fail("node " + std::to_string(frame.node) + " is reachable more than once (the node graph must be a forest: no cycles, no shared children)");
            }
            visited[frame.node] = 1;
            if (frame.depth > limits_.max_node_depth) {
                fail("the node hierarchy is deeper than " + std::to_string(limits_.max_node_depth));
            }
            const NodeInfo& node = nodes_[frame.node];
            const Mat4 world = frame.parent_world * node.local;
            if (node.mesh >= 0) {
                if (asset_.instances.size() >= limits_.max_instances) {
                    fail("the scene has more than " + std::to_string(limits_.max_instances) + " mesh instances");
                }
                MeshInstance inst;
                inst.mesh = static_cast<int32_t>(node.mesh);
                inst.node = static_cast<int32_t>(frame.node);
                inst.name = node.name;
                inst.world = c * world * c_inverse;
                const Mesh& mesh = asset_.meshes[static_cast<std::size_t>(node.mesh)];
                const Aabb& b = mesh.bounds;
                if (node.skin >= 0) {
                    const Skin& skin = asset_.skins[static_cast<std::size_t>(node.skin)];
                    for (const Primitive& prim : mesh.primitives) {
                        if (prim.joints.empty()) fail("node " + std::to_string(frame.node) + " has a skin, but its mesh has a primitive without JOINTS_0 and WEIGHTS_0");
                        for (const uint16_t j : prim.joints) {
                            if (j >= skin.joint_count()) fail("node " + std::to_string(frame.node) + ": a vertex names joint " + std::to_string(j) + " but the skin has " + std::to_string(skin.joint_count()));
                        }
                    }
                    inst.skin = static_cast<int32_t>(node.skin);
                    inst.world = Mat4::identity();  // the joint matrices carry the whole hierarchy (see MeshInstance::world)
                    // Bounds at rest: the skinned positions of the rest pose.
                    const std::vector<Mat4> rest = skin_matrices(skin, rest_pose(skin));
                    for (const Primitive& prim : mesh.primitives) {
                        for (std::size_t v = 0; v < prim.vertex_count(); ++v) asset_.bounds.expand(skin_position(prim, v, rest));
                    }
                } else {
                    for (const Primitive& prim : mesh.primitives) {
                        if (!prim.joints.empty()) {
                            warn("a primitive has JOINTS_0 and WEIGHTS_0 but its node has no skin: it is drawn as a rigid mesh");
                            break;
                        }
                    }
                }
                if (b.valid() && node.skin < 0) {
                    for (int corner = 0; corner < 8; ++corner) {
                        const Float3 p{(corner & 1) != 0 ? b.max.x : b.min.x, (corner & 2) != 0 ? b.max.y : b.min.y,
                                       (corner & 4) != 0 ? b.max.z : b.min.z};
                        asset_.bounds.expand(render::transform_point(inst.world, p));
                    }
                }
                asset_.instances.push_back(std::move(inst));
            }
            for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
                stack.push_back({*it, world, frame.depth + 1});
            }
        }
    }

    std::span<const uint8_t> bytes_;
    const ResourceReader& reader_;
    GltfLimits limits_;
    std::string_view json_text_;
    std::span<const uint8_t> glb_bin_;
    bool have_bin_ = false;
    JsonValue root_;
    std::vector<BufferData> buffers_;
    std::vector<BufferView> views_;
    std::vector<Accessor> accessors_;
    std::vector<NodeInfo> nodes_;
    std::set<std::string> warnings_;
    GltfAsset asset_;
};

} // namespace

GltfLoadResult load_gltf(std::span<const uint8_t> bytes, const ResourceReader& reader, const GltfLimits& limits)
{
    GltfLoadResult result;
    try {
        Loader loader(bytes, reader, limits);
        result.asset = loader.run();
    } catch (const GltfError& e) {
        result.asset.reset();
        result.error = std::string("glTF: ") + e.what();
    } catch (const std::bad_alloc&) {
        result.asset.reset();
        result.error = "glTF: out of memory while importing";
    } catch (const std::exception& e) {
        result.asset.reset();
        result.error = std::string("glTF: internal error: ") + e.what();
    }
    return result;
}

GltfLoadResult load_gltf_file(const std::filesystem::path& path, const GltfLimits& limits)
{
    namespace fs = std::filesystem;
    GltfLoadResult result;
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec) {
        result.error = "glTF: cannot open '" + path.string() + "': " + ec.message();
        return result;
    }
    if (size > limits.max_file_bytes) {
        result.error = "glTF: '" + path.string() + "' is over the file size limit";
        return result;
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        result.error = "glTF: cannot read '" + path.string() + "'";
        return result;
    }
    const fs::path directory = fs::weakly_canonical(path.parent_path(), ec);
    if (ec) {
        result.error = "glTF: cannot resolve the directory of '" + path.string() + "'";
        return result;
    }
    const ResourceReader reader = [directory, limits](const std::string& relative) -> std::optional<std::vector<uint8_t>> {
        std::error_code e;
        const fs::path resolved = fs::weakly_canonical(directory / relative, e);
        if (e) {
            return std::nullopt;
        }
        // Confinement: after resolving symlinks the file must still live inside the asset's directory.
        auto dir_it = directory.begin();
        auto res_it = resolved.begin();
        for (; dir_it != directory.end(); ++dir_it, ++res_it) {
            if (res_it == resolved.end() || *res_it != *dir_it) {
                return std::nullopt;
            }
        }
        const uintmax_t file_size = fs::file_size(resolved, e);
        if (e || file_size > limits.max_buffer_bytes) {
            return std::nullopt;
        }
        std::ifstream f(resolved, std::ios::binary);
        std::vector<uint8_t> data(static_cast<std::size_t>(file_size));
        if (!f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
            return std::nullopt;
        }
        return data;
    };
    return load_gltf(bytes, reader, limits);
}

} // namespace saige::gfx
