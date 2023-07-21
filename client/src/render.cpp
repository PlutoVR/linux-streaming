// Copyright 2023, Pluto VR, Inc.
// SPDX-License-Identifier: MIT
/*!
 * @file
 * @brief Very simple GLES3 renderer for WebRTC client.
 * @author Moshi Turner <moses@collabora.com>
 */

#include <cstdio>
#include "render.hpp"

// Initialize EGL context. We'll need this going forward.
void
initializeEGL(struct state_t &state)
{
	state.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

	if (state.display == EGL_NO_DISPLAY) {
		printf("Failed to get EGL display");
		return;
	}

	bool success = eglInitialize(state.display, NULL, NULL);

	if (!success) {
		printf("Failed to initialize EGL");
		return;
	}

	EGLint configCount;
	EGLConfig configs[1024];
	success = eglGetConfigs(state.display, configs, 1024, &configCount);

	if (!success) {
		printf("Failed to get EGL configs");
		return;
	}

	const EGLint attributes[] = {EGL_RED_SIZE,   8, EGL_GREEN_SIZE,   8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
	                             EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0, EGL_SAMPLES,   0, EGL_NONE};

	for (EGLint i = 0; i < configCount && !state.config; i++) {
		EGLint renderableType;
		EGLint surfaceType;

		eglGetConfigAttrib(state.display, configs[i], EGL_RENDERABLE_TYPE, &renderableType);
		eglGetConfigAttrib(state.display, configs[i], EGL_SURFACE_TYPE, &surfaceType);

		if ((renderableType & EGL_OPENGL_ES3_BIT) == 0) {
			continue;
		}

		if ((surfaceType & (EGL_PBUFFER_BIT | EGL_WINDOW_BIT)) != (EGL_PBUFFER_BIT | EGL_WINDOW_BIT)) {
			continue;
		}

		for (size_t a = 0; a < sizeof(attributes) / sizeof(attributes[0]); a += 2) {
			if (attributes[a] == EGL_NONE) {
				state.config = configs[i];
				break;
			}

			EGLint value;
			eglGetConfigAttrib(state.display, configs[i], attributes[a], &value);
			if (value != attributes[a + 1]) {
				break;
			}
		}
	}

	if (!state.config) {
		printf("Failed to find suitable EGL config");
	}

	EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};

	if ((state.context = eglCreateContext(state.display, state.config, EGL_NO_CONTEXT, contextAttributes)) ==
	    EGL_NO_CONTEXT) {
		printf("Failed to create EGL context");
	}

	EGLint surfaceAttributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};

	if ((state.surface = eglCreatePbufferSurface(state.display, state.config, surfaceAttributes)) ==
	    EGL_NO_SURFACE) {
		printf("Failed to create EGL surface");
		eglDestroyContext(state.display, state.context);
		return;
	}

	if (eglMakeCurrent(state.display, state.surface, state.surface, state.context) == EGL_FALSE) {
		printf("Failed to make EGL context current");
		eglDestroySurface(state.display, state.surface);
		eglDestroyContext(state.display, state.context);
	}
}

// Vertex shader source code
const GLchar *vertexShaderSource = R"(
    #version 300 es
    in vec3 position;
    in vec2 uv;
    out vec2 frag_uv;

    void main() {
        gl_Position = vec4(position, 1.0);
        frag_uv = uv;
    }
)";

// Fragment shader source code
const GLchar *fragmentShaderSource = R"(
    #version 300 es
    precision lowp float;

    in vec2 frag_uv;
    out vec4 frag_color;
    uniform sampler2D textureSampler;

    void main() {
        frag_color = texture(textureSampler, frag_uv);
    }
)";

GLuint shaderProgram;
GLuint quadVAO, quadVBO;

// Function to check shader compilation errors
void
checkShaderCompilation(GLuint shader)
{
	GLint success;
	GLchar infoLog[512];
	glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
	if (!success) {
		glGetShaderInfoLog(shader, 512, NULL, infoLog);
		printf("Shader compilation failed: %s\n", infoLog);
	}
}

// Function to check shader program linking errors
void
checkProgramLinking(GLuint program)
{
	GLint success;
	GLchar infoLog[512];
	glGetProgramiv(program, GL_LINK_STATUS, &success);
	if (!success) {
		glGetProgramInfoLog(program, 512, NULL, infoLog);
		printf("Shader program linking failed: %s\n", infoLog);
	}
}

void
setupShaders()
{
	// Compile the vertex shader
	GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(vertexShader, 1, &vertexShaderSource, NULL);
	glCompileShader(vertexShader);
	checkShaderCompilation(vertexShader);

	// Compile the fragment shader
	GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL);
	glCompileShader(fragmentShader);
	checkShaderCompilation(fragmentShader);

	// Create and link the shader program
	shaderProgram = glCreateProgram();
	glAttachShader(shaderProgram, vertexShader);
	glAttachShader(shaderProgram, fragmentShader);
	glLinkProgram(shaderProgram);
	checkProgramLinking(shaderProgram);

	// Clean up the shaders as they're no longer needed
	glDeleteShader(vertexShader);
	glDeleteShader(fragmentShader);
}

void
setupQuadVertexData()
{
	// Set up the quad vertex data
#if 1
	// old, chatgpt
	GLfloat quadVertices[] = {// Positions    // UVs
	                          -1.0f, 1.0f,  0.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f,
	                          1.0f,  -1.0f, 0.0f, 1.0f, 0.0f, 1.0f,  1.0f,  0.0f, 1.0f, 1.0f};
#else
	// also chatgpt
	GLfloat quadVertices[] = {// Positions    // Texture Coords
	                          -1.0f, -1.0f, 0.0f, 0.0f, 1.0f,  -1.0f, 1.0f, 0.0f,
	                          1.0f,  1.0f,  1.0f, 1.0f, -1.0f, 1.0f,  0.0f, 1.0f};
#endif

	glGenVertexArrays(1, &quadVAO);
	glGenBuffers(1, &quadVBO);

	glBindVertexArray(quadVAO);
	glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), (GLvoid *)0);
	glEnableVertexAttribArray(0);

	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), (GLvoid *)(3 * sizeof(GLfloat)));
	glEnableVertexAttribArray(1);

	glBindVertexArray(0);
}

void
setupRender()
{
	setupShaders();
	setupQuadVertexData();
}

void
draw(GLuint framebuffer, GLuint texture)
{
	//    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);

	// Use the shader program
	glUseProgram(shaderProgram);

	// Bind the texture
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glUniform1i(glGetUniformLocation(shaderProgram, "textureSampler"), 0);

	// Draw the quad
	glBindVertexArray(quadVAO);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	glBindVertexArray(0);

	GLenum err;
	while ((err = glGetError()) != GL_NO_ERROR) {
		const char *errorStr;
		switch (err) {
		case GL_INVALID_ENUM: errorStr = "GL_INVALID_ENUM"; break;
		case GL_INVALID_VALUE: errorStr = "GL_INVALID_VALUE"; break;
		case GL_INVALID_OPERATION: errorStr = "GL_INVALID_OPERATION"; break;
		case GL_INVALID_FRAMEBUFFER_OPERATION: errorStr = "GL_INVALID_FRAMEBUFFER_OPERATION"; break;
		case GL_OUT_OF_MEMORY: errorStr = "GL_OUT_OF_MEMORY"; break;
		default: errorStr = "Unknown error"; break;
		}
		printf("error! %s", errorStr);
	}

	// Unbind the framebuffer
	//    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}