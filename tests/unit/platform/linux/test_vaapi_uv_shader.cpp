/**
 * @file tests/unit/platform/linux/test_vaapi_uv_shader.cpp
 * @brief Pixel regression for the production OpenGL 4:2:0 conversion shader.
 */
#include "../../../tests_common.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {
  struct egl_context_t {
    EGLDisplay display;
    EGLContext context {EGL_NO_CONTEXT};

    ~egl_context_t() {
      if (context != EGL_NO_CONTEXT) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
      }
      eglTerminate(display);
    }
  };

  GLuint compile_shader(GLenum type, const char *path) {
    std::ifstream file(path);
    std::string text {std::istreambuf_iterator<char>(file), {}};
    const char *source = text.c_str();
    auto shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
      char log[4096] {};
      glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
      ADD_FAILURE() << path << ": " << log;
    }
    return shader;
  }

  void store_texture(GLuint texture, GLenum unit, int width, int height, const unsigned char *pixels) {
    glActiveTexture(unit);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  }

  TEST(VaapiUvShader, SamplesFullOddHeightWithoutMovingInteriorCenters) {
    auto display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
      GTEST_SKIP() << "A surfaceless EGL display is unavailable";
    }
    egl_context_t egl {display};
    const EGLint config_attrs[] {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig config;
    EGLint config_count;
    if (!eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(display, config_attrs, &config, 1, &config_count) || config_count == 0) {
      GTEST_SKIP() << "A surfaceless GLES 3 configuration is unavailable";
    }
    const EGLint context_attrs[] {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    egl.context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    if (egl.context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl.context)) {
      GTEST_SKIP() << "A surfaceless GLES 3 context is unavailable";
    }

    auto program = glCreateProgram();
    glAttachShader(program, compile_shader(GL_VERTEX_SHADER, VAAPI_TEST_SHADER_DIR "/ConvertUV.vert"));
    glAttachShader(program, compile_shader(GL_FRAGMENT_SHADER, VAAPI_TEST_SHADER_DIR "/ConvertUV.frag"));
    glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    ASSERT_TRUE(linked);
    glUseProgram(program);
    glUniform1f(glGetUniformLocation(program, "width_i"), 0.5f);
    glUniform1i(glGetUniformLocation(program, "image"), 0);
    glUniform1i(glGetUniformLocation(program, "output_lut"), 1);
    auto delta_location = glGetUniformLocation(program, "chroma_coord_delta");
    ASSERT_GE(delta_location, 0);

    // Use an identity LUT and output the source red value as U.
    auto block = glGetUniformBlockIndex(program, "ColorMatrix");
    ASSERT_NE(block, GL_INVALID_INDEX);
    GLint byte_count;
    GLint count;
    glGetActiveUniformBlockiv(program, block, GL_UNIFORM_BLOCK_DATA_SIZE, &byte_count);
    glGetActiveUniformBlockiv(program, block, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &count);
    std::vector<GLint> members(count);
    glGetActiveUniformBlockiv(program, block, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, members.data());
    std::vector<char> matrix(byte_count);
    for (GLuint member : members) {
      char name[128];
      GLsizei array_size;
      GLenum type;
      glGetActiveUniform(program, member, sizeof(name), nullptr, &array_size, &type, name);
      if (std::strstr(name, "color_vec_u") || std::strstr(name, "range_uv")) {
        GLint offset;
        glGetActiveUniformsiv(program, 1, &member, GL_UNIFORM_OFFSET, &offset);
        const float one = 1.0f;
        std::memcpy(matrix.data() + offset, &one, sizeof(one));
      }
    }
    GLuint uniform_buffer;
    glGenBuffers(1, &uniform_buffer);
    glBindBuffer(GL_UNIFORM_BUFFER, uniform_buffer);
    glBufferData(GL_UNIFORM_BUFFER, matrix.size(), matrix.data(), GL_STATIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, uniform_buffer);
    glUniformBlockBinding(program, block, 0);

    constexpr int chroma_height = 590;
    GLuint textures[3];
    glGenTextures(3, textures);
    const unsigned char lut[] {0, 0, 0, 255, 255, 255, 255, 255};
    store_texture(textures[1], GL_TEXTURE1, 2, 1, lut);
    store_texture(textures[2], GL_TEXTURE2, 1, chroma_height, nullptr);
    GLuint framebuffer;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[2], 0);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
    glViewport(0, 0, 1, chroma_height);

    auto sample = [&](int height, float delta) {
      // Two identical columns isolate vertical sampling. The step at row 900
      // reveals a moved interior center. A red last row reveals edge clamping.
      std::vector<unsigned char> source(2 * height * 4);
      for (int y = 0; y < height; ++y) {
        auto red = ((y >= 900 && y < 1177) || y == height - 1) ? 255 : 0;
        source[8 * y] = source[8 * y + 4] = red;
      }
      store_texture(textures[0], GL_TEXTURE0, 2, height, source.data());
      glUniform2f(delta_location, 0.0f, delta);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      std::array<unsigned char, chroma_height * 4> result;
      glReadPixels(0, 0, 1, chroma_height, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
      EXPECT_EQ(glGetError(), GL_NO_ERROR);
      return std::pair {result[4 * 450], result[4 * 589]};
    };

    const auto odd_control = sample(1179, 0.0f);
    const auto odd = sample(1179, 1.0f / 1179);
    const auto even = sample(1180, 0.0f);
    EXPECT_LT(odd_control.first, 220);
    EXPECT_LT(odd_control.second, 180);
    EXPECT_EQ(odd.first, 255);
    EXPECT_EQ(odd.second, 255);
    EXPECT_EQ(even.first, 255);
    EXPECT_NEAR(even.second, 128, 1);
  }
}  // namespace
