#version 330 core
out vec4 FragColor;
uniform sampler2D sceneTexture;   // ray-traced frame; alpha = 1 where the ray escaped to the sky
uniform vec2 viewportSize;
void main() {
    float sky = texture(sceneTexture, gl_FragCoord.xy / viewportSize).a;
    FragColor = vec4(0.45, 0.55, 0.75, 0.35 * sky);   // translucent blue-grey spacetime grid
}
