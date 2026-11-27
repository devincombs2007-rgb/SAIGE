#include "saige/editor_project.hpp"

#include <cstring>

namespace saige::editor {

namespace {

[[nodiscard]] std::uint32_t crc32_compute(const std::uint8_t* data,
                                          std::size_t len) noexcept {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      const std::uint32_t mask = (crc & 1u) ? 0xEDB88320u : 0u;
      crc = (crc >> 1) ^ mask;
    }
  }
  return ~crc;
}

void write_u8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }
void write_u16_le(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}
void write_u32_le(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
  }
}
void write_i32_le(std::vector<std::uint8_t>& out, std::int32_t v) {
  write_u32_le(out, static_cast<std::uint32_t>(v));
}
void write_u64_le(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
  }
}
void write_i64_le(std::vector<std::uint8_t>& out, std::int64_t v) {
  write_u64_le(out, static_cast<std::uint64_t>(v));
}
void write_f64_le(std::vector<std::uint8_t>& out, double v) {
  std::uint64_t u = 0;
  std::memcpy(&u, &v, sizeof(u));
  write_u64_le(out, u);
}
void write_vec3(std::vector<std::uint8_t>& out, const Vec3d& v) {
  write_f64_le(out, v.x);
  write_f64_le(out, v.y);
  write_f64_le(out, v.z);
}
void write_str(std::vector<std::uint8_t>& out, const std::string& s) {
  const auto len = static_cast<std::uint32_t>(s.size());
  write_u32_le(out, len);
  out.insert(out.end(), s.begin(), s.end());
}

bool read_u8(const std::vector<std::uint8_t>& in, std::size_t& off, std::uint8_t& v) {
  if (off + 1 > in.size()) return false;
  v = in[off++];
  return true;
}
bool read_u16_le(const std::vector<std::uint8_t>& in, std::size_t& off, std::uint16_t& v) {
  if (off + 2 > in.size()) return false;
  v = static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[off]) |
                                 static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[off + 1]) << 8));
  off += 2;
  return true;
}
bool read_u32_le(const std::vector<std::uint8_t>& in, std::size_t& off, std::uint32_t& v) {
  if (off + 4 > in.size()) return false;
  v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(in[off + static_cast<std::size_t>(i)]) << (i * 8);
  }
  off += 4;
  return true;
}
bool read_i32_le(const std::vector<std::uint8_t>& in, std::size_t& off, std::int32_t& v) {
  std::uint32_t u = 0;
  if (!read_u32_le(in, off, u)) return false;
  v = static_cast<std::int32_t>(u);
  return true;
}
bool read_u64_le(const std::vector<std::uint8_t>& in, std::size_t& off, std::uint64_t& v) {
  if (off + 8 > in.size()) return false;
  v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(in[off + static_cast<std::size_t>(i)]) << (i * 8);
  }
  off += 8;
  return true;
}
bool read_i64_le(const std::vector<std::uint8_t>& in, std::size_t& off, std::int64_t& v) {
  std::uint64_t u = 0;
  if (!read_u64_le(in, off, u)) return false;
  v = static_cast<std::int64_t>(u);
  return true;
}
bool read_f64_le(const std::vector<std::uint8_t>& in, std::size_t& off, double& v) {
  std::uint64_t u = 0;
  if (!read_u64_le(in, off, u)) return false;
  std::memcpy(&v, &u, sizeof(v));
  return true;
}
bool read_vec3(const std::vector<std::uint8_t>& in, std::size_t& off, Vec3d& v) {
  return read_f64_le(in, off, v.x) && read_f64_le(in, off, v.y) &&
         read_f64_le(in, off, v.z);
}
bool read_str(const std::vector<std::uint8_t>& in, std::size_t& off,
              std::size_t max_payload, std::string& out) {
  std::uint32_t len = 0;
  if (!read_u32_le(in, off, len)) return false;
  if (len > kMaxEditorStringLen || off + len > max_payload) return false;
  out.assign(reinterpret_cast<const char*>(in.data() + off), len);
  off += len;
  return true;
}

}  // namespace

std::vector<std::uint8_t> EditorProjectSerializer::serialize(
    const EditorScene& scene) {
  std::vector<std::uint8_t> out;
  out.reserve(128 + scene.nodes().size() * 128);

  write_u32_le(out, kEditorProjectMagic);
  write_u16_le(out, kEditorProjectVersion);
  write_f64_le(out, scene.chunk_size_m());
  write_u64_le(out, scene.next_id());

  const auto& snap = scene.snap_settings();
  write_u8(out, snap.grid_snap_enabled ? 1 : 0);
  write_f64_le(out, snap.grid_step_m);
  write_u8(out, snap.angle_snap_enabled ? 1 : 0);
  write_f64_le(out, snap.angle_step_deg);

  write_u64_le(out, static_cast<std::uint64_t>(scene.nodes().size()));
  for (const auto& n : scene.nodes()) {
    write_u64_le(out, n.id);
    write_u64_le(out, n.parent_id);
    write_str(out, n.name);
    write_u8(out, static_cast<std::uint8_t>(n.kind));
    write_vec3(out, n.local_transform.position);
    write_vec3(out, n.local_transform.rotation_deg);
    write_vec3(out, n.local_transform.scale);
    write_str(out, n.asset.asset_uri);
    write_u32_le(out, n.asset.asset_version);
    write_u64_le(out, n.asset.content_hash);
    write_i32_le(out, n.chunk.x);
    write_i32_le(out, n.chunk.y);
    write_u8(out, n.visible ? 1 : 0);
    write_u8(out, n.locked ? 1 : 0);

    write_u32_le(out, static_cast<std::uint32_t>(n.properties.size()));
    for (const auto& p : n.properties) {
      write_str(out, p.key);
      write_u8(out, static_cast<std::uint8_t>(p.value.type));
      write_u8(out, p.value.bool_val ? 1 : 0);
      write_i64_le(out, p.value.int_val);
      write_f64_le(out, p.value.double_val);
      write_str(out, p.value.str_val);
      write_vec3(out, p.value.vec3_val);
    }
  }

  const std::uint32_t crc = crc32_compute(out.data(), out.size());
  write_u32_le(out, crc);
  return out;
}

bool EditorProjectSerializer::deserialize(const std::vector<std::uint8_t>& blob,
                                          EditorScene& out_scene) {
  if (blob.size() < 52) return false;
  const std::size_t payload_len = blob.size() - 4;
  std::uint32_t expected_crc = 0;
  std::size_t crc_off = payload_len;
  if (!read_u32_le(blob, crc_off, expected_crc)) return false;
  if (crc32_compute(blob.data(), payload_len) != expected_crc) return false;

  std::size_t off = 0;
  std::uint32_t magic = 0;
  std::uint16_t ver = 0;
  double chunk_size = kDefaultChunkSizeM;
  EntityId next_id = 1;
  if (!read_u32_le(blob, off, magic) || magic != kEditorProjectMagic) return false;
  if (!read_u16_le(blob, off, ver) || ver != kEditorProjectVersion) return false;
  if (!read_f64_le(blob, off, chunk_size) || !read_u64_le(blob, off, next_id)) {
    return false;
  }

  SnapSettings snap{};
  std::uint8_t gs = 0, as = 0;
  if (!read_u8(blob, off, gs) || !read_f64_le(blob, off, snap.grid_step_m) ||
      !read_u8(blob, off, as) || !read_f64_le(blob, off, snap.angle_step_deg)) {
    return false;
  }
  snap.grid_snap_enabled = (gs != 0);
  snap.angle_snap_enabled = (as != 0);

  std::uint64_t n_nodes = 0;
  if (!read_u64_le(blob, off, n_nodes)) return false;
  if (n_nodes > kMaxEditorNodes || n_nodes * 96ULL > payload_len - off) {
    return false;
  }

  EditorScene restored(chunk_size);
  restored.snap_settings() = snap;

  for (std::uint64_t i = 0; i < n_nodes; ++i) {
    EditorNode n{};
    std::uint8_t kind_u8 = 0, vis_u8 = 0, lock_u8 = 0;
    std::uint32_t n_props = 0;
    if (!read_u64_le(blob, off, n.id) || !read_u64_le(blob, off, n.parent_id) ||
        !read_str(blob, off, payload_len, n.name) || !read_u8(blob, off, kind_u8) ||
        !read_vec3(blob, off, n.local_transform.position) ||
        !read_vec3(blob, off, n.local_transform.rotation_deg) ||
        !read_vec3(blob, off, n.local_transform.scale) ||
        !read_str(blob, off, payload_len, n.asset.asset_uri) ||
        !read_u32_le(blob, off, n.asset.asset_version) ||
        !read_u64_le(blob, off, n.asset.content_hash) ||
        !read_i32_le(blob, off, n.chunk.x) || !read_i32_le(blob, off, n.chunk.y) ||
        !read_u8(blob, off, vis_u8) || !read_u8(blob, off, lock_u8) ||
        !read_u32_le(blob, off, n_props)) {
      return false;
    }
    if (n_props > 1024u) return false;
    n.kind = static_cast<EditorNodeKind>(kind_u8);
    n.visible = (vis_u8 != 0);
    n.locked = (lock_u8 != 0);
    n.properties.resize(n_props);
    for (std::uint32_t p = 0; p < n_props; ++p) {
      std::uint8_t pt = 0, bv = 0;
      if (!read_str(blob, off, payload_len, n.properties[p].key) ||
          !read_u8(blob, off, pt) || !read_u8(blob, off, bv) ||
          !read_i64_le(blob, off, n.properties[p].value.int_val) ||
          !read_f64_le(blob, off, n.properties[p].value.double_val) ||
          !read_str(blob, off, payload_len, n.properties[p].value.str_val) ||
          !read_vec3(blob, off, n.properties[p].value.vec3_val)) {
        return false;
      }
      n.properties[p].value.type = static_cast<PropertyType>(pt);
      n.properties[p].value.bool_val = (bv != 0);
    }
    if (!restored.insert_node(std::move(n))) return false;
  }

  restored.set_next_id(next_id);
  out_scene = std::move(restored);
  return true;
}

}  // namespace saige::editor
