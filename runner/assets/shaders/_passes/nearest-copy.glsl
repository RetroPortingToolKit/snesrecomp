// SPDX-License-Identifier: CC0-1.0
// Present an integer-scaled filter result without adding another color filter.
#if defined(VERTEX)
in vec4 VertexCoord;
in vec2 TexCoord;
out vec2 SourceCoord;
uniform mat4 MVPMatrix;
void main() {
    gl_Position = MVPMatrix * VertexCoord;
    SourceCoord = TexCoord;
}
#elif defined(FRAGMENT)
in vec2 SourceCoord;
out vec4 FragColor;
uniform sampler2D Texture;
void main() {
    FragColor = texture(Texture, SourceCoord);
}
#endif
