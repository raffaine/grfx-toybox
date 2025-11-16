cmake_minimum_required(VERSION 3.20)
project(DX12MeshSkinningMinimal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT MSVC)
    message(FATAL_ERROR "This project requires MSVC on Windows.")
endif()

add_executable(DX12MeshSkinningMinimal
    WIN32
    src/main.cpp
    src/tinygltf.cpp
)

find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h")
target_include_directories(DX12MeshSkinningMinimal
    PRIVATE 
        ${TINYGLTF_INCLUDE_DIRS}
)

target_link_libraries(DX12MeshSkinningMinimal
    PRIVATE
        d3d12
        dxgi
        dxguid
)

add_custom_command(TARGET DX12MeshSkinningMinimal POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:DX12MeshSkinningMinimal>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/as.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ms.hlsl"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ps.hlsl"
        "$<TARGET_FILE_DIR:DX12MeshSkinningMinimal>/shaders/"
)
