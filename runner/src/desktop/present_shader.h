#pragma once

/* Shared by the stock desktop presenter and game-owned HD compositors.
 * Keep nearest HD resolve on the same exact integer pixel centers as the
 * visible-sample renderer. Interpolated UVs can round below a texel boundary
 * at noninteger window scales. Linear filtering and presets keep normal UVs.
 * nearestViewport is the GL viewport (bottom-left origin); source row0 is top. */
static inline const char *SnesGlPresentFragmentShader(void) {
  return "#version 330 core\n"
      "out vec4 FragColor; in vec2 TexCoord; uniform sampler2D texture1;\n"
      "uniform bool exactNearest; uniform ivec4 nearestViewport;\n"
      "void main(void) {\n"
      "  if (exactNearest) {\n"
      "    ivec2 p=ivec2(gl_FragCoord.xy)-nearestViewport.xy;\n"
      "    p.y=nearestViewport.w-1-p.y;\n"
      "    ivec2 size=textureSize(texture1,0);\n"
      "    ivec2 at=((2*p+1)*size)/(2*nearestViewport.zw);\n"
      "    FragColor=texelFetch(texture1,clamp(at,ivec2(0),size-1),0);\n"
      "  } else FragColor=texture(texture1,TexCoord);\n"
      "}\n";
}
