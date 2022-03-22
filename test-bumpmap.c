/*
 * (c) drspod 2022
 * This is free and unencumbered software released into the public domain.
 * For more information, please refer to <https://unlicense.org>
 */

#include <yaul.h>
#include <tga.h>
#include <math/fix16.h>

#include <stdio.h>
#include <stdlib.h>

extern const uint8_t asset_wall_img_tga[];
extern const uint8_t asset_wall_hmap_tga[];

extern const uint8_t asset_sega_img_tga[];
extern const uint8_t asset_sega_hmap_tga[];

static vdp1_vram_partitions_t vdp1_vram_partitions;

static void vblank_out_handler(void*);

// max number of vdp1 commands
#define MAX_CMDT 255

#define ARRAY_SIZEOF(x) (sizeof(x) / sizeof(*x))

#define SHADOW_OFFSET 10

#define INV_SQRT_2 (0.7071)

static fix16_t light_angle = FIX16(0.0);

typedef struct
{
    int16_t width;
    int16_t height;
    void* p_tex_vram;
    void* p_normal_map_vram;
    void* p_clut_vram;
    uint8_t hmap_height;
    uint8_t max_light_intensity;
} texture_normap_t;

// order of palette normals is unimportant with the exception of position 0 which is the transparent pixel
static fix16_vec3_t normal_vectors[] = {
    FIX16_VEC3_INITIALIZER(0, 0, 1),
    FIX16_VEC3_INITIALIZER(-1, 0, 0),
    FIX16_VEC3_INITIALIZER(-INV_SQRT_2, -INV_SQRT_2, 0),
    FIX16_VEC3_INITIALIZER(0, -1, 0),
    FIX16_VEC3_INITIALIZER(INV_SQRT_2, -INV_SQRT_2, 0),
    FIX16_VEC3_INITIALIZER(1, 0, 0),
    FIX16_VEC3_INITIALIZER(INV_SQRT_2, INV_SQRT_2, 0),
    FIX16_VEC3_INITIALIZER(0, 1, 0),
    FIX16_VEC3_INITIALIZER(-INV_SQRT_2, INV_SQRT_2, 0),
    FIX16_VEC3_INITIALIZER(-INV_SQRT_2, 0, INV_SQRT_2),
    FIX16_VEC3_INITIALIZER(0, -INV_SQRT_2, INV_SQRT_2),
    FIX16_VEC3_INITIALIZER(INV_SQRT_2, 0, INV_SQRT_2),
    FIX16_VEC3_INITIALIZER(0, INV_SQRT_2, INV_SQRT_2)
};

static int closest_normal_index(fix16_vec3_t vec)
{
    fix16_t pmax = FIX16_MIN;
    int pmax_idx = 0;
    for (uint32_t i = 0; i < ARRAY_SIZEOF(normal_vectors); ++i)
    {
        fix16_t p = fix16_vec3_dot(&vec, &normal_vectors[i]);
        pmax = fix16_max(p, pmax);
        if (pmax == p)
            pmax_idx = i;
    }
    return pmax_idx;
}

static void tga_load_texture_with_heightmap(texture_normap_t* p_texture,
        const uint8_t* tex_tga_data, const uint8_t* hmap_tga_data, void** pp_tex_vram, void** pp_clut_vram)
{
    tga_t img_tga;
    int ret = tga_read(&img_tga, tex_tga_data);
    assert(ret == TGA_FILE_OK);
    
    img_tga.tga_options.transparent_pixel = 0x00000000;
    img_tga.tga_options.msb = 0;
    
    tga_image_decode(&img_tga, *pp_tex_vram);
    p_texture->p_tex_vram = *pp_tex_vram;
    p_texture->width = img_tga.tga_width;
    p_texture->height = img_tga.tga_height;

    *pp_tex_vram += img_tga.tga_width * img_tga.tga_height * 2;

    tga_t hmap_tga;
    ret = tga_read(&hmap_tga, hmap_tga_data);
    assert(ret == TGA_FILE_OK);
    
    uint8_t* hmap_buf = (uint8_t*)malloc(hmap_tga.tga_width * hmap_tga.tga_height);
    tga_image_decode(&hmap_tga, hmap_buf);
    
    uint32_t normal_map_size = (hmap_tga.tga_width * hmap_tga.tga_height) >> 1;
    uint8_t* normal_map = (uint8_t*)malloc(normal_map_size);
    for (int y = 0; y < hmap_tga.tga_height; ++y)
    {
        for (int x = 0; x < hmap_tga.tga_width;  ++x)
        {
            int hmap_idx = y * hmap_tga.tga_width + x;
            int pal_normal_idx = 0;

            // skip first row and last column
            if (x < hmap_tga.tga_width - 1 && y > 0)
            {
                int dz_dx = hmap_buf[hmap_idx + 1] - hmap_buf[hmap_idx];
                int dz_dy = hmap_buf[hmap_idx - hmap_tga.tga_width] - hmap_buf[hmap_idx];
                fix16_vec3_t v1 = FIX16_VEC3_INITIALIZER(1, 0, dz_dx * p_texture->hmap_height / 255.0);
                fix16_vec3_t v2 = FIX16_VEC3_INITIALIZER(0, 1, dz_dy * p_texture->hmap_height / 255.0);
                fix16_vec3_t normal = {0};
                fix16_vec3_cross(&v1, &v2, &normal);
                pal_normal_idx = closest_normal_index(normal);
            }

            if (hmap_idx & 1)
                normal_map[hmap_idx >> 1] |= (pal_normal_idx & 0xF);
            else
                normal_map[hmap_idx >> 1] = (pal_normal_idx & 0xF) << 4;
        }
    }

    scu_dma_transfer(0, *pp_tex_vram, normal_map, normal_map_size);
    scu_dma_transfer_wait(0);
    p_texture->p_normal_map_vram = *pp_tex_vram;
    *pp_tex_vram += normal_map_size;
    
    p_texture->p_clut_vram = *pp_clut_vram;
    *pp_clut_vram += sizeof(vdp1_clut_t);

    free(hmap_buf);
    free(normal_map);
}

void add_command_system_clipping(vdp1_cmdt_list_t* command_list, int width, int height)
{
    int16_vec2_t system_clip_coord = INT16_VEC2_INITIALIZER(width, height);
    vdp1_cmdt_system_clip_coord_set(&command_list->cmdts[command_list->count]);
    vdp1_cmdt_param_vertex_set(&command_list->cmdts[command_list->count], CMDT_VTX_SYSTEM_CLIP, &system_clip_coord);
    command_list->count++;
}

void add_command_local_coordinates(vdp1_cmdt_list_t* command_list, int origin_x, int origin_y)
{
    int16_vec2_t local_coord_center = INT16_VEC2_INITIALIZER(origin_x, origin_y);
    vdp1_cmdt_local_coord_set(&command_list->cmdts[command_list->count]);
    vdp1_cmdt_param_vertex_set(&command_list->cmdts[command_list->count], CMDT_VTX_LOCAL_COORD, &local_coord_center);
    command_list->count++;
}

void add_command_draw_end(vdp1_cmdt_list_t* command_list)
{
    vdp1_cmdt_end_set(&command_list->cmdts[command_list->count]);
    command_list->count++;
}

void add_command_shadow_quad(const texture_normap_t* texture, vdp1_cmdt_list_t* command_list, int x, int y)
{
    vdp1_cmdt_draw_mode_t draw_mode = {0};
    int16_vec2_t coords = INT16_VEC2_INITIALIZER(x, y);

    // texture polygon
    vdp1_cmdt_t* p_cmd = &command_list->cmdts[command_list->count];
    command_list->count++;

    vdp1_cmdt_normal_sprite_set(p_cmd);
    
    draw_mode.bits.cc_mode = 0x1;
    vdp1_cmdt_param_draw_mode_set(p_cmd, draw_mode);

    // vdp1 color mode 5 (RGB)
    p_cmd->cmd_pmod &= 0xFFC7;
    p_cmd->cmd_pmod |= 0x0028;
    p_cmd->cmd_colr = 0x0000;

    vdp1_cmdt_param_char_base_set(p_cmd, (vdp1_vram_t)texture->p_tex_vram);
    vdp1_cmdt_param_size_set(p_cmd, texture->width, texture->height);
    vdp1_cmdt_param_vertex_set(p_cmd, 0, &coords);
}

void add_command_normap_quad(const texture_normap_t* texture, vdp1_cmdt_list_t* command_list, int x, int y)
{
    vdp1_cmdt_draw_mode_t draw_mode = {0};
    int16_vec2_t coords = INT16_VEC2_INITIALIZER(x, y);

    // texture polygon
    vdp1_cmdt_t* p_cmd = &command_list->cmdts[command_list->count];
    command_list->count++;

    vdp1_cmdt_normal_sprite_set(p_cmd);
    vdp1_cmdt_param_draw_mode_set(p_cmd, draw_mode);

    // vdp1 color mode 5 (RGB)
    p_cmd->cmd_pmod &= 0xFFC7;
    p_cmd->cmd_pmod |= 0x0028;
    p_cmd->cmd_colr = 0x0000;

    vdp1_cmdt_param_char_base_set(p_cmd, (vdp1_vram_t)texture->p_tex_vram);
    vdp1_cmdt_param_size_set(p_cmd, texture->width, texture->height);
    vdp1_cmdt_param_vertex_set(p_cmd, 0, &coords);

    // lightmap overlay polygon
    p_cmd = &command_list->cmdts[command_list->count];
    command_list->count++;

    vdp1_cmdt_normal_sprite_set(p_cmd);

    draw_mode.bits.cc_mode = 0x3;
    vdp1_cmdt_param_draw_mode_set(p_cmd, draw_mode);

    vdp1_cmdt_param_color_mode1_set(p_cmd, (vdp1_vram_t)texture->p_clut_vram);
    vdp1_cmdt_param_char_base_set(p_cmd, (vdp1_vram_t)texture->p_normal_map_vram);
    vdp1_cmdt_param_size_set(p_cmd, texture->width, texture->height);
    vdp1_cmdt_param_vertex_set(p_cmd, 0, &coords);
}

void main(void)
{
    dbgio_dev_default_init(DBGIO_DEV_VDP2);
    dbgio_dev_font_load();

    vdp1_cmdt_list_t* vdp1_command_list = vdp1_cmdt_list_alloc(MAX_CMDT);
    memset(vdp1_command_list->cmdts, 0, sizeof(vdp1_cmdt_t) * MAX_CMDT);

    add_command_system_clipping(vdp1_command_list, 320, 240);
    add_command_local_coordinates(vdp1_command_list, 160, 120);

    void* next_vdp1_texture = vdp1_vram_partitions.texture_base;
    void* next_vdp1_clut = vdp1_vram_partitions.clut_base;

    texture_normap_t wall_texture = {.hmap_height = 4, .max_light_intensity = 15};
    texture_normap_t sign_texture = {.hmap_height = 8, .max_light_intensity = 31};

    tga_load_texture_with_heightmap(&wall_texture, asset_wall_img_tga, asset_wall_hmap_tga, &next_vdp1_texture, &next_vdp1_clut);
    tga_load_texture_with_heightmap(&sign_texture, asset_sega_img_tga, asset_sega_hmap_tga, &next_vdp1_texture, &next_vdp1_clut);

    add_command_normap_quad(&wall_texture, vdp1_command_list, -160, -120);
    
    add_command_shadow_quad(&sign_texture, vdp1_command_list, -160, -96);
    vdp1_cmdt_t* p_shadow_cmd = &vdp1_command_list->cmdts[vdp1_command_list->count - 1];
    
    add_command_normap_quad(&sign_texture, vdp1_command_list, -128, -64);

    add_command_draw_end(vdp1_command_list);

    while (true)
    {
        int16_vec2_t shadow_coords = INT16_VEC2_INITIALIZER(-128, -64);
        int16_t shadow_dx = fix16_int32_to(fix16_int16_mul(fix16_cos(light_angle), SHADOW_OFFSET));
        int16_t shadow_dy = -fix16_int32_to(fix16_int16_mul(fix16_sin(light_angle), SHADOW_OFFSET));
        shadow_coords.x += shadow_dx;
        shadow_coords.y += shadow_dy;

        vdp1_cmdt_param_vertex_set(p_shadow_cmd, 0, &shadow_coords);

        /* dbgio_printf("[H[2Jshadow_dx: %d\nshadow_dy: %d", shadow_dx, shadow_dy);
        dbgio_flush(); */

        fix16_vec3_t light_vector = {{fix16_cos(light_angle), fix16_sin(light_angle), FIX16(-1)}};
        fix16_vec3_normalize(&light_vector);

        vdp1_clut_t wall_lighting = {0};
        vdp1_clut_t sign_lighting = {0};
        for (uint32_t i = 0; i < ARRAY_SIZEOF(normal_vectors); ++i)
        {
            fix16_t light = -fix16_vec3_dot(&light_vector, &normal_vectors[i]);

            int32_t wall_light_col = fix16_int32_to(fix16_int16_mul(light, wall_texture.max_light_intensity));
            int32_t sign_light_col = fix16_int32_to(fix16_int16_mul(light, sign_texture.max_light_intensity));

            wall_light_col = clamp(wall_light_col, 0, 31);
            sign_light_col = clamp(sign_light_col, 0, 31);

            wall_lighting.entries[i].color = COLOR_RGB1555(1, wall_light_col, wall_light_col, wall_light_col);
            sign_lighting.entries[i].color = COLOR_RGB1555(1, sign_light_col, sign_light_col, sign_light_col);
        }

        scu_dma_transfer(0, wall_texture.p_clut_vram, &wall_lighting, sizeof(wall_lighting));
        scu_dma_transfer(0, sign_texture.p_clut_vram, &sign_lighting, sizeof(sign_lighting));
        scu_dma_transfer_wait(0);

        vdp1_sync_cmdt_list_put(vdp1_command_list, 0);
        vdp1_sync_render();
        vdp1_sync();
        vdp2_sync();
        vdp1_sync_wait();
    }
}

void user_init(void)
{
        vdp2_tvmd_display_res_set(VDP2_TVMD_INTERLACE_NONE, VDP2_TVMD_HORZ_NORMAL_A, VDP2_TVMD_VERT_240);
        vdp2_scrn_back_screen_color_set(VDP2_VRAM_ADDR(3, 0x01FFFE), COLOR_RGB1555(1, 10, 10, 10));
        vdp2_sprite_priority_set(0, 6);

        vdp1_env_t env;
        vdp1_env_default_init(&env);
        env.bpp = VDP1_ENV_BPP_16;
        env.erase_color = COLOR_RGB1555(1, 10, 10, 10);
        vdp1_env_set(&env);

        // automatic frame timing, no cap
        vdp1_sync_interval_set(-1);

        vdp_sync_vblank_out_set(&vblank_out_handler, NULL);

        vdp2_tvmd_display_set();

        vdp1_vram_partitions_get(&vdp1_vram_partitions);

        vdp2_sync();
        vdp2_sync_wait();
}

static void vblank_out_handler(void *work __unused)
{
        smpc_peripheral_intback_issue();
        
        light_angle += FIX16(0.05);
        if (light_angle > FIX16_2PI)
            light_angle = FIX16(0.0);
}
