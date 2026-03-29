# Generate a C file that embeds a binary resource.
# Usage: embed_resource(<input_file> <output_c_file> <symbol_prefix>)

function(embed_resource INPUT_FILE OUTPUT_C SYMBOL_PREFIX)
    get_filename_component(INPUT_ABS "${INPUT_FILE}" ABSOLUTE)

    add_custom_command(
        OUTPUT "${OUTPUT_C}"
        COMMAND ${CMAKE_COMMAND} -E echo "// Auto-generated. Do not edit." > "${OUTPUT_C}"
        COMMAND ${CMAKE_COMMAND} -E echo "#include <stddef.h>" >> "${OUTPUT_C}"
        COMMAND ${CMAKE_COMMAND} -E echo "" >> "${OUTPUT_C}"
        # Use xxd to generate the data array
        COMMAND sh -c "echo 'const unsigned char ${SYMBOL_PREFIX}_start[] = {' >> '${OUTPUT_C}' && xxd -i < '${INPUT_ABS}' >> '${OUTPUT_C}' && echo '};' >> '${OUTPUT_C}'"
        COMMAND sh -c "echo 'const unsigned char ${SYMBOL_PREFIX}_end[] = {};' >> '${OUTPUT_C}'"
        COMMAND sh -c "echo 'const size_t ${SYMBOL_PREFIX}_size = sizeof(${SYMBOL_PREFIX}_start);' >> '${OUTPUT_C}'"
        DEPENDS "${INPUT_ABS}"
        COMMENT "Embedding ${INPUT_FILE} as ${SYMBOL_PREFIX}"
        VERBATIM
    )
endfunction()
