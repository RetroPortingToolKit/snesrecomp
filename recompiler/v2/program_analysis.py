"""The whole-program analysis manifest: variant keys, node summaries, demands.

The native analyzer (recompiler-rs, `snesrecomp-analyze`) produces the
manifest; these types are its Python-side contract, used by the emitter to
decide which exact ``(pc24, M, X)`` variants get AOT bodies and which stay on
the interpreter. Dynamic or unresolved transfers remain explicit LLE edges
instead of inventing a generated target variant.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
import json
from typing import Mapping, Optional, Tuple


class NodeDisposition(str, Enum):
    """Materialization state from the LLE-first analysis contract."""

    LLE_ONLY = "lle_only"
    AOT_ELIGIBLE = "aot_eligible"
    AOT_EMITTED = "aot_emitted"
    HLE_OVERLAY = "hle_overlay"


class EdgeKind(str, Enum):
    DIRECT_CALL = "direct_call"
    DIRECT_TAIL_CALL = "direct_tail_call"
    STATIC_DISPATCH = "static_dispatch"
    DYNAMIC_DISPATCH = "dynamic_dispatch"
    UNRESOLVED_INDIRECT = "unresolved_indirect"
    SUPPRESSED_INDIRECT_CALL = "suppressed_indirect_call"


class EdgeResolution(str, Enum):
    AOT_EXACT = "aot_exact"
    LLE_EXACT = "lle_exact"
    LLE_DYNAMIC = "lle_dynamic"


@dataclass(frozen=True, order=True)
class VariantKey:
    pc24: int
    m: int
    x: int

    def __post_init__(self) -> None:
        object.__setattr__(self, "pc24", self.pc24 & 0xFFFFFF)
        object.__setattr__(self, "m", self.m & 1)
        object.__setattr__(self, "x", self.x & 1)

    @property
    def manifest_key(self) -> str:
        return f"{self.pc24:06X}:M{self.m}X{self.x}"


@dataclass(frozen=True, order=True)
class DemandEdge:
    site_pc24: int
    kind: EdgeKind
    resolution: EdgeResolution
    target: Optional[VariantKey] = None
    detail: str = ""

    def __post_init__(self) -> None:
        object.__setattr__(self, "site_pc24", self.site_pc24 & 0xFFFFFF)


@dataclass(frozen=True)
class NodeSummary:
    key: VariantKey
    disposition: NodeDisposition
    instruction_count: int
    min_pc24: int
    max_pc24: int
    demands: Tuple[DemandEdge, ...] = ()
    reasons: Tuple[str, ...] = ()
    digest: str = ""

    @property
    def propagates_demands(self) -> bool:
        # Structural poison is a wrong-width/invalid decode and must not
        # manufacture transitive reachability.  Ordinary LLE-only nodes can
        # still carry proven direct demands.
        return "structural_poison" not in self.reasons


@dataclass(frozen=True)
class ProgramManifest:
    roots: Tuple[VariantKey, ...]
    nodes: Mapping[VariantKey, NodeSummary]
    # Exact architectural exit state proven for each reachable entry.  Calls
    # are decoded against this fixed point; keeping it in the manifest makes
    # the analysis result self-contained and the emitter/cache independent of
    # hidden mutable cfg feedback.
    exit_modes: Mapping[VariantKey, Tuple[int, int]] = field(
        default_factory=dict)
    # Proven multi-mode exit sets for entries whose return paths provably
    # disagree on (m, x) (conditional SEP/REP callees).  Callers fork the
    # post-call continuation across the set and dispatch on the live width
    # at runtime.  Disjoint from exit_modes: an exact proof supersedes.
    exit_mode_sets: Mapping[VariantKey, frozenset] = field(
        default_factory=dict)
    format_version: int = 3

    def to_dict(self) -> dict:
        def key_dict(key: VariantKey) -> dict:
            return {"pc24": key.pc24, "m": key.m, "x": key.x}

        def edge_dict(edge: DemandEdge) -> dict:
            return {
                "site_pc24": edge.site_pc24,
                "kind": edge.kind.value,
                "resolution": edge.resolution.value,
                "target": (key_dict(edge.target) if edge.target else None),
                "detail": edge.detail,
            }

        def node_dict(node: NodeSummary) -> dict:
            return {
                "key": key_dict(node.key),
                "disposition": node.disposition.value,
                "instruction_count": node.instruction_count,
                "min_pc24": node.min_pc24,
                "max_pc24": node.max_pc24,
                "demands": [edge_dict(e) for e in node.demands],
                "reasons": list(node.reasons),
                "digest": node.digest,
            }

        return {
            "format_version": self.format_version,
            "roots": [key_dict(k) for k in sorted(self.roots)],
            "exit_modes": {
                key.manifest_key: {"m": pair[0] & 1, "x": pair[1] & 1}
                for key, pair in sorted(self.exit_modes.items())
            },
            "exit_mode_sets": {
                key.manifest_key: [
                    {"m": m & 1, "x": x & 1}
                    for m, x in sorted(mode_set)
                ]
                for key, mode_set in sorted(self.exit_mode_sets.items())
            },
            "nodes": {
                key.manifest_key: node_dict(self.nodes[key])
                for key in sorted(self.nodes)
            },
        }

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), indent=2, sort_keys=True) + "\n"

    @classmethod
    def from_dict(cls, value: Mapping) -> "ProgramManifest":
        """Load the stable manifest wire format from any analyzer backend."""
        version = int(value.get("format_version", 0))
        if version != 3:
            raise ValueError(
                f"unsupported program manifest format_version={version}")

        def parse_key(item) -> VariantKey:
            return VariantKey(
                int(item["pc24"]), int(item["m"]), int(item["x"]))

        def parse_manifest_key(text: str) -> VariantKey:
            try:
                address, modes = text.split(":", 1)
                return VariantKey(
                    int(address, 16), int(modes[1]), int(modes[3]))
            except (IndexError, TypeError, ValueError) as exc:
                raise ValueError(f"invalid manifest variant key {text!r}") \
                    from exc

        nodes = {}
        for text_key, item in value.get("nodes", {}).items():
            key = parse_key(item["key"])
            if key != parse_manifest_key(text_key):
                raise ValueError(
                    f"manifest node key mismatch for {text_key!r}")
            demands = []
            for edge in item.get("demands", ()):
                demands.append(DemandEdge(
                    site_pc24=int(edge["site_pc24"]),
                    kind=EdgeKind(edge["kind"]),
                    resolution=EdgeResolution(edge["resolution"]),
                    target=(parse_key(edge["target"])
                            if edge.get("target") is not None else None),
                    detail=str(edge.get("detail", "")),
                ))
            nodes[key] = NodeSummary(
                key=key,
                disposition=NodeDisposition(item["disposition"]),
                instruction_count=int(item["instruction_count"]),
                min_pc24=int(item["min_pc24"]),
                max_pc24=int(item["max_pc24"]),
                demands=tuple(demands),
                reasons=tuple(str(reason)
                              for reason in item.get("reasons", ())),
                digest=str(item.get("digest", "")),
            )
        exit_modes = {
            parse_manifest_key(text_key): (int(item["m"]), int(item["x"]))
            for text_key, item in value.get("exit_modes", {}).items()
        }
        exit_mode_sets = {
            parse_manifest_key(text_key): frozenset(
                (int(item["m"]), int(item["x"])) for item in mode_set)
            for text_key, mode_set in value.get("exit_mode_sets", {}).items()
        }
        return cls(
            roots=tuple(sorted(parse_key(item)
                               for item in value.get("roots", ()))),
            nodes=nodes,
            exit_modes=exit_modes,
            exit_mode_sets=exit_mode_sets,
            format_version=version,
        )
