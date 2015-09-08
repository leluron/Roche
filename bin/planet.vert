#version 330
layout(location = 0)in vec4 in_position;
layout(location = 1)in vec2 in_uv;

uniform mat4 viewprojMat;
uniform mat4 modelMat;

out vec2 pass_uv;
out vec3 pass_normal;
void main(void)
{
	pass_uv = in_uv;
	pass_normal = mat3(modelMat)*in_position.xyz;
	gl_Position = viewprojMat*modelMat*in_position;
}