add_executable(x87sidecar
    src/main.cpp
    src/macho_loader.cpp
    src/offset_finder.cpp
    src/opcode_table.cpp
    src/sidecar.cpp
    src/stub_asm.cpp
)

target_link_libraries(x87sidecar PRIVATE rosetta_core)

# Ad-hoc signed, NO entitlements.  Cooperative attach receives the task+thread
# ports via a voluntary Mach handshake, so it needs neither
# com.apple.security.cs.debugger nor com.apple.security.get-task-allow.
add_custom_command(TARGET x87sidecar POST_BUILD
    COMMAND codesign -s - --force "$<TARGET_FILE:x87sidecar>"
    COMMENT "Signing x87sidecar"
    VERBATIM
)
