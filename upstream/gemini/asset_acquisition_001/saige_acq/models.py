from dataclasses import dataclass, field
from typing import Dict, List, Optional


@dataclass
class CatalogEntry:
    asset_id: str
    name: str
    category: str
    subcategory: str
    source_name: str
    source_url: str
    creator: str
    license_spdx: str
    redistribution_allowed: bool
    acquisition_mode: str  # "polyhaven_api", "ambientcg_api", "direct_url", "manual_pack"
    api_slug: Optional[str] = None
    preferred_format: Optional[str] = None
    preferred_resolution: str = "2k"
    download_url: Optional[str] = None
    expected_sha256: Optional[str] = None
    notes: str = ""


@dataclass
class ModelMetadata:
    file_format: str
    triangle_count: int = 0
    mesh_count: int = 0
    node_count: int = 0
    has_rig: bool = False
    bone_names: List[str] = field(default_factory=list)
    animation_names: List[str] = field(default_factory=list)
    bbox_min: Optional[List[float]] = None
    bbox_max: Optional[List[float]] = None
    detected_up_axis: str = "unknown"


@dataclass
class AudioMetadata:
    file_format: str
    sample_rate_hz: int = 0
    channels: int = 0
    sample_width_bits: int = 0
    frame_count: int = 0
    duration_sec: float = 0.0


@dataclass
class TextureMetadata:
    file_format: str
    width: int = 0
    height: int = 0
    bit_depth: int = 0
    pbr_map_type: str = "unknown"


@dataclass
class PreparedAsset:
    asset_id: str
    name: str
    category: str
    subcategory: str
    relative_path: str
    source_url: str
    creator: str
    license_spdx: str
    redistribution_allowed: bool
    file_format: str
    size_bytes: int
    sha256: str
    model_meta: Optional[ModelMetadata] = None
    audio_meta: Optional[AudioMetadata] = None
    texture_meta: Optional[TextureMetadata] = None
    warnings: List[str] = field(default_factory=list)
    is_duplicate_of: Optional[str] = None


@dataclass
class AcquisitionSummary:
    total_catalog_entries: int = 0
    downloaded_count: int = 0
    skipped_manual_count: int = 0
    failed_count: int = 0
    duplicate_count: int = 0
    total_bytes: int = 0
    errors: List[str] = field(default_factory=list)
