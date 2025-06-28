#include <iostream>
#include <vector>
#include <string>
#include <fcntl.h>      // For open()
#include <unistd.h>     // For close(), mmap(), munmap()
#include <sys/ioctl.h>  // For ioctl()
#include <sys/mman.h>   // For mmap(), munmap()
#include <chrono>       // For timing (optional, but good for debugging)
#include <thread>       // For sleep_for (optional)

// DRM includes (you'll need to link against libdrm)
//#include <xf86drm.h>
//#include <xf86drmMode.h>
#include <libdrm/drm.h>
#include <libdrm/drm_mode.h>
#include <libdrm/drm_fourcc.h> // Add this line
#include <gbm.h>        // GBM (Generic Buffer Management) is often used with DRM/EGL

// Add these definitions if your gbm.h or drm.h doesn't define them
// These values are standard for DRM/GBM FourCC codes and usage flags.
#ifndef GBM_FORMAT_ARGB8888
#define GBM_FORMAT_ARGB8888 DRM_FORMAT_ARGB8888 // GBM format usually mirrors DRM format
#endif

#ifndef GBM_BO_USE_TEXTURE
#define GBM_BO_USE_TEXTURE (1 << 4) // Common value for this flag in GBM
#endif



// EGL includes
#include <EGL/egl.h>
#include <EGL/eglext.h> // For EGL_LINUX_DMA_BUF_EXT

// GLES2 includes
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h> // For glEGLImageTargetTexture2DOES

// --- Global EGL/GL variables ---
EGLDisplay egl_display = EGL_NO_DISPLAY;
EGLContext egl_context = EGL_NO_CONTEXT;
EGLSurface egl_surface = EGL_NO_SURFACE;

// --- DRM/GBM related variables ---
int drm_fd = -1;
struct gbm_device *gbm_dev = nullptr;
struct gbm_bo *gbm_bo = nullptr; // GBM Buffer Object for zero-copy
GLuint texture_id = 0;
EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;

// --- EGL Extension Pointers (must be loaded at runtime) ---
// These are defined in eglext.h but need to be fetched via eglGetProcAddress
PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ptr = nullptr;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ptr = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ptr = nullptr;

// --- GLES2 Shader Program ---
GLuint program_object = 0;
GLuint vertex_shader = 0;
GLuint fragment_shader = 0;
GLint position_loc = -1;
GLint texcoord_loc = -1;
GLint sampler_loc = -1;



// --- Helper function for EGL error checking ---
const char* egl_error_string(EGLint error) {
    switch (error) {
        case EGL_SUCCESS: return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
        case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
        case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
        case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
        default: return "Unknown EGL Error";
    }
}

// --- Helper function to load EGL extensions ---
bool load_egl_extensions() {
    eglCreateImageKHR_ptr = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR_ptr = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES_ptr = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

    if (!eglCreateImageKHR_ptr || !eglDestroyImageKHR_ptr || !glEGLImageTargetTexture2DOES_ptr) {
        std::cerr << "ERROR: Failed to load required EGL extensions (eglCreateImageKHR, eglDestroyImageKHR, glEGLImageTargetTexture2DOES)." << std::endl;
        return false;
    }
    return true;
}

// --- Shader compilation helper ---
GLuint load_shader(GLenum type, const char* shader_src) {
    GLuint shader;
    GLint compiled;

    shader = glCreateShader(type);
    if (shader == 0) return 0;

    glShaderSource(shader, 1, &shader_src, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);

    if (!compiled) {
        GLint info_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 1) {
            std::vector<char> info_log(info_len);
            glGetShaderInfoLog(shader, info_len, NULL, info_log.data());
            std::cerr << "Error compiling shader:\n" << info_log.data() << std::endl;
        }
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// --- Initialize GLES2 shaders and program ---
bool setup_gles_program() {
    const char v_shader_str[] =
        "attribute vec4 a_position;   \n"
        "attribute vec2 a_texcoord;   \n"
        "varying vec2 v_texcoord;     \n"
        "void main()                  \n"
        "{                            \n"
        "   gl_Position = a_position; \n"
        "   v_texcoord = a_texcoord;  \n"
        "}                            \n";

    const char f_shader_str[] =
        "precision mediump float;     \n"
        "varying vec2 v_texcoord;     \n"
        "uniform sampler2D s_texture; \n"
        "void main()                  \n"
        "{                            \n"
        "   gl_FragColor = texture2D(s_texture, v_texcoord); \n"
        "}                            \n";

    vertex_shader = load_shader(GL_VERTEX_SHADER, v_shader_str);
    fragment_shader = load_shader(GL_FRAGMENT_SHADER, f_shader_str);

    if (!vertex_shader || !fragment_shader) {
        std::cerr << "Failed to load shaders." << std::endl;
        return false;
    }

    program_object = glCreateProgram();
    if (program_object == 0) {
        std::cerr << "Failed to create program object." << std::endl;
        return false;
    }

    glAttachShader(program_object, vertex_shader);
    glAttachShader(program_object, fragment_shader);
    glLinkProgram(program_object);

    GLint linked;
    glGetProgramiv(program_object, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint info_len = 0;
        glGetProgramiv(program_object, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 1) {
            std::vector<char> info_log(info_len);
            glGetProgramInfoLog(program_object, info_len, NULL, info_log.data());
            std::cerr << "Error linking program:\n" << info_log.data() << std::endl;
        }
        glDeleteProgram(program_object);
        return false;
    }

    // Get attribute and uniform locations
    position_loc = glGetAttribLocation(program_object, "a_position");
    texcoord_loc = glGetAttribLocation(program_object, "a_texcoord");
    sampler_loc = glGetUniformLocation(program_object, "s_texture");

    if (position_loc == -1 || texcoord_loc == -1 || sampler_loc == -1) {
        std::cerr << "Failed to get shader attribute/uniform locations." << std::endl;
        return false;
    }

    return true;
}

// --- Initialization ---
bool init_egl_gles(int width, int height) {
    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_display == EGL_NO_DISPLAY) {
        std::cerr << "ERROR: Failed to get EGL display. Error: " << egl_error_string(eglGetError()) << std::endl;
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(egl_display, &major, &minor)) {
        std::cerr << "ERROR: Failed to initialize EGL. Error: " << egl_error_string(eglGetError()) << std::endl;
        return false;
    }
    std::cout << "EGL initialized. Version: " << major << "." << minor << std::endl;

    if (!load_egl_extensions()) {
        return false;
    }

    EGLConfig config;
    EGLint num_configs;
    // EGL_SURFACE_TYPE can be EGL_WINDOW_BIT if connecting to a native window,
    // but for a minimal example and headless operation, PBUFFER is simpler.
    EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, // Using PBUFFER for headless rendering
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };

    if (!eglChooseConfig(egl_display, config_attribs, &config, 1, &num_configs) || num_configs == 0) {
        std::cerr << "ERROR: Failed to choose EGL config. Error: " << egl_error_string(eglGetError()) << std::endl;
        return false;
    }

    EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attribs);
    if (egl_context == EGL_NO_CONTEXT) {
        std::cerr << "ERROR: Failed to create EGL context. Error: " << egl_error_string(eglGetError()) << std::endl;
        return false;
    }

    EGLint pbuffer_attribs[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    egl_surface = eglCreatePbufferSurface(egl_display, config, pbuffer_attribs);
    if (egl_surface == EGL_NO_SURFACE) {
        std::cerr << "ERROR: Failed to create EGL pbuffer surface. Error: " << egl_error_string(eglGetError()) << std::endl;
        return false;
    }

    if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
        std::cerr << "ERROR: Failed to make EGL context current. Error: " << egl_error_string(eglGetError()) << std::endl;
        return false;
    }

    glViewport(0, 0, width, height);
    return true;
}

// --- Initialize DRM/GBM and prepare for DMA_BUF ---
bool init_drm_gbm(int width, int height) {
    drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        std::cerr << "ERROR: Failed to open DRM device (/dev/dri/card0). Errno: " << errno << std::endl;
        std::cerr << "       Ensure your user has permissions or run with sudo. Also ensure vc4 driver is loaded." << std::endl;
        return false;
    }

    gbm_dev = gbm_create_device(drm_fd);
    if (!gbm_dev) {
        std::cerr << "ERROR: Failed to create GBM device. Errno: " << errno << std::endl;
        close(drm_fd); drm_fd = -1;
        return false;
    }

    // Allocate a GBM buffer object that can be used for zero-copy
    // Use DRM_FORMAT_ARGB8888 for 32-bit RGBA. GBM_BO_USE_LINEAR for CPU access.
    gbm_bo = gbm_bo_create(gbm_dev, width, height, GBM_FORMAT_ARGB8888, /*GBM_BO_USE_TEXTURE |*/ GBM_BO_USE_LINEAR);
    if (!gbm_bo) {
        std::cerr << "ERROR: Failed to create GBM buffer object. Errno: " << errno << std::endl;
        gbm_device_destroy(gbm_dev); gbm_dev = nullptr;
        close(drm_fd); drm_fd = -1;
        return false;
    }
    std::cout << "GBM buffer object created (width=" << width << ", height=" << height << ", stride=" << gbm_bo_get_stride(gbm_bo) << ")" << std::endl;

    return true;
}


// --- Create Texture from DMA_BUF ---
bool create_dma_buf_texture(int width, int height) {
    int dma_buf_fd = gbm_bo_get_fd(gbm_bo);
    if (dma_buf_fd < 0) {
        std::cerr << "ERROR: Failed to get DMA_BUF FD from GBM BO. Errno: " << errno << std::endl;
        return false;
    }

    uint32_t stride = gbm_bo_get_stride(gbm_bo);

    EGLAttrib attribs[] = {
        EGL_WIDTH, (EGLAttrib)width,
        EGL_HEIGHT, (EGLAttrib)height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, (EGLAttrib)dma_buf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0, // Single plane, offset 0
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLAttrib)stride,
        EGL_NONE
    };

    egl_image = eglCreateImageKHR_ptr(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL, attribs);
    if (egl_image == EGL_NO_IMAGE_KHR) {
        std::cerr << "ERROR: Failed to create EGLImage from DMA_BUF. EGL error: " << egl_error_string(eglGetError()) << std::endl;
        close(dma_buf_fd); // Close the FD as EGLImage creation failed
        return false;
    }
    std::cout << "EGLImageKHR created from DMA_BUF." << std::endl;

    // DMA_BUF FD can be closed after EGLImageKHR is created, as EGL takes ownership
    close(dma_buf_fd);

    glGenTextures(1, &texture_id);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glEGLImageTargetTexture2DOES_ptr(GL_TEXTURE_2D, egl_image);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::cerr << "ERROR: glEGLImageTargetTexture2DOES failed: " << err << std::endl;
        return false;
    }
    std::cout << "GLES texture created from EGLImageKHR." << std::endl;

    return true;
}

// --- Update Texture Data (CPU writes to mapped buffer) ---
void update_dma_buf_data(int width, int height, int frame_idx) {
    // Map the GBM BO to CPU address space
    // GBM_BO_TRANSFER_WRITE hints that we are writing data
    void *cpu_map_ptr = gbm_bo_map(gbm_bo, 0, 0, width, height, GBM_BO_TRANSFER_WRITE, NULL, NULL);
    if (!cpu_map_ptr) {
        std::cerr << "ERROR: Failed to map GBM BO." << std::endl;
        return;
    }

    uint32_t stride = gbm_bo_get_stride(gbm_bo);
    unsigned char *pixels = static_cast<unsigned char*>(cpu_map_ptr);

    // Simulate PNG decode into the mapped buffer
    // IMPORTANT: Respect 'stride' for each row.
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Simple animated gradient (RGBA)
            pixels[y * stride + x * 4 + 0] = (unsigned char)((x + frame_idx * 5) % 256); // R
            pixels[y * stride + x * 4 + 1] = (unsigned char)((y + frame_idx * 3) % 256); // G
            pixels[y * stride + x * 4 + 2] = (unsigned char)((frame_idx * 10) % 256);   // B
            pixels[y * stride + x * 4 + 3] = 255;                                        // A
        }
    }

    gbm_bo_unmap(gbm_bo, cpu_map_ptr);
}

// --- Render Frame ---
void render_frame(int width, int height) {
    glClearColor(0.2f, 0.2f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(program_object);

    // Vertices for a full-screen quad
    GLfloat vertices[] = {
        -1.0f,  1.0f, 0.0f, // Top-left
        -1.0f, -1.0f, 0.0f, // Bottom-left
         1.0f, -1.0f, 0.0f, // Bottom-right
         1.0f,  1.0f, 0.0f  // Top-right
    };

    // Texture coordinates
    GLfloat texcoords[] = {
        0.0f, 0.0f, // Top-left
        0.0f, 1.0f, // Bottom-left
        1.0f, 1.0f, // Bottom-right
        1.0f, 0.0f  // Top-right
    };

    // Indices for drawing two triangles to form a quad
    GLushort indices[] = { 0, 1, 2, 0, 2, 3 };

    glVertexAttribPointer(position_loc, 3, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(position_loc);

    glVertexAttribPointer(texcoord_loc, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
    glEnableVertexAttribArray(texcoord_loc);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    glUniform1i(sampler_loc, 0);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);

    // For a pbuffer, you'd typically read the pixels back here for display/saving.
    // For a window surface, eglSwapBuffers would be called.
    // E.g., read back to a CPU buffer: glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixel_buffer);
}

// --- Cleanup ---
void cleanup() {
    std::cout << "Cleaning up..." << std::endl;

    if (program_object) glDeleteProgram(program_object);
    if (vertex_shader) glDeleteShader(vertex_shader);
    if (fragment_shader) glDeleteShader(fragment_shader);
    if (texture_id) glDeleteTextures(1, &texture_id);

    if (egl_image != EGL_NO_IMAGE_KHR && eglDestroyImageKHR_ptr) {
        eglDestroyImageKHR_ptr(egl_display, egl_image);
        egl_image = EGL_NO_IMAGE_KHR;
    }

    if (gbm_bo) {
        gbm_bo_destroy(gbm_bo);
        gbm_bo = nullptr;
    }
    if (gbm_dev) {
        gbm_device_destroy(gbm_dev);
        gbm_dev = nullptr;
    }
    if (drm_fd != -1) {
        close(drm_fd);
        drm_fd = -1;
    }

    if (egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_surface != EGL_NO_SURFACE) {
            eglDestroySurface(egl_display, egl_surface);
            egl_surface = EGL_NO_SURFACE;
        }
        if (egl_context != EGL_NO_CONTEXT) {
            eglDestroyContext(egl_display, egl_context);
            egl_context = EGL_NO_CONTEXT;
        }
        eglTerminate(egl_display);
        egl_display = EGL_NO_DISPLAY;
    }
    std::cout << "Cleanup complete." << std::endl;
}

int main() {
    const int WIDTH = 640;
    const int HEIGHT = 480;
    const int NUM_FRAMES = 300; // Simulate 300 frames

    std::cout << "Starting EGL_LINUX_DMA_BUF_EXT example..." << std::endl;

    // 1. Initialize EGL and GLES context
    if (!init_egl_gles(WIDTH, HEIGHT)) {
        std::cerr << "Initialization of EGL/GLES failed. Exiting." << std::endl;
        cleanup();
        return 1;
    }
    std::cout << "EGL/GLES initialized successfully." << std::endl;

    // 2. Initialize DRM/GBM for DMA_BUF allocation
    // This must happen after EGL is initialized to ensure driver readiness.
    if (!init_drm_gbm(WIDTH, HEIGHT)) {
        std::cerr << "Initialization of DRM/GBM failed. Exiting." << std::endl;
        cleanup();
        return 1;
    }
    std::cout << "DRM/GBM initialized successfully." << std::endl;

    // 3. Create the GLES2 shader program
    if (!setup_gles_program()) {
        std::cerr << "GLES2 program setup failed. Exiting." << std::endl;
        cleanup();
        return 1;
    }
    std::cout << "GLES2 program setup successfully." << std::endl;

    // 4. Create the GLES texture from the DMA_BUF
    if (!create_dma_buf_texture(WIDTH, HEIGHT)) {
        std::cerr << "Creation of DMA_BUF texture failed. Exiting." << std::endl;
        cleanup();
        return 1;
    }
    std::cout << "DMA_BUF texture created and linked to GLES." << std::endl;

    // --- Main Rendering Loop ---
    for (int frame_idx = 0; frame_idx < NUM_FRAMES; ++frame_idx) {
        std::cout << "Rendering frame " << frame_idx << "..." << std::endl;

        auto start_time = std::chrono::high_resolution_clock::now();

        // 5. Update the pixel data in the DMA_BUF (simulating PNG decode)
        update_dma_buf_data(WIDTH, HEIGHT, frame_idx);

        // 6. Render the frame using the texture
        render_frame(WIDTH, HEIGHT);

        // In a real application, if using a window surface, you'd call:
        // eglSwapBuffers(egl_display, egl_surface);
        // For a pbuffer, you might read back the pixels for verification/saving.

        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> frame_time = end_time - start_time;
        std::cout << "Frame " << frame_idx << " time: " << frame_time.count() << " ms" << std::endl;

        // Basic frame rate control (adjust as needed for your target FPS)
        // For 60 FPS, target ~16.67ms per frame.
        double target_ms = 1000.0 / 60.0;
        if (frame_time.count() < target_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(target_ms - frame_time.count())));
        }
    }

    cleanup();
    std::cout << "Application finished." << std::endl;
    return 0;
}