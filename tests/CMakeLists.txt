# The unit tests as CMake targets (-DSB_TESTS=ON at the top level, or
# -DSB_TESTS_ONLY=ON for these alone), built as each file's header says to
# build it by hand: -O2 with assertions on, no -march. Run them with ctest
# from the build folder. None needs game files, SDL3 or Vulkan;
# write_watch_test and kernel_dispatcher_test need ppc/ (ppc_config.h,
# generated from your own XEX) and tools/XenonRecomp, and are left out
# without them.
#
# A test is one line: sb_test(<name> [SOURCES <runtime .cpp ...>]
# [ARGS <arguments>] [LABELS <labels>]) builds tests/<name>.cpp with them.
# The label "timing" marks a test that measures this machine's scheduling
# against fixed thresholds: meaningful on a quiet machine, flaky on a shared
# CI runner, which leaves them out (ctest -LE timing).
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(RT ${ROOT}/runtime)
find_package(Threads REQUIRED)

function(sb_test name)
    cmake_parse_arguments(T "" "" "SOURCES;ARGS;LABELS" ${ARGN})
    add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp ${T_SOURCES})
    target_include_directories(${name} PRIVATE ${RT})
    target_link_libraries(${name} PRIVATE Threads::Threads)
    target_compile_options(${name} PRIVATE -O2 -UNDEBUG)
    add_test(NAME ${name} COMMAND ${name} ${T_ARGS})
    if(T_LABELS)
        set_tests_properties(${name} PROPERTIES LABELS "${T_LABELS}")
    endif()
endfunction()

sb_test(block_ranges_test)
# Where guest memory and the upload ring go (gpu/memory_plan.h, header only).
sb_test(memory_plan_test)
sb_test(zpd_report_test)
# The resolve shader's 24-bit depth pack (gpu/depth24_glsl.h), header only;
# renderer.cpp is read to check its resolve shader uses it.
sb_test(depth24_pack_test ARGS ${RT}/gpu/renderer.cpp)
# Depth-only draws run no pixel shader (gpu/depth_only.h, header only): the
# sun's shadow pass as the game draws it, and renderer.cpp's Draw using the
# rule (the cars' and tunnel roofs' shadows, v0.1.3).
sb_test(depth_only_test ARGS ${RT}/gpu/renderer.cpp)
# The internal resolution as a ratio, 1.5x included: guest <-> host pixels
# and the scaled resolve memory's layout (gpu/render_scale.h, header only);
# renderer.cpp is read to check its scaled-memory shaders use it.
sb_test(render_scale_test ARGS ${RT}/gpu/renderer.cpp)
# The police vid-cam's side bars, the intro's black frames, the results
# backdrop and menus' inset overlays, and the fill past the 16:9 band
# (gpu/edge_bars.h, header only).
sb_test(edge_bars_test)
# Which of Xenos's two polygon offsets (front and back faces) a draw gets, and
# when its depth bias is set in unorm24's steps (gpu/poly_offset.h, header
# only); renderer.cpp is read to check Draw uses them.
sb_test(gpu_poly_offset_test ARGS ${RT}/gpu/renderer.cpp)
# The texture cache's limits (gpu/texture_budget.h, header only): when it
# gives textures back and which, over a modelled long free roam (bounded, the
# scene never evicted, no reloads); renderer.cpp is read to check every use
# marks the texture and the swap trims the cache.
sb_test(gpu_texture_budget_test ARGS ${RT}/gpu/renderer.cpp)
# Texture formats that hold another's data (the *_AS_16_16_16_16 aliases,
# k_8_A) and the components a host format lacks (gpu/texture_formats.h,
# header only); renderer.cpp and shader_translator.cpp are read to check
# LookupTexture and the untile's new loads use them.
sb_test(gpu_texture_formats_test ARGS ${RT}/gpu/renderer.cpp ${RT}/gpu/shader_translator.cpp)
# The vertex snapshots' spans (gpu/vertex_snapshot.h, header only): a span
# covers every address a shader's fetch reads inside its range (the shaders'
# addressing redone in C++), and the translator notes which fetches index
# with the vertex index (shader_translator.cpp, on hand-assembled Xenos
# shaders); renderer.cpp and shader_translator.cpp are read to check Draw
# copies the spans and the shaders address vertices as modelled.
sb_test(gpu_vertex_snapshot_test SOURCES ${RT}/gpu/shader_translator.cpp ${RT}/gpu/xenos/ucode.cpp
    ARGS ${RT}/gpu/renderer.cpp ${RT}/gpu/shader_translator.cpp)
# Draw state the renderer records two ways (gpu/draw_state.h, header only):
# the depth and stencil tests a pipeline bakes in or a draw sets as dynamic
# state, and the push-constant rows a draw changes; renderer.cpp is read to
# check both ways take the tests from it and Draw sets what its pipelines
# leave dynamic.
sb_test(gpu_draw_state_test ARGS ${RT}/gpu/renderer.cpp)
# The rules a draw's textures are served again by without being looked up
# (renderer.cpp, TextureDrawMemo, CP_OPT_TEXTURE_MEMO): renderer.cpp is read
# to check every part of the memo's key is compared, the memo is made only
# from kept lookups, an eviction forgets it and Draw compares its bound
# textures again whenever the lookups' results may have changed.
sb_test(gpu_texture_memo_test ARGS ${RT}/gpu/renderer.cpp)
# Which front image the renderer's next swap writes (gpu/front_ring.h,
# header only): a model of the presenter and the renderer, which must show
# every frame as itself or count it as dropped (v0.1.4's single image lost
# a race to the presenter's read); renderer.cpp and presenter.cpp are read
# to check they choose and hand over the image as the model does.
sb_test(gpu_front_ring_test ARGS ${RT}/gpu/renderer.cpp ${RT}/video/presenter.cpp)
# Every Vulkan wait, submit and present in the renderer and the presenter looks
# at its result, and a lost device goes to video::DeviceLost (v0.1.4's
# completion thread spun on one, and the presenter drew to an image index a
# failed acquire never wrote); planted v0.1.4 lines must be caught.
# renderer.cpp and presenter.cpp are read.
sb_test(gpu_device_lost_test ARGS ${RT}/gpu/renderer.cpp ${RT}/video/presenter.cpp)
# A saved pipeline cache from another GPU, driver or OS version, and the log
# line that says so (gpu/pipeline_cache_header.h, header only).
sb_test(pipeline_cache_header_test)
# The pipeline key list the renderer makes pipelines ahead from: its file
# read back, or set aside when another build's, another GPU's, cut short or
# corrupt, and the most recently used keys kept past its cap
# (gpu/pipeline_keys.h, header only); renderer.cpp is read to check it notes
# every pipeline it draws with and prewarms only what its Draw would make.
sb_test(pipeline_keys_test ARGS ${RT}/gpu/renderer.cpp)
sb_test(picture_fit_test SOURCES ${RT}/video/picture_fit.cpp)
# Which Vulkan device the presenter draws with, and its log line
# (video/gpu_choice.h, header only).
sb_test(gpu_choice_test)
# The sun's glow fitted to the view scales (game/glare.h, header only), on
# video::FitPicture's pictures.
sb_test(glare_fit_test SOURCES ${RT}/video/picture_fit.cpp)
# The post-processing's split at the 16:9 band's edge (video/band_filter.h,
# video::BandOf) against the edge-bar fills and a model of the filters.
sb_test(band_filter_test SOURCES ${RT}/video/picture_fit.cpp)
# Internal Resolution Auto's budget on iPhones and iPads, chip by chip
# (video/scale_budget.cpp), and the scale it makes on the phones' screens.
sb_test(scale_budget_test SOURCES ${RT}/video/scale_budget.cpp ${RT}/video/picture_fit.cpp)
# Settings > Display > Brightness's curve and the 360's display gamma ramp
# (video/display_tone.h, video/display_gamma.h: the DC_LUT_* registers as
# Direct3D writes them), header only; presenter.cpp is read to check the
# tone shader's wrapping of the scale shaders, settings.cpp for the two
# options' defaults (Xbox 360 Display Gamma on since v0.1.4).
sb_test(display_tone_test ARGS ${RT}/video/presenter.cpp ${RT}/user/settings.cpp)
sb_test(refresh_fit_test SOURCES ${RT}/video/refresh_fit.cpp)
# How late the guest vblank's thread wakes on this machine, alone and with
# every core busy (video/vblank_timer.cpp; checked on macOS, where its
# scheduling changed in v0.1.4). Its thresholds (1.5 ms in 99% of vblanks,
# 0.5 ms at the median) hold on a quiet machine, not on a shared CI runner.
sb_test(vblank_timer_test SOURCES ${RT}/video/vblank_timer.cpp LABELS timing)
# When the Mac shows a new frame at a set time after its vblank, and the
# watchdog that turns it off (video/present_schedule.h, header only).
sb_test(present_schedule_test)
# The audio render thread's cadence (apu/audio.h's RenderCadence, header
# only) against a simulated output device: following its clock, and the
# watchdog for a device that stops taking audio (GitHub issue #20).
sb_test(audio_cadence_test)
sb_test(frame_rate_policy_test SOURCES ${RT}/video/frame_rate_policy.cpp)
# When the presenter builds its swapchain again and at what size (an iPhone's
# view turned portrait by SDL, sizes seen while inactive, SUBOPTIMAL), and the
# UI's display size: scenarios, then planted wrong rules that must fail one.
sb_test(swapchain_policy_test SOURCES ${RT}/video/swapchain_policy.cpp)
# The front end's matrix after the window changes shape: the game's
# pointer-keyed matrix cache and the hook's resend (game/front_end_matrix.h,
# header only; the "EVENT HANDLER" label above the boot PSA).
sb_test(front_end_matrix_test)
# Settings > Display > Fullscreen against the window's own state
# (video/fullscreen_policy.h, header only): the green button and other system
# changes never change the saved setting, the menu does; presenter.cpp is read
# to check its window events don't write the setting.
sb_test(fullscreen_policy_test ARGS ${RT}/video/presenter.cpp)
# Game time in whole vblanks (game/whole_vblanks.h), header only.
sb_test(whole_vblanks_test)
# The timedemo (game/timedemo.h, header only): its switches, which Joylog
# path goes where, the DbgBreakPoint callers that are the log's checks, the
# fixed step, the uncapped swap and the capture's .json; the runtime folder
# is read to check vfs.cpp, frame_time.cpp, movie_pacing.cpp, main.cpp,
# ui.cpp, imports_unleashed.cpp and timedemo.cpp use it.
sb_test(timedemo_logic_test ARGS ${RT})
# The timedemo benchmark's statistics and report formats (debug/bench_stats.h,
# header only): nearest-rank percentiles, the 1% and 0.1% low averages,
# FNV-1a 64, the JSON and CSV writers.
sb_test(bench_stats_test)
# The timedemo's frame images (debug/bench_shots.h, header only):
# NFSMW_BENCH_SHOTS's steps, the main loop's holds, which swap is a frame's
# picture, when a held game has settled, the PPM.
sb_test(bench_shots_test)
# The nitrous light trails' ring at the 360's 30 fps length (game/light_trails.h,
# header only; GitHub issue #10).
sb_test(light_trails_test)
sb_test(guest_time_test SOURCES ${RT}/cpu/guest_time.cpp)
# When the mouse cursor shows over the game (ui/cursor_idle.h, header only),
# and how ui.cpp drives it.
sb_test(cursor_idle_test ARGS ${RT}/ui/ui.cpp)
sb_test(report_zip_test SOURCES ${RT}/report/zip.cpp ARGS ${CMAKE_CURRENT_BINARY_DIR}/report_zip_out)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    # What report_zip_test wrote, read back with independent code.
    add_test(NAME report_zip_check COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/report_zip_check.py
        ${CMAKE_CURRENT_BINARY_DIR}/report_zip_out)
    set_tests_properties(report_zip_check PROPERTIES DEPENDS report_zip_test)
    # scripts/bench_compare.py on made-up timedemo reports (docs/timedemo.md).
    add_test(NAME bench_compare_test COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/bench_compare_test.py)
    # scripts/compare_shots.py on made-up timedemo frame images.
    add_test(NAME compare_shots_test COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/compare_shots_test.py)
endif()
# "Choose a folder..." in the installer (install/placement.h): where the game
# goes in a picked folder and what is refused, on planted facts and in a
# scratch folder with a sandbox HOME. POSIX only (mkdtemp, chmod, ACLs and
# links in its disk checks); the screen hides the entry on Windows until
# placement.cpp is ported there.
if(NOT WIN32)
    sb_test(install_folder_test SOURCES ${RT}/install/placement.cpp ${RT}/install/installer.cpp ${RT}/install/locate.cpp
        ${RT}/install/manifest.cpp ${RT}/install/result.cpp ${RT}/install/disc_source.cpp ${RT}/install/sha256.cpp
        ${RT}/user/portable.cpp
        ARGS ${CMAKE_BINARY_DIR}/install_folder_scratch)  # not beside the test: that's its "own folder", refused
    # Portable mode (user/portable.h) and --portable on|off
    # (install/portable_switch.h): the layout, the records that move with
    # the folder, the placement rules inside it, the startup write check, and
    # the copies (a stand-in copies the game), in a scratch folder.
    sb_test(portable_test SOURCES ${RT}/user/portable.cpp ${RT}/install/portable_switch.cpp ${RT}/install/placement.cpp
        ${RT}/install/installer.cpp ${RT}/install/locate.cpp ${RT}/install/manifest.cpp ${RT}/install/result.cpp
        ${RT}/install/disc_source.cpp ${RT}/install/sha256.cpp
        ARGS ${CMAKE_BINARY_DIR}/portable_scratch)
    # The game folder checks startup makes before any of the game runs
    # (install/installer.h: CheckInstalledFiles, every file by stat in any case;
    # CheckInstalledXex, the supported default.xex; GitHub issue #4), in a
    # scratch folder.
    sb_test(startup_game_files_test SOURCES ${RT}/install/placement.cpp ${RT}/install/installer.cpp ${RT}/install/locate.cpp
        ${RT}/install/manifest.cpp ${RT}/install/result.cpp ${RT}/install/disc_source.cpp ${RT}/install/sha256.cpp
        ${RT}/user/portable.cpp
        ARGS ${CMAKE_BINARY_DIR}/startup_game_files_scratch)
    if(APPLE AND NOT IOS)
        target_link_libraries(install_folder_test PRIVATE "-framework CoreFoundation")  # placement.cpp's iCloud check
        target_link_libraries(portable_test PRIVATE "-framework CoreFoundation")
        target_link_libraries(startup_game_files_test PRIVATE "-framework CoreFoundation")
    endif()
    # Saves written atomically (kernel/vfs.h, header only): the temporary files'
    # names, short writes, whole-file writes that keep the old file when they
    # fail, the rewrites the file objects make of saves (vfs::Staging), and the
    # clean-up of what crashed writes left, in a scratch folder. The XDG base
    # folders (user/paths.h, header only): an empty or relative $XDG_DATA_HOME or
    # $XDG_CACHE_HOME is ignored. POSIX only (mkdtemp, setrlimit, setenv).
    sb_test(saves_atomic_test ARGS ${CMAKE_CURRENT_BINARY_DIR})
    sb_test(saves_xdg_paths_test)
    # NFSMW_SAVE_DIR (user/paths.h, header only): saves and
    # profile_settings.bin from an absolute folder, as before otherwise;
    # imports_user.cpp and xam.cpp are read. POSIX only (setenv).
    sb_test(timedemo_save_dir_test ARGS ${RT})
endif()
# The SteamOS launcher's hybrid-graphics part (NVIDIA PRIME render offload,
# scripts/steamos/bundle/speedbreaker.sh) on made-up /sys, /proc and Vulkan
# folders. Bash, so not on Windows.
if(NOT WIN32)
    find_program(SB_BASH bash)
    if(SB_BASH)
        add_test(NAME launcher_prime_test COMMAND ${SB_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/launcher_prime_test.sh
            ${ROOT}/scripts/steamos/bundle/speedbreaker.sh)
    endif()
    # The launcher's start: the x86-64-v3 check (the loader's own answer, else
    # every flag of the level), the system's libstdc++ on multilib systems,
    # which C and C++ runtimes run the game, its library folder in a folder
    # named with ':', ';' or '$' (through /proc/self/fd), the log's private
    # last place; then the whole launcher on a made-up bundle. Linux only, as
    # the launcher is (/proc, bash 4).
    if(SB_BASH AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME launcher_start_test COMMAND ${SB_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/launcher_start_test.sh
            ${ROOT}/scripts/steamos/bundle/speedbreaker.sh)
    endif()
endif()
# The entitlements in a Mach-O's code signature (platform/code_signature.h),
# on files built byte by byte: what the iOS app reads from its own.
sb_test(code_signature_test SOURCES ${RT}/platform/code_signature.cpp)
# The [memory] line, the page shown when the guest's 4 GB can't be reserved
# (an iOS copy without Extended Virtual Addressing) and the low-memory notice
# (kernel/memory_status.h).
sb_test(memory_status_test SOURCES ${RT}/kernel/memory_status.cpp ${RT}/platform/code_signature.cpp)
# The version line in the log, and when the "What's new" note shows
# (user/version_check.h, header only).
sb_test(version_check_test)
# Check for Updates' logic: versions, GitHub's JSON, curl's output and the
# release notes (update/check.cpp, self-contained).
sb_test(update_check_test SOURCES ${RT}/update/check.cpp)
# Its transport on Linux and SteamOS (update/fetch_curl.cpp), with stand-in
# curls through NFSMW_UPDATE_CURL: answers, curl's exit codes, the deadline
# and the cap, and the arguments, environment and descriptors curl gets.
# Linux only, as the transport is (about six seconds: two deadlines).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    sb_test(update_fetch_test SOURCES ${RT}/update/fetch_curl.cpp ${RT}/update/check.cpp)
endif()
# Settings > Quit Game: where it is offered (not on iPhone or iPad) and its
# confirmation (ui/quit_game.h, header only); the runtime's code around it
# is read: the button and prompt gated, the focus on Cancel, B and Escape
# cancelling, and the quit taking the window close's way out.
sb_test(quit_game_test ARGS ${RT})
# The jump tables XenonRecomp makes switches of (config/switch_tables.toml)
# against the leaf functions' bounds (config/nfsmw.toml): no label or default
# outside its function, and the tables a borrowed bounds check broke (the
# Performance Tuning labels, GitHub issue #8) as the guest dispatches them.
sb_test(switch_tables_test ARGS ${ROOT}/config/switch_tables.toml ${ROOT}/config/nfsmw.toml)
# The write watch around CPU write faults, on physical memory mapped as the
# runtime maps it. Through stdafx.h it includes ppc/'s headers (generated
# from your own XEX) and XenonRecomp's, so only where both are: never in a
# fresh clone (SB_TESTS_ONLY, CI).
if(EXISTS ${ROOT}/ppc/ppc_config.h AND EXISTS ${ROOT}/tools/XenonRecomp/XenonUtils)
    sb_test(write_watch_test SOURCES ${RT}/kernel/write_watch.cpp ${RT}/cpu/guest_time.cpp)
    target_include_directories(write_watch_test PRIVATE ${RT}/kernel ${RT}/thirdparty/o1heap ${ROOT}/ppc
        ${ROOT}/tools/XenonRecomp/XenonUtils ${ROOT}/tools/XenonRecomp/thirdparty/simde)
    # The kernel dispatcher (kernel/dispatcher.cpp, with kernel/xdm.cpp's
    # reference counts) on stub guest memory: waits keep their objects when the
    # handles are closed, sets and pulses release only the threads waiting then
    # (a pulse never leaves the event signaled), guest-embedded KEVENT/KSEMAPHORE
    # headers are written outside the dispatcher's lock, and a closed periodic
    # timer stops. The same headers as write_watch_test.
    sb_test(kernel_dispatcher_test SOURCES ${RT}/kernel/dispatcher.cpp ${RT}/kernel/xdm.cpp ${RT}/cpu/guest_time.cpp)
    target_include_directories(kernel_dispatcher_test PRIVATE ${RT}/kernel ${RT}/thirdparty/o1heap ${ROOT}/ppc
        ${ROOT}/tools/XenonRecomp/XenonUtils ${ROOT}/tools/XenonRecomp/thirdparty/simde)
else()
    message(STATUS "write_watch_test and kernel_dispatcher_test left out: they need ppc/ (scripts/recompile.sh) and tools/XenonRecomp (scripts/setup_tools.sh)")
endif()
# The guest vblank lock's closed-loop simulation (a tool more than a test).
add_executable(vblank_lock_sim ${CMAKE_CURRENT_SOURCE_DIR}/vblank_lock_sim.cpp ${RT}/video/refresh_fit.cpp ${RT}/video/vblank_lock.cpp)
target_include_directories(vblank_lock_sim PRIVATE ${RT})
