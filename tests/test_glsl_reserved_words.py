"""GLSL sources embedded in the desktop presenter must not use reserved words.

GLSL reserves words such as `half`, `fixed` or `input` for future use. Some
drivers accept them as identifiers anyway, others reject the whole shader. On
Intel UHD Graphics a variable named `half` made the HD composition shader fail
to compile, so the GPU path silently fell back to the CPU path.
"""
import pathlib
import re

REPO = pathlib.Path(__file__).resolve().parents[1]
HD_GPU = REPO / "runner" / "src" / "desktop" / "hd_gpu.c"

# GLSL 4.60 specification, section 3.6 "Keywords": reserved for future use.
RESERVED = set("""
common partition active asm class union enum typedef template this resource
goto inline noinline public static extern external interface long short half
fixed unsigned superp input output hvec2 hvec3 hvec4 fvec2 fvec3 fvec4
sampler3DRect filter sizeof cast namespace using
""".split())

SOURCE = re.compile(r"static const char \*(\w+_source) =")
LITERAL = re.compile(r'\s*"((?:[^"\\]|\\.)*)"')
IDENT = re.compile(r"\b[A-Za-z_]\w*\b")


def _shader_sources():
    """Each `static const char *<name>_source =` and the adjacent string
    literals that make up its initializer."""
    text = HD_GPU.read_text(encoding="utf-8")
    sources = {}
    for match in SOURCE.finditer(text):
        pos, parts = match.end(), []
        while (literal := LITERAL.match(text, pos)):
            parts.append(literal.group(1))
            pos = literal.end()
        sources[match.group(1)] = "".join(parts)
    return sources


def test_shader_sources_are_found():
    sources = _shader_sources()
    assert {"vertex_source", "fragment_source", "coordinate_source"} <= set(sources)
    assert all("void main" in glsl for glsl in sources.values()), \
        {name: len(glsl) for name, glsl in sources.items()}


def test_no_reserved_words_in_shader_sources():
    used = {name: sorted(RESERVED & set(IDENT.findall(glsl)))
            for name, glsl in _shader_sources().items()}
    assert not any(used.values()), used
