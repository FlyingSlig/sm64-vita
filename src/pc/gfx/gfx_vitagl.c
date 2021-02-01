#ifdef TARGET_VITA

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include <vitaGL.h>

#include <psp2/io/fcntl.h>
#include <psp2/message_dialog.h>

#include "gfx_cc.h"
#include "gfx_rendering_api.h"

struct ShaderProgram {
    uint32_t shader_id;
    GLuint opengl_program_id;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;
    GLint attrib_locations[7];
    uint8_t attrib_sizes[7];
    uint8_t num_attribs;
    bool used_noise;
    GLint frame_count_location;
    GLint window_height_location;
};

static GLuint opengl_vbo = 0;

static struct ShaderProgram shader_program_pool[64];
static uint8_t shader_program_pool_size;

static struct ShaderProgram *cur_shader = NULL;

static uint32_t frame_count;
static uint32_t window_height;

void check_for_shader_compiler() {
    if(!vglHasRuntimeShaderCompiler()) {
        SceMsgDialogParam param;
        sceMsgDialogParamInit(&param);

        SceMsgDialogUserMessageParam user_msg;
        memset(&user_msg, 0, sizeof(SceMsgDialogUserMessageParam));
        user_msg.msg =  "You do not have the runtime shader compiler installed. It must be installed to run this program.\n\n"
                        "Please check the README.md in the repository for a link to instructions on installing it.";
        user_msg.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;

        param.userMsgParam = &user_msg;
        param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
        sceMsgDialogInit(&param);

        while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
            glClear(GL_COLOR_BUFFER_BIT);
            vglSwapBuffers(GL_TRUE);
        }
        sceKernelExitProcess(0);
    }
}

static bool gfx_vitagl_z_is_from_0_to_1() {
    return false;
}

static void gfx_vitagl_vertex_array_set_attribs(struct ShaderProgram *prg) {
    size_t num_floats = prg->num_floats;
    size_t pos = 0;

    for (int i = 0; i < prg->num_attribs; i++) {
        glEnableVertexAttribArray(prg->attrib_locations[i]);
        glVertexAttribPointer(prg->attrib_locations[i], prg->attrib_sizes[i], GL_FLOAT, GL_FALSE,
                              num_floats * sizeof(float), (void *) (pos * sizeof(float)));
        pos += prg->attrib_sizes[i];
    }
}

static void gfx_vitagl_set_uniforms(struct ShaderProgram *prg) {
    if (prg->used_noise) {
        glUniform1i(prg->frame_count_location, frame_count);
        glUniform1i(prg->window_height_location, window_height);
    }
}

static void gfx_vitagl_unload_shader(struct ShaderProgram *old_prg) {
    if (old_prg != NULL) {
        for (int i = 0; i < old_prg->num_attribs; i++) {
            glDisableVertexAttribArray(old_prg->attrib_locations[i]);
        }
    }
}

static void gfx_vitagl_load_shader(struct ShaderProgram *new_prg) {
    glUseProgram(new_prg->opengl_program_id);
    gfx_vitagl_vertex_array_set_attribs(new_prg);
    gfx_vitagl_set_uniforms(new_prg);
}

static void append_str(char *buf, size_t *len, const char *str) {
    while (*str != '\0')
        buf[(*len)++] = *str++;
}

static void append_line(char *buf, size_t *len, const char *str) {
    while (*str != '\0')
        buf[(*len)++] = *str++;
    buf[(*len)++] = '\n';
}

static const char *shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha,
                                      bool inputs_have_alpha, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "float4(0.0, 0.0, 0.0, 0.0)" : "float3(0.0, 0.0, 0.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element
                           ? "texVal0.a"
                           : (with_alpha ? "float4(texelVal0.a, texelVal0.a, texelVal0.a, texelVal0.a)"
                                         : "float3(texelVal0.a, texelVal0.a, texelVal0.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_INPUT_1:
                return "vInput1.a";
            case SHADER_INPUT_2:
                return "vInput2.a";
            case SHADER_INPUT_3:
                return "vInput3.a";
            case SHADER_INPUT_4:
                return "vInput4.a";
            case SHADER_TEXEL0:
                return "texVal0.a";
            case SHADER_TEXEL0A:
                return "texVal0.a";
            case SHADER_TEXEL1:
                return "texVal1.a";
        }
    }
}

static void append_formula(char *buf, size_t *len, uint8_t c[2][4], bool do_single, bool do_multiply,
                           bool do_mix, bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        append_str(buf, len,
                   shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        append_str(buf, len,
                   shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " * ");
        append_str(buf, len,
                   shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        append_str(buf, len, "lerp(");
        append_str(buf, len,
                   shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len,
                   shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len,
                   shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, ")");
    } else {
        append_str(buf, len, "(");
        append_str(buf, len,
                   shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " - ");
        append_str(buf, len,
                   shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ") * ");
        append_str(buf, len,
                   shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, " + ");
        append_str(buf, len,
                   shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    }
}

static struct ShaderProgram *gfx_vitagl_create_and_load_new_shader(uint32_t shader_id) {
    struct CCFeatures cc_features;
    gfx_cc_get_features(shader_id, &cc_features);

    char vs_buf[1024];
    char fs_buf[1024];
    size_t vs_len = 0;
    size_t fs_len = 0;
    size_t num_floats = 4;

    // Vertex shader
    append_line(vs_buf, &vs_len, "float4 main(");
    append_line(vs_buf, &vs_len, "float4 aVtxPos,");
    if (cc_features.used_textures[0] || cc_features.used_textures[1]) {
        append_line(vs_buf, &vs_len, "float2 aTexCoord,");
        append_line(vs_buf, &vs_len, "float2 out vTexCoord : TEXCOORD0,");
        num_floats += 2;
    }
    if (cc_features.opt_fog) {
        append_line(vs_buf, &vs_len, "float4 aFog,");
        append_line(vs_buf, &vs_len, "float4 out vFog : TEXCOORD1,");
        num_floats += 4;
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "float%d aInput%d,\n", cc_features.opt_alpha ? 4 : 3, i + 1);
        vs_len += sprintf(vs_buf + vs_len, "float%d out vInput%d : TEXCOORD%d,\n",
                          cc_features.opt_alpha ? 4 : 3, i + 1, i + 2);
        num_floats += cc_features.opt_alpha ? 4 : 3;
    }
    vs_buf[vs_len - 2] = ' ';
    append_line(vs_buf, &vs_len, ") : POSITION\n{");
    if (cc_features.used_textures[0] || cc_features.used_textures[1]) {
        append_line(vs_buf, &vs_len, "vTexCoord = aTexCoord;");
    }
    if (cc_features.opt_fog) {
        append_line(vs_buf, &vs_len, "vFog = aFog;");
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "vInput%d = aInput%d;\n", i + 1, i + 1);
    }
    append_line(vs_buf, &vs_len, "return aVtxPos;");
    append_line(vs_buf, &vs_len, "}");

    // Fragment shader
    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(fs_buf, &fs_len, "float random(float3 value) {");
        append_line(fs_buf, &fs_len, "    float r = dot(sin(value), float3(12.9898, 78.233, 37.719));");
        append_line(fs_buf, &fs_len, "    return frac(sin(r) * 143758.5453);");
        append_line(fs_buf, &fs_len, "}");
    }
    append_line(fs_buf, &fs_len, "float4 main(");
    if (cc_features.used_textures[0] || cc_features.used_textures[1]) {
        append_line(fs_buf, &fs_len, "float2 vTexCoord : TEXCOORD0,");
    }
    if (cc_features.opt_fog) {
        append_line(fs_buf, &fs_len, "float4 vFog : TEXCOORD1,");
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        fs_len += sprintf(fs_buf + fs_len, "float%d vInput%d : TEXCOORD%d,\n",
                          cc_features.opt_alpha ? 4 : 3, i + 1, i + 2);
    }
    if (cc_features.used_textures[0]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex0 : TEXUNIT0,");
    }
    if (cc_features.used_textures[1]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex1 : TEXUNIT1,");
    }

    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(fs_buf, &fs_len, "uniform int frame_count,");
        append_line(fs_buf, &fs_len, "uniform int window_height,");
        append_line(fs_buf, &fs_len, "float2 gl_FragCoord : WPOS,");
    }

    fs_buf[fs_len - 2] = ' ';
    append_line(fs_buf, &fs_len, ") : COLOR\n{");

    if (cc_features.used_textures[0]) {
        append_line(fs_buf, &fs_len, "float4 texVal0 = tex2D(uTex0, vTexCoord);");
    }
    if (cc_features.used_textures[1]) {
        append_line(fs_buf, &fs_len, "float4 texVal1 = tex2D(uTex1, vTexCoord);");
    }

    append_str(fs_buf, &fs_len, cc_features.opt_alpha ? "float4 texel = " : "float3 texel = ");
    if (!cc_features.color_alpha_same && cc_features.opt_alpha) {
        append_str(fs_buf, &fs_len, "float4(");
        append_formula(fs_buf, &fs_len, cc_features.c, cc_features.do_single[0],
                       cc_features.do_multiply[0], cc_features.do_mix[0], false, false, true);
        append_str(fs_buf, &fs_len, ", ");
        append_formula(fs_buf, &fs_len, cc_features.c, cc_features.do_single[1],
                       cc_features.do_multiply[1], cc_features.do_mix[1], true, true, true);
        append_str(fs_buf, &fs_len, ")");
    } else {
        append_formula(fs_buf, &fs_len, cc_features.c, cc_features.do_single[0],
                       cc_features.do_multiply[0], cc_features.do_mix[0], cc_features.opt_alpha, false,
                       cc_features.opt_alpha);
    }
    append_line(fs_buf, &fs_len, ";");

    if (cc_features.opt_texture_edge && cc_features.opt_alpha) {
        append_line(fs_buf, &fs_len, "if (texel.a > 0.3) texel.a = 1.0; else discard;");
    }
    // TODO discard if alpha is 0?
    if (cc_features.opt_fog) {
        if (cc_features.opt_alpha) {
            append_line(fs_buf, &fs_len, "texel = float4(lerp(texel.rgb, vFog.rgb, vFog.a), texel.a);");
        } else {
            append_line(fs_buf, &fs_len, "texel = lerp(texel, vFog.rgb, vFog.a);");
        }
    }

    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(fs_buf, &fs_len,
                    "texel.a *= floor(random(float3(floor(gl_FragCoord.xy * (240.0 / "
                    "float(window_height))), float(frame_count))) + 0.5);");
    }

    if (cc_features.opt_alpha) {
        append_line(fs_buf, &fs_len, "return texel;");
    } else {
        append_line(fs_buf, &fs_len, "return float4(texel, 1.0);");
    }
    append_line(fs_buf, &fs_len, "}");

    vs_buf[vs_len] = '\0';
    fs_buf[fs_len] = '\0';

    /*puts("Vertex shader:");
    puts(vs_buf);
    puts("Fragment shader:");
    puts(fs_buf);
    puts("End");*/

    const GLchar *sources[2] = { vs_buf, fs_buf };
    const GLint lengths[2] = { vs_len, fs_len };
    GLint success;

    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &sources[0], &lengths[0]);
    glCompileShader(vertex_shader);
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint max_length = 0;
        glGetShaderiv(vertex_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        printf("Vertex shader compilation failed\n");
        glGetShaderInfoLog(vertex_shader, max_length, &max_length, &error_log[0]);
        printf("%s\n", &error_log[0]);
        abort();
    }

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &sources[1], &lengths[1]);
    glCompileShader(fragment_shader);
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint max_length = 0;
        glGetShaderiv(fragment_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        printf("Fragment shader compilation failed\n");
        glGetShaderInfoLog(fragment_shader, max_length, &max_length, &error_log[0]);
        printf("%s\n", &error_log[0]);
        abort();
    }

    GLuint shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);

    size_t cnt = 0;

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];
    glBindAttribLocation(shader_program, cnt, "aVtxPos");
    prg->attrib_locations[cnt] = cnt;
    prg->attrib_sizes[cnt] = 4;
    ++cnt;

    if (cc_features.used_textures[0] || cc_features.used_textures[1]) {
        glBindAttribLocation(shader_program, cnt, "aTexCoord");
        prg->attrib_locations[cnt] = cnt;
        prg->attrib_sizes[cnt] = 2;
        ++cnt;
    }

    if (cc_features.opt_fog) {
        glBindAttribLocation(shader_program, cnt, "aFog");
        prg->attrib_locations[cnt] = cnt;
        prg->attrib_sizes[cnt] = 4;
        ++cnt;
    }

    for (int i = 0; i < cc_features.num_inputs; i++) {
        char name[16];
        sprintf(name, "aInput%d", i + 1);
        glBindAttribLocation(shader_program, cnt, name);
        prg->attrib_locations[cnt] = cnt;
        prg->attrib_sizes[cnt] = cc_features.opt_alpha ? 4 : 3;
        ++cnt;
    }

    glLinkProgram(shader_program);

    for (int i = 0; i < cnt; i++)
    {
        sceClibPrintf("attrib_location[%d] = %d\n", i, prg->attrib_locations[i]);
    }

    prg->shader_id = shader_id;
    prg->opengl_program_id = shader_program;
    prg->num_inputs = cc_features.num_inputs;
    prg->used_textures[0] = cc_features.used_textures[0];
    prg->used_textures[1] = cc_features.used_textures[1];
    prg->num_floats = num_floats;
    prg->num_attribs = cnt;

    gfx_vitagl_load_shader(prg);

    if (cc_features.opt_alpha && cc_features.opt_noise) {
        prg->frame_count_location = glGetUniformLocation(shader_program, "frame_count");
        prg->window_height_location = glGetUniformLocation(shader_program, "window_height");
        prg->used_noise = true;
    } else {
        prg->used_noise = false;
    }

    return prg;
}

static struct ShaderProgram *gfx_vitagl_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < shader_program_pool_size; i++) {
        if (shader_program_pool[i].shader_id == shader_id) {
            return &shader_program_pool[i];
        }
    }
    return NULL;
}

static void gfx_vitagl_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs,
                                               bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static GLuint gfx_vitagl_new_texture(void) {
    GLuint ret;
    glGenTextures(1, &ret);
    return ret;
}

static void gfx_vitagl_select_texture(int tile, GLuint texture_id) {
    glActiveTexture(GL_TEXTURE0 + tile);
    glBindTexture(GL_TEXTURE_2D, texture_id);
}

static void gfx_vitagl_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
}

static uint32_t gfx_cm_to_opengles(uint32_t val) {
    if (val & G_TX_CLAMP) {
        return GL_CLAMP_TO_EDGE;
    }
    return (val & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static void gfx_vitagl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms,
                                                      uint32_t cmt) {
    glActiveTexture(GL_TEXTURE0 + tile);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear_filter ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gfx_cm_to_opengles(cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gfx_cm_to_opengles(cmt));
}

static void gfx_vitagl_set_depth_test(bool depth_test) {
    if (depth_test) {
        glEnable(GL_DEPTH_TEST);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
}

static void gfx_vitagl_set_depth_mask(bool z_upd) {
    glDepthMask(z_upd ? GL_TRUE : GL_FALSE);
}

static void gfx_vitagl_set_zmode_decal(bool zmode_decal) {
    if (zmode_decal) {
        glPolygonOffset(-2, -2);
        glEnable(GL_POLYGON_OFFSET_FILL);
    } else {
        glPolygonOffset(0, 0);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
}

static void gfx_vitagl_set_viewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);
    window_height = height;
}

static void gfx_vitagl_set_scissor(int x, int y, int width, int height) {
    glScissor(x, y, width, height);
}

static void gfx_vitagl_set_use_alpha(bool use_alpha) {
    if (use_alpha) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
}

static void gfx_vitagl_draw_triangles(float buf_vbo[], size_t buf_vbo_len,
                                              size_t buf_vbo_num_tris) {
    // printf("flushing %d tris\n", buf_vbo_num_tris);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * buf_vbo_len, buf_vbo, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 3 * buf_vbo_num_tris);
}

static void gfx_vitagl_init(void) {
    vglEnableRuntimeShaderCompiler(GL_TRUE);
    vglUseVram(GL_TRUE);
    vglWaitVblankStart(GL_TRUE);

    vglInitExtended(960, 544, 0x8000000, SCE_GXM_MULTISAMPLE_4X);
    
    check_for_shader_compiler();

    glGenBuffers(1, &opengl_vbo);

    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);

    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

static void gfx_vitagl_on_resize() {
}

static void gfx_vitagl_start_frame(void) {
    frame_count++;

    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE); // Must be set to clear Z-buffer
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
}

static void gfx_vitagl_end_frame() {
}

static void gfx_vitagl_finish_render() {
}

struct GfxRenderingAPI gfx_vitagl_api = {
    gfx_vitagl_z_is_from_0_to_1, gfx_vitagl_unload_shader,
    gfx_vitagl_load_shader,      gfx_vitagl_create_and_load_new_shader,
    gfx_vitagl_lookup_shader,    gfx_vitagl_shader_get_info,
    gfx_vitagl_new_texture,      gfx_vitagl_select_texture,
    gfx_vitagl_upload_texture,   gfx_vitagl_set_sampler_parameters,
    gfx_vitagl_set_depth_test,   gfx_vitagl_set_depth_mask,
    gfx_vitagl_set_zmode_decal,  gfx_vitagl_set_viewport,
    gfx_vitagl_set_scissor,      gfx_vitagl_set_use_alpha,
    gfx_vitagl_draw_triangles,   gfx_vitagl_init,
    gfx_vitagl_on_resize,        gfx_vitagl_start_frame,
    gfx_vitagl_end_frame,        gfx_vitagl_finish_render
};

#endif
