# The tests' own wxl.render programs (tests/vk/glsl/), compiled as the renderer's are, into
# headers for the target the tests are built as (wxl_vk_add_test_programs(<target>)).
# Copyright (c) 2026 WarcraftXL. MIT (see LICENSE).

function(wxl_vk_add_test_programs target)
    set(glsl "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/glsl")
    set(out "${CMAKE_CURRENT_BINARY_DIR}/generated/vk_test_spirv")
    wxl_vk_glsl(${target}-programs "${out}"
        render_tint_frag=${glsl}/render_tint.frag
        render_sample_frag=${glsl}/render_sample.frag
        render_depth_frag=${glsl}/render_depth.frag
        render_flat_frag=${glsl}/render_flat.frag
        render_pulled_vert=${glsl}/render_pulled.vert
        render_pulled_frag=${glsl}/render_pulled.frag
        render_rows_frag=${glsl}/render_rows.frag
        render_fill_comp=${glsl}/render_fill.comp
        render_bones_frag=${glsl}/render_bones.frag
        # The SDK's example mod wxl.outline, its programs drawn here as the mod draws them.
        outline_mask_frag=${WXL_SDK_DIR}/examples/outline/shaders/mask.frag
        outline_edge_frag=${WXL_SDK_DIR}/examples/outline/shaders/edge.frag)
    target_include_directories(${target} PRIVATE "${out}")
    add_dependencies(${target} ${target}-programs)
endfunction()
