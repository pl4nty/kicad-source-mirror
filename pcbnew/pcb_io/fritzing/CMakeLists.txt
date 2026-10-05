# Fritzing (.fzz/.fz) board importer

include_directories( ${CMAKE_CURRENT_SOURCE_DIR} )

set( FRITZING_SRCS
    pcb_io_fritzing.cpp
    fritzing_parser.cpp
    fritzing_part_library.cpp
    fritzing_generated_parts.cpp
    fritzing_svg.cpp
    )

add_library( fritzing2pcbnew STATIC ${FRITZING_SRCS} )

target_link_libraries( fritzing2pcbnew pcbcommon nanosvg )

target_include_directories( fritzing2pcbnew PRIVATE
                            "${CMAKE_CURRENT_SOURCE_DIR}/../" )
